// SkyrimNet's plugin settings: PublicGetPluginConfig and PublicGetPluginConfigValue, exports of SkyrimNet.dll since its
// API version 3, declared here by their signatures (no SkyrimNet header is used). Both sides are MSVC builds on the
// same runtime, so the std::string they return crosses the boundary.
//
// PublicGetPluginConfigValue answers the default it is given for a config SkyrimNet hasn't registered (yet), which
// can't be told from the player's value; PublicGetPluginConfig answers "null" (or "{}", or nothing) for one, so the
// settings count as read only once it names the config.

#include "plugin/Plugin.h"

namespace bfarb::plugin {
namespace {

using GetVersionFn = int (*)();
using GetConfigFn = std::string (*)(const char* pluginName);
using GetConfigValueFn = std::string (*)(const char* pluginName, const char* path, const char* defaultValue);

HMODULE SkyrimNet() { return ::GetModuleHandleW(L"SkyrimNet.dll"); }

}  // namespace

bool SkyrimNetLoaded() { return SkyrimNet() != nullptr; }

SettingsRead ReadSkyrimNetSettings(Settings& out, std::vector<std::string>& warnings) {
    out = Settings{};
    const HMODULE sn = SkyrimNet();
    if (!sn) return SettingsRead::NoSkyrimNet;
    const auto version = reinterpret_cast<GetVersionFn>(reinterpret_cast<void*>(::GetProcAddress(sn, "PublicGetVersion")));
    const auto config = reinterpret_cast<GetConfigFn>(reinterpret_cast<void*>(::GetProcAddress(sn, "PublicGetPluginConfig")));
    const auto get = reinterpret_cast<GetConfigValueFn>(reinterpret_cast<void*>(::GetProcAddress(sn, "PublicGetPluginConfigValue")));
    try {
        if (!version || version() < 3 || !config || !get) return SettingsRead::NoSkyrimNet;
        const std::string whole = config(kConfigName);
        const std::string_view trimmed = Unquote(whole);
        if (trimmed.empty() || trimmed == "null" || trimmed == "{}") return SettingsRead::NotRegistered;
    } catch (...) {
        return SettingsRead::NotRegistered;
    }
    out = ReadSettings(
        [get](std::string_view path, std::string_view def) {
            return get(kConfigName, std::string(path).c_str(), std::string(def).c_str());
        },
        &warnings);
    return SettingsRead::Read;
}

}  // namespace bfarb::plugin
