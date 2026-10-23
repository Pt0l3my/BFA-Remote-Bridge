#include "core/Client.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <utility>

#include "core/Version.h"

namespace bfarb {
namespace {

using proto::MsgType;
using Lock = std::lock_guard<std::timed_mutex>;
using TryLock = std::unique_lock<std::timed_mutex>;
using Failure = ExchangeFailure;
using Note = std::function<void(const std::string&)>;

constexpr std::chrono::seconds kBackoff[] = {std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
                                             std::chrono::seconds(10), std::chrono::seconds(30)};
// A run holds the connection: the thread looks again this soon.
constexpr std::chrono::milliseconds kBusyRetry{100};

std::string Join(const std::vector<std::string>& parts) {
    std::string s;
    for (const auto& p : parts) s += (s.empty() ? "" : "; ") + p;
    return s;
}

std::string ErrorText(const proto::Error& e) {
    return "ERROR " + std::to_string(e.code) + ", " + proto::ErrorCodeName(e.code) + (e.message.empty() ? "" : ": " + e.message);
}

int MillisLeft(Clock::time_point deadline) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(ms, 1, 600000));
}

std::string Ms(std::uint64_t micros) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(micros) / 1000.0);
    return b;
}

std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

bool IsType(const proto::Header& h, MsgType t) { return h.type == static_cast<std::uint16_t>(t); }

// One request and its answer on t by the deadline. With owed, whole frames answering a request that timed out earlier
// on this connection (RUN_OK or ERROR with an owed request id other than `awaited`) are read and discarded first; a
// discarded ERROR that ends the connection fails the exchange. False with failure and reason otherwise.
bool Transact(Transport& t, MsgType type, const std::vector<std::uint8_t>& payload, Clock::time_point deadline,
              std::vector<std::uint32_t>* owed, std::uint32_t awaited, proto::Header& header, std::vector<std::uint8_t>& answer,
              Failure& failure, std::string& reason, const Note& note) {
    const auto fail = [&](Failure f, std::string why) {
        failure = f;
        reason = std::move(why);
        return false;
    };
    std::vector<std::uint8_t> frame;
    std::string err;
    if (!proto::EncodeFrame(type, payload, frame, err)) return fail(Failure::Encode, "couldn't encode the request (" + err + ")");
    Transport::Result r = t.SendAll(frame.data(), frame.size(), deadline, err);
    if (r != Transport::Result::Ok) {
        const Failure f = r == Transport::Result::TimedOut ? Failure::MidFrame
                          : r == Transport::Result::Closed ? Failure::Closed
                          : r == Transport::Result::Reset  ? Failure::Reset
                                                           : Failure::SendFailed;
        return fail(f, "couldn't send to the server (" + err + ")");
    }
    bool any = false;  // a byte of an answer arrived in this exchange
    for (;;) {
        std::uint8_t head[proto::kHeaderSize];
        // the first byte apart: a connection closed before it is one the server had closed already, and a deadline
        // passed before it leaves the stream at a frame's start
        r = t.ReceiveAll(head, 1, deadline, err);
        if (r == Transport::Result::TimedOut) return fail(Failure::NoByte, "the server didn't answer in time");
        if (r != Transport::Result::Ok) {
            if (any) return fail(Failure::Lost, "the connection ended (" + err + ")");
            return fail(r == Transport::Result::Reset ? Failure::Reset : Failure::Closed, "the server had closed the connection (" + err + ")");
        }
        any = true;
        r = t.ReceiveAll(head + 1, sizeof head - 1, deadline, err);
        if (r != Transport::Result::Ok)
            return fail(r == Transport::Result::TimedOut ? Failure::MidFrame : Failure::Lost, "the server's answer was cut short (" + err + ")");
        if (!proto::DecodeHeader(head, header, err)) return fail(Failure::Malformed, "the server's answer is malformed (" + err + ")");
        answer.resize(header.length);
        if (header.length > 0) {
            r = t.ReceiveAll(answer.data(), answer.size(), deadline, err);
            if (r != Transport::Result::Ok)
                return fail(r == Transport::Result::TimedOut ? Failure::MidFrame : Failure::Lost, "the server's answer was cut short (" + err + ")");
        }
        if (owed && !owed->empty() && (IsType(header, MsgType::RunOk) || IsType(header, MsgType::Error)) && answer.size() >= 4) {
            const std::uint32_t id = U32(answer.data());
            const auto it = std::find(owed->begin(), owed->end(), id);
            if (it != owed->end() && id != awaited) {
                owed->erase(it);
                if (IsType(header, MsgType::Error)) {
                    proto::Error e;
                    if (proto::DecodeError(answer.data(), answer.size(), e, err) && proto::ErrorEndsConnection(e.code))
                        return fail(Failure::LateClosing, "the late answer to request " + std::to_string(id) + " was " + ErrorText(e) +
                                                              ", which ends the connection");
                    if (note) note("Discarded the late ERROR to request " + std::to_string(id));
                } else if (note) {
                    note("Discarded the late answer to request " + std::to_string(id));
                }
                continue;
            }
        }
        failure = Failure::None;
        return true;
    }
}

struct HelloAnswer {
    ProbeResult result;
    std::vector<proto::ModelReply> models;
};

