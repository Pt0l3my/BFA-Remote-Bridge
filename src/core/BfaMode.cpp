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

void BfaCpuMode::SetBfaVersion(const std::string& version) {
    std::lock_guard lock(mutex_);
    bfaVersion_ = version;
}

// BFA's INI is there: the bridge never creates it (BFA would run its migrations on a file it didn't write).
bool BfaCpuMode::PresentLocked() {
    if (store_->BfaIniPresent()) return true;
    if (!absentLogged_)
        Log(Level::Info,
            "BFA's mode is left as it is: Data\\SKSE\\Plugins\\BetterFacialAnimations.ini isn't there, and the bridge never "
            "creates it");
    absentLogged_ = true;
    return false;
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
    v2_ = false;
    PutBackLocked(
        "the server speaks protocol version 1 only, and the bridge sets BFA's CPU mode only for a server that speaks version 2");
}

void BfaCpuMode::OnGameSave() {
    std::lock_guard lock(mutex_);
    if (store_ && v2_) SetLocked();
}

// A value the bridge set (its record says so) goes back to 0, and the record is cleared. Without the record, nothing
// is put back.
void BfaCpuMode::PutBackLocked(const std::string& because) {
    if (!store_->ReadSetByBridge()) return;
    std::string err;
    if (!PresentLocked()) {
        // nothing left to put back: the record goes
        if (!store_->WriteSetByBridge(false, err)) Log(Level::Warn, "The bridge's record of BFA's ForceCPU couldn't be cleared (" + err + ")");
        return;
    }
    const int now = store_->ReadForceCpu();
    if (now != 0 && !store_->WriteForceCpu(0, err)) {
        Log(Level::Warn, "BFA's [General] ForceCPU couldn't be put back to 0 (" + err + ")");
        return;
    }
    if (!store_->WriteSetByBridge(false, err)) Log(Level::Warn, "The bridge's record of BFA's ForceCPU couldn't be cleared (" + err + ")");
    if (now != 0)
        Log(Level::Info, "Put BFA's [General] ForceCPU back to 0 in BetterFacialAnimations.ini, as it was: " + because +
                             ". It applies from the next game start.");
}

// ForceCPU=1 over a 0, recorded first; for BFA 4.3.2.3 alone, with its INI there.
void BfaCpuMode::SetLocked() {
    if (bfaVersion_ != kCpuModeBfaVersion) {
        if (!otherLogged_)
            Log(Level::Info, "BFA's mode is left as it is: this is Better Facial Animations " +
                                 (bfaVersion_.empty() ? std::string("of an unknown version") : bfaVersion_) +
                                 ", and the bridge sets BFA's CPU mode only for " + kCpuModeBfaVersion +
                                 ", whose sizes version 2 servers carry");
        otherLogged_ = true;
        PutBackLocked(std::string("the bridge sets BFA's CPU mode only for Better Facial Animations ") + kCpuModeBfaVersion);
        return;
    }
    if (!PresentLocked()) return;
    const int now = store_->ReadForceCpu();
    if (now == 1) return;  // BFA's CPU mode already: the player's, or the bridge's own; nothing to record
    std::string err;
    if (!store_->ReadSetByBridge() && !store_->WriteSetByBridge(true, err)) {
        // without the record nothing could be put back: BFA's file stays as it is
        if (!failLogged_) Log(Level::Warn, "BFA's mode is left as it is: the bridge couldn't record that it sets it (" + err + ")");
        failLogged_ = true;
        return;
    }
    if (!store_->WriteForceCpu(1, err)) {
        if (!failLogged_) Log(Level::Warn, "BFA's [General] ForceCPU couldn't be set to 1 (" + err + ")");
        failLogged_ = true;
        return;
    }
    if (setLogged_) return;
    setLogged_ = true;
    Log(Level::Info, "Set BFA's [General] ForceCPU to 1 in BetterFacialAnimations.ini (it was 0): the server speaks protocol version "
                     "2, so BFA uses its CPU mode from the next game start and opens nothing on this PC's graphics card. The model "
                     "still runs on the server.");
}

}  // namespace bfarb
