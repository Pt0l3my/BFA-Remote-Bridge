#include "core/BfaMode.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace bfarb {
namespace {

// The record of EmotionDevice: the card's number as the bridge wrote it (decimal digits, 0 to 999), or nothing.
std::optional<long> EmotionRecord(const std::string& text) {
    if (text.empty() || text.size() > 3 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::strtol(text.c_str(), nullptr, 10);
}

}  // namespace

bool IsBfa506(const std::string& version) noexcept {
    const std::string v = kCpuModeBfa5Version;
    return version == v || version.rfind(v + ".", 0) == 0;
}

// The last layer that holds the key wins: BFA's own INI first, then the choice files from the last back.
std::optional<std::string> Bfa5Value(IniStore& store, const char* section, const char* key) {
    if (auto v = store.ReadBfaValue(nullptr, section, key)) return v;
    for (auto it = kBfa5InstallerChoices.rbegin(); it != kBfa5InstallerChoices.rend(); ++it)
        if (auto v = store.ReadBfaValue(*it, section, key)) return v;
    return std::nullopt;
}

long Bfa5Long(const std::optional<std::string>& value, long def) {
    if (!value || value->empty()) return def;
    const char* text = value->c_str();
    char* end = nullptr;
    errno = 0;
    long n = def;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (!text[2]) return def;
        n = std::strtol(text + 2, &end, 16);
    } else {
        n = std::strtol(text, &end, 10);
    }
    if (!end || *end || errno == ERANGE) return def;
    return n;
}

int Bfa5Batch(IniStore& store) {
    const long batch = Bfa5Long(Bfa5Value(store, "General", "Batch"), 2);
    return static_cast<int>(std::clamp<long>(batch, 1, INT_MAX));
}

// As BFA 5.0.6's LoadIni: ForceCPU over the legacy GPU=0; EmotionDevice over -1 (with ForceCPU) or GpuAdapter.
// The defaults' keys are read only when needed.
Bfa5Devices ReadBfa5Devices(IniStore& store) {
    Bfa5Devices d;
    const auto forceCpu = Bfa5Value(store, "General", "ForceCPU");
    const bool legacyCpu = Bfa5Long(forceCpu, LONG_MIN) == LONG_MIN && Bfa5Long(Bfa5Value(store, "General", "GPU"), 1) == 0;
    d.forceCpu = Bfa5Long(forceCpu, legacyCpu ? 1 : 0) != 0 ? 1 : 0;
    const auto emotion = Bfa5Value(store, "General", "EmotionDevice");
    long fallback = -1;
    if (!d.forceCpu && Bfa5Long(emotion, LONG_MIN) == LONG_MIN)
        fallback = std::max(0L, Bfa5Long(Bfa5Value(store, "General", "GpuAdapter"), 0));
    d.emotionDevice = std::max(-1L, Bfa5Long(emotion, fallback));
    return d;
}

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

void BfaCpuMode::FailOnceLocked(const std::string& line) {
    if (!failLogged_) Log(Level::Warn, line);
    failLogged_ = true;
}

// Every value the bridge set goes back, each by its own record.
void BfaCpuMode::PutBackLocked(const std::string& because) {
    PutBackForceCpuLocked(because);
    PutBackEmotionLocked(because);
}

