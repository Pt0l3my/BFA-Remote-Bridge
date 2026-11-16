#pragma once
// The bridge's side of the connection (docs/PROTOCOL.md, section 3): one main connection for every run, guarded by a
// mutex (BFA may run its sentiment model from another thread while its lip-sync model runs), short-lived connections
// of their own for model loads and status checks, and one background thread of the bridge's own.
//
// - The protocol's version: every connection opens with a version 3 HELLO. An older server answers it with ERROR 6
//   framed in its newest version (1 or 2): the client then connects again at once in that version, and stays on it
//   for that server (the address setting and port, or the address found) until the game restarts or the address
//   changes, logged once. A HELLO that gets no frame back is said once more in its version, then in the version below.
//   Nothing of version 3 goes to a server that hasn't answered in version 3, nor of version 2 to one that hasn't
//   answered in 2 or 3.
// - A line's state on the server (version 3, docs/PROTOCOL.md "Version 3"): for a model whose row in the main
//   connection's HELLO_OK says held 1, a run whose inputs pair past_X with outputs next_X (proto::DescribeLineState)
//   goes as a start (past_frames 0) or a continue, past_X and next_X left out, on one stream per model per connection.
//   The window inputs of the line in progress are kept per model (at most kLineWindows windows and kLineBytes); a
//   continue answered ERROR 8 replays them from a start within the run's own deadline, then sends itself again. A
//   replay that can't finish is answered as a timeout is.
// - The server's address: the setting's, looked up (Resolve); or, when the setting is empty, found on the local network
//   (FIND to each IPv4 adapter's broadcast address, the first FOUND taken) at start-up and after a lost connection, at
//   most one search every kFindEvery while none answers. A search runs off the game's threads and off the speech path:
//   in a model load, a status check or the background thread, never in a run.
// - NOTICE (versions 2 and 3): a line that fell back is told to the server on the main connection, at most one a
//   minute, by the background thread, only while no request there awaits an answer; a version 1 server is told by the
//   status check's `fallback REASON` instead, as by 1.2.0.
//
// - Configure never waits: the settings go to a small mutex of their own with a generation count, and the next Run or
//   the background thread (which take the connection's lock) applies them: a change to the address, port, key or
//   timeout closes the connection and ends a pause. The game's main thread calls it.
// - Resolve looks the server's name up (getaddrinfo can't be bounded by a timeout), off the connection's lock, once per
//   settings change; connections use the addresses it found. A failed lookup is logged once, and runs fall back until
//   the next settings change.
// - Admit: at BFA's CreateSession, the new model alone in a HELLO on a side connection of its own (kConnectTimeout to
//   connect, kHelloTimeout for the server to load and warm it up), closed after the answer. The main connection is
//   never held by a load. While a side load is in flight the background thread holds its own HELLOs (the server makes
//   any HELLO wait for a load in progress); status checks name no model and aren't held. A small gate keeps the two
//   apart: Admit waits for a main-connection HELLO already in flight before its side HELLO goes. SetModels then lists
//   every live model, and the background thread (or the next run) says HELLO with that list on the main connection.
// - Run: RUN and its answer on the main connection, all within the call's timeout counted from its start: waiting for
//   the connection's lock, a connect and a HELLO included. HELLO is sent first only when the connection has no answer
//   for the run's model yet. A connection the server closed or reset, or a send that failed, is opened
//   again within the same call, once. A RUN timeout with no byte of its answer arrived keeps the connection: the next
//   exchange reads and discards the late answer (the server answers in order), and a late ERROR 1, 2, 5, 6 or 7 ends
//   the connection. A second timeout in a row, a timeout in the middle of a frame, or any connection failure closes
//   the connection and starts the pause: runs then fall back at once, without the network, until the background
//   thread's HELLO is answered.
// - The background thread sleeps on a condition variable between its timers: reconnecting with a backoff of 1, 2, 5,
//   10 and 30 s (then every 30 s), its HELLO's answer ending the pause; a HELLO on the main connection when the live
//   models changed; a PING after 60 s idle on a connection whose HELLO listed models; a status check on a connection
//   of its own every 5 minutes when the counters changed, and within seconds of a fallback (at most one per 30 s) with
//   the fallback's reason in its client string; the summary line every 10 minutes when anything happened.
//   It never makes a run wait past the run's own timeout: connecting and HELLO go on a new connection outside the
//   lock, swapped in under it, and its HELLO and PING on the main connection use a try-lock, give way to runs, and
//   last no longer than the run timeout.
//   Tick is one round of its work, which the tests call directly instead of starting the thread.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Log.h"
#include "core/Protocol.h"
#include "core/Reasons.h"
#include "core/Settings.h"
#include "core/Transport.h"

