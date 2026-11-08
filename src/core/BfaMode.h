#pragma once
// BFA's mode, set by the bridge (the owner's rule for 1.3.0, extended to BFA 5.0.6 in 1.5.0): with a server that
// answered in protocol version 2 this session, BFA 4.3.2.3 and BFA 5.0.6 run in their CPU modes from the next game
// start (BFA 4.3.2.3 opening nothing on this PC's graphics card, BFA 5.0.6 creating no graphics device there); the
// model runs on the server either way. A version 2 server fills in BFA 4.3.2.3's sizes
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
// which wins; a key's last value counts. It reads them with SimpleIni, whose rules the bridge follows in its own code
// (core/IniText.h). Its installer writes ForceCPU and EmotionDevice in those files and leaves them commented out in
// the INI, so the bridge reads both through the layers, and writes its values in BFA's own INI, which wins. BFA 5's
// menu saves (its SaveIni) write every setting back into its own INI from what BFA loaded at its start, as BFA
// 4.3.2.3's do for ForceCPU. IndependentGpuDevice is left as it is. The rule:
//
//   a game start         a record whose value BFA's own INI no longer holds (ForceCPU 1, EmotionDevice -1) is cleared:
//                        BFA loaded another value at this start, the one put back or the player's own (SKSE's
//                        post-load message, after BFA's own load). The EmotionDevice record only at a start of BFA
//                        5.0.6, whose key it is. A start without BFA's INI leaves both: BFA loaded nothing of it
//                        (its mod disabled for a while, say), and the records wait for the file's return
//   version 2 answered   with BFA 4.3.2.3 and its INI there: ForceCPU=1 written when it was 0, and the bridge's own
//                        state records that it set it (only then: a 1 the bridge found is the player's, or one it set
//                        whose record was lost, and either way nothing is put back for it); logged once, at info
//                        level.
//                        With BFA 5.0.6 (any 5.0.6.N) and its INI there: the same for ForceCPU, read through BFA 5's
//                        layers; and EmotionDevice=-1 written when it was a graphics card. Each record says how to put
//                        the key back: the value BFA's own INI held (ForceCPU 0, EmotionDevice the card's number), or
//                        "added" when BFA's own INI held no such key, so that the installer's files decide again. A card
//                        picked in BFA's menu since replaces the card recorded. Another BFA version: left as it is,
//                        and a value the bridge set put back (at an answer: so only once the bridge has attached to
//                        that BFA and a server has answered)
//   version 1 answered   the values the bridge set put back: written as recorded, or the key the bridge added removed.
//                        The records stay until a start has loaded what was put back; a record whose value is no
//                        longer in BFA's own INI is cleared, with nothing put back
//   each game save       what the last answer of this session did, done again where BFA's own INI no longer shows it:
//                        a save of BFA's menu writes the settings BFA holds, those it loaded at its start unless changed
//                        in the menu since (SKSE's save message: never DllMain or the process's exit). A change in
//                        BFA's menu after the last game save stays, and BFA loads it at the next start
//   nothing answered     nothing
//
// When in doubt, nothing is put back: only a record that reads exactly as the bridge wrote it counts, and only a value
// still the bridge's in BFA's own INI (ForceCPU 1, EmotionDevice -1) is changed back. So a CPU picked in BFA's menu, or
// set by hand before the next start, while the bridge's value is there can't be told from the bridge's own, and is put
// back too. BFA's INI there but unreadable (another program holding it, say): nothing is cleared or written at that
// start, answer or save, logged once a session. With BFA 4.3.2.3, which has no EmotionDevice, that record is left as it
// is. No setting and no note on
// screen: the log lines are all. The files are the plugin's (src/plugin/BfaIni.cpp), behind IniStore, so the rule is
// tested on Linux.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "core/Log.h"