// A value the bridge set (its record says so) goes back to 0, and the record is cleared. Without the record, nothing
// is put back.
void BfaCpuMode::PutBackForceCpuLocked(const std::string& because) {
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

// BFA 5's EmotionDevice: the card the record names goes back, while BFA's own INI still holds the bridge's -1, and the
// record is cleared. A record that doesn't read as the bridge wrote it puts nothing back.
void BfaCpuMode::PutBackEmotionLocked(const std::string& because) {
    const std::optional<long> before = EmotionRecord(store_->ReadRecord(kEmotionDeviceRecord));
    if (!before) return;
    std::string err;
    if (!PresentLocked()) {
        if (!store_->WriteRecord(kEmotionDeviceRecord, nullptr, err))
            Log(Level::Warn, "The bridge's record of BFA's EmotionDevice couldn't be cleared (" + err + ")");
        return;
    }
    const bool bridges = Bfa5Long(store_->ReadBfaValue(nullptr, "General", "EmotionDevice"), LONG_MIN) == -1;
    const std::string value = std::to_string(*before);
    if (bridges && !store_->WriteBfaValue("EmotionDevice", value, err)) {
        Log(Level::Warn, "BFA's [General] EmotionDevice couldn't be put back to " + value + " (" + err + ")");
        return;
    }
    if (!store_->WriteRecord(kEmotionDeviceRecord, nullptr, err))
        Log(Level::Warn, "The bridge's record of BFA's EmotionDevice couldn't be cleared (" + err + ")");
    if (bridges)
        Log(Level::Info, "Put BFA's [General] EmotionDevice back to " + value + " in BetterFacialAnimations.ini, as it was: " + because +
                             ". It applies from the next game start.");
}

void BfaCpuMode::SetLocked() {
    if (bfaVersion_ == kCpuModeBfaVersion) return SetBfa4Locked();
    if (IsBfa506(bfaVersion_)) return SetBfa5Locked();
    if (!otherLogged_)
        Log(Level::Info, "BFA's mode is left as it is: this is Better Facial Animations " +
                             (bfaVersion_.empty() ? std::string("of an unknown version") : bfaVersion_) +
                             ", and the bridge sets BFA's CPU mode only for " + kCpuModeBfaVersion + " and " + kCpuModeBfa5Version +
                             ", whose sizes it knows");
    otherLogged_ = true;
    PutBackLocked(std::string("the bridge sets BFA's CPU mode only for Better Facial Animations ") + kCpuModeBfaVersion + " and " +
                  kCpuModeBfa5Version);
}

// ForceCPU=1 over a 0, recorded first; for BFA 4.3.2.3, with its INI there.
void BfaCpuMode::SetBfa4Locked() {
    if (!PresentLocked()) return;
    const int now = store_->ReadForceCpu();
    if (now == 1) return;  // BFA's CPU mode already: the player's, or the bridge's own; nothing to record
    std::string err;
    if (!store_->ReadSetByBridge() && !store_->WriteSetByBridge(true, err)) {
        // without the record nothing could be put back: BFA's file stays as it is
        FailOnceLocked("BFA's mode is left as it is: the bridge couldn't record that it sets it (" + err + ")");
        return;
    }
    if (!store_->WriteForceCpu(1, err)) {
        FailOnceLocked("BFA's [General] ForceCPU couldn't be set to 1 (" + err + ")");
        return;
    }
    if (setLogged_) return;
    setLogged_ = true;
    Log(Level::Info, "Set BFA's [General] ForceCPU to 1 in BetterFacialAnimations.ini (it was 0): the server speaks protocol version "
                     "2, so BFA uses its CPU mode from the next game start and opens nothing on this PC's graphics card. The model "
                     "still runs on the server.");
}

// BFA 5.0.6: ForceCPU=1 over a 0 and EmotionDevice=-1 over a card, each recorded first, as BFA 5 reads them (its
// layers); written in BFA's own INI, which wins over the installer's files.
void BfaCpuMode::SetBfa5Locked() {
    if (!PresentLocked()) return;
    const Bfa5Devices now = ReadBfa5Devices(*store_);
    std::string changed;
    if (now.forceCpu == 0) {
        std::string err;
        if (!store_->ReadSetByBridge() && !store_->WriteSetByBridge(true, err))
            FailOnceLocked("BFA's ForceCPU is left as it is: the bridge couldn't record that it sets it (" + err + ")");
        else if (!store_->WriteForceCpu(1, err))
            FailOnceLocked("BFA's [General] ForceCPU couldn't be set to 1 (" + err + ")");
        else
            changed = "ForceCPU to 1 (it was 0)";
    }
    if (now.emotionDevice >= 0) {
        std::string err;
        const std::string card = std::to_string(now.emotionDevice);
        // the card's number, once: a record already there names the card the bridge found first
        if (!EmotionRecord(store_->ReadRecord(kEmotionDeviceRecord)) &&
            (now.emotionDevice > 999 || !store_->WriteRecord(kEmotionDeviceRecord, card.c_str(), err)))
            FailOnceLocked("BFA's EmotionDevice is left as it is: the bridge couldn't record that it sets it (" +
                           (err.empty() ? "card " + card + " is out of range" : err) + ")");
        else if (!store_->WriteBfaValue("EmotionDevice", "-1", err))
            FailOnceLocked("BFA's [General] EmotionDevice couldn't be set to -1 (" + err + ")");
        else
            changed += (changed.empty() ? "" : " and ") + std::string("EmotionDevice to -1 (it was ") + card + ")";
    }
    if (changed.empty() || setLogged_) return;
    setLogged_ = true;
    Log(Level::Info, "Set BFA's [General] " + changed +
                         " in BetterFacialAnimations.ini: the server speaks protocol version 2, so BFA runs Audio2Face and "
                         "Audio2Emotion in its CPU mode from the next game start and opens nothing on this PC's graphics card. The "
                         "model still runs on the server.");
}

}  // namespace bfarb
