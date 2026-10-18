#pragma once
// The bridge's side of the connection (docs/PROTOCOL.md): one TCP connection, guarded by a mutex (BFA may run
// its sentiment model from another thread while its lip-sync model runs).
//
// - Configure never waits: the settings go to a small mutex of their own with a generation count, and the next Admit
//   or Run (which hold the connection's lock) applies them: a change to the address, port, key or timeout closes the
//   connection and ends a backoff. The game's main thread calls it, and the connection may be busy for a minute.
// - Resolve looks the server's name up (getaddrinfo can't be bounded by a timeout), off the connection's lock, once per
//   settings change; connections use the addresses it found. A failed lookup is logged once, and runs answer
//   Skipped until the next settings change.
// - Admit: at BFA's CreateSession, a fresh connection and HELLO listing every live bridge session's model and the new
//   one, waiting up to kHelloTimeout for the server to load and warm it up. A running backoff doesn't stop it.
// - Run: RUN and its answer within the timeout. HELLO is sent first when the connection is new or the set of live
//   models changed since the last one. A reused connection the server has closed (it may close one idle for 10
//   minutes) is opened again within the same call: when sending fails, or the connection closes before any byte of
//   the answer, the call reconnects, says HELLO and sends again, once. Any other failure (no connection, a timeout,
//   an ERROR, a malformed or mismatched answer, the model no longer ready) closes the connection unless the server
//   keeps it open after that error, starts a 10-second backoff and logs its reason once; during the backoff Run
//   answers Skipped at once, without touching the network. The first run after it reconnects.
// - The client never sends PING.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "core/Log.h"
#include "core/Protocol.h"
#include "core/Settings.h"
#include "core/Transport.h"

namespace bfarb {

constexpr std::chrono::seconds kBackoff{10};
constexpr std::chrono::seconds kHelloTimeout{60};

std::string ClientName();  // "BFARemoteBridge 1.0.0"

// What a HELLO (the probe's, or Admit's) learnt.
struct ProbeResult {
    enum class Kind {
        NotTried,  // the probe hasn't finished, or nothing asked it
        NoAddress,
        Answered,  // HELLO_OK
        NoAnswer,  // no lookup, no connection, a timeout, the connection closed
        WrongKey,  // ERROR 2
        Refused,   // any other ERROR, or an answer that isn't protocol version 1
    };
    Kind kind = Kind::NotTried;
    std::string server;
    std::string device;
    std::string detail;
};

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
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Never waits for the connection: applied by the next Admit or Run.
    void Configure(const Settings& settings);
    void SetOrtVersion(const std::string& version);

    // Looks the latest settings' server name up and keeps the addresses. Called off the game's threads.
    bool Resolve();

    // The live bridge sessions' models; the next run sends HELLO again when they changed. A name listed twice is sent
    // once, as its newest session (the last in the list), since RUN names a model by its name alone.
    void SetModels(const std::vector<proto::ModelEntry>& models);

    // HELLO with `live` and `model` (the newest), now, on a fresh connection, for a new session.
    Admission Admit(const std::vector<proto::ModelEntry>& live, const proto::ModelEntry& model);

    enum class Outcome {
        Forwarded,  // reply holds RUN_OK
        Skipped,    // a backoff, no address looked up, or another run holding the connection past the timeout
        Failed,     // the backoff has started and the reason is logged
    };
    struct Reply {
        std::vector<std::uint8_t> payload;
        proto::DecodedRunOk ok;
        std::chrono::microseconds roundTrip{0};
        std::size_t bytesUp = 0;
    };
    // Sends RUN for `model` (one SetModels listed) with request.requestId set here.
    Outcome Run(const proto::ModelEntry& model, proto::RunRequest& request, Reply& reply, std::string& reason);

    // A failure found after a RUN_OK (its outputs didn't fit the call): closes the connection, starts the backoff and
    // logs the reason.
    void Fail(const std::string& reason);

    // Whether a backoff is running now (for the tests; it waits for the connection's lock).
    bool BackingOff() const;

    // A fresh connection of its own: HELLO with no models, the answer, then closed. Never touches the main connection,
    // its lock or the backoff.
    ProbeResult Probe();

    // For the tests: the HELLO messages and the connections made on the main connection so far.
    int HellosSent() const;
    int Reconnects() const;

private:
    struct Listed {
        proto::ModelEntry entry;
        std::uint8_t status = 0xFF;  // not asked yet
        std::string detail;
    };
    enum class Failure { None, Early, TimedOut, Late };

    void ApplyPendingLocked();
    bool EndpointsLocked(std::vector<Endpoint>& out, std::string& reason, bool lookUpNow);
    bool EnsureConnected(Clock::time_point deadline, std::string& reason);
    bool Hello(Clock::time_point deadline, ProbeResult& result, std::string& reason, Failure* failure = nullptr);
    bool Exchange(proto::MsgType type, const std::vector<std::uint8_t>& payload, Clock::time_point deadline, proto::Header& header,
                  std::vector<std::uint8_t>& answer, std::string& reason, Failure* failure = nullptr);
    void FailLocked(const std::string& reason, bool keepConnection);
    void CloseLocked() noexcept;
    static std::vector<Listed> Newest(const std::vector<proto::ModelEntry>& models);

    TransportFactory factory_;
    Resolver resolver_;
    LogSink log_;
    Now now_;

    // the settings as given, and the names looked up: never held across the network
    mutable std::mutex configMutex_;
    Settings pending_;
    std::uint64_t pendingGeneration_ = 0;
    std::string ortVersion_;
    struct Lookup {
        bool done = false;
        bool ok = false;
        std::string host;
        int port = 0;
        std::vector<Endpoint> endpoints;
        std::string error;
    } lookup_;
    std::atomic<int> timeoutMs_{1000};

    // the connection
    mutable std::timed_mutex mutex_;
    Settings settings_;
    std::uint64_t appliedGeneration_ = 0;
    std::unique_ptr<Transport> transport_;
    std::vector<Listed> listed_;
    bool helloCurrent_ = false;
    bool lookupFailureLogged_ = false;
    std::string lastStatusLine_;
    Clock::time_point backoffUntil_{};
    std::uint32_t nextRequest_ = 1;
    int hellosSent_ = 0;
    int connects_ = 0;
};

}  // namespace bfarb