namespace bfarb {

constexpr std::chrono::milliseconds kConnectTimeout{3000};  // a connect off the speech path
constexpr std::chrono::seconds kHelloTimeout{60};           // a HELLO off the speech path, which may load a model
constexpr std::chrono::seconds kIdlePing{60};
constexpr std::chrono::minutes kStatusEvery{5};
constexpr std::chrono::minutes kSummaryEvery{10};
constexpr std::uint64_t kSummaryRuns = 250;
constexpr std::size_t kClientStringMax = 120;  // PandorumLLM's log lines keep 120 characters since its patch4
constexpr std::chrono::seconds kFallbackReportEvery{30};  // the prompt status check after a fallback, at most this often
constexpr std::chrono::seconds kNoticeEvery{60};          // NOTICE at most this often: as the fallback note on screen
constexpr std::chrono::seconds kFindEvery{5};             // one search of the local network at most this often
constexpr std::chrono::milliseconds kFindWait{1000};      // a search waits this long for FOUND
// A line kept for a replay (version 3): at most this many windows and bytes per stateful model. BFA 7.0.0's
// performance.onnx window is 43,216 bytes of inputs for 3 seconds of speech, so 64 windows (about 2.8 MB) keep a line
// of more than three minutes; a longer line stops being kept, and its state, once gone on the server, isn't rebuilt.
constexpr std::size_t kLineWindows = 64;
constexpr std::size_t kLineBytes = 4u * 1024u * 1024u;

// The background thread's wait before its next try, after `failures` failed tries since the pause began (or the last
// success): 1, 2, 5, 10, 30 s, then 30 s.
std::chrono::seconds BackoffDelay(int failures) noexcept;

std::string ClientName();  // "BFARemoteBridge 1.7.0"

// Why a run got no answer from the server.
enum class Why : std::uint8_t {
    NoConnection,     // no address looked up, couldn't connect
    ConnectTimedOut,  // the connect took the run's whole time
    RunTimedOut,      // no answer within the timeout (the run's HELLO's included)
    ServerError,      // the server answered ERROR
    NotReady,         // the server doesn't have the model ready
    ConnectionLost,   // closed, reset or failed during the exchange
    Paused,           // answered at once during a pause
    Other,            // a malformed or mismatched answer, the connection busy, a replaced session, an input the protocol
                      // can't carry
};
constexpr std::size_t kWhyCount = 8;
const char* WhyName(Why why) noexcept;
// The reason the player and the server are told (core/Reasons.h) for a run's: Other is ServerError (an answer that
// doesn't fit the call); the bridge says Incompatible itself for an input the protocol can't carry.
Reason ReasonOf(Why why) noexcept;
struct Fallback;
Reason ReasonOf(const Fallback& fallback) noexcept;

// How one exchange on a connection ended.
enum class ExchangeFailure {
    None,
    Encode,       // nothing was sent
    SendFailed,   // sending failed: the server may have closed the connection
    Closed,       // the connection closed before any byte of an answer
    Reset,        // the connection was reset before any byte of an answer
    NoByte,       // the deadline passed with no byte of the answer arrived: the stream is still at a frame's start
    MidFrame,     // the deadline passed in the middle of a frame, or of the send
    Lost,         // the connection closed or failed after some bytes
    Malformed,    // not a frame of the connection's protocol version, or not the answer expected
    Refused,      // the server answered ERROR
    LateClosing,  // a discarded late answer was an ERROR that ends the connection
    OlderVersion, // a HELLO answered with ERROR 6 framed in an older version: a server that speaks only up to that one
};

struct Fallback {
    Why why = Why::Other;
    std::string text;
    bool quiet = false;  // counted, not logged (runs answered at once without the network, after the first of a series)
    bool busy = false;   // Other because the connection was held past the run's timeout: told as TimedOut
};

// Cumulative from Skyrim's start. fallbacks counts every run the server didn't answer; timeouts is a part of it.
struct Counters {
    std::uint64_t answered = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t timeouts = 0;
    bool operator==(const Counters&) const = default;
};

// The versions on the gaming PC, in one place: BFA's (from its SKSEPlugin_Version) and its runtime's (from
// GetVersionString). The client string names them today; a HELLO field may carry them once a server takes one.
struct Versions {
    std::string bfa;  // "4.3.2.3"; empty when unknown
    std::string ort;  // "1.24.4"; empty until BFA loads its runtime
};

// The HELLO's client string: "BFARemoteBridge 1.7.0 (answered N, fallbacks N, timeouts N, fallback REASON, BFA V,
// ORT V)", keys in that order, each of the last three only when known (fallback only in a status check sent for a
// fallback), new keys only at the end. Longer than max: the trailing keys are dropped until it fits, and the name
// alone when none fits.
std::string ClientString(const Counters& counters, const Versions& versions = {}, const Reason* fallback = nullptr,
                         std::size_t max = kClientStringMax);

// The runs since the last summary line.
struct Period {
    std::uint64_t runs = 0;
    std::uint64_t answered = 0;
    std::array<std::uint64_t, kWhyCount> byWhy{};
    std::uint64_t roundTripMicros = 0;
    std::uint64_t serverMicros = 0;
    std::uint64_t slowestMicros = 0;
};
std::string SummaryLine(const Period& period);

// What a HELLO (a status check's, or Admit's) learnt.
struct ProbeResult {
    enum class Kind {
        NotTried,  // the probe hasn't finished, or nothing asked it
        NotFound,  // no server address is set, and no server answered FIND on the local network
        Answered,  // HELLO_OK
        NoAnswer,  // no lookup, no connection, a timeout, the connection closed
        WrongKey,  // ERROR 2
        Refused,   // any other ERROR, or an answer that isn't in the version the HELLO opened with
    };
    Kind kind = Kind::NotTried;
    std::string server;
    std::string device;
    std::string detail;
};

// One model's state in a HELLO_OK of the main connection, for the monitor.
struct ModelState {
    std::string name;
    std::uint8_t status = 0xFF;  // proto::ModelStatus
    std::string detail;
};
using StatesSink = std::function<void(const std::string& server, const std::string& device, const std::vector<ModelState>& models)>;

// Admit's answer for the model asked.
struct Admission {
    ProbeResult hello;          // kind Answered when the server answered HELLO_OK
    std::uint8_t status = 0xFF; // the model's status in HELLO_OK (proto::ModelStatus), when answered
    std::string detail;         // the server's detail (ready models may carry one too), or the failure's reason
    bool Ready() const { return hello.kind == ProbeResult::Kind::Answered && status == 0; }
};

class Client {
public:
    using Now = Clock::time_point (*)();

