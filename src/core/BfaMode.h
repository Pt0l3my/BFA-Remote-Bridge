#pragma once
// BFA's mode, set by the bridge (the owner's rule for 1.3.0, extended to BFA 5.0.6 in 1.5.0): with a server that
// answered in protocol version 2 this session, BFA 4.3.2.3 and BFA 5.0.6 run in their CPU modes, so they open nothing
// on this PC's graphics card; the model runs on the server either way. A version 2 server fills in BFA 4.3.2.3's sizes
// (the sizes rule, docs/PROTOCOL.md); for BFA 5.0.6 the bridge lists them itself (core/BfaSizes.h). A version 1 server
// (every PandorumLLM release before the one that speaks version 2, patch7 included) gets BFA's own values back: the
// bridge tells a server's version by its frames alone, and sets BFA's CPU mode only for a version 2 server.
//
// From BFA 4.3.2's source (read for facts only): BFA reads Data\SKSE\Plugins\BetterFacialAnimations.ini once, in its
// SKSE load; its CPU mode is [General] ForceCPU=1 (the legacy form [General] GPU=0, read when ForceCPU isn't there);
// its menu applies changes live and its SaveIni writes the whole set back, ForceCPU included. BFA's DLL loads before
// the bridge's, so a write takes effect from the next start. A file BFA didn't write would make it run its
// migrations from version 0, so the bridge never creates it.
//
// From BFA 5.0.6's source (read for facts only): the CPU switch is split in two, [General] ForceCPU for Audio2Face
// and [General] EmotionDevice for Audio2Emotion (-1 the CPU; 0 and up a graphics card, 0 by default; when the key is
// missing, -1 with ForceCPU=1, else GpuAdapter). BFA 5 reads its settings in layers (its ReadIni): the installer's
// choice files Data\SKSE\Plugins\BetterFacialAnimations\Installer\<choice>.ini in a fixed order, then its own INI,
// which wins; a key's last value counts. Its installer writes ForceCPU and EmotionDevice in those files and leaves
// them commented out in the INI, so the bridge reads both as BFA does, and writes its values in BFA's own INI, which
// wins. IndependentGpuDevice is left as it is. The rule:
//
//   version 2 answered   with BFA 4.3.2.3 and its INI there: ForceCPU=1 written when it was 0, and the bridge's own
//                        state records that it set it (only then: a 1 the bridge found is the player's, or one it set
//                        whose record was lost, and either way nothing is put back for it); logged once, at info
//                        level.
//                        With BFA 5.0.6 (any 5.0.6.N) and its INI there: the same for ForceCPU, read through BFA 5's
//                        layers; and EmotionDevice=-1 written when it was a graphics card, the card's number recorded
//                        in the bridge's state. Another BFA version: left as it is, and a value the bridge set put back
//   each game save       the same again while version 2 was answered this session, so a save of BFA's menu in this
//                        session can't undo it (SKSE's save message: never DllMain or the process's exit)
//   version 1 answered   the values the bridge set put back (ForceCPU=0, EmotionDevice the card recorded), and the
//                        records cleared
//   nothing answered     nothing
//
// When in doubt, nothing is put back: only a record that reads exactly as the bridge wrote it counts, and only a value
// still the bridge's in BFA's own INI (ForceCPU 1, EmotionDevice -1) is changed back. No setting and no note on screen:
// the log lines are all. The files are the plugin's (src/plugin/BfaIni.cpp), behind IniStore, so the rule is tested on
// Linux.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/Log.h"

