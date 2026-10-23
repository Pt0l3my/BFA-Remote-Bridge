// The plugin's IniStore (core/BfaMode.h): BFA's INI and the bridge's own state, through Windows' profile calls, as
// BFA itself reads and writes its file (GetPrivateProfileIntA and WritePrivateProfileStringA, with the same relative
// path, from the game's folder). Called on the game's main thread only: at a task the client's version sink queued,
// and at SKSE's save message; never from DllMain or at the process's exit.

#include "plugin/Plugin.h"

namespace bfarb::plugin {
namespace {

constexpr char kBfaIni[] = "Data\\SKSE\\Plugins\\BetterFacialAnimations.ini";
constexpr char kStateIni[] = "Data\\SKSE\\Plugins\\BFARemoteBridge.ini";
constexpr char kStateSection[] = "BfaMode";
constexpr char kStateKey[] = "ForceCPUSetByBridge";  // "1": the bridge set ForceCPU=1 over a 0

class ProfileIniStore final : public IniStore {
public:
    bool BfaIniPresent() override {
        const DWORD attributes = ::GetFileAttributesA(kBfaIni);
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
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
    bool ReadSetByBridge() override {
        // exactly as written, or nothing: a damaged or lost record puts nothing back
        char value[8] = {};
        ::GetPrivateProfileStringA(kStateSection, kStateKey, "", value, sizeof value, kStateIni);
        return std::string_view(value) == "1";
    }
    bool WriteSetByBridge(bool set, std::string& err) override {
        return Write(kStateSection, kStateKey, set ? "1" : nullptr, kStateIni, err);
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
