#pragma once
// The bridge's side of the connection (docs/PROTOCOL.md): one TCP connection, guarded by a mutex (BFA may run
// its sentiment model from another thread while its lip-sync model runs).
//
// - Admit: at BFA's CreateSession, HELLO listing every live bridge session's model and the new one, waiting up to
//   kHelloTimeout for the server to load and warm it up. A running backoff doesn't stop it: a model load is rare
//   and worth a fresh try.
// - Run: RUN and its answer within the timeout. HELLO is sent first when the connection is new or the set of live
//   models changed since the last one. Any failure (no connection, a timeout, an ERROR, a malformed or mismatched
//   answer, the model no longer ready) closes the connection unless the server keeps it open after that error,
//   starts a 10-second backoff and logs its reason once; during the backoff Run answers Skipped at once, without
//   touching the network. The first run after it reconnects.

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
        NoAnswer,  // no connection, a timeout, the connection closed
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
    std::string detail;         // the server's detail, or the failure's reason
    bool Ready() const { return hello.kind == ProbeResult::Kind::Answered && status == 0; }
};

class Client {
public:
    using Now = Clock::time_point (*)();

    Client(TransportFactory factory, LogSink log, Now now = &Clock::now);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // A change to the address, port, key or timeout closes the connection and ends a backoff.
    void Configure(const Settings& settings);
    void SetOrtVersion(const std::string& version);

    // The live bridge sessions' models, in the order the sessions were made; the next run sends HELLO again when
    // they changed. A name listed twice is sent once (the first), since RUN names a model by its name alone.
    void SetModels(const std::vector<proto::ModelEntry>& models);

    // HELLO with `live` and `model` (in that order), now, for a new session.
    Admission Admit(const std::vector<proto::ModelEntry>& live, const proto::ModelEntry& model);

    enum class Outcome {
        Forwarded,  // reply holds RUN_OK
        Skipped,    // a backoff is running, or another run held the connection past the timeout: nothing logged
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

    // A failure found after a RUN_OK (its outputs didn't fit the call): closes the connection, starts the backoff
    // and logs the reason.
    void Fail(const std::string& reason);

    // Whether a backoff is running now.
    bool BackingOff() const;

    // A fresh connection of its own: HELLO with no models, the answer, then closed. Never touches the main
    // connection or the backoff.
    ProbeResult Probe();

    // For the tests: the HELLO messages sent on the main connection so far.
    int HellosSent() const;

private:
    struct Listed {
        proto::ModelEntry entry;
        std::uint8_t status = 0xFF;  // not asked yet
        std::string detail;
    };

    bool EnsureConnected(std::string& reason);
    // HELLO with listed_ within `wait`; on HELLO_OK the statuses are stored. result says what happened.
    bool Hello(std::chrono::milliseconds wait, ProbeResult& result, std::string& reason);
    bool Exchange(proto::MsgType type, const std::vector<std::uint8_t>& payload, std::chrono::milliseconds wait, proto::Header& header,
                  std::vector<std::uint8_t>& answer, std::string& reason);
    void FailLocked(const std::string& reason, bool keepConnection);
    void CloseLocked() noexcept;
    static std::vector<Listed> Dedupe(const std::vector<proto::ModelEntry>& models);

    TransportFactory factory_;
    LogSink log_;
    Now now_;

    mutable std::timed_mutex mutex_;
    std::atomic<int> timeoutMs_{1000};
    Settings settings_;
    std::string ortVersion_;
    std::unique_ptr<Transport> transport_;
    std::vector<Listed> listed_;
    bool helloCurrent_ = false;
    std::string lastStatusLine_;
    Clock::time_point backoffUntil_{};
    std::uint32_t nextRequest_ = 1;
    int hellosSent_ = 0;
};

}  // namespace bfarb