namespace bfarb {

// The BFA version whose files the sizes rule names: the first the bridge sets the CPU mode of.
constexpr const char* kCpuModeBfaVersion = "4.3.2.3";
// BFA 5.0.6, whose builds report 5.0.6.N (5.0.6.3 the first released): the second.
constexpr const char* kCpuModeBfa5Version = "5.0.6";

// BFA 5.0.6: "5.0.6" or "5.0.6.N".
bool IsBfa506(const std::string& version) noexcept;

// BFA 5.0.6's installer choice files, in the order its ReadIni loads them (Installer\<name>.ini), before its own INI.
inline constexpr std::array<const char*, 14> kBfa5InstallerChoices{
    "GraphicsRoute", "Startup", "FaceModel", "EmotionModel", "VanillaEmotions", "ActiveOnly", "PlayerAnimation",
    "PrepareEarly",  "Tongue",  "Gaze",      "Logging",      "Khajiit",         "Argonian",   "EmotionDevice"};

// The bridge's record of BFA 5's EmotionDevice, in its own state: the graphics card's number it replaced with -1.
constexpr const char* kEmotionDeviceRecord = "EmotionDeviceSetByBridge";

class IniStore {
public:
    virtual ~IniStore() = default;
    // Whether BFA's INI is there: the bridge never creates it.
    virtual bool BfaIniPresent() = 0;
    // BFA's CPU mode as BFA 4.3.2 reads it at its start, from its INI alone: 1 for ForceCPU=1 (or, without ForceCPU,
    // the legacy GPU=0), else 0.
    virtual int ReadForceCpu() = 0;
    // Writes [General] ForceCPU (0 or 1) in BFA's INI. False, with err, when it can't.
    virtual bool WriteForceCpu(int value, std::string& err) = 0;
    // The bridge's own record that it set ForceCPU=1 over a 0: true only when it reads exactly as written.
    virtual bool ReadSetByBridge() = 0;
    // Records it, or clears the record. False, with err, when it can't.
    virtual bool WriteSetByBridge(bool set, std::string& err) = 0;

    // [section] key, as written, in one of BFA's files: its own INI (choice null) or the installer's choice file
    // Installer\<choice>.ini. Empty when the file or the key isn't there.
    virtual std::optional<std::string> ReadBfaValue(const char* choice, const char* section, const char* key) = 0;
    // Writes [General] key in BFA's own INI, never creating it. False, with err, when it can't.
    virtual bool WriteBfaValue(const char* key, const std::string& value, std::string& err) = 0;
    // A key of the bridge's own state ([BfaMode] in BFARemoteBridge.ini), as written; empty when it isn't there.
    virtual std::string ReadRecord(const char* key) = 0;
    // Writes it, or clears it (value null). False, with err, when it can't.
    virtual bool WriteRecord(const char* key, const char* value, std::string& err) = 0;
};

// [section] key as BFA 5.0.6 reads it: the installer's choice files in kBfa5InstallerChoices' order, then its own
// INI; the last file that holds the key wins. Empty when none does.
std::optional<std::string> Bfa5Value(IniStore& store, const char* section, const char* key);
// A number as BFA reads one (SimpleIni's GetLongValue): def for a missing or empty value, or one that isn't wholly a
// decimal number (or 0x and hexadecimal digits).
long Bfa5Long(const std::optional<std::string>& value, long def);
// BFA 5.0.6's [General] Batch, the frames per run of its Legacy model: at least 1, 2 by default.
int Bfa5Batch(IniStore& store);

// BFA 5.0.6's processing devices as it reads them at its start.
struct Bfa5Devices {
    int forceCpu = 0;       // Audio2Face: 1 the CPU, 0 a graphics card
    long emotionDevice = 0;  // Audio2Emotion: -1 the CPU, 0 and up a graphics card's number
};
Bfa5Devices ReadBfa5Devices(IniStore& store);

class BfaCpuMode {
public:
    BfaCpuMode(std::unique_ptr<IniStore> store, LogSink log);

    // The gaming PC's BFA version, as the client string reports it ("4.3.2.3"; empty when unknown).
    void SetBfaVersion(const std::string& version);
    // The protocol version the server answered in: 2 sets BFA's CPU mode, 1 puts back the values the bridge set, 0
    // nothing.
    void OnServerVersion(std::uint16_t version);
    // A game save: BFA's CPU mode written again, while a server answered in version 2 this session.
    void OnGameSave();
    bool Set() const;  // a server answered in version 2 this session (and not version 1 since)

private:
    bool PresentLocked();
    void SetLocked();
    void SetBfa4Locked();
    void SetBfa5Locked();
    void PutBackLocked(const std::string& because);
    void PutBackForceCpuLocked(const std::string& because);
    void PutBackEmotionLocked(const std::string& because);
    void FailOnceLocked(const std::string& line);
    void Log(Level level, const std::string& line) const;

    std::unique_ptr<IniStore> store_;
    LogSink log_;
    mutable std::mutex mutex_;
    std::string bfaVersion_;
    bool v2_ = false;
    bool setLogged_ = false;
    bool failLogged_ = false;
    bool absentLogged_ = false;
    bool otherLogged_ = false;
};

}  // namespace bfarb