    Client(TransportFactory factory, Resolver resolver, LogSink log, Now now = &Clock::now);
    ~Client();  // stops and joins the background thread (the plugin's client is never destroyed)
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Never waits for the connection: applied by the next Run or the background thread. An empty server address means
    // the server is found on the local network.
    void Configure(const Settings& settings);
    // The search for the server on the local network (the plugin's: Winsock and the adapter list Windows reports).
    // Without one, an empty address finds nothing.
    void SetDiscovery(DiscoveryFactory discovery);
    // The protocol version the server answered in, each time it changes (1, 2 or 3; again after the address changes).
    // Called on whichever thread learnt it, maybe with the connection's lock held: the sink must not wait or call the
    // client (the plugin's queues a task for the game's main thread).
    void OnVersion(std::function<void(std::uint16_t)> sink);
    void SetOrtVersion(const std::string& version);
    void SetBfaVersion(const std::string& version);
    Versions CurrentVersions() const;
    // Each HELLO_OK on the main connection: the server, its device and every model's state. Called with the
    // connection's lock held: the sink must not call the client.
    void OnStates(StatesSink sink);

    // Looks the latest settings' server name up and keeps the addresses. Called off the game's threads.
    bool Resolve();

    // The live bridge sessions' models; the main connection says HELLO again when they changed. A name listed twice is
    // sent once, as its newest session (the last in the list), since RUN names a model by its name alone.
    void SetModels(const std::vector<proto::ModelEntry>& models);

    // HELLO with `model` alone, now, on a side connection, for a new session. Never touches the main connection.
    Admission Admit(const proto::ModelEntry& model);

