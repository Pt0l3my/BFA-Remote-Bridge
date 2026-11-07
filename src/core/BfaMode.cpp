#include "core/BfaMode.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bfarb {
namespace {

constexpr const char* kVersionOneReason =
    "the server speaks protocol version 1 only, and the bridge sets BFA's CPU mode only for a server that speaks version 2";

// The C library's isspace, in the C locale.
bool Space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

int DigitIn(char c, int base) {
    int d = 99;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    return d < base ? d : -1;
}

// strtol(text, &end, base) with a 32-bit long, as on Windows: a value only when it uses the whole text and fits.
std::optional<long> WholeLong(std::string_view text, int base) {
    std::size_t i = 0;
    while (i < text.size() && Space(text[i])) ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    // base 16 takes its own 0x too, when a digit follows it
    if (base == 16 && i + 2 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X') && DigitIn(text[i + 2], 16) >= 0)
        i += 2;
    constexpr std::int64_t kLimit = std::int64_t{1} << 31;  // a long's magnitude, at most, on Windows
    std::int64_t n = 0;
    std::size_t digits = 0;
    for (; i < text.size(); ++i, ++digits) {
        const int d = DigitIn(text[i], base);
        if (d < 0) break;
        if (n <= kLimit) n = n * base + d;
    }
    if (digits == 0 || i != text.size()) return std::nullopt;
    if (negative ? n > kLimit : n > kLimit - 1) return std::nullopt;  // out of range: strtol's ERANGE
    return static_cast<long>(negative ? -n : n);
}

// What a ForceCPU record puts back: "1" the value 0, "added" nothing (empty: the key is removed). Anything else is no
// record.
std::optional<std::string> ForceCpuPutBack(const std::string& text) {
    if (text == "1") return std::string("0");
    if (text == kAddedRecord) return std::string();
    return std::nullopt;
}

// What an EmotionDevice record puts back: the card's number as the bridge wrote it (decimal digits, 0 to 999), or for
// "added" nothing (empty: the key is removed). Anything else is no record.
std::optional<std::string> EmotionPutBack(const std::string& text) {
    if (text == kAddedRecord) return std::string();
    if (text.empty() || text.size() > 3 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::to_string(std::stol(text));
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

std::optional<long> Bfa5Number(const std::optional<std::string>& value) {
    if (!value || value->empty() || value->size() >= 64) return std::nullopt;  // 64 or more: SimpleIni's buffer is short
    const std::string_view text = *value;
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return text.size() == 2 ? std::nullopt : WholeLong(text.substr(2), 16);
    return WholeLong(text, 10);
}

long Bfa5Long(const std::optional<std::string>& value, long def) { return Bfa5Number(value).value_or(def); }

int Bfa5Batch(IniStore& store) {
    const long batch = Bfa5Long(Bfa5Value(store, "General", "Batch"), 2);
    return static_cast<int>(std::clamp<long>(batch, 1, INT_MAX));
}

// As BFA 5.0.6's LoadIni: ForceCPU over the legacy GPU=0; EmotionDevice over -1 (with ForceCPU) or GpuAdapter, at
// least 0. The defaults' keys are read only when needed.
Bfa5Devices ReadBfa5Devices(IniStore& store) {
    Bfa5Devices d;
    const auto forceCpu = Bfa5Number(Bfa5Value(store, "General", "ForceCPU"));
    const bool legacyCpu = !forceCpu && Bfa5Long(Bfa5Value(store, "General", "GPU"), 1) == 0;
    d.forceCpu = forceCpu ? (*forceCpu != 0 ? 1 : 0) : (legacyCpu ? 1 : 0);
    const auto emotion = Bfa5Number(Bfa5Value(store, "General", "EmotionDevice"));
    long fallback = -1;
    if (!d.forceCpu && !emotion) fallback = std::max(0L, Bfa5Long(Bfa5Value(store, "General", "GpuAdapter"), 0));
    d.emotionDevice = std::max(-1L, emotion.value_or(fallback));
    return d;
}

BfaCpuMode::BfaCpuMode(std::unique_ptr<IniStore> store, LogSink log) : store_(std::move(store)), log_(std::move(log)) {}

void BfaCpuMode::Log(Level level, const std::string& line) const {
    if (log_) log_(level, line);
}

bool BfaCpuMode::Set() const {
    std::lock_guard lock(mutex_);
    return answered_ == 2;
}

void BfaCpuMode::SetBfaVersion(const std::string& version) {
    std::lock_guard lock(mutex_);
    bfaVersion_ = version;
}

bool BfaCpuMode::Bfa4Locked() const { return bfaVersion_ == kCpuModeBfaVersion; }

// BFA's INI as found now, a missing or unreadable one logged once a session. Missing: the bridge never creates it (BFA
// would run its migrations on a file it didn't write). Unreadable: what it holds can't be told, so nothing is changed.
BfaIniState BfaCpuMode::StateLocked() {
    const BfaIniState state = store_->BfaIni();
    if (state == BfaIniState::Missing && !absentLogged_) {
        Log(Level::Info,
            "BFA's mode is left as it is: Data\\SKSE\\Plugins\\BetterFacialAnimations.ini isn't there, and the bridge never "
            "creates it");
        absentLogged_ = true;
    }
    if (state == BfaIniState::Unreadable && !unreadableLogged_) {
        Log(Level::Warn,
            "BFA's mode is left as it is for now: Data\\SKSE\\Plugins\\BetterFacialAnimations.ini is there but couldn't be read "
            "(another program may hold it), so the bridge changes nothing in it and keeps its records");
        unreadableLogged_ = true;
    }
    return state;
}

bool BfaCpuMode::PresentLocked() { return StateLocked() == BfaIniState::Readable; }

bool BfaCpuMode::OwnHolds(const char* key) { return store_->ReadBfaValue(nullptr, "General", key).has_value(); }

// The bridge's ForceCPU=1 in BFA's own INI, as this BFA reads it: BFA 4.3.2.3 with Windows' profile calls, any other
// as BFA 5 does.
bool BfaCpuMode::ForceCpuIsBridgesLocked() {
    if (Bfa4Locked()) return store_->ReadForceCpu() == 1;
    return Bfa5Number(store_->ReadBfaValue(nullptr, "General", "ForceCPU")) == 1L;
}

bool BfaCpuMode::EmotionIsBridgesLocked() { return Bfa5Number(store_->ReadBfaValue(nullptr, "General", "EmotionDevice")) == -1L; }

void BfaCpuMode::ClearRecordLocked(const char* record, const char* key) {
    std::string err;
    if (!store_->WriteRecord(record, nullptr, err))
        Log(Level::Warn, std::string("The bridge's record of BFA's ") + key + " couldn't be cleared (" + err + ")");
}

void BfaCpuMode::OnStart() {
    std::lock_guard lock(mutex_);
    if (!store_) return;
    const bool force = ForceCpuPutBack(store_->ReadRecord(kForceCpuRecord)).has_value();
    // EmotionDevice is BFA 5.0.6's key: another BFA loaded nothing of it at this start
    const bool emotion = IsBfa506(bfaVersion_) && EmotionPutBack(store_->ReadRecord(kEmotionDeviceRecord)).has_value();
    if (!force && !emotion) return;
    // BFA's INI not there: BFA loaded nothing of it, and the records wait for its return. Unreadable: what BFA loaded
    // can't be told. Either way both records stay.
    if (StateLocked() != BfaIniState::Readable) return;
    if (force && !ForceCpuIsBridgesLocked()) ClearRecordLocked(kForceCpuRecord, "ForceCPU");
    if (emotion && !EmotionIsBridgesLocked()) ClearRecordLocked(kEmotionDeviceRecord, "EmotionDevice");
}

void BfaCpuMode::OnServerVersion(std::uint16_t version) {
    std::lock_guard lock(mutex_);
    if (!store_ || (version != 1 && version != 2)) return;
    answered_ = version;
    if (version == 2)
        SetLocked();
    else
        PutBackLocked(kVersionOneReason);
}

void BfaCpuMode::OnGameSave() {
    std::lock_guard lock(mutex_);
    if (!store_) return;
    if (answered_ == 2)
        SetLocked();
    else if (answered_ == 1)
        PutBackLocked(kVersionOneReason);
}

void BfaCpuMode::FailOnceLocked(const std::string& key, const std::string& line) {
    if (failLogged_.insert(key).second) Log(Level::Warn, line);
}

// Every value the bridge set goes back, each by its own record. BFA 4.3.2.3 has no EmotionDevice: that record, made
// for BFA 5.0.6's INI, is left as it is.
void BfaCpuMode::PutBackLocked(const std::string& because) {
    const auto force = ForceCpuPutBack(store_->ReadRecord(kForceCpuRecord));
    const auto emotion = Bfa4Locked() ? std::nullopt : EmotionPutBack(store_->ReadRecord(kEmotionDeviceRecord));
    if (!force && !emotion) return;
    const BfaIniState state = StateLocked();
    if (state == BfaIniState::Unreadable) return;  // nothing cleared, nothing written
    if (state == BfaIniState::Missing) {  // nothing left to put back: the records go
        if (force) ClearRecordLocked(kForceCpuRecord, "ForceCPU");
        if (emotion) ClearRecordLocked(kEmotionDeviceRecord, "EmotionDevice");
        return;
    }
    if (force) PutBackKeyLocked(kForceCpuRecord, "ForceCPU", *force, ForceCpuIsBridgesLocked(), because);
    if (emotion) PutBackKeyLocked(kEmotionDeviceRecord, "EmotionDevice", *emotion, EmotionIsBridgesLocked(), because);
}

// The bridge's value still in BFA's own INI: written back as recorded (value), or the key the bridge added removed (value
// empty), and the record kept until a start has loaded it (OnStart). Not there any more: the player's own value, or
// what was put back already this session; nothing is written, and the record goes unless it was put back this session
// (BFA's menu may write the bridge's value back before the session ends).
void BfaCpuMode::PutBackKeyLocked(const char* record, const char* key, const std::string& value, bool bridges, const std::string& because) {
    if (!bridges) {
        if (!putBack_.count(key)) ClearRecordLocked(record, key);
        return;
    }
    const bool remove = value.empty();
    std::string err;
    bool ok = false;
    if (remove)
        ok = store_->RemoveBfaValue(key, err);
    else if (Bfa4Locked() && std::string_view(key) == "ForceCPU")
        ok = store_->WriteForceCpu(0, err);  // as BFA 4.3.2.3 reads it
    else
        ok = store_->WriteBfaValue(key, value, err);
    if (!ok) {
        FailOnceLocked(std::string("putback ") + key, "BFA's [General] " + std::string(key) +
                                                          (remove ? " couldn't be removed" : " couldn't be put back to " + value) + " (" +
                                                          err + "); the bridge keeps its record, to try again");
        return;
    }
    if (!putBack_.insert(key).second) return;  // done again after a save of BFA's menu: once in the log
    if (remove)
        Log(Level::Info, "Removed the bridge's [General] " + std::string(key) +
                             " from BetterFacialAnimations.ini, which had none before the bridge set it, so BFA's installer "
                             "choice applies again: " + because + ". It applies from the next game start.");
    else
        Log(Level::Info, "Put BFA's [General] " + std::string(key) + " back to " + value + " in BetterFacialAnimations.ini, as it was: " +
                             because + ". It applies from the next game start.");
}

void BfaCpuMode::SetLocked() {
    if (Bfa4Locked()) return SetBfa4Locked();
    if (IsBfa506(bfaVersion_)) return SetBfa5Locked();
    if (!otherLogged_)
        Log(Level::Info, "BFA's mode is left as it is: this is Better Facial Animations " +
                             (bfaVersion_.empty() ? std::string("of an unknown version") : bfaVersion_) +
                             ", and the bridge sets BFA's CPU mode only for " + kCpuModeBfaVersion + " and " + kCpuModeBfa5Version +
                             ", the versions it is made for");
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
    if (!ForceCpuPutBack(store_->ReadRecord(kForceCpuRecord)) && !store_->WriteRecord(kForceCpuRecord, "1", err)) {
        // without the record nothing could be put back: BFA's file stays as it is
        FailOnceLocked("ForceCPU", "BFA's mode is left as it is: the bridge couldn't record that it sets it (" + err + ")");
        return;
    }
    if (!store_->WriteForceCpu(1, err)) {
        FailOnceLocked("ForceCPU", "BFA's [General] ForceCPU couldn't be set to 1 (" + err + ")");
        return;
    }
    if (!setLogged_.insert("ForceCPU").second) return;
    Log(Level::Info, "Set BFA's [General] ForceCPU to 1 in BetterFacialAnimations.ini (it was 0): the server speaks protocol version "
                     "2, so BFA uses its CPU mode from the next game start and opens nothing on this PC's graphics card. The model "
                     "still runs on the server.");
}

// BFA 5.0.6: ForceCPU=1 over a 0 and EmotionDevice=-1 over a card, as BFA 5 reads them (its layers), each recorded
// first by how to put it back; written in BFA's own INI, which wins over the installer's files. The record follows
// what BFA's own INI holds each time, so a card picked in BFA's menu since replaces the one recorded.
void BfaCpuMode::SetBfa5Locked() {
    if (!PresentLocked()) return;
    const Bfa5Devices now = ReadBfa5Devices(*store_);
    std::vector<std::string> changed;
    std::string err;
    if (now.forceCpu == 0) {
        const std::string want = OwnHolds("ForceCPU") ? "1" : kAddedRecord;
        if (store_->ReadRecord(kForceCpuRecord) != want && !store_->WriteRecord(kForceCpuRecord, want.c_str(), err))
            FailOnceLocked("ForceCPU", "BFA's ForceCPU is left as it is: the bridge couldn't record that it sets it (" + err + ")");
        else if (!store_->WriteBfaValue("ForceCPU", "1", err))
            FailOnceLocked("ForceCPU", "BFA's [General] ForceCPU couldn't be set to 1 (" + err + ")");
        else
            changed.push_back("ForceCPU");
    }
    std::string card;
    if (now.emotionDevice >= 0) {
        card = std::to_string(now.emotionDevice);
        const std::string want = OwnHolds("EmotionDevice") ? card : kAddedRecord;
        err.clear();
        if (now.emotionDevice > 999)  // the record holds three digits at most: nothing could be put back
            FailOnceLocked("EmotionDevice", "BFA's EmotionDevice is left as it is: the bridge couldn't record that it sets it (card " + card +
                                                " is out of range)");
        else if (store_->ReadRecord(kEmotionDeviceRecord) != want && !store_->WriteRecord(kEmotionDeviceRecord, want.c_str(), err))
            FailOnceLocked("EmotionDevice", "BFA's EmotionDevice is left as it is: the bridge couldn't record that it sets it (" + err + ")");
        else if (!store_->WriteBfaValue("EmotionDevice", "-1", err))
            FailOnceLocked("EmotionDevice", "BFA's [General] EmotionDevice couldn't be set to -1 (" + err + ")");
        else
            changed.push_back("EmotionDevice");
    }
    bool fresh = false;  // a key not logged yet this session: a save's writes again are silent
    for (const auto& key : changed) fresh = setLogged_.insert(key).second || fresh;
    if (!fresh) return;
    std::string what;
    for (const auto& key : changed)
        what += (what.empty() ? "" : " and ") + key + (key == "ForceCPU" ? " to 1 (it was 0)" : " to -1 (it was " + card + ")");
    // what BFA reads now, so the line claims no more than was written
    const Bfa5Devices after = ReadBfa5Devices(*store_);
    const bool face = after.forceCpu == 1, emotion = after.emotionDevice == -1;
    if (!face && !emotion) return;
    const std::string models = face && emotion ? "Audio2Face and Audio2Emotion" : face ? "Audio2Face" : "Audio2Emotion";
    Log(Level::Info, "Set BFA's [General] " + what + " in BetterFacialAnimations.ini: the server speaks protocol version 2, so BFA runs " +
                         models + " in its CPU mode from the next game start" +
                         (face && emotion ? " and creates no graphics device on this PC" : "") + ". The model still runs on the server.");
}

}  // namespace bfarb