namespace bfarb {

// The BFA version whose files the sizes rule names: the first the bridge sets the CPU mode of.
constexpr const char* kCpuModeBfaVersion = "4.3.2.3";
// BFA 5.0.6, whose builds report 5.0.6.N (N its re-release count; the bridge was checked against 5.0.6.3): the
// second. The rule and the sizes (core/BfaSizes.h) apply to every 5.0.6.N.
constexpr const char* kCpuModeBfa5Version = "5.0.6";

// BFA 5.0.6: "5.0.6" or "5.0.6.N".
bool IsBfa506(const std::string& version) noexcept;

// BFA 5.0.6's installer choice files, in the order its ReadIni loads them (Installer\<name>.ini), before its own INI.
inline constexpr std::array<const char*, 14> kBfa5InstallerChoices{
    "GraphicsRoute", "Startup", "FaceModel", "EmotionModel", "VanillaEmotions", "ActiveOnly", "PlayerAnimation",
    "PrepareEarly",  "Tongue",  "Gaze",      "Logging",      "Khajiit",         "Argonian",   "EmotionDevice"};

// The bridge's records, in its own state ([BfaMode] in BFARemoteBridge.ini). ForceCPU's: "1" (put back as 0) or
// "added". EmotionDevice's: the graphics card's number it replaced with -1 (0 to 999, put back as written) or "added".
constexpr const char* kForceCpuRecord = "ForceCPUSetByBridge";
constexpr const char* kEmotionDeviceRecord = "EmotionDeviceSetByBridge";
// A record's value when BFA's own INI held no such key: put back by removing the key.
constexpr const char* kAddedRecord = "added";

// BFA's own INI as the bridge finds it: not there, there but not readable just now (opened or read whole), or read.
enum class BfaIniState { Missing, Unreadable, Readable };

class IniStore {
public:
    virtual ~IniStore() = default;
    // Whether BFA's INI is there (the bridge never creates it) and, if so, whether it can be read now.
    virtual BfaIniState BfaIni() = 0;
    // BFA's CPU mode as BFA 4.3.2 reads it at its start, from its INI alone (Windows' profile calls): 1 for ForceCPU=1
    // (or, without ForceCPU, the legacy GPU=0), else 0.
    virtual int ReadForceCpu() = 0;
    // Writes [General] ForceCPU (0 or 1) in BFA's INI, as BFA 4.3.2 reads it. False, with err, when it can't.
    virtual bool WriteForceCpu(int value, std::string& err) = 0;

    // [section] key in one of BFA's files, as BFA 5 reads each (core/IniText.h): its own INI (choice null) or the
    // installer's choice file Installer\<choice>.ini. Empty when the file or the key isn't there.
    virtual std::optional<std::string> ReadBfaValue(const char* choice, const char* section, const char* key) = 0;
    // Writes [General] key in BFA's own INI, never creating it. False, with err, when it can't, or when BFA 5 would
    // still read another value there.
    virtual bool WriteBfaValue(const char* key, const std::string& value, std::string& err) = 0;
    // Removes [General] key from BFA's own INI. False, with err, when it can't, or when BFA 5 would still read one.
    virtual bool RemoveBfaValue(const char* key, std::string& err) = 0;
    // A key of the bridge's own state ([BfaMode] in BFARemoteBridge.ini), as written; empty when it isn't there.
    virtual std::string ReadRecord(const char* key) = 0;
    // Writes it, or clears it (value null). False, with err, when it can't.
    virtual bool WriteRecord(const char* key, const char* value, std::string& err) = 0;
};

// [section] key as BFA 5.0.6 reads it: the installer's choice files in kBfa5InstallerChoices' order, then its own
// INI; the last file that holds the key wins. Empty when none does.
std::optional<std::string> Bfa5Value(IniStore& store, const char* section, const char* key);
// A number as BFA 5 reads one (SimpleIni's GetLongValue, on Windows, where a long has 32 bits): the C library's strtol
// on the whole value, decimal, or hexadecimal after 0x or 0X. Empty, when BFA takes its default instead: a value
// missing, empty, of 64 characters or more, not wholly a number, or outside a long's range.
std::optional<long> Bfa5Number(const std::optional<std::string>& value);
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
    // A game start, once BFA has loaded its INI and the version is set: the records BFA's own INI no longer shows
    // cleared (EmotionDevice's only for BFA 5.0.6); none when the INI isn't there or can't be read.
    void OnStart();
    // The protocol version the server answered in: 2 sets BFA's CPU mode, 1 puts back the values the bridge set, 0
    // nothing.
    void OnServerVersion(std::uint16_t version);
    // A game save: what the last answer of this session did, done again where BFA's own INI no longer shows it.
    void OnGameSave();
    bool Set() const;  // a server answered in version 2 this session (and not version 1 since)

private:
    BfaIniState StateLocked();
    bool PresentLocked();
    bool Bfa4Locked() const;
    bool OwnHolds(const char* key);
    bool ForceCpuIsBridgesLocked();
    bool EmotionIsBridgesLocked();
    void ClearRecordLocked(const char* record, const char* key);
    void SetLocked();
    void SetBfa4Locked();
    void SetBfa5Locked();
    void PutBackLocked(const std::string& because);
    void PutBackKeyLocked(const char* record, const char* key, const std::string& value, bool bridges, const std::string& because);
    void FailOnceLocked(const std::string& key, const std::string& line);
    void Log(Level level, const std::string& line) const;

    std::unique_ptr<IniStore> store_;
    LogSink log_;
    mutable std::mutex mutex_;
    std::string bfaVersion_;
    std::uint16_t answered_ = 0;  // the last answer's version this session: 0 none, 1 or 2
    std::set<std::string> setLogged_, failLogged_;  // by key: logged once a session
    std::set<std::string> putBack_;  // the keys put back this session (and logged so, once)
    bool absentLogged_ = false;
    bool unreadableLogged_ = false;
    bool otherLogged_ = false;
};

}  // namespace bfarb