// HELLO on t and its answer, checked against the models it listed (the same, in the same order).
bool SayHello(Transport& t, const proto::Hello& hello, Clock::time_point deadline, std::vector<std::uint32_t>* owed, HelloAnswer& out,
              Failure& failure, std::string& reason, const Note& note) {
    out = HelloAnswer{};
    std::vector<std::uint8_t> payload, answer;
    std::string err;
    if (!proto::EncodeHello(hello, payload, err)) {
        failure = Failure::Encode;
        out.result.kind = ProbeResult::Kind::Refused;
        reason = out.result.detail = "couldn't encode HELLO (" + err + ")";
        return false;
    }
    proto::Header header;
    if (!Transact(t, MsgType::Hello, payload, deadline, owed, 0, header, answer, failure, reason, note)) {
        out.result.kind = failure == Failure::Malformed ? ProbeResult::Kind::Refused : ProbeResult::Kind::NoAnswer;
        out.result.detail = reason;
        return false;
    }
    out.result.kind = ProbeResult::Kind::Refused;
    if (IsType(header, MsgType::Error)) {
        proto::Error e;
        if (!proto::DecodeError(answer.data(), answer.size(), e, err)) {
            failure = Failure::Malformed;
            reason = out.result.detail = "the server's answer to HELLO is malformed (" + err + ")";
            return false;
        }
        failure = Failure::Refused;
        if (e.code == static_cast<std::uint16_t>(proto::ErrorCode::WrongKey)) out.result.kind = ProbeResult::Kind::WrongKey;
        out.result.detail = ErrorText(e);
        reason = "the server refused HELLO: " + ErrorText(e);
        return false;
    }
    failure = Failure::Malformed;
    proto::HelloOk ok;
    if (!IsType(header, MsgType::HelloOk) || !proto::DecodeHelloOk(answer.data(), answer.size(), ok, err)) {
        reason = out.result.detail = "the server's answer to HELLO is malformed" + (err.empty() ? std::string() : " (" + err + ")");
        return false;
    }
    if (ok.models.size() != hello.models.size()) {
        reason = out.result.detail = "the server's answer to HELLO lists " + std::to_string(ok.models.size()) + " models, not " +
                                     std::to_string(hello.models.size());
        return false;
    }
    for (std::size_t i = 0; i < ok.models.size(); ++i)
        if (ok.models[i].name != hello.models[i].name) {
            reason = out.result.detail = "the server's answer to HELLO names its models out of order";
            return false;
        }
    failure = Failure::None;
    out.result.kind = ProbeResult::Kind::Answered;
    out.result.server = ok.server;
    out.result.device = ok.device;
    out.models = std::move(ok.models);
    return true;
}

bool Early(Failure f) { return f == Failure::SendFailed || f == Failure::Closed || f == Failure::Reset; }

}  // namespace

// ---- the free parts

std::chrono::seconds BackoffDelay(int failures) noexcept {
    const int last = static_cast<int>(std::size(kBackoff)) - 1;
    return kBackoff[std::clamp(failures, 0, last)];
}

std::string ClientName() { return std::string("BFARemoteBridge ") + kVersionText; }

const char* WhyName(Why why) noexcept {
    switch (why) {
        case Why::NoConnection: return "no connection";
        case Why::ConnectTimedOut: return "connect timed out";
        case Why::RunTimedOut: return "RUN timed out";
        case Why::ServerError: return "ERROR";
        case Why::NotReady: return "not ready";
        case Why::ConnectionLost: return "connection lost";
        case Why::Paused: return "paused";
        case Why::Other: return "other";
    }
    return "other";
}

Reason ReasonOf(Why why) noexcept {
    switch (why) {
        case Why::NoConnection:
        case Why::ConnectTimedOut:
        case Why::ConnectionLost: return Reason::Unreachable;
        case Why::RunTimedOut: return Reason::TimedOut;
        case Why::ServerError: return Reason::ServerError;
        case Why::NotReady: return Reason::NotReady;
        case Why::Paused: return Reason::Paused;
        case Why::Other: return Reason::ServerError;
    }
    return Reason::ServerError;
}

Reason ReasonOf(const Fallback& f) noexcept { return f.busy ? Reason::TimedOut : ReasonOf(f.why); }

std::string ClientString(const Counters& c, const Versions& v, const Reason* fallback, std::size_t max) {
    const std::string name = ClientName();
    std::vector<std::string> keys = {"answered " + std::to_string(c.answered), "fallbacks " + std::to_string(c.fallbacks),
                                     "timeouts " + std::to_string(c.timeouts)};
    if (fallback) keys.push_back(std::string("fallback ") + ReasonKey(*fallback));
    if (!v.bfa.empty()) keys.push_back("BFA " + v.bfa);
    if (!v.ort.empty()) keys.push_back("ORT " + v.ort);
    for (std::size_t n = keys.size(); n > 0; --n) {
        std::string s = name + " (";
        for (std::size_t i = 0; i < n; ++i) s += (i ? ", " : "") + keys[i];
        s += ")";
        if (s.size() <= max) return s;
    }
    return name.substr(0, max);
}

std::string SummaryLine(const Period& p) {
    const auto by = [&](Why w) { return p.byWhy[static_cast<std::size_t>(w)]; };
    const std::uint64_t noConnection = by(Why::NoConnection) + by(Why::ConnectTimedOut) + by(Why::ConnectionLost);
    std::string s = std::to_string(p.runs) + " runs since the last summary: " + std::to_string(p.answered) + " answered, " +
                    std::to_string(p.runs - p.answered) + " not (" + std::to_string(by(Why::RunTimedOut)) + " timed out, " +
                    std::to_string(by(Why::ServerError)) + " refused, " + std::to_string(by(Why::NotReady)) + " not ready, " +
                    std::to_string(noConnection) + " no connection, " + std::to_string(by(Why::Paused)) + " paused, " +
                    std::to_string(by(Why::Other)) + " other)";
    if (p.answered > 0)
        s += "; round trip " + Ms(p.roundTripMicros / p.answered) + " ms, server " + Ms(p.serverMicros / p.answered) + " ms, slowest " +
             Ms(p.slowestMicros) + " ms";
    return s;
}

// ---- the client

struct Client::Snapshot {
    std::uint64_t pauses = 0;
    bool paused = false;
    bool connected = false;
    bool helloCurrent = false;
    bool anyNotReady = false;
    bool pingable = false;
    Clock::time_point lastActivity{};
    std::vector<proto::ModelEntry> models;
    std::uint64_t modelsGeneration = 0;
    Settings settings;
};

// A side load in flight, for H: the background thread holds its HELLOs meanwhile, and looks again when it ends. It
// begins once no HELLO is in flight on the main connection, so that HELLO is never caught waiting for this load.
class Client::SideLoad {
public:
    explicit SideLoad(Client& c) : c_(c) {
        std::unique_lock lock(c_.gateMutex_);
        c_.gateCv_.wait(lock, [this] { return c_.mainHellos_ == 0; });
        ++c_.sideLoads_;
    }
    ~SideLoad() {
        {
            std::lock_guard lock(c_.gateMutex_);
            --c_.sideLoads_;
        }
        c_.Wake();
    }
    SideLoad(const SideLoad&) = delete;
    SideLoad& operator=(const SideLoad&) = delete;

private:
    Client& c_;
};

// A HELLO on the main connection in flight, for H: a side load waits for it to end before its own HELLO goes. The
// background thread's is held while a side load is in flight (Held: nothing may be sent); a run's never is.
class Client::MainHello {
public:
    MainHello(Client& c, bool holdForLoads) : c_(c) {
        std::lock_guard lock(c_.gateMutex_);
        if (holdForLoads && c_.sideLoads_.load() > 0) {
            held_ = true;
            return;
        }
        ++c_.mainHellos_;
    }
    ~MainHello() {
        if (held_) return;
        {
            std::lock_guard lock(c_.gateMutex_);
            --c_.mainHellos_;
        }
        c_.gateCv_.notify_all();
    }
    bool Held() const noexcept { return held_; }
    MainHello(const MainHello&) = delete;
    MainHello& operator=(const MainHello&) = delete;

private:
    Client& c_;
    bool held_ = false;
};

