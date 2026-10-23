#pragma once
// The bridge's settings: their keys on SkyrimNet's Settings > Plugins page (config BFARemoteBridge, the file
// data/SKSE/Plugins/SkyrimNet/external/pt0l3my.bfaremotebridge/settings/BFARemoteBridge.yaml), their defaults, which
// must equal that file's, their ranges, and the parse of the text SkyrimNet returns for each. Keys of older versions
// (1.1's enabled, accessKey, timeoutMs and checkCalls) are never asked for, so their saved values are ignored.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bfarb {

constexpr const char* kConfigName = "BFARemoteBridge";
constexpr int kRunTimeoutMs = 1000;  // each lip-sync call's time limit, fixed since 1.2

enum class LogDetail { Normal, Detailed };

// The page holds only what the server can't tell the bridge: where the server is (an address for a server that
// doesn't answer the search, a version 1 one; and the port), whether BFA's lip sync reaches speakers in a scene, and
// how the monitor shows (status on load, log detail). The hardware is chosen on PandorumLLM's side.
struct Settings {
    std::string serverAddress;  // empty: found on the local network (FIND and FOUND, protocol version 2)
    int serverPort = 7871;
    std::string accessKey;  // not on the page since 1.2 (PandorumLLM admits by its Remote IP): empty; never logged
    int timeoutMs = kRunTimeoutMs;  // not on the page since 1.2: fixed (kRunTimeoutMs); the tests set their own
    bool sceneLipSync = true;  // BFA's scene checks answered "not in a scene" (core/SceneLipSync.h); applies at once
    bool showStatus = true;
    LogDetail logDetail = LogDetail::Normal;

    bool operator==(const Settings&) const = default;
};

enum class SettingType { Bool, Int, String, Select };

struct SettingKey {
    const char* path;
    SettingType type;
    const char* defaultText;
    int min = 0;
    int max = 0;
    std::vector<const char*> options = {};
};

// Every key, in the page's order.
const std::vector<SettingKey>& SettingKeys();

// get(path, default) returns SkyrimNet's text for the key (PublicGetPluginConfigValue). A value that doesn't
// parse falls back on the default, a number out of range is clamped, and each such repair adds a line to
// warnings (never with the access key's value).
using ConfigGetter = std::function<std::string(std::string_view path, std::string_view defaultText)>;
Settings ReadSettings(const ConfigGetter& get, std::vector<std::string>* warnings = nullptr);

// One line for the log: every setting but the server address and the access key, which show only as set or not
// (a log may be shared, and an address says where someone's PC is).
std::string Describe(const Settings& s);

// Trims and drops one pair of matching quotes, as YAML text may carry them.
std::string_view Unquote(std::string_view s) noexcept;

}  // namespace bfarb