    enum class Outcome {
        Forwarded,  // reply holds RUN_OK
        Skipped,    // answered at once, without the network (a pause, a model not ready, no address looked up, ...)
        Failed,     // tried, and the server didn't answer it
    };
    struct Reply {
        std::vector<std::uint8_t> payload;
        proto::DecodedRunOk ok;
        std::chrono::microseconds roundTrip{0};
        std::size_t bytesUp = 0;
        // Version 3: per output asked by the request, the index of the past_X input whose next_X the server held and
        // left out of the answer (ok.outputs then holds only the others, in order); -1 for an output answered. Empty
        // when every output was answered.
        std::vector<int> heldFrom;
        std::uint8_t mode = 0;  // the mode of the run answered (proto::RunMode)
        int replayed = 0;       // windows replayed first, after ERROR 8
    };
    // Sends RUN for `model` (one SetModels listed) with request.requestId set here (after a replay, reply.ok answers
    // the last RUN sent, whose id is its own). Anything but Forwarded fills fallback; the caller records every run once,
    // with RecordAnswered or RecordFallback.
    Outcome Run(const proto::ModelEntry& model, proto::RunRequest& request, Reply& reply, Fallback& fallback);

    // Each run counted once: the counters, the summary line at each 250 runs, and each fallback's log line (its model,
    // its reason and the time it took) unless it is quiet.
    void RecordAnswered(const Reply& reply);
    void RecordFallback(const std::string& model, const Fallback& fallback, std::chrono::microseconds took);
    Counters Totals() const;

    // Whether the pause is on: runs fall back at once until the background HELLO is answered.
    bool Paused() const noexcept;

    // A connection of its own: HELLO with no models and the counters in its client string (with the fallback's
    // reason when one is given), the answer, then closed. Never touches the main connection, its lock or the pause,
    // and never waits for a side load.
    ProbeResult Probe(const Reason* fallback = nullptr);

    // A line fell back (`model` names it; empty for a model load or a runtime entry, which are no line). To a server
    // that answered in version 2 or 3, a line is told by NOTICE on the main connection alone (its model, reason, detail
    // and the lines since the last), at most one every kNoticeEvery. Anything else, a line to any other server, or a model
    // load's refusal or a runtime entry to every server, goes by the background thread's status check with the reason,
    // at once or, within kFallbackReportEvery of the last, when that has passed. NoServer has nowhere to go and is
    // ignored.
    void ReportFallback(Reason reason, const std::string& model = {}, const std::string& detail = {});
    int FallbackReports() const;  // status checks sent for a fallback, for the tests
    int NoticesSent() const;      // for the tests
    int Searches() const;         // FIND rounds sent, for the tests
    // The protocol version the server answered in (1, 2 or 3), 0 while unknown; it applies to the current address.
    std::uint16_t ServerVersion() const;

    // The background thread: allowed by StartBackground, it starts once the settings are given (now, or at a later
    // Configure): with a server address, or to search for one. StopBackground ends and joins it.
    void StartBackground();
    void StopBackground();
    // One round of the background work, due by the client's clock: the time of the next one.
    Clock::time_point Tick();

    // For the tests.
    int HellosSent() const;  // on the main connection
    int SideHellos() const;
    int Reconnects() const;  // main connections opened
    int Pings() const;
    int SideLoadsInFlight() const noexcept;
    int Replays() const;        // lines replayed after ERROR 8, finished or not
    std::size_t LineBytes(const std::string& model) const;  // the window inputs kept for a model's line

private:
    struct Listed {
        proto::ModelEntry entry;
    };
    // What the main connection's last HELLO_OK said of a model.
    struct Answered {
        proto::ModelEntry entry;
        std::uint8_t status = 0xFF;
        std::string detail;
        std::uint8_t held = 0;  // version 3: the server keeps the model's line state
    };
    // The line in progress of a stateful model (version 3): how it began, and, when its state is held on the server,
    // its windows' inputs (past_X left out) for a replay.
    struct StoredTensor {
        std::string name;
        std::uint8_t dtype = 0;
        std::vector<std::int64_t> dims;
        std::vector<std::uint8_t> data;
    };
    struct Line {
        // Ended: a held line whose run fell back. BFA ends a line at the NaN answer, and its own next_X are
        // placeholders, so a continue of it can't be carried; it gets the NaN answer too.
        enum class Kind { None, Carried, Held, Ended } kind = Kind::None;
        bool full = false;  // longer than kLineWindows or kLineBytes: no longer kept, so it can't be replayed
        std::size_t bytes = 0;
        std::vector<std::vector<StoredTensor>> windows;
    };
    struct Snapshot;
    class SideLoad;
    class MainHello;
    struct Opened;
    // Where a connection goes: the addresses, and the server's identity for the version rule.
    struct Target {
        std::vector<Endpoint> endpoints;
        std::string key;     // the address setting and port, or the address found and port
        bool found = false;  // found on the local network
    };