Client::Client(TransportFactory factory, Resolver resolver, LogSink log, Now now)
    : factory_(std::move(factory)), resolver_(std::move(resolver)), log_(std::move(log)), now_(now) {
    lastSummary_ = now_();
}

Client::~Client() {
    StopBackground();
    Lock lock(mutex_);
    CloseLocked("the bridge is shutting down");
}

void Client::Log(Level level, const std::string& line) const {
    if (log_) log_(level, line);
}

void Client::Detail(const std::string& line) const {
    if (Detailed()) Log(Level::Info, line);
}

void Client::Configure(const Settings& settings) {
    {
        std::lock_guard lock(configMutex_);
        if (settings.serverAddress != pending_.serverAddress || settings.serverPort != pending_.serverPort) lookupFailureLogged_.store(false);
        pending_ = settings;
        ++pendingGeneration_;
        pendingGen_.store(pendingGeneration_);
        timeoutMs_.store(settings.timeoutMs);
        detailed_.store(settings.logDetail == LogDetail::Detailed);
    }
    std::lock_guard lock(bgMutex_);
    active_ = !settings.serverAddress.empty();
    MaybeStartLocked();
    wake_ = true;
    wakeCv_.notify_all();
}

void Client::SetOrtVersion(const std::string& version) {
    std::lock_guard lock(configMutex_);
    versions_.ort = version;
}

void Client::SetBfaVersion(const std::string& version) {
    std::lock_guard lock(configMutex_);
    versions_.bfa = version;
}

Versions Client::CurrentVersions() const {
    std::lock_guard lock(configMutex_);
    return versions_;
}

void Client::OnStates(StatesSink sink) {
    Lock lock(mutex_);
    states_ = std::move(sink);
}

void Client::TellStatesLocked(const std::string& server, const std::string& device) {
    if (!states_) return;
    std::vector<ModelState> models;
    for (const auto& a : answered_) models.push_back(ModelState{a.entry.name, a.status, a.detail});
    try {
        states_(server, device, models);
    } catch (...) {
    }
}

bool Client::Resolve() {
    std::string host;
    int port = 0;
    {
        std::lock_guard lock(configMutex_);
        host = pending_.serverAddress;
        port = pending_.serverPort;
    }
    Lookup found;
    found.done = true;
    found.host = host;
    found.port = port;
    if (host.empty()) {
        found.error = "no server address is set";
    } else if (!resolver_) {
        found.error = "no name lookup";
    } else {
        try {
            found.ok = resolver_(host, static_cast<std::uint16_t>(port), found.endpoints, found.error) && !found.endpoints.empty();
        } catch (...) {
            found.ok = false;
        }
        if (!found.ok && found.error.empty()) found.error = "the address doesn't resolve";
    }
    std::lock_guard lock(configMutex_);
    lookup_ = std::move(found);
    return lookup_.ok;
}

bool Client::SettingsPending() const { return pendingGen_.load() != appliedGen_.load(); }

// Under mutex_: the settings Configure left, applied.
void Client::ApplyPendingLocked() {
    Settings s;
    {
        std::lock_guard lock(configMutex_);
        if (pendingGeneration_ == appliedGeneration_) return;
        appliedGeneration_ = pendingGeneration_;
        appliedGen_.store(appliedGeneration_);
        s = pending_;
    }
    const bool changed = s.serverAddress != settings_.serverAddress || s.serverPort != settings_.serverPort ||
                         s.accessKey != settings_.accessKey || s.timeoutMs != settings_.timeoutMs;
    settings_ = s;
    if (changed) {
        CloseLocked("the settings changed");
        if (paused_.load()) EndPauseLocked("the settings changed");
        timeoutsInARow_ = 0;
        lastStatusLine_.clear();
        toldNoConnection_.clear();
        toldNotReady_.clear();
        std::lock_guard lock(bgMutex_);
        failedTries_ = 0;
        nextTry_ = {};
    }
}

bool Client::Endpoints(const std::string& host, int port, std::vector<Endpoint>& out, std::string& reason, bool lookUpNow) {
    if (host.empty()) {
        reason = "no server address is set";
        return false;
    }
    const auto matches = [&] { return lookup_.done && lookup_.host == host && lookup_.port == port; };
    bool found = false;
    {
        std::lock_guard lock(configMutex_);
        found = matches();
    }
    if (!found && lookUpNow) Resolve();
    std::lock_guard lock(configMutex_);
    if (!matches()) {
        reason = "the server's address hasn't been looked up yet";
        return false;
    }
    if (!lookup_.ok) {
        reason = "the server's address couldn't be looked up (" + lookup_.error + ")";
        return false;
    }
    out = lookup_.endpoints;
    return true;
}

bool Client::EndpointsLocked(std::vector<Endpoint>& out, std::string& reason, bool lookUpNow) {
    return Endpoints(settings_.serverAddress, settings_.serverPort, out, reason, lookUpNow);
}

std::vector<Client::Listed> Client::Newest(const std::vector<proto::ModelEntry>& models) {
    std::vector<Listed> listed;
    for (const auto& m : models) {
        const auto same = std::find_if(listed.begin(), listed.end(), [&](const Listed& l) { return l.entry.name == m.name; });
        if (same != listed.end()) listed.erase(same);  // the newer session of that name replaces the older
        listed.push_back(Listed{m});
    }
    return listed;
}

void Client::SetModels(const std::vector<proto::ModelEntry>& models) {
    {
        Lock lock(mutex_);
        std::vector<Listed> listed = Newest(models);
        bool same = listed.size() == listed_.size();
        for (std::size_t i = 0; same && i < listed.size(); ++i) same = listed[i].entry == listed_[i].entry;
        if (same) return;
        listed_ = std::move(listed);
        ++modelsGeneration_;
        helloCurrent_ = false;
    }
    Wake();
}

bool Client::Paused() const noexcept { return paused_.load(); }

int Client::HellosSent() const {
    Lock lock(mutex_);
    return hellosSent_;
}

int Client::SideHellos() const { return sideHellos_.load(); }

int Client::Reconnects() const {
    Lock lock(mutex_);
    return connects_;
}

