#pragma once
// The forwarding logic over ONNX Runtime's C API (README.md, "How it works"). BFA gets the runtime's entry point,
// OrtGetApiBase, through the bridge's replacement (src/plugin/Hook.cpp patches BFA's import of GetProcAddress for
// that one lookup). That is the bridge's only attach: it changes nothing else of BFA's.
//
// The API version BFA asks for is served from the real table (GetApi below):
//   - an older or the same version: a copy of the real table's entries, only as many as the runtime's own API
//     version has (OrtApiEntries: a shorter table is never read past), with these entries replaced;
//   - a newer version: 1.24.4's entries, wrapped the same way, then every entry past 1.24.4's replaced by one that
//     returns an error status naming it (NewerEntry). The bridge can't tell which of them could meet its sessions,
//     so none of them reaches the runtime: a run that needs one fails safely, and the log and the monitor name it;
//   - a runtime older than API version 10 (whose table length the header can't tell), one whose version can't be
//     read, or an API version past kNewestOrtApi: known to be incompatible, so GetApi answers null. BFA then has no
//     model (its own "wrong version"), lip sync is the game's own, and the note says to update the bridge (or BFA,
//     for a runtime older than 1.10).
//
// With the bridge, no BFA model runs on this PC: every model load is the server's or is refused.
//
//   CreateSession   never the real one. The model file is hashed (base name, size, SHA-256), its options' overrides
//                   taken, and the server asked (HELLO). Ready there: BFA gets a bridge session, a handle of the
//                   bridge's own. Not, or no server found, or the settings not read yet: BFA gets an error
//                   status made by the real CreateStatus ("BFA Remote Bridge: ..."), so it has no model, and lines
//                   get the game's own lip sync.
//   Run             on a bridge session, always the server, BFA's warm-up run included. When the server doesn't answer
//                   in full, the call still succeeds, with each requested output a one-element float tensor holding
//                   NaN: BFA's output check then drops that line only (its own comment: "A failed validation fails the
//                   line only: the session and device stay"), where an error status would make it drop the model. A
//                   connection failure pauses the bridge (src/core/Client.h): runs answer so at once until the
//                   background thread's HELLO is answered. Every run is counted once, answered or by its reason.
//   ReleaseSession  a bridge session is freed by the bridge, never passed to the runtime.
//
// AddFreeDimensionOverrideByName and ReleaseSessionOptions are wrapped to remember each options object's overrides;
// GetExecutionProviderApi ("DML" at API version 24: a copy of the real OrtDmlApi table with its three
// SessionOptionsAppendExecutionProvider_DML entries wrapped) and SessionOptionsAppendExecutionProvider ("DML") only
// to see that BFA is set to use the graphics card, which the bridge logs at Detailed, as a fact: the hardware is
// PandorumLLM's choice, and the bridge advises nothing about it. Every one of these passes the call
// through unchanged. Every other entry that takes a session answers a bridge session with an error status. The
// runtime's other session makers (CreateSessionFromArray, the two with prepacked weights, the model editor's three)
// answer with an error status naming them, since a model loaded so would run on this PC. Everything else is the real
// runtime.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Client.h"
#include "core/Log.h"
#include "core/Monitor.h"
#include "core/Protocol.h"
#include "core/Settings.h"
#include "core/Sha256.h"
#include "core/Transport.h"
#include "onnxruntime_c_api.h"

namespace bfarb {

using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)(void) noexcept;

// ---- the runtime's API versions

// OrtApi is append-only: API version N's table is version N-1's with entries added at its end. The first entry of
// each version from 1.11 on, by its "\since Version 1.N" in the vendored 1.24.4 header (tests/CoreTests.cpp checks
// the list against the header); the versions 19 and 21 added none.
struct OrtApiStart {
    unsigned version;
    std::size_t index;
    const char* name;
};
const std::vector<OrtApiStart>& OrtApiStarts();

constexpr std::size_t kKnownOrtEntries = sizeof(OrtApi) / sizeof(void*);  // 1.24.4's: 415
constexpr unsigned kOldestOrtApi = 10;                                     // the oldest whose table length is known
constexpr unsigned kNewestOrtApi = 30;                                     // the newest served (24's entries and stubs)
constexpr std::size_t kNewerOrtSlots = 512;                                // the stubs past 1.24.4's entries

// How many OrtApi entries API version `version` has; 0 when the header can't tell (before 10). 24 and later:
// 1.24.4's (the entries past them are the stubs').
std::size_t OrtApiEntries(unsigned version) noexcept;

// The API version of a runtime's version string ("1.24.4" is 24); 0 when it isn't "1.N...".
unsigned OrtApiVersionOf(const char* versionString) noexcept;

struct Stats {
    std::uint64_t forwarded = 0;   // runs the server answered
    std::uint64_t unanswered = 0;  // runs answered with NaN (the line gets the game's own lip sync)
    std::uint64_t roundTripMicros = 0;
    std::uint64_t serverMicros = 0;
    std::uint64_t bytesUp = 0;
    std::uint64_t bytesDown = 0;
};