    // Under mutex_.
    void ApplyPendingLocked();
    bool EndpointsLocked(Target& out, std::string& reason, bool lookUpNow);
    bool ConnectMainLocked(int timeoutMs, std::string& reason, bool& timedOut);
    bool HelloMainLocked(Clock::time_point deadline, ExchangeFailure& failure, std::string& reason);
    void StartPauseLocked(const std::string& reason);
    void EndPauseLocked(const std::string& why);
    void CloseLocked(const std::string& why) noexcept;
    void ScheduleNotReadyLocked();
    std::uint32_t NextRequestLocked();
    std::uint64_t StreamLocked(const std::string& model);
    void KeepWindowLocked(Line& line, const proto::RunRequest& request, const proto::LineState& state);
    // The line's earlier windows from a start, after ERROR 8, then `current` again, each with a request id of its own:
    // the last one's answer in header and answer. False with failure and reason when an exchange failed (its id in
    // sentId), or with failure None and reason when the replay can't finish (no window kept, or the deadline passed).
    bool ReplayLocked(const Line& line, proto::RunRequest current, Clock::time_point deadline, proto::Header& header,
                      std::vector<std::uint8_t>& answer, std::uint32_t& sentId, std::size_t& bytesUp, ExchangeFailure& failure,
                      std::string& reason);
    Answered* FindAnsweredLocked(const proto::ModelEntry& model);
    bool ToldNotReadyLocked(const Answered& a);
    void AnsweredLocked(std::vector<Answered> fresh);

    // The addresses of host (looked up now when lookUpNow and not yet), or, for an empty host, the server found on the
    // local network (searched now when mayFind, within kFindEvery of the last search; notFound says so on failure).
    bool Endpoints(const std::string& host, int port, Target& out, std::string& reason, bool lookUpNow, bool mayFind = false,
                   bool* notFound = nullptr);
    bool Discover(int port, Target& out, std::string& reason, bool mayFind);
    bool Search(const DiscoveryFactory& factory, int port, std::vector<Endpoint>& found, std::string& server, std::string& why);
    void MarkFoundStale();
    std::uint16_t VersionFor(const std::string& key) const;
    // An older version learnt (ERROR 6 framed in it, or no frame back twice) is logged once, with why.
    void LearnVersion(const std::string& key, std::uint16_t version, const std::string& why = {});
    // A connection of its own to target, opened with HELLO in the server's version; on an older server's ERROR 6, again
    // in the version it was framed in; on no frame back twice, in the version below.
    Opened OpenWithHello(const Target& target, const proto::Hello& hello, std::chrono::milliseconds helloTimeout, bool notes);
    bool SettingsPending() const;
    bool Detailed() const noexcept { return detailed_.load(); }
    void Log(Level level, const std::string& line) const;
    void Detail(const std::string& line) const;
    void Wake();
    void Loop();
    void MaybeStartLocked();
    bool Recover(const Snapshot& snap);
    bool RehelloMain();
    void PingMain();
    void FirstProbe();
    void StatusCheck();
    void FallbackReport();
    Clock::duration NoticeMain();
    void SignalNoticeLocked();
    void TellStatesLocked(const std::string& server, const std::string& device);
    void Summary(bool timed);
    std::string StatusLine(const std::string& server, const std::string& device, const std::vector<Answered>& models) const;
    static std::vector<Listed> Newest(const std::vector<proto::ModelEntry>& models);

    TransportFactory factory_;
    Resolver resolver_;
    LogSink log_;
    Now now_;

