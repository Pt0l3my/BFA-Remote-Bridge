// SkyrimNet's plugin settings: PublicGetPluginConfigValue, an export of SkyrimNet.dll since its API version 3, declared
// here by its signature (pluginName, dot-path, default) -> the value's text. No SkyrimNet header is used. Both sides are
// MSVC builds on the same runtime, so the std::string it returns crosses the boundary.

#include "plugin/Plugin.h"

namespace bfarb::plugin {
namespace {

using GetVersionFn = int (*)();
using GetConfigValueFn = std::string (*)(const char* pluginName, const char* path, const char* defaultValue);

}  // namespace

bool ReadSkyrimNetSettings(Settings& out, std::vector<std::string>& warnings) {
    out = Settings{};
    const HMODULE sn = ::GetModuleHandleW(L"SkyrimNet.dll");
    if (!sn) return false;
    const auto version = reinterpret_cast<GetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(sn, "PublicGetVersion")));
    const auto get = reinterpret_cast<GetConfigValueFn>(reinterpret_cast<void*>(::GetProcAddress(sn, "PublicGetPluginConfigValue")));
    try {
        if (!version || version() < 3 || !get) return false;
    } catch (...) {
        return false;
    }
    out = ReadSettings(
        [get](std::string_view path, std::string_view def) {
            return get(kConfigName, std::string(path).c_str(), std::string(def).c_str());
        },
        &warnings);
    return true;
}

}  // namespace bfarb::plugin