int Client::Pings() const {
    Lock lock(mutex_);
    return pings_;
}

int Client::SideLoadsInFlight() const noexcept { return sideLoads_.load(); }

Counters Client::Totals() const {
    std::lock_guard lock(statsMutex_);
    return totals_;
}

void Client::CloseLocked(const std::string& why) noexcept {
    if (transport_) {
        transport_->Close();
        transport_.reset();
        try {
            Detail("Closed the connection to the server: " + why);
        } catch (...) {
        }
    }
    answered_.clear();
    helloCurrent_ = false;
    owed_.clear();
}

void Client::StartPauseLocked(const std::string& reason) {
    CloseLocked(reason);
    timeoutsInARow_ = 0;
    ++pauses_;
    const bool already = paused_.exchange(true);
    {
        std::lock_guard lock(bgMutex_);
        failedTries_ = 0;
        nextTry_ = now_() + BackoffDelay(0);
        if (!already) pausedSince_ = now_();
        wake_ = true;
        wakeCv_.notify_all();
    }
    if (!already) {
        std::lock_guard lock(statsMutex_);
        pausedRuns_ = 0;
    }
    Log(Level::Warn, reason + "; lines get the game's own lip sync until the server answers again");
}

void Client::EndPauseLocked(const std::string& why) {
    if (!paused_.exchange(false)) return;
    Clock::time_point since;
    {
        std::lock_guard lock(bgMutex_);
        since = pausedSince_;
    }
    std::uint64_t runs = 0;
    {
        std::lock_guard lock(statsMutex_);
        runs = pausedRuns_;
        pausedRuns_ = 0;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now_() - since).count();
    Log(Level::Info, "The pause ends after " + std::to_string(seconds) + " s (" + why + "); " + std::to_string(runs) +
                         " runs got the game's own lip sync meanwhile");
}

Client::Answered* Client::FindAnsweredLocked(const proto::ModelEntry& model) {
    for (auto& a : answered_)
        if (a.entry == model) return &a;
    return nullptr;
}

// Whether a run's fallback for a model in this state (not ready, and why) was logged already; true from the second on.
bool Client::ToldNotReadyLocked(const Answered& a) {
    for (const auto& t : toldNotReady_)
        if (t.entry == a.entry && t.status == a.status && t.detail == a.detail) return true;
    toldNotReady_.push_back(a);
    return false;
}

// A HELLO_OK on the main connection: its models' states. A model whose state changed is logged again at its next
// fallback; one the server answers the same way on each reconnect isn't.
void Client::AnsweredLocked(std::vector<Answered> fresh) {
    answered_ = std::move(fresh);
    std::erase_if(toldNotReady_, [this](const Answered& t) {
        const Answered* a = FindAnsweredLocked(t.entry);
        return !a || a->status != t.status || a->detail != t.detail;
    });
}

// After a HELLO_OK on the main connection: a model it says isn't ready is asked about again, on the backoff schedule.
void Client::ScheduleNotReadyLocked() {
    const bool all = std::all_of(answered_.begin(), answered_.end(), [](const Answered& a) { return a.status == 0; });
    std::lock_guard lock(bgMutex_);
    if (all) {
        failedTries_ = 0;
        return;
    }
    nextTry_ = now_() + BackoffDelay(failedTries_);
    ++failedTries_;
}

std::string Client::StatusLine(const std::string& server, const std::string& device, const std::vector<Answered>& models) const {
    std::vector<std::string> parts;
    for (const auto& m : models) {
        // a ready model may carry a detail too (another model running in its place): it is always logged
        std::string part = m.entry.name + " " + proto::ModelStatusName(m.status);
        if (!m.detail.empty()) part += " (" + m.detail + ")";
        parts.push_back(std::move(part));
    }
    return "Server " + server + " on " + device + ": " + (parts.empty() ? std::string("no models asked") : Join(parts));
}

bool Client::ConnectMainLocked(int timeoutMs, std::string& reason, bool& timedOut) {
    timedOut = false;
    std::vector<Endpoint> endpoints;
    if (!EndpointsLocked(endpoints, reason, false)) return false;
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    const Clock::time_point start = Clock::now();
    if (!t || !t->Connect(endpoints, timeoutMs, err)) {
        timedOut = Clock::now() >= start + std::chrono::milliseconds(timeoutMs);
        reason = "couldn't connect to the server (" + err + ")";
        return false;
    }
    transport_ = std::move(t);
    answered_.clear();
    helloCurrent_ = false;
    owed_.clear();
    ++connects_;
    lastActivity_ = now_();
    Log(Level::Info, "Connected to the server on port " + std::to_string(settings_.serverPort));
    return true;
}

// HELLO with every live model on the main connection.
bool Client::HelloMainLocked(Clock::time_point deadline, ExchangeFailure& failure, std::string& reason) {
    proto::Hello hello;
    const Versions versions = CurrentVersions();
    hello.client = ClientString(Totals(), versions);
    hello.key = settings_.accessKey;
    hello.ortVersion = versions.ort;
    for (const auto& l : listed_) hello.models.push_back(l.entry);
    ++hellosSent_;
    HelloAnswer ans;
    const bool ok = SayHello(*transport_, hello, deadline, &owed_, ans, failure, reason, [this](const std::string& l) { Detail(l); });
    lastActivity_ = now_();
    if (!ok) {
        Detail("HELLO on the main connection: " + reason);
        return false;
    }
    std::vector<Answered> fresh;
    for (std::size_t i = 0; i < hello.models.size(); ++i) fresh.push_back(Answered{hello.models[i], ans.models[i].status, ans.models[i].detail});
    AnsweredLocked(std::move(fresh));
    helloCurrent_ = true;
    const std::string line = StatusLine(ans.result.server, ans.result.device, answered_);
    if (line != lastStatusLine_)
        Log(Level::Info, line);
    else
        Detail(line);
    lastStatusLine_ = line;
    TellStatesLocked(ans.result.server, ans.result.device);
    ScheduleNotReadyLocked();
    return true;
}