    // the settings as given, and the names looked up: never held across the network
    mutable std::mutex configMutex_;
    Settings pending_;
    std::uint64_t pendingGeneration_ = 0;
    Versions versions_;
    StatesSink states_;
    DiscoveryFactory discovery_;
    std::function<void(std::uint16_t)> versionSink_;
    bool configured_ = false;  // Configure was called: the settings were read
    // what the last search found: kept until a lost connection makes it stale, or the address or port setting changes
    struct Found {
        bool valid = false;
        bool stale = false;
        int port = 0;
        std::vector<Endpoint> endpoints;
        std::string server;
    } found_;
    bool anyFind_ = false;
    Clock::time_point lastFind_{};
    std::string lastFindError_;
    bool notFoundLogged_ = false;
    // the version rule: the protocol version the server at versionKey_ answered in (0 while unknown)
    std::string versionKey_;
    std::uint16_t serverVersion_ = 0;
    struct Lookup {
        bool done = false;
        bool ok = false;
        std::string host;
        int port = 0;
        std::vector<Endpoint> endpoints;
        std::string error;
    } lookup_;
    std::atomic<std::uint64_t> pendingGen_{0};
    std::atomic<std::uint64_t> appliedGen_{0};
    std::atomic<int> timeoutMs_{1000};
    std::atomic<bool> detailed_{false};
    std::atomic<bool> lookupFailureLogged_{false};
    std::mutex findMutex_;  // one search at a time; taken before configMutex_, never with mutex_ held
    std::atomic<int> searches_{0};

    // the main connection
    mutable std::timed_mutex mutex_;
    Settings settings_;
    std::uint64_t appliedGeneration_ = 0;
    std::unique_ptr<Transport> transport_;
    std::uint16_t mainVersion_ = proto::kNewestVersion;  // the main connection's protocol version
    std::string mainKey_;                            // its server's, for the version rule
    bool mainFound_ = false;                         // its address was found on the local network
    bool mainAnswered_ = false;                      // its HELLO was answered, in mainVersion_
    std::vector<Listed> listed_;
    std::uint64_t modelsGeneration_ = 0;
    std::vector<Answered> answered_;    // the connection's last HELLO_OK
    bool helloCurrent_ = false;         // that HELLO listed exactly listed_
    std::vector<std::uint32_t> owed_;   // requests that timed out on this connection, their answers still to come
    std::vector<std::pair<std::string, std::uint64_t>> streams_;  // version 3: the connection's stream per stateful model
    std::uint64_t nextStream_ = 1;
    std::vector<std::pair<std::string, Line>> lines_;  // by model: each stateful model's line in progress
    int replays_ = 0;
    int timeoutsInARow_ = 0;
    std::uint64_t pauses_ = 0;          // pauses begun: an attempt begun before a pause doesn't end it
    Clock::time_point lastActivity_{};
    std::string lastStatusLine_;
    std::uint32_t nextRequest_ = 1;
    int hellosSent_ = 0;
    int connects_ = 0;
    int pings_ = 0;

    std::string toldNoConnection_;        // a series of runs with nowhere to connect to: its reason, logged once
    std::vector<Answered> toldNotReady_;  // the not-ready fallbacks logged, by model and state, across connections

    std::atomic<bool> paused_{false};
    std::atomic<int> runsWaiting_{0};
    std::atomic<int> sideHellos_{0};

    // H's gate: side loads in flight and main-connection HELLOs in flight, never both at once from the background
    // thread's side. Never held across the network, and taken after mutex_ when both are.
    mutable std::mutex gateMutex_;
    std::condition_variable gateCv_;
    std::atomic<int> sideLoads_{0};
    int mainHellos_ = 0;

    // the background thread and its timers: never held across the network or while taking mutex_
    mutable std::mutex bgMutex_;
    std::condition_variable wakeCv_;
    bool wake_ = false;
    bool stop_ = false;
    bool allowThread_ = false;
    bool active_ = false;
    std::thread thread_;
    int failedTries_ = 0;
    Clock::time_point nextTry_{};
    Clock::time_point pausedSince_{};
    bool probeWanted_ = true;
    Clock::time_point nextStatus_{};
    Counters reported_;
    bool fallbackPending_ = false;
    Reason fallbackReason_ = Reason::Unreachable;
    Clock::time_point fallbackDue_{};
    Clock::time_point lastFallbackReport_{};
    bool fallbackReported_ = false;
    int fallbackReports_ = 0;

    // NOTICE: the latest line that fell back, and the lines since the last NOTICE; never held across the network, and
    // taken after mutex_ when both are
    mutable std::mutex noticeMutex_;
    bool noticePending_ = false;
    proto::Notice notice_;
    std::uint32_t noticeLines_ = 0;
    bool anyNotice_ = false;
    Clock::time_point lastNotice_{};
    int noticesSent_ = 0;
    bool noticeWaiting_ = false;  // under mutex_: a NOTICE due waits for the connection; a run's answer wakes the thread

    // the counters
    mutable std::mutex statsMutex_;
    Counters totals_;
    Period period_;
    Clock::time_point lastSummary_{};
    std::uint64_t pausedRuns_ = 0;
};

}  // namespace bfarb