class Bridge {
public:
    static constexpr std::uint64_t kSessionMagic = 0x4252444745425246ull;  // "FRBEGDRB": a bridge session

    Bridge(TransportFactory factory, Resolver resolver, LogSink log, Client::Now now = &Clock::now);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // The bridge the wrappers use: one at a time (the plugin's lives until the game exits).
    static void SetCurrent(Bridge* bridge) noexcept;
    static Bridge* Current() noexcept;

    // Remembers the real OrtGetApiBase and returns the replacement to hand to BFA.
    static OrtGetApiBaseFn Hook(OrtGetApiBaseFn real) noexcept;

    enum class ApiState { NotAsked, Served, CantServe };
    static ApiState State() noexcept;
    static unsigned ServedApiVersion() noexcept;  // the last version BFA asked for and got the bridge's table

    // The settings read from SkyrimNet. Until the first, a model load is refused: with "the settings weren't read
    // yet" when ExpectSettings(true) said SkyrimNet is loaded (its config may not be registered yet), else as having
    // no server address.
    void Apply(const Settings& settings);
    void ExpectSettings(bool skyrimNet);
    Settings CurrentSettings() const;
    Client& Connection() noexcept { return client_; }
    Monitor& Notes() noexcept { return monitor_; }
    Stats Statistics() const;

    // The live bridge sessions, and whether a handle is one (by the bridge's own registry, never by reading it).
    std::size_t BridgeSessions() const;
    bool IsBridgeSession(const OrtSession* session) const;

    void LogLine(Level level, const std::string& line) const;

    // ---- the wrappers' work (public for the static wrappers; not for callers)
    void OnOverride(OrtSessionOptions* options, const char* name, std::int64_t value);
    void OnReleaseOptions(const OrtSessionOptions* options);
    void OnGpuOptions(const OrtSessionOptions* options);
    void OnNewerEntry(std::size_t index);
    void OnCantServe(const std::string& runtime, const std::string& why, bool newer);
    void OnUnservedEntry(const std::string& name);
    OrtStatus* CreateSession(const OrtApi* real, const OrtEnv* env, const ORTCHAR_T* path, const OrtSessionOptions* options,
                             OrtSession** out) noexcept;
    void ReleaseSession(const OrtApi* real, OrtSession* session) noexcept;
    OrtStatus* DoRun(const OrtApi* real, OrtSession* session, const OrtRunOptions* runOptions, const char* const* inputNames,
                     const OrtValue* const* inputs, std::size_t inputCount, const char* const* outputNames, std::size_t outputCount,
                     OrtValue** outputs) noexcept;

private:
    struct Session {
        std::uint64_t magic = kSessionMagic;
        std::uint64_t order = 0;
        proto::ModelEntry entry;
        bool unsupportedLogged = false;
        bool superseded = false;  // a newer session of the same model replaced it
    };
    struct HashEntry {
        std::uint64_t size = 0;
        std::filesystem::file_time_type written{};
        Sha256::Digest digest{};
    };
    struct OptionsInfo {
        std::vector<proto::Override> overrides;
        bool gpu = false;
    };

    OrtStatus* Refuse(const OrtApi* real, const std::string& model, const Admission& a);
    // The error status BFA gets; logged when it differs from the model's last refusal (BFA retries failed loads).
    OrtStatus* RefuseWith(const OrtApi* real, const std::string& model, const std::string& why);
    // The model's size and SHA-256, hashed again only when its path, size or modification time changed.
    bool HashCached(const std::filesystem::path& file, std::uint64_t& size, Sha256::Digest& digest, std::string& err);
    void PushModelsLocked();
    std::vector<proto::ModelEntry> LiveModelsLocked() const;
    void Log(Level level, const std::string& line) const;
    void NanAnswer(const OrtApi* real, std::size_t outputCount, OrtValue** outputs) noexcept;
    // A run the server didn't answer: counted (and logged unless quiet) by the client, told to the monitor and the
    // server (unless quiet), then the NaN answer.
    void FallBack(const OrtApi* real, const std::string& model, const Fallback& fallback, Reason reason, Clock::time_point start,
                  std::size_t outputCount, OrtValue** outputs) noexcept;
    void OnStates(const std::string& server, const std::string& device, const std::vector<ModelState>& models);

    LogSink log_;
    Client client_;
    Monitor monitor_;

    mutable std::mutex settingsMutex_;
    Settings settings_;
    std::atomic<bool> settingsRead_{false};
    std::atomic<bool> skyrimNet_{false};

    std::mutex hashMutex_;
    std::map<std::string, HashEntry> hashes_;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<const OrtSessionOptions*, OptionsInfo> options_;
    std::map<const OrtSession*, std::unique_ptr<Session>> sessions_;
    std::map<std::string, std::string> lastRefusal_;  // by model: the last refusal logged
    std::uint64_t nextOrder_ = 1;
    bool gpuLogged_ = false;
    std::vector<std::size_t> newerLogged_;
    std::vector<std::string> unservedLogged_;

    mutable std::mutex statsMutex_;
    Stats stats_;
};

}  // namespace bfarb
