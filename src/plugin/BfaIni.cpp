// The plugin's IniStore (core/BfaMode.h): BFA's INI, BFA 5's installer choice files and the bridge's own state, with
// the relative paths BFA itself uses, from the game's folder. BFA 4.3.2 reads its file with GetPrivateProfileIntA, and
// so does ReadForceCpu. BFA 5.0.6 reads its files with SimpleIni, so ReadBfaValue reads each file's text and finds the
// key by SimpleIni's rules (core/IniText.h). Writes and removals go through Windows' profile calls, each checked
// afterwards by reading the file again as BFA 5 does. Called on the game's main thread only: at a task the client's
// version sink queued, at SKSE's save message, and at its post-load message (the start's records, BFA 5's Batch);
// never from DllMain or at the process's exit. The mode asks BfaIni first and changes nothing while BFA's INI can't be
// read; a read that fails in the moment after that answer still reads as the key's absence.
//
// BFA 5's own saves start its INI with a UTF-8 byte-order mark, which Windows' profile calls are known to take as part
// of the first line (not checked on Windows here). A file whose first line is [General] would then hide that section
// from them: a write would add a second [General] at the file's end, whose value BFA 5 reads (the last), and a
// removal there would find nothing, which the check reports. BFA's shipped INI starts with a comment, which its saves
// keep first.

#include <fstream>
#include <iterator>

#include "core/IniText.h"
#include "plugin/Plugin.h"

namespace bfarb::plugin {
namespace {

constexpr char kBfaIni[] = "Data\\SKSE\\Plugins\\BetterFacialAnimations.ini";
constexpr char kStateIni[] = "Data\\SKSE\\Plugins\\BFARemoteBridge.ini";
constexpr char kStateSection[] = "BfaMode";
constexpr char kInstallerFolder[] = "Data\\SKSE\\Plugins\\BetterFacialAnimations\\Installer\\";
// What a profile call answers for a key that isn't there: no value holds it.
constexpr char kAbsent[] = "\x01" "bfarb absent" "\x01";

// The bridge's own state: [section] key in file, as written (the profile call trims its blanks); empty when the file
// or the key isn't there.
std::optional<std::string> ReadValue(const char* file, const char* section, const char* key) {
    char value[256] = {};
    ::GetPrivateProfileStringA(section, key, kAbsent, value, sizeof value, file);
    if (std::string_view(value) == kAbsent) return std::nullopt;
    return std::string(value);
}

// BFA 5's files: [section] key as BFA 5 reads it; empty when the file can't be read or the key isn't there.
std::optional<std::string> ReadBfa5(const char* file, const char* section, const char* key) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return IniTextValue(text, section, key);
}

bool BfaIniPresent() {
    const DWORD attributes = ::GetFileAttributesA(kBfaIni);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

class ProfileIniStore final : public IniStore {
public:
    // There, and opened and read whole now: a file another program holds (a backup or sync tool, say) is unreadable,
    // where a profile call would answer its default and the bridge would take the key for gone.
    BfaIniState BfaIni() override {
        if (!BfaIniPresent()) return BfaIniState::Missing;
        std::ifstream in(kBfaIni, std::ios::binary);
        if (!in) return BfaIniState::Unreadable;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return in.bad() ? BfaIniState::Unreadable : BfaIniState::Readable;
    }
    int ReadForceCpu() override {
        // as BFA's LoadIni: the legacy GPU=0 is the default, ForceCPU overrides it
        const bool legacy = ::GetPrivateProfileIntA("General", "GPU", 1, kBfaIni) == 0;
        return ::GetPrivateProfileIntA("General", "ForceCPU", legacy ? 1 : 0, kBfaIni) != 0 ? 1 : 0;
    }
    bool WriteForceCpu(int value, std::string& err) override {
        if (!BfaIniPresent()) {  // never created: the profile call would make a file BFA didn't write
            err = "BFA's INI isn't there";
            return false;
        }
        return Write("General", "ForceCPU", value ? "1" : "0", kBfaIni, err);
    }
    std::optional<std::string> ReadBfaValue(const char* choice, const char* section, const char* key) override {
        if (!choice) return ReadBfa5(kBfaIni, section, key);
        return ReadBfa5((std::string(kInstallerFolder) + choice + ".ini").c_str(), section, key);
    }
    bool WriteBfaValue(const char* key, const std::string& value, std::string& err) override {
        if (!BfaIniPresent()) {  // never created
            err = "BFA's INI isn't there";
            return false;
        }
        if (!Write("General", key, value.c_str(), kBfaIni, err)) return false;
        const auto now = ReadBfa5(kBfaIni, "General", key);
        if (now == value) return true;
        err = "BFA would read " + (now ? "[General] " + std::string(key) + "=" + *now : std::string("no ") + key) +
              " there: its INI holds the key more than once, or its first line is hidden by a byte-order mark";
        return false;
    }
    bool RemoveBfaValue(const char* key, std::string& err) override {
        if (!BfaIniPresent()) {
            err = "BFA's INI isn't there";
            return false;
        }
        // each removal takes the first the profile calls find; a key given twice takes more than one
        for (int k = 0; k < 4 && ReadBfa5(kBfaIni, "General", key); ++k)
            if (!Write("General", key, nullptr, kBfaIni, err)) return false;
        if (const auto now = ReadBfa5(kBfaIni, "General", key)) {
            err = "BFA would still read [General] " + std::string(key) + "=" + *now +
                  " there: its first line is hidden by a byte-order mark, or it holds the key more often than removed";
            return false;
        }
        return true;
    }
    std::string ReadRecord(const char* key) override { return ReadValue(kStateIni, kStateSection, key).value_or(std::string()); }
    bool WriteRecord(const char* key, const char* value, std::string& err) override {
        return Write(kStateSection, key, value, kStateIni, err);
    }

private:
    static bool Write(const char* section, const char* key, const char* value, const char* file, std::string& err) {
        if (::WritePrivateProfileStringA(section, key, value, file)) return true;
        err = "Windows error " + std::to_string(::GetLastError());
        return false;
    }
};

}  // namespace

std::unique_ptr<IniStore> BfaIniStore() { return std::make_unique<ProfileIniStore>(); }

}  // namespace bfarb::plugin