Admission Client::Admit(const proto::ModelEntry& model) {
    Admission a;
    Settings s;
    Versions versions;
    {
        std::lock_guard lock(configMutex_);
        s = pending_;
        versions = versions_;
    }
    if (s.serverAddress.empty()) {
        a.hello.kind = ProbeResult::Kind::NoAddress;
        a.detail = "no server address is set";
        return a;
    }
    std::vector<Endpoint> endpoints;
    std::string reason;
    if (!Endpoints(s.serverAddress, s.serverPort, endpoints, reason, true)) {
        a.hello.kind = ProbeResult::Kind::NoAnswer;
        a.detail = reason;
        return a;
    }
    SideLoad inFlight(*this);
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(endpoints, static_cast<int>(kConnectTimeout.count()), err)) {
        a.hello.kind = ProbeResult::Kind::NoAnswer;
        a.detail = "couldn't connect to the server (" + err + ")";
        Detail("Side connection for " + model.name + ": " + a.detail);
        return a;
    }
    Detail("Side connection for " + model.name + ": connected");
    proto::Hello hello;
    hello.client = ClientString(Totals(), versions);
    hello.key = s.accessKey;
    hello.ortVersion = versions.ort;
    hello.models = {model};
    ++sideHellos_;
    HelloAnswer ans;
    Failure f = Failure::None;
    const bool ok = SayHello(*t, hello, Clock::now() + kHelloTimeout, nullptr, ans, f, reason, [this](const std::string& l) { Detail(l); });
    t->Close();
    Detail("Side connection for " + model.name + ": closed " + (ok ? std::string("after HELLO_OK") : "(" + reason + ")"));
    a.hello = ans.result;
    if (!ok) {
        a.detail = reason;
        return a;
    }
    a.status = ans.models[0].status;
    a.detail = ans.models[0].detail;
    Detail("HELLO on the side connection: " +
           StatusLine(ans.result.server, ans.result.device, {Answered{model, a.status, a.detail}}));
    if (a.Ready()) {
        // the server is there: a paused bridge tries again at once
        std::lock_guard lock(bgMutex_);
        nextTry_ = {};
    }
    return a;
}

Client::Outcome Client::Run(const proto::ModelEntry& model, proto::RunRequest& request, Reply& reply, Fallback& fallback) {
    fallback = Fallback{};
    const auto answer = [&](Outcome o, Why why, std::string text, bool quiet = false) {
        fallback = Fallback{why, std::move(text), quiet};
        return o;
    };
    // the call's whole time, counted from here: waiting for the connection, a connect, a HELLO and RUN
    const Clock::time_point entry = Clock::now();
    const int timeoutMs = timeoutMs_.load();
    const Clock::time_point deadline = entry + std::chrono::milliseconds(timeoutMs);
    // during a pause: at once, without the connection's lock
    if (paused_.load() && !SettingsPending()) return answer(Outcome::Skipped, Why::Paused, "the bridge is paused", true);
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    ++runsWaiting_;
    const bool got = lock.try_lock_until(deadline);
    --runsWaiting_;
    // another run, or the background thread's PING or HELLO, holding the connection past this call's timeout: this line
    // goes without
    if (!got) {
        fallback = Fallback{Why::Other, "the connection was busy past the timeout", false, true};
        return Outcome::Skipped;
    }
    ApplyPendingLocked();
    if (paused_.load()) return answer(Outcome::Skipped, Why::Paused, "the bridge is paused", true);
    const auto listed = std::find_if(listed_.begin(), listed_.end(), [&](const Listed& l) { return l.entry.name == model.name; });
    if (listed == listed_.end() || !(listed->entry == model))  // a session a newer one of its model replaced
        return answer(Outcome::Skipped, Why::Other, "a newer session of " + model.name + " replaced this one", true);
    std::string reason;
    if (!transport_) {
        std::vector<Endpoint> unused;
        if (!EndpointsLocked(unused, reason, false)) {
            // no address, or none looked up: only the first of a series is logged
            bool quiet = reason == toldNoConnection_;
            toldNoConnection_ = reason;
            if (reason.find("couldn't be looked up") != std::string::npos) {
                quiet = true;
                if (!lookupFailureLogged_.exchange(true)) Log(Level::Warn, reason + "; lines get the game's own lip sync until the settings change");
            }
            return answer(Outcome::Skipped, Why::NoConnection, reason, quiet);
        }
        toldNoConnection_.clear();
    }
    if (Answered* a = FindAnsweredLocked(model); a && a->status != static_cast<std::uint8_t>(proto::ModelStatus::Ready)) {
        const bool quiet = ToldNotReadyLocked(*a);
        return answer(Outcome::Skipped, Why::NotReady,
                      std::string("the server doesn't have it ready (") + proto::ModelStatusName(a->status) +
                          (a->detail.empty() ? "" : ": " + a->detail) + ")",
                      quiet);
    }

    request.requestId = nextRequest_++;
    if (nextRequest_ == 0) nextRequest_ = 1;
    std::vector<std::uint8_t> payload;
    std::string err;
    if (!proto::EncodeRun(request, payload, err)) return answer(Outcome::Skipped, Why::Other, "couldn't encode RUN (" + err + ")");

    const Clock::time_point start = Clock::now();
    if (start >= deadline) {
        fallback = Fallback{Why::Other, "the connection was busy past the timeout", false, true};
        return Outcome::Skipped;
    }
    const Note note = [this](const std::string& l) { Detail(l); };
    proto::Header header;
    bool reused = transport_ != nullptr;
    for (int attempt = 0;; ++attempt) {
        if (!transport_) {
            bool timedOut = false;
            if (!ConnectMainLocked(std::min(settings_.timeoutMs, MillisLeft(deadline)), reason, timedOut)) {
                StartPauseLocked(reason);
                return answer(Outcome::Failed, timedOut ? Why::ConnectTimedOut : Why::NoConnection, reason);
            }
        }
        Failure f = Failure::None;
        bool ok = true;
        bool inHello = false;
        if (!FindAnsweredLocked(model)) {
            inHello = true;
            {
                const MainHello inFlight(*this, false);  // a run's own HELLO is never held; a side load waits for it
                ok = HelloMainLocked(deadline, f, reason);
            }
            if (ok) {
                Answered* a = FindAnsweredLocked(model);
                if (!a || a->status != static_cast<std::uint8_t>(proto::ModelStatus::Ready)) {
                    if (a) ToldNotReadyLocked(*a);
                    return answer(Outcome::Failed, Why::NotReady,
                                  std::string("the server no longer has it ready (") +
                                      (a ? std::string(proto::ModelStatusName(a->status)) + (a->detail.empty() ? "" : ": " + a->detail)
                                         : std::string("not listed")) +
                                      ")");
                }
            }
        }
        if (ok) {
            inHello = false;
            ok = Transact(*transport_, MsgType::Run, payload, deadline, &owed_, request.requestId, header, reply.payload, f, reason, note);
            lastActivity_ = now_();
        }
        if (ok) break;
        // a connection the server had closed or reset, or a send that failed: open it again and send again, once,
        // within the same deadline
        if (attempt == 0 && reused && Early(f) && Clock::now() < deadline) {
            const std::string which = f == Failure::Reset    ? "The server had reset the connection"
                                      : f == Failure::Closed ? "The server had closed the connection"
                                                             : "Sending to the server failed (" + reason + ")";
            Log(Level::Info, which + "; reconnecting");
            CloseLocked(which);
            reused = false;
            continue;
        }
        if (inHello) {
            const Why why = f == Failure::NoByte || f == Failure::MidFrame ? Why::RunTimedOut
                            : f == Failure::Refused                         ? Why::ServerError
                            : f == Failure::Malformed                       ? Why::Other
                                                                            : Why::ConnectionLost;
            StartPauseLocked(reason);
            return answer(Outcome::Failed, why, "HELLO: " + reason);
        }
        if (f == Failure::NoByte) {
            // no byte of the answer arrived: the stream is still whole, and its late answer comes first
            const std::string text = "no answer within " + std::to_string(timeoutMs) + " ms";
            owed_.push_back(request.requestId);
            if (++timeoutsInARow_ >= 2) {
                StartPauseLocked("A second RUN in a row timed out");
            } else {
                Detail("Keeping the connection after a RUN timeout: the late answer to request " + std::to_string(request.requestId) +
                       " will be discarded");
            }
            return answer(Outcome::Failed, Why::RunTimedOut, text);
        }
        const Why why = f == Failure::MidFrame ? Why::RunTimedOut : f == Failure::Malformed || f == Failure::Encode ? Why::Other : Why::ConnectionLost;
        if (f == Failure::Encode) return answer(Outcome::Failed, why, reason);
        StartPauseLocked(reason);
        return answer(Outcome::Failed, why, reason);
    }
    reply.roundTrip = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    reply.bytesUp = payload.size() + proto::kHeaderSize;
    if (IsType(header, MsgType::Error)) {
        proto::Error e;
        if (!proto::DecodeError(reply.payload.data(), reply.payload.size(), e, err)) {
            reason = "the server's answer to RUN is malformed (" + err + ")";
            StartPauseLocked(reason);
            return answer(Outcome::Failed, Why::Other, reason);
        }
        timeoutsInARow_ = 0;
        reason = ErrorText(e);
        if (proto::ErrorEndsConnection(e.code)) StartPauseLocked("The server answered " + reason);
        return answer(Outcome::Failed, Why::ServerError, reason);
    }
    if (!IsType(header, MsgType::RunOk) || !proto::DecodeRunOk(reply.payload.data(), reply.payload.size(), reply.ok, err)) {
        reason = "the server's answer to RUN is malformed" + (err.empty() ? std::string() : " (" + err + ")");
        StartPauseLocked(reason);
        return answer(Outcome::Failed, Why::Other, reason);
    }
    if (reply.ok.requestId != request.requestId) {
        reason = "the server answered request " + std::to_string(reply.ok.requestId) + ", not " + std::to_string(request.requestId);
        StartPauseLocked(reason);
        return answer(Outcome::Failed, Why::Other, reason);
    }
    timeoutsInARow_ = 0;
    return Outcome::Forwarded;
}

