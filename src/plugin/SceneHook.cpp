// Lip sync in scenes: Better Facial Animations leaves the face of anyone in a SexLab or OStim scene to the scene, by
// asking the game whether the actor is in one of three factions (core/SceneLipSync.h). The bridge redirects one
// virtual, Actor::IsInFaction, on the main vtables of Character and PlayerCharacter (CommonLibSSE-NG's VTABLE_Character
// and VTABLE_PlayerCharacter, slot 0xF9 on SE and AE) to a thunk that answers "no" to BFA's own questions about those
// factions while the setting is on, and passes every other call to the game's function it replaced: OStim, SexLab,
// SkyrimNet, the engine and every other plugin get the game's answer. BFA's files are never changed.
//
// Installed once, at the data-loaded message, when the factions are loaded (BFA finds them then too). Nothing is
// installed, with one log line saying why, when BFA isn't loaded, its image's range can't be read, none of the
// factions is loaded, a slot doesn't hold a function in the game's code (another plugin redirected it first), or a
// vtable can't be written; a first slot already written is then put back. The thunks change an answer only once
// both slots are written (g_installed): should the first slot also fail to be put back, its thunk stays in place
// and passes every call on, and the log says so.
//
// The thunk runs on any thread, often every frame: it reads two atomics and fixed data, and logs only at Detailed, for
// a line (core/SceneLipSync.h's KeptLines), at most once a minute.

#include "plugin/Plugin.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "core/PeImage.h"
#include "core/SceneLipSync.h"

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define BFARB_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#else
#define BFARB_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#endif

