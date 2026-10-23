#pragma once
// BFA's mode, set by the bridge (the owner's rule for 1.3.0): with a server that answered in protocol version 2 this
// session, BFA 4.3.2.3 runs in its CPU mode, so it opens nothing on this PC's graphics card; the model runs on the
// server either way, which fills in BFA 4.3.2.3's sizes (the sizes rule, docs/PROTOCOL.md). A version 1 server (every
// PandorumLLM release before the one that speaks version 2, patch7 included) gets BFA's own value back: the bridge
// tells a server's version by its frames alone, and sets BFA's CPU mode only for a version 2 server.
//
// From BFA 4.3.2's source (read for facts only): BFA reads Data\SKSE\Plugins\BetterFacialAnimations.ini once, in its
// SKSE load; its CPU mode is [General] ForceCPU=1 (the legacy form [General] GPU=0, read when ForceCPU isn't there);
// its menu applies changes live and its SaveIni writes the whole set back, ForceCPU included. BFA's DLL loads before
// the bridge's, so a write takes effect from the next start. A file BFA didn't write would make it run its
// migrations from version 0, so the bridge never creates it. The rule:
//
//   version 2 answered   with BFA 4.3.2.3 and its INI there: ForceCPU=1 written when it was 0, and the bridge's own
//                        state records that it set it (only then: a 1 the bridge found is the player's, or one it set
//                        whose record was lost, and either way nothing is put back for it); logged once, at info
//                        level. Another BFA version: left as it is, and a value the bridge set put back
//   each game save       ForceCPU=1 written again while version 2 was answered this session, so a save of BFA's menu
//                        in this session can't undo it (SKSE's save message: never DllMain or the process's exit)
//   version 1 answered   a value the bridge set put back (ForceCPU=0), and the record cleared
//   nothing answered     nothing
//
// When in doubt, nothing is put back: only a record that reads exactly as the bridge wrote it counts. No setting and no
// note on screen: the log lines are all. The files are the plugin's (src/plugin/BfaIni.cpp), behind IniStore, so the
// rule is tested on Linux.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/Log.h"

namespace bfarb {

// The BFA version whose files the sizes rule names: the only one the bridge sets the CPU mode of.
constexpr const char* kCpuModeBfaVersion = "4.3.2.3";

class IniStore {
public:
    virtual ~IniStore() = default;
    // Whether BFA's INI is there: the bridge never creates it.
    virtual bool BfaIniPresent() = 0;
    // BFA's CPU mode as BFA reads it at its start: 1 for ForceCPU=1 (or, without ForceCPU, the legacy GPU=0), else 0.
    virtual int ReadForceCpu() = 0;
    // Writes [General] ForceCPU (0 or 1) in BFA's INI. False, with err, when it can't.
    virtual bool WriteForceCpu(int value, std::string& err) = 0;
    // The bridge's own record that it set ForceCPU=1 over a 0: true only when it reads exactly as written.
    virtual bool ReadSetByBridge() = 0;
    // Records it, or clears the record. False, with err, when it can't.
    virtual bool WriteSetByBridge(bool set, std::string& err) = 0;
};

class BfaCpuMode {
public:
    BfaCpuMode(std::unique_ptr<IniStore> store, LogSink log);

    // The gaming PC's BFA version, as the client string reports it ("4.3.2.3"; empty when unknown).
    void SetBfaVersion(const std::string& version);
    // The protocol version the server answered in: 2 sets BFA's CPU mode, 1 puts back a value the bridge set, 0
    // nothing.
    void OnServerVersion(std::uint16_t version);
    // A game save: BFA's CPU mode written again, while a server answered in version 2 this session.
    void OnGameSave();
    bool Set() const;  // a server answered in version 2 this session (and not version 1 since)

private:
    bool PresentLocked();
    void SetLocked();
    void PutBackLocked(const std::string& because);
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
