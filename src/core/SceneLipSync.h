#pragma once
// Lip sync in scenes: what the plugin's hook of Actor::IsInFaction decides, written without the game so it is tested
// on Linux.
//
// Better Facial Animations leaves the face of anyone it thinks is in a SexLab or OStim scene to the scene: it asks
// the game whether the actor is in one of three factions, by the actor's virtual IsInFaction, from one function of
// its own, which it calls from seven places (the provider's actor check, a line's start, the face's update every
// frame, its morphs, its gaze). With the setting on, the hook answers "no" to exactly those questions: one of the
// three factions, asked from inside BFA's own module (the call's return address lies in its image). Every other
// caller, OStim, SexLab, SkyrimNet, the engine and every other plugin, gets the game's own answer.
//
// The plugin side (src/plugin/SceneHook.cpp) finds the factions at the data-loaded message, by plugin and local ID,
// checks the vtable slots and writes them, or installs nothing.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bfarb::scene {

// A scene faction BFA checks, by plugin and the local ID in it (the EditorID is for the log only, never looked up).
struct SceneFaction {
    const char* plugin;
    std::uint32_t localId;
    const char* editorId;
};

// BFA 4.3.2's three: SexLab's, and OStim Standalone's and OStim NG's two.
inline constexpr std::array<SceneFaction, 3> kSceneFactions{{
    {"SexLab.esm", 0xE50F, "SexLabAnimatingFaction"},
    {"OStim.esp", 0xECA, "OStimActorCountFaction"},
    {"OStim.esp", 0xD93, "OStimExcitementFaction"},
}};

// Actor::IsInFaction(const TESFaction*) const: its index in the main vtables of Character and PlayerCharacter on SE
// and AE (CommonLibSSE-NG's Actor.h declares it at 0F9, and its Actor::IsInFaction calls
// RelocateVirtual(0x0F9, 0x0FB): 0xFB is VR's, which the bridge refuses).
inline constexpr std::size_t kIsInFactionSlot = 0xF9;

// A range of addresses, [begin, end).
struct Range {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    bool Empty() const noexcept { return end <= begin; }
    bool Contains(std::uintptr_t a) const noexcept { return begin < end && a >= begin && a < end; }
};

// The scene factions found, in kSceneFactions' order; null where its plugin isn't loaded.
using Factions = std::array<const void*, kSceneFactions.size()>;

// The decision: true when the hook answers false in place of the game's answer. Only with the setting on, for one of
// the factions found (never a null one), asked from inside BFA's module (caller: the call's return address; bfa: its
// image). Everything else gets the game's answer.
bool AnswersFalse(bool enabled, const void* faction, const Factions& factions, std::uintptr_t caller, Range bfa) noexcept;

// The form ID of `localId` in a loaded plugin: a full one at compileIndex (0x00 to 0xFD, local IDs up to 0xFFFFFF),
// or a light one (0xFE, at smallIndex up to 0xFFF, local IDs up to 0xFFF). 0 when there is none: a local ID that
// doesn't fit, or an index out of range (0xFF: not loaded).
std::uint32_t FormIdIn(bool light, std::uint8_t compileIndex, std::uint16_t smallIndex, std::uint32_t localId) noexcept;

// Whether a loaded file's name is the plugin's, ignoring case (as BFA compares them).
bool SamePlugin(std::string_view fileName, std::string_view plugin) noexcept;

// Whether a vtable slot looks right to be written: the slot is pointer-aligned and lies in the game's image, and the
// function it holds lies in the game's code. A slot another plugin already redirected out of the game's code is left
// alone.
bool SlotLooksRight(std::uintptr_t slot, std::uintptr_t entry, Range gameImage, Range gameCode) noexcept;

// The site of a call, for the log: of the return addresses captured (innermost first), the first in BFA's image is
// inside its scene check; when the next is in BFA's image too, that one, the scene check's caller, is the site.
// Otherwise the first one in BFA; 0 when none is.
std::uintptr_t SiteOf(const std::uintptr_t* frames, std::size_t count, Range bfa) noexcept;

// Which of the calls the hook answered "no" to (for an actor really in a scene faction) are lines, for the log at
// Detailed. BFA asks from a few places once per line (a line's start, a provider's check) and from others every
// frame (the face's update, its morphs); a call is told by its site and its actor:
//   - the same site asking about the same actor again within kSameCheck is the same check, asking about another of
//     the actor's factions (OStim puts everyone in a scene in both of its): it counts for nothing;
//   - a site asked about the same actor again within kFrameGap is a per-frame site: learned for the session, never a
//     line;
//   - a call from any other site is a line, unless the same actor's last line was within kSameLine (one line's start
//     and its provider check, or the first frames of a face's updates);
//   - a log line is due at a line, at most once every kEvery, and counts the lines since the last log line.
// Not thread-safe: the plugin holds a lock around it.
class KeptLines {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kSameCheck{5};
    static constexpr std::chrono::milliseconds kFrameGap{250};
    static constexpr std::chrono::seconds kSameLine{2};
    static constexpr std::chrono::seconds kEvery{60};

    struct Due {
        bool log = false;      // a log line now
        std::uint32_t lines = 0;  // the lines kept since the last log line, this one included
    };

    Due Kept(std::uint32_t actor, std::uintptr_t site, Clock::time_point now);

private:
    struct Seen {
        std::uint32_t actor;
        std::uintptr_t site;
        Clock::time_point at;
    };
    struct Line {
        std::uint32_t actor;
        Clock::time_point at;
    };
    static constexpr std::size_t kMaxSeen = 64, kMaxFrameSites = 16, kMaxLines = 32;

    std::vector<Seen> seen_;
    std::vector<std::uintptr_t> frameSites_;
    std::vector<Line> lines_;
    std::uint32_t since_ = 0;
    bool logged_ = false;
    Clock::time_point lastLog_{};
};

// The load's log line when the hook is installed: the factions found, by their form IDs in this load order (0: its
// plugin isn't loaded), and those not found.
std::string InstalledText(const std::array<std::uint32_t, kSceneFactions.size()>& formIds);

// The factions as the log names them: "OStim.esp 0xECA (OStimActorCountFaction)".
std::string FactionName(std::size_t index);

// The Detailed log line for a line kept: the actor's form ID, the faction it is in, and the lines since the last.
std::string KeptText(std::uint32_t actorFormId, std::size_t factionIndex, std::uint32_t lines);

}  // namespace bfarb::scene