void Client::RecordAnswered(const Reply& reply) {
    std::string line;
    {
        std::lock_guard lock(statsMutex_);
        ++totals_.answered;
        const auto rt = static_cast<std::uint64_t>(reply.roundTrip.count());
        ++period_.runs;
        ++period_.answered;
        period_.roundTripMicros += rt;
        period_.serverMicros += reply.ok.serverMicros;
        period_.slowestMicros = std::max(period_.slowestMicros, rt);
        if (period_.runs >= kSummaryRuns) {
            line = SummaryLine(period_);
            period_ = Period{};
            lastSummary_ = now_();
        }
    }
    if (!line.empty()) Log(Level::Info, line);
}

void Client::RecordFallback(const std::string& model, const Fallback& fb, std::chrono::microseconds took) {
    std::string line;
    {
        std::lock_guard lock(statsMutex_);
        ++totals_.fallbacks;
        if (fb.why == Why::RunTimedOut) ++totals_.timeouts;
        if (fb.why == Why::Paused) ++pausedRuns_;
        ++period_.runs;
        ++period_.byWhy[static_cast<std::size_t>(fb.why)];
        if (period_.runs >= kSummaryRuns) {
            line = SummaryLine(period_);
            period_ = Period{};
            lastSummary_ = now_();
        }
    }
    if (!fb.quiet) {
        // a text that begins with the reason's name ("ERROR 4, ...") stands for both
        const std::string why = WhyName(fb.why);
        const std::string what = fb.text.empty() ? why : fb.text.rfind(why + " ", 0) == 0 ? fb.text : why + ": " + fb.text;
        Log(Level::Warn, model + ": not answered (" + what + ") after " + Ms(static_cast<std::uint64_t>(took.count())) +
                             " ms; its line gets the game's own lip sync");
    }
    if (!line.empty()) Log(Level::Info, line);
}

void Client::Summary(bool timed) {
    std::string line;
    {
        std::lock_guard lock(statsMutex_);
        if (period_.runs == 0) return;
        if (timed && now_() < lastSummary_ + kSummaryEvery) return;
        line = SummaryLine(period_);
        period_ = Period{};
        lastSummary_ = now_();
    }
    Log(Level::Info, line);
}

ProbeResult Client::Probe(const Reason* fallback) {
    Settings s;
    Versions versions;
    {
        std::lock_guard lock(configMutex_);
        s = pending_;
        versions = versions_;
    }
    ProbeResult r;
    if (s.serverAddress.empty()) {
        r.kind = ProbeResult::Kind::NoAddress;
        return r;
    }
    std::vector<Endpoint> endpoints;
    std::string reason;
    if (!Endpoints(s.serverAddress, s.serverPort, endpoints, reason, true)) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = reason;
        return r;
    }
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(endpoints, static_cast<int>(kConnectTimeout.count()), err)) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = "couldn't connect (" + err + ")";
        return r;
    }
    const Counters sent = Totals();
    proto::Hello hello;
    hello.client = ClientString(sent, versions, fallback);
    hello.key = s.accessKey;
    hello.ortVersion = versions.ort;
    HelloAnswer ans;
    Failure f = Failure::None;
    SayHello(*t, hello, Clock::now() + kConnectTimeout, nullptr, ans, f, reason, nullptr);
    t->Close();
    r = ans.result;
    if (r.kind == ProbeResult::Kind::Answered) {
        std::lock_guard lock(bgMutex_);
        reported_ = sent;
    }
    return r;
}