namespace bfarb::plugin {
namespace {

using IsInFactionFn = bool (*)(const RE::Actor*, const RE::TESFaction*);

constexpr std::size_t kHeaderBytes = 0x1000;  // a loaded module's first page: its headers

std::atomic<bool> g_on{true};  // the setting; on until the page is read, as its default
std::atomic<bool> g_detailed{false};
// Both slots written: until then (and for good, when only one could be), every call is passed on.
std::atomic<bool> g_installed{false};
// Set before the vtables are written, then fixed: the thunk reads them without a lock.
scene::Factions g_factions{};
scene::Range g_bfa{};

std::mutex g_keptMutex;
scene::KeptLines g_kept;  // under g_keptMutex

// At Detailed: whether this call kept a line from the scene (the actor really is in the faction), and the log line
// when one is due. Never throws.
void NoteKept(const RE::Actor* actor, const RE::TESFaction* faction, IsInFactionFn original) noexcept {
    try {
        if (!actor || !original || !original(actor, faction)) return;  // not in that faction: nothing was kept
        void* frames[8] = {};
        const USHORT n = ::RtlCaptureStackBackTrace(0, static_cast<ULONG>(std::size(frames)), frames, nullptr);
        std::uintptr_t addresses[std::size(frames)] = {};
        for (USHORT i = 0; i < n; ++i) addresses[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
        const std::uintptr_t site = scene::SiteOf(addresses, n, g_bfa);
        const std::uint32_t formId = actor->GetFormID();
        scene::KeptLines::Due due;
        {
            std::lock_guard lock(g_keptMutex);
            due = g_kept.Kept(formId, site, scene::KeptLines::Clock::now());
        }
        if (!due.log) return;
        std::size_t index = 0;
        while (index < g_factions.size() && g_factions[index] != faction) ++index;
        Log(Level::Info, scene::KeptText(formId, index, due.lines));
    } catch (...) {
    }
}

// One thunk per vtable, each with the function it replaced.
template <std::size_t Which>
struct IsInFactionHook {
    static inline std::atomic<IsInFactionFn> original{nullptr};

    static bool Thunk(const RE::Actor* actor, const RE::TESFaction* faction) {
        const std::uintptr_t caller = BFARB_RETURN_ADDRESS();
        const IsInFactionFn real = original.load(std::memory_order_acquire);
        const bool enabled = g_installed.load(std::memory_order_acquire) && g_on.load(std::memory_order_relaxed);
        if (!scene::AnswersFalse(enabled, faction, g_factions, caller, g_bfa))
            return real(actor, faction);
        if (g_detailed.load(std::memory_order_relaxed)) NoteKept(actor, faction, real);
        return false;
    }
};

struct Slot {
    const char* name;
    REL::VariantID vtable;
    std::atomic<IsInFactionFn>* original;
    IsInFactionFn thunk;
    void** at = nullptr;
    IsInFactionFn entry = nullptr;
};

bool WriteSlot(void** at, void* value, DWORD& error) {
    DWORD old = 0;
    if (!::VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old)) {
        error = ::GetLastError();
        return false;
    }
    ::InterlockedExchangePointer(at, value);
    DWORD restored = 0;
    ::VirtualProtect(at, sizeof(void*), old, &restored);
    return true;
}

std::string Hex(std::uintptr_t v) {
    char s[24];
    std::snprintf(s, sizeof s, "0x%llX", static_cast<unsigned long long>(v));
    return s;
}

struct Outcome {
    std::string why;  // empty: installed
    bool slotLeft = false;  // a thunk stayed in a vtable (passing every call on): the first couldn't be put back
};

// Installs the hook, or says why not (and changes nothing, but in Outcome::slotLeft's case). formIds: the factions
// found.
Outcome Install(std::array<std::uint32_t, scene::kSceneFactions.size()>& formIds) {
    const HMODULE bfa = ::GetModuleHandleW(L"BetterFacialAnimations.dll");
    if (!bfa) return {"Better Facial Animations isn't loaded"};
    const auto* bfaBase = reinterpret_cast<const std::uint8_t*>(bfa);
    const std::size_t bfaSize = pe::ImageSize(bfaBase, kHeaderBytes);
    if (bfaSize == 0) return {"Better Facial Animations' image range couldn't be read from its headers"};
    const scene::Range bfaRange{reinterpret_cast<std::uintptr_t>(bfaBase), reinterpret_cast<std::uintptr_t>(bfaBase) + bfaSize};

    // the factions, by plugin and local ID, as BFA finds them (never by EditorID)
    scene::Factions factions{};
    auto* data = RE::TESDataHandler::GetSingleton();
    if (!data) return {"the game's data handler isn't there"};
    for (RE::TESFile* file : data->files) {
        if (!file || file->compileIndex == 0xFF) continue;  // not loaded
        for (std::size_t i = 0; i < scene::kSceneFactions.size(); ++i) {
            const scene::SceneFaction& f = scene::kSceneFactions[i];
            if (factions[i] || !scene::SamePlugin(file->GetFilename(), f.plugin)) continue;
            const std::uint32_t id = scene::FormIdIn(file->compileIndex == 0xFE, file->compileIndex, file->smallFileCompileIndex, f.localId);
            if (auto* faction = id ? RE::TESForm::LookupByID<RE::TESFaction>(id) : nullptr) {
                factions[i] = faction;
                formIds[i] = id;
            }
        }
    }
    if (std::none_of(factions.begin(), factions.end(), [](const void* f) { return f != nullptr; }))
        return {"none of BFA's scene factions is loaded (" + scene::FactionName(0) + ", " + scene::FactionName(1) + ", " +
                scene::FactionName(2) + "), so BFA has no scene to leave faces to"};

    // the slots: in the game's image, each holding a function in the game's code
    const auto& game = REL::Module::get();
    const std::uintptr_t gameBase = game.base();
    const std::size_t gameSize = pe::ImageSize(reinterpret_cast<const std::uint8_t*>(gameBase), kHeaderBytes);
    const auto text = game.segment(REL::Segment::textx);
    const scene::Range image{gameBase, gameBase + gameSize};
    const scene::Range code{text.address(), text.address() + text.size()};
    if (image.Empty() || code.Empty()) return {"the game's image range couldn't be read"};
    Slot slots[] = {
        {"Character", RE::VTABLE_Character[0], &IsInFactionHook<0>::original, &IsInFactionHook<0>::Thunk},
        {"PlayerCharacter", RE::VTABLE_PlayerCharacter[0], &IsInFactionHook<1>::original, &IsInFactionHook<1>::Thunk},
    };
    for (Slot& s : slots) {
        const std::uintptr_t vtable = s.vtable.address();
        const std::uintptr_t at = vtable ? vtable + scene::kIsInFactionSlot * sizeof(void*) : 0;
        if (!vtable || !image.Contains(at)) return {std::string(s.name) + "'s vtable isn't in the game's image"};
        s.at = reinterpret_cast<void**>(at);
        s.entry = reinterpret_cast<IsInFactionFn>(*s.at);
        if (!scene::SlotLooksRight(at, reinterpret_cast<std::uintptr_t>(s.entry), image, code))
            return {std::string(s.name) + "'s IsInFaction slot doesn't hold a function in the game's code (" +
                    Hex(reinterpret_cast<std::uintptr_t>(s.entry)) + "; another plugin may have redirected it)"};
    }

    // fixed before any thunk can run
    g_factions = factions;
    g_bfa = bfaRange;
    for (Slot& s : slots) s.original->store(s.entry, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    DWORD error = 0;
    if (!WriteSlot(slots[0].at, reinterpret_cast<void*>(slots[0].thunk), error))
        return {std::string(slots[0].name) + "'s vtable couldn't be written (error " + std::to_string(error) + ")"};
    if (!WriteSlot(slots[1].at, reinterpret_cast<void*>(slots[1].thunk), error)) {
        // g_installed stays false: the thunk in the first slot, if it stays there, passes every call on
        DWORD undoError = 0;
        const std::string why = std::string(slots[1].name) + "'s vtable couldn't be written (error " + std::to_string(error) + ")";
        if (WriteSlot(slots[0].at, reinterpret_cast<void*>(slots[0].entry), undoError))
            return {why + "; " + slots[0].name + "'s was put back"};
        return {why + ", and " + slots[0].name + "'s couldn't be put back (error " + std::to_string(undoError) + ")", true};
    }
    g_installed.store(true, std::memory_order_release);
    return {};
}

}  // namespace

void InstallSceneHook() {
    static bool tried = false;  // main thread only
    if (tried) return;
    tried = true;
    std::array<std::uint32_t, scene::kSceneFactions.size()> formIds{};
    Outcome outcome;
    try {
        outcome = Install(formIds);
    } catch (const std::exception& e) {
        outcome = {std::string("an error: ") + e.what()};
    } catch (...) {
        outcome = {"an unknown error"};
    }
    if (outcome.why.empty())
        Log(Level::Info, scene::InstalledText(formIds));
    else if (!outcome.slotLeft)
        Log(Level::Warn, "Lip sync in scenes: not installed, since " + outcome.why +
                             ". Nothing was changed: BFA leaves the faces of everyone in a scene to the scene, as without the bridge.");
    else
        Log(Level::Warn, "Lip sync in scenes: not installed, since " + outcome.why +
                             ". That slot still holds the bridge's redirect, which passes every call on to the game's function, so no "
                             "answer changes: BFA leaves the faces of everyone in a scene to the scene, as without the bridge.");
}

void SetSceneLipSync(bool on, bool detailed) {
    g_on.store(on, std::memory_order_relaxed);
    g_detailed.store(detailed, std::memory_order_relaxed);
}

}  // namespace bfarb::plugin
