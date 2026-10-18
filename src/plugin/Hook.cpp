// The hook: one entry of Better Facial Animations' import address table, KERNEL32's GetProcAddress, points at the
// bridge's replacement, which calls the real GetProcAddress and changes one answer only: a lookup of OrtGetApiBase (by
// name) in a module whose file is onnxruntime.dll gets the bridge's OrtGetApiBase (core/Bridge.h). BFA's files are never
// changed; the patch lives in memory for the session.
//
// BFA loads onnxruntime.dll only after a save loads (BFA 4.3.2's LoadApi, on its worker thread), so SKSE's post-load
// message, when every plugin is loaded, is early enough.

#include "plugin/Plugin.h"

#include "core/PeImage.h"

namespace bfarb::plugin {
namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
std::atomic<GetProcAddressFn> g_real{nullptr};

std::string Utf8(const wchar_t* w) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n < path.size() ? n : 0);
    return path;
}

FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR name) {
    const GetProcAddressFn real = g_real.load();
    FARPROC proc = real ? real(module, name) : nullptr;
    if (!proc || IS_INTRESOURCE(name) || std::strcmp(name, "OrtGetApiBase") != 0) return proc;
    try {
        const std::string file = Utf8(ModulePath(module).c_str());
        if (!pe::IsOrtGetApiBaseLookup(name, file)) return proc;
        return reinterpret_cast<FARPROC>(Bridge::Hook(reinterpret_cast<OrtGetApiBaseFn>(proc)));
    } catch (...) {
        return proc;
    }
}

}  // namespace

BfaFacts FindAndHookBfa() {
    BfaFacts facts;
    const HMODULE bfa = ::GetModuleHandleW(L"BetterFacialAnimations.dll");
    if (!bfa) {
        Log(Level::Info, "Better Facial Animations isn't loaded: the bridge stays idle");
        return facts;
    }
    facts.found = true;
    const auto* image = reinterpret_cast<const std::uint8_t*>(bfa);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;

    // its version, from the data SKSE reads
    std::uint32_t rva = 0;
    if (pe::FindExport(image, size, "SKSEPlugin_Version", rva) && rva + sizeof(pe::SksePluginVersion) <= size) {
        pe::SksePluginVersion v{};
        std::memcpy(&v, image + rva, sizeof v);
        facts.version = pe::UnpackVersion(v.pluginVersion);
    }
    Log(Level::Info, "Better Facial Animations " + (facts.version.empty() ? std::string("(version unknown)") : facts.version) + " found");
    if (facts.version.rfind("4.3.2", 0) != 0)
        Log(Level::Warn, "The bridge was made for Better Facial Animations 4.3.2; it attaches anyway, and passes everything through if its ONNX "
                         "Runtime isn't the one it knows");

    // BFA's runtime already loaded: BFA has its OrtGetApiBase already, so the bridge stays out of the way
    std::wstring folder = ModulePath(bfa);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    if (::GetModuleHandleW((folder + L"BetterFacialAnimations\\onnxruntime.dll").c_str())) {
        facts.problem = "its ONNX Runtime was loaded before the bridge could attach";
        Log(Level::Warn, "Better Facial Animations' ONNX Runtime is already loaded, so the bridge stays out of the way");
        return facts;
    }

    std::size_t slotRva = 0;
    if (!pe::FindImportSlot(image, size, "KERNEL32.dll", "GetProcAddress", slotRva)) {
        facts.problem = "it doesn't import GetProcAddress from KERNEL32 by name";
        Log(Level::Warn, "Better Facial Animations doesn't import GetProcAddress from KERNEL32 by name: the bridge can't attach and stays idle");
        return facts;
    }
    auto* slot = reinterpret_cast<void**>(const_cast<std::uint8_t*>(image) + slotRva);
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        facts.problem = "its import table couldn't be written";
        Log(Level::Warn, "Better Facial Animations' import table couldn't be made writable (error " + std::to_string(::GetLastError()) +
                             "): the bridge stays idle");
        return facts;
    }
    g_real.store(reinterpret_cast<GetProcAddressFn>(*slot));
    *slot = reinterpret_cast<void*>(&HookedGetProcAddress);
    DWORD restored = 0;
    ::VirtualProtect(slot, sizeof(void*), old, &restored);
    facts.hooked = true;
    Log(Level::Info, "Attached to Better Facial Animations' lookup of ONNX Runtime");
    return facts;
}

}  // namespace bfarb::plugin
