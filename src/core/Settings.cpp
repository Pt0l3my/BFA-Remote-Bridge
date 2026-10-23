#include "core/Settings.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace bfarb {
namespace {

std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool EqualsNoCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::optional<bool> ParseBool(std::string_view s) noexcept {
    s = Unquote(s);
    for (const std::string_view t : {"true", "1", "yes", "on"})
        if (EqualsNoCase(s, t)) return true;
    for (const std::string_view f : {"false", "0", "no", "off"})
        if (EqualsNoCase(s, f)) return false;
    return std::nullopt;
}

std::optional<double> ParseNumber(std::string_view s) noexcept {
    s = Unquote(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return std::nullopt;
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

const SettingKey& Key(std::string_view path) {
    for (const SettingKey& k : SettingKeys())
        if (path == k.path) return k;
    throw std::logic_error("unknown setting");
}

class Reader {
public:
    Reader(const ConfigGetter& get, std::vector<std::string>* warnings) : get_(get), warnings_(warnings) {}

    std::string Raw(const SettingKey& k) {
        try {
            if (get_) return get_(k.path, k.defaultText);
        } catch (...) {
            Warn(std::string(k.path) + ": the value couldn't be read, so the default is used.");
        }
        return k.defaultText;
    }

    bool Bool(std::string_view path) {
        const SettingKey& k = Key(path);
        const std::string raw = Raw(k);
        if (const auto v = ParseBool(raw)) return *v;
        Warn(std::string(k.path) + ": \"" + raw + "\" isn't true or false, so the default " + k.defaultText + " is used.");
        return ParseBool(k.defaultText).value_or(false);
    }

    int Int(std::string_view path) {
        const SettingKey& k = Key(path);
        const std::string raw = Raw(k);
        std::optional<double> v = ParseNumber(raw);
        if (!v) {
            Warn(std::string(k.path) + ": \"" + raw + "\" isn't a number, so the default " + k.defaultText + " is used.");
            v = ParseNumber(k.defaultText);
        }
        double x = std::round(v.value_or(0.0));
        if (x < k.min) {
            Warn(std::string(k.path) + ": below the minimum, so " + std::to_string(k.min) + " is used.");
            x = k.min;
        } else if (x > k.max) {
            Warn(std::string(k.path) + ": above the maximum, so " + std::to_string(k.max) + " is used.");
            x = k.max;
        }
        return static_cast<int>(x);
    }

    std::string String(std::string_view path) { return std::string(Unquote(Raw(Key(path)))); }

    std::string Select(std::string_view path) {
        const SettingKey& k = Key(path);
        const std::string raw = Raw(k);
        const std::string_view s = Unquote(raw);
        for (const char* option : k.options)
            if (EqualsNoCase(s, option)) return option;
        Warn(std::string(k.path) + ": \"" + raw + "\" isn't one of the options, so " + k.defaultText + " is used.");
        return k.defaultText;
    }

private:
    void Warn(std::string line) {
        if (warnings_) warnings_->push_back(std::move(line));
    }
    const ConfigGetter& get_;
    std::vector<std::string>* warnings_;
};

}  // namespace

std::string_view Unquote(std::string_view s) noexcept {
    s = Trim(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = Trim(s.substr(1, s.size() - 2));
    return s;
}

const std::vector<SettingKey>& SettingKeys() {
    static const std::vector<SettingKey> keys = {
        {"serverAddress", SettingType::String, ""},
        {"serverPort", SettingType::Int, "7871", 1, 65535},
        {"showStatus", SettingType::Bool, "true"},
        {"logDetail", SettingType::Select, "Normal", 0, 0, {"Normal", "Detailed"}},
    };
    return keys;
}

Settings ReadSettings(const ConfigGetter& get, std::vector<std::string>* warnings) {
    Reader r(get, warnings);
    Settings s;
    s.serverAddress = r.String("serverAddress");
    s.serverPort = r.Int("serverPort");
    s.showStatus = r.Bool("showStatus");
    s.logDetail = r.Select("logDetail") == "Detailed" ? LogDetail::Detailed : LogDetail::Normal;
    return s;
}

std::string Describe(const Settings& s) {
    return std::string("server address ") + (s.serverAddress.empty() ? "not set (searched for on the local network)" : "set") + ", port " +
           std::to_string(s.serverPort) +
           ", timeout " + std::to_string(s.timeoutMs) + " ms, status on load " +
           (s.showStatus ? "on" : "off") + ", log detail " + (s.logDetail == LogDetail::Detailed ? "Detailed" : "Normal");
}

}  // namespace bfarb
