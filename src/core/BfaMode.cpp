#include "core/BfaMode.h"

namespace bfarb {

BfaCpuMode::BfaCpuMode(std::unique_ptr<IniStore> store, LogSink log) : store_(std::move(store)), log_(std::move(log)) {}

void BfaCpuMode::Log(Level level, const std::string& line) const {
    if (log_) log_(level, line);
}

bool BfaCpuMode::Set() const {
    std::lock_guard lock(mutex_);
    return v2_;
}

void BfaCpuMode::OnServerVersion(std::uint16_t version) {
    std::lock_guard lock(mutex_);
    if (!store_) return;
    if (version == 2) {
        v2_ = true;
        SetLocked();
        return;
    }
    if (version != 1) return;
    // a version 1 server can't serve BFA's CPU mode: the value BFA had goes back
    v2_ = false;
    const int recorded = store_->ReadRecorded();
    if (recorded < 0) return;
    std::string err;
    const int now = store_->ReadForceCpu();
    if (now != recorded && !store_->WriteForceCpu(recorded, err)) {
        Log(Level::Warn, "BFA's [General] ForceCPU couldn't be put back to " + std::to_string(recorded) + " (" + err + ")");
        return;
    }
    if (!store_->WriteRecorded(-1, err)) Log(Level::Warn, "The bridge's record of BFA's ForceCPU couldn't be cleared (" + err + ")");
    if (now != recorded)
        Log(Level::Info, "Put BFA's [General] ForceCPU back to " + std::to_string(recorded) +
                             " in BetterFacialAnimations.ini, as it was: the server speaks protocol version 1 only, which can't serve "
                             "BFA's CPU mode. It applies from the next game start.");
}

void BfaCpuMode::OnGameSave() {
    std::lock_guard lock(mutex_);
    if (store_ && v2_) SetLocked();
}

// The value found recorded (the first time only), then ForceCPU=1, when it isn't already.
void BfaCpuMode::SetLocked() {
    std::string err;
    const int now = store_->ReadForceCpu();
    if (store_->ReadRecorded() < 0 && !store_->WriteRecorded(now, err)) {
        // without the record nothing could be put back: BFA's file stays as it is
        if (!failLogged_) Log(Level::Warn, "BFA's mode is left as it is: the bridge couldn't record it (" + err + ")");
        failLogged_ = true;
        return;
    }
    if (now == 1) return;
    if (!store_->WriteForceCpu(1, err)) {
        if (!failLogged_) Log(Level::Warn, "BFA's [General] ForceCPU couldn't be set to 1 (" + err + ")");
        failLogged_ = true;
        return;
    }
    if (setLogged_) return;
    setLogged_ = true;
    Log(Level::Info, "Set BFA's [General] ForceCPU to 1 in BetterFacialAnimations.ini (it was " + std::to_string(now) +
                         "): the server speaks protocol version 2, so BFA uses its CPU mode from the next game start and opens "
                         "nothing on this PC's graphics card. The model still runs on the server.");
}

}  // namespace bfarb
