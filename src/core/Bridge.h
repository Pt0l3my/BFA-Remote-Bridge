#pragma once
// The forwarding logic over ONNX Runtime's C API (README.md, "How it works"). BFA gets the runtime's entry point,
// OrtGetApiBase, through the bridge's replacement (src/plugin/Hook.cpp patches BFA's import of GetProcAddress for
// that one lookup). The replacement's GetApi(24) returns a copy of the real OrtApi table with these entries replaced;
// any other API version gets the real table, untouched, and BFA runs as if the bridge weren't installed.
//
// While the bridge is on (setting `enabled` and a server address), it takes BFA's model inference over completely:
// no model runs on this PC, on its graphics card or its processor.
//
//   CreateSession   never the real one. The model file is hashed (base name, size, SHA-256), its options' overrides
//                   taken, and the server asked (HELLO). Ready there: BFA gets a bridge session, a handle of the
//                   bridge's own. Not: BFA gets an error status made by the real CreateStatus ("BFA Remote Bridge: ..."),
//                   so it has no model, and lines get the game's own lip sync.
//   Run             on a bridge session, always the server, BFA's warm-up run included. When the server doesn't answer
//                   in full, the call still succeeds, with each requested output a one-element float tensor holding
//                   NaN: BFA's output check then drops that line only (its own comment: "A failed validation fails the
//                   line only: the session and device stay"), where an error status would make it drop the model. A
//                   10-second backoff follows, during which runs answer so at once.
//   ReleaseSession  a bridge session is freed by the bridge, never passed to the runtime.
//
// Sessions made while the bridge was off are the runtime's own, and every call on them is the real one; the on/off
// setting therefore takes effect at BFA's next model load. AddFreeDimensionOverrideByName and ReleaseSessionOptions
// are wrapped to remember each options object's overrides; GetExecutionProviderApi ("DML": a copy of the real
// OrtDmlApi table with its three SessionOptionsAppendExecutionProvider_DML entries wrapped) and
// SessionOptionsAppendExecutionProvider ("DML") only to see that BFA is set to use the graphics card. Every one of
// these passes the call through unchanged. Everything else is the real runtime.
//
// Check mode (setting checkCalls > 0) is a diagnostic that does run the model here: only while it is on, CreateSession
// also makes the real session, and the first checkCalls runs of each session run both ways and log the largest
// difference.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Client.h"
#include "core/Log.h"
#include "core/Protocol.h"
#include "core/Settings.h"
#include "core/Transport.h"
#include "onnxruntime_c_api.h"

namespace bfarb {

using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)(void) noexcept;

struct Stats {
    std::uint64_t forwarded = 0;   // runs the server answered
    std::uint64_t unanswered = 0;  // runs answered with NaN (the line gets the game's own lip sync)
    std::uint64_t failures = 0;    // failures that started a backoff
    std::uint64_t roundTripMicros = 0;
    std::uint64_t serverMicros = 0;
    std::uint64_t bytesUp = 0;
    std::uint64_t bytesDown = 0;
};

// What the player is told of (src/core/Status.h turns it into text).
struct Notice {
    enum class Kind {
        Served,          // a model is served by the server
        NoAnswer,        // the server couldn't be reached at a model load
        WrongKey,
        Missing,         // the server has no such file, or another copy of it
        CouldNotLoad,    // the server failed to load it, refused it, or answered wrongly
        UnknownRuntime,  // BFA asked for an ONNX Runtime API version the bridge doesn't know
    };
    Kind kind = Kind::Served;
    std::string model;
    std::string detail;
};

class Bridge {
public:
    static constexpr std::uint64_t kSessionMagic = 0x4252444745425246ull;  // "FRBEGDRB": a bridge session

    Bridge(TransportFactory factory, LogSink log, Client::Now now = &Clock::now);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // The bridge the wrappers use: one at a time (the plugin's lives until the game exits).
    static void SetCurrent(Bridge* bridge) noexcept;
    static Bridge* Current() noexcept;

    // Remembers the real OrtGetApiBase and returns the replacement to hand to BFA.
    static OrtGetApiBaseFn Hook(OrtGetApiBaseFn real) noexcept;

    enum class ApiState { NotAsked, Known, Unknown };
    static ApiState State() noexcept;

    void Apply(const Settings& settings);
    Settings CurrentSettings() const;
    Client& Connection() noexcept { return client_; }
    Stats Statistics() const;

    // Called on the thread that made it (BFA's worker) for each model load's outcome and for an unknown runtime;
    // the plugin queues the notification to the game's main thread.
    using NoticeSink = std::function<void(const Notice&)>;
    void OnNotice(NoticeSink sink);

    // The live bridge sessions, and whether a handle is one (by the bridge's own registry, never by reading it).
    std::size_t BridgeSessions() const;
    bool IsBridgeSession(const OrtSession* session) const;
    // Whether a live bridge session also has the real one check mode made.
    bool HasLocalSession(const OrtSession* session) const;

    void LogLine(Level level, const std::string& line) const;
    void Tell(const Notice& notice);

    // ---- the wrappers' work (public for the static wrappers; not for callers)
    void OnOverride(OrtSessionOptions* options, const char* name, std::int64_t value);
    void OnReleaseOptions(const OrtSessionOptions* options);
    void OnGpuOptions(const OrtSessionOptions* options);
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
        OrtSession* local = nullptr;  // check mode's real session, or null
        int checks = 0;
        bool unsupportedLogged = false;
    };
    struct OptionsInfo {
        std::vector<proto::Override> overrides;
        bool gpu = false;
    };

    OrtStatus* Refuse(const OrtApi* real, const std::string& model, const Admission& a);
    void PushModelsLocked();
    std::vector<proto::ModelEntry> LiveModelsLocked() const;
    void Log(Level level, const std::string& line) const;
    void NanAnswer(const OrtApi* real, std::size_t outputCount, OrtValue** outputs) noexcept;
    void Check(const OrtApi* real, OrtSession* local, const char* const* inputNames, const OrtValue* const* inputs, std::size_t inputCount,
               const char* const* outputNames, std::size_t outputCount, const std::string& model, int index, int of,
               const Client::Reply& reply);

    LogSink log_;
    Client client_;

    mutable std::mutex settingsMutex_;
    Settings settings_;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<const OrtSessionOptions*, OptionsInfo> options_;
    std::map<const OrtSession*, std::unique_ptr<Session>> sessions_;
    std::uint64_t nextOrder_ = 1;
    bool gpuLogged_ = false;

    mutable std::mutex noticeMutex_;
    NoticeSink notice_;

    mutable std::mutex statsMutex_;
    Stats stats_;
};

}  // namespace bfarb