// ---- the background thread

void Client::Wake() {
    std::lock_guard lock(bgMutex_);
    wake_ = true;
    wakeCv_.notify_all();
}

void Client::StartBackground() {
    std::lock_guard lock(bgMutex_);
    allowThread_ = true;
    MaybeStartLocked();
}

void Client::MaybeStartLocked() {
    if (!allowThread_ || !active_ || stop_ || thread_.joinable()) return;
    try {
        thread_ = std::thread([this] { Loop(); });
    } catch (...) {
        Log(Level::Error, "The bridge's background thread couldn't start: a failed connection stays paused");
    }
}

void Client::StopBackground() {
    {
        std::lock_guard lock(bgMutex_);
        stop_ = true;
        wakeCv_.notify_all();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void Client::Loop() {
    std::unique_lock lock(bgMutex_);
    while (!stop_) {
        wake_ = false;
        lock.unlock();
        Clock::time_point next;
        try {
            next = Tick();
        } catch (...) {
            next = now_() + std::chrono::seconds(1);
        }
        lock.lock();
        if (stop_) break;
        if (!wake_) wakeCv_.wait_until(lock, Clock::now() + (next - now_()), [this] { return wake_ || stop_; });
    }
}

Clock::time_point Client::Tick() {
    const Clock::time_point now = now_();
    Clock::time_point next = now + std::chrono::hours(1);
    const auto soon = [&](Clock::time_point t) { next = std::min(next, t); };
    Settings s;
    bool looked = false;
    {
        std::lock_guard lock(configMutex_);
        s = pending_;
        looked = lookup_.done && lookup_.host == s.serverAddress && lookup_.port == s.serverPort;
    }
    const bool on = !s.serverAddress.empty();

    // the name lookup, once per change of the address or port
    if (!s.serverAddress.empty() && !looked && !Resolve()) {
        std::string reason;
        std::vector<Endpoint> unused;
        Endpoints(s.serverAddress, s.serverPort, unused, reason, false);
        if (!lookupFailureLogged_.exchange(true)) Log(Level::Warn, reason + "; lines get the game's own lip sync until the settings change");
    }
    if (on) FirstProbe();

    // the main connection
    Snapshot snap;
    bool got = false;
    {
        TryLock lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && runsWaiting_.load() == 0) {
            got = true;
            ApplyPendingLocked();
            snap.pauses = pauses_;
            snap.paused = paused_.load();
            snap.connected = transport_ != nullptr;
            snap.helloCurrent = helloCurrent_;
            for (const auto& l : listed_) snap.models.push_back(l.entry);
            for (const auto& a : answered_)
                if (a.status != 0 && std::any_of(listed_.begin(), listed_.end(), [&](const Listed& l) { return l.entry == a.entry; }))
                    snap.anyNotReady = true;
            snap.pingable = transport_ && helloCurrent_ && !answered_.empty();
            snap.lastActivity = lastActivity_;
            snap.modelsGeneration = modelsGeneration_;
            snap.settings = settings_;
            if (snap.paused && listed_.empty()) {
                EndPauseLocked("no model is left to serve");
                snap.paused = false;
            }
        }
    }
    if (!got) {
        soon(now + kBusyRetry);
    } else if (!snap.settings.serverAddress.empty() && !snap.models.empty()) {
        Clock::time_point due;
        {
            std::lock_guard lock(bgMutex_);
            due = nextTry_;
        }
        // H: no HELLO of ours while our own side load is in flight; its end wakes the thread
        if (snap.paused || !snap.connected || snap.anyNotReady) {
            if (now < due) {
                soon(due);
            } else if (Recover(snap)) {
                std::lock_guard lock(bgMutex_);
                soon(nextTry_);
            }
        } else if (!snap.helloCurrent) {
            if (RehelloMain()) soon(now + kBusyRetry);
        } else if (snap.pingable) {
            if (now >= snap.lastActivity + kIdlePing) {
                PingMain();
                soon(now + kIdlePing);
            } else {
                soon(snap.lastActivity + kIdlePing);
            }
        }
    }

    if (on) {
        StatusCheck();
        FallbackReport();
        std::lock_guard lock(bgMutex_);
        soon(nextStatus_);
        if (fallbackPending_) soon(fallbackDue_);
    }
    Summary(true);
    {
        std::lock_guard lock(statsMutex_);
        if (period_.runs > 0) soon(lastSummary_ + kSummaryEvery);
    }
    return std::max(next, now);
}

// A new connection and HELLO with every live model, outside the connection's lock; swapped in under it. False when
// held, untried, for a side load in flight (H): its end wakes the thread.
bool Client::Recover(const Snapshot& snap) {
    const MainHello inFlight(*this, true);
    if (inFlight.Held()) {
        Detail("The background HELLO waits for the model load in progress");
        return false;
    }
    int attempt = 0;
    {
        std::lock_guard lock(bgMutex_);
        attempt = failedTries_ + 1;
    }
    Detail("Background attempt " + std::to_string(attempt) + ": connecting to the server and saying HELLO");
    std::string reason;
    const auto failed = [&](const std::string& why) {
        Detail("Background attempt " + std::to_string(attempt) + " failed: " + why);
        std::lock_guard lock(bgMutex_);
        ++failedTries_;
        nextTry_ = now_() + BackoffDelay(failedTries_);
        return true;
    };
    std::vector<Endpoint> endpoints;
    if (!Endpoints(snap.settings.serverAddress, snap.settings.serverPort, endpoints, reason, false)) return failed(reason);
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(endpoints, static_cast<int>(kConnectTimeout.count()), err)) return failed("couldn't connect (" + err + ")");
    proto::Hello hello;
    const Versions versions = CurrentVersions();
    hello.client = ClientString(Totals(), versions);
    hello.key = snap.settings.accessKey;
    hello.ortVersion = versions.ort;
    hello.models = snap.models;
    HelloAnswer ans;
    Failure f = Failure::None;
    if (!SayHello(*t, hello, Clock::now() + kHelloTimeout, nullptr, ans, f, reason, nullptr)) {
        t->Close();
        return failed(reason);
    }
    Lock lock(mutex_);
    ApplyPendingLocked();
    if (settings_.serverAddress != snap.settings.serverAddress || settings_.serverPort != snap.settings.serverPort ||
        settings_.accessKey != snap.settings.accessKey) {
        t->Close();
        Detail("Background attempt " + std::to_string(attempt) + ": the settings changed meanwhile");
        std::lock_guard bg(bgMutex_);
        nextTry_ = {};
        return true;
    }
    if (pauses_ != snap.pauses) {
        // a failure after this attempt began: an answer to a HELLO sent before it doesn't end its pause
        t->Close();
        Detail("Background attempt " + std::to_string(attempt) + ": a failure came meanwhile; trying again on the schedule");
        return true;
    }
    CloseLocked("the background thread's new connection replaces it");
    transport_ = std::move(t);
    ++connects_;
    ++hellosSent_;
    // at the normal level only when it ends a pause: a model the server keeps not ready reconnects every 30 s
    const std::string connected = "Connected to the server on port " + std::to_string(settings_.serverPort);
    if (paused_.load())
        Log(Level::Info, connected);
    else
        Detail(connected);
    std::vector<Answered> fresh;
    for (std::size_t i = 0; i < hello.models.size(); ++i) fresh.push_back(Answered{hello.models[i], ans.models[i].status, ans.models[i].detail});
    AnsweredLocked(std::move(fresh));
    helloCurrent_ = modelsGeneration_ == snap.modelsGeneration;
    lastActivity_ = now_();
    timeoutsInARow_ = 0;
    const std::string line = StatusLine(ans.result.server, ans.result.device, answered_);
    if (line != lastStatusLine_)
        Log(Level::Info, line);
    else
        Detail(line);
    lastStatusLine_ = line;
    TellStatesLocked(ans.result.server, ans.result.device);
    EndPauseLocked("the server answered the background HELLO");
    ScheduleNotReadyLocked();
    return true;
}

