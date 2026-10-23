#pragma once
// BFA's mode, set by the bridge (the owner's rule for 1.3.0): with a server that answered in protocol version 2 this
// session, BFA runs in its CPU mode, so it opens nothing on this PC's graphics card; the model runs on the server
// either way. A version 1 server (PandorumLLM v3.97-p6 and earlier) can't serve BFA's CPU mode, so with one the value
// BFA had before is put back.
//
// From BFA 4.3.2's source (read for facts only): BFA reads Data\SKSE\Plugins\BetterFacialAnimations.ini once, in its
// SKSE load; its CPU mode is [General] ForceCPU=1 (the legacy form [General] GPU=0, read when ForceCPU isn't there);
// its menu applies changes live and its SaveIni writes the whole set back, ForceCPU included. BFA's DLL loads before
// the bridge's, so a write takes effect from the next start. The rule:
//
//   version 2 answered   the value found is recorded in the bridge's own state, the first time only (until it is
//                        put back), and ForceCPU=1 is written; logged once, at info level
//   each game save       ForceCPU=1 written again while version 2 was answered this session, so a save of BFA's menu
//                        in this session can't undo it (SKSE's save message: never DllMain or the process's exit)
//   version 1 answered   the recorded value is put back, and the record cleared
//   nothing answered     nothing
//
// No setting and no note on screen: the log line is all. The files are the plugin's (src/plugin/BfaIni.cpp), behind
// IniStore, so the rule is tested on Linux.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/Log.h"

namespace bfarb {

class IniStore {
public:
    virtual ~IniStore() = default;
    // BFA's CPU mode as BFA reads it at its start: 1 for ForceCPU=1 (or, without ForceCPU, the legacy GPU=0), else 0.
    virtual int ReadForceCpu() = 0;
    // Writes [General] ForceCPU (0 or 1) in BFA's INI. False, with err, when it can't.
    virtual bool WriteForceCpu(int value, std::string& err) = 0;
    // The bridge's own record of the value it found (0 or 1); -1 when there is none.
    virtual int ReadRecorded() = 0;
    // Records it; -1 clears the record. False, with err, when it can't.
    virtual bool WriteRecorded(int value, std::string& err) = 0;
};

class BfaCpuMode {
public:
    BfaCpuMode(std::unique_ptr<IniStore> store, LogSink log);

    // The protocol version the server answered in: 2 sets BFA's CPU mode, 1 puts the recorded value back, 0 nothing.
    void OnServerVersion(std::uint16_t version);
    // A game save: BFA's CPU mode written again, while a server answered in version 2 this session.
    void OnGameSave();
    bool Set() const;  // a server answered in version 2 this session (and not version 1 since)

private:
    void SetLocked();
    void Log(Level level, const std::string& line) const;

    std::unique_ptr<IniStore> store_;
    LogSink log_;
    mutable std::mutex mutex_;
    bool v2_ = false;
    bool setLogged_ = false;
    bool failLogged_ = false;
};

}  // namespace bfarb