// HELLO with the live models on the open main connection, answered at once: a try-lock, giving way to runs. False when
// held for a side load in flight (H): its end wakes the thread.
bool Client::RehelloMain() {
    TryLock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || runsWaiting_.load() > 0) return true;
    ApplyPendingLocked();
    if (!transport_ || helloCurrent_ || listed_.empty() || paused_.load()) return true;
    const MainHello inFlight(*this, true);
    if (inFlight.Held()) {
        Detail("The background HELLO waits for the model load in progress");
        return false;
    }
    Failure f = Failure::None;
    std::string reason;
    if (!HelloMainLocked(Clock::now() + std::chrono::milliseconds(settings_.timeoutMs), f, reason))
        StartPauseLocked("The server didn't answer HELLO on the main connection (" + reason + ")");
    return true;
}

// PING on a connection idle for kIdlePing whose HELLO listed models; PONG within the RUN timeout.
void Client::PingMain() {
    TryLock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || runsWaiting_.load() > 0) return;
    ApplyPendingLocked();
    if (!transport_ || !helloCurrent_ || answered_.empty() || paused_.load() || now_() < lastActivity_ + kIdlePing) return;
    ++pings_;
    proto::Header header;
    std::vector<std::uint8_t> answer;
    Failure f = Failure::None;
    std::string reason;
    bool ok = Transact(*transport_, MsgType::Ping, {}, Clock::now() + std::chrono::milliseconds(settings_.timeoutMs), &owed_, 0, header,
                       answer, f, reason, [this](const std::string& l) { Detail(l); });
    lastActivity_ = now_();
    if (ok && !IsType(header, MsgType::Pong)) {
        ok = false;
        reason = "the answer was message type " + std::to_string(header.type) + ", not PONG";
    }
    if (ok) return;
    Detail("PING went unanswered: " + reason);
    StartPauseLocked("The server didn't answer PING (" + reason + ")");
}

// The first status check, when the bridge comes on: logged.
void Client::FirstProbe() {
    {
        std::lock_guard lock(bgMutex_);
        if (!probeWanted_) return;
        probeWanted_ = false;
        nextStatus_ = now_() + kStatusEvery;
    }
    const ProbeResult r = Probe();
    switch (r.kind) {
        case ProbeResult::Kind::Answered: Log(Level::Info, "The server answered: " + r.server + " on " + r.device); break;
        case ProbeResult::Kind::NoAnswer: Log(Level::Warn, "The server didn't answer: " + r.detail); break;
        case ProbeResult::Kind::WrongKey: Log(Level::Warn, "The server refused the access key"); break;
        case ProbeResult::Kind::NoAddress: break;
        default: Log(Level::Warn, "The server refused the bridge: " + r.detail); break;
    }
}

// Every kStatusEvery, when the counters changed since the last one the server answered: a status check, logged at
// Detailed only.
void Client::StatusCheck() {
    const Counters now = Totals();
    {
        std::lock_guard lock(bgMutex_);
        if (now_() < nextStatus_) return;
        nextStatus_ = now_() + kStatusEvery;
        if (now == reported_) return;
    }
    const ProbeResult r = Probe();
    Detail(r.kind == ProbeResult::Kind::Answered ? "Status check: answered by " + r.server
                                                 : "Status check: no answer" + (r.detail.empty() ? std::string() : " (" + r.detail + ")"));
}

// A line fell back: a status check with its reason, within seconds, at most one per kFallbackReportEvery.
void Client::ReportFallback(Reason reason) {
    if (reason == Reason::NoServer) return;
    {
        std::lock_guard lock(bgMutex_);
        const Clock::time_point now = now_();
        const bool waiting = fallbackPending_;
        fallbackPending_ = true;
        fallbackReason_ = reason;
        if (waiting) return;  // the one already due carries the latest reason
        fallbackDue_ = fallbackReported_ ? std::max(now, lastFallbackReport_ + kFallbackReportEvery) : now;
        wake_ = true;  // the thread sends it now, or sets its timer for it
        wakeCv_.notify_all();
    }
}

int Client::FallbackReports() const {
    std::lock_guard lock(bgMutex_);
    return fallbackReports_;
}

void Client::FallbackReport() {
    Reason reason;
    {
        std::lock_guard lock(bgMutex_);
        if (!fallbackPending_ || now_() < fallbackDue_) return;
        fallbackPending_ = false;
        fallbackReported_ = true;
        lastFallbackReport_ = now_();
        reason = fallbackReason_;
        ++fallbackReports_;
    }
    const ProbeResult r = Probe(&reason);
    Detail(std::string("Status check for a fallback (") + ReasonKey(reason) + "): " +
           (r.kind == ProbeResult::Kind::Answered ? "answered by " + r.server : "no answer" + (r.detail.empty() ? std::string() : " (" + r.detail + ")")));
}

}  // namespace bfarb
