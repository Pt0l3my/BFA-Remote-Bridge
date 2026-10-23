#include "core/Client.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <utility>

#include "core/Scrub.h"
#include "core/Version.h"
#include "core/Monitor.h"

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

// The server's texts go on one line and lose any path or address before they are logged or shown (a note becomes a
// SkyrimNet event).
std::string ErrorText(const proto::Error& e) {
    return "ERROR " + std::to_string(e.code) + ", " + proto::ErrorCodeName(e.code) + (e.message.empty() ? "" : ": " + ServerText(e.message));
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

// One request and its answer on t by the deadline, framed in `version`; the answer must be framed in it too (with
// olderError, an ERROR framed in an older version also passes: a version 1 server's answer to a version 2 HELLO). With
// owed, whole frames answering a request that timed out earlier on this connection (RUN_OK or ERROR with an owed
// request id other than `awaited`) are read and discarded first; a discarded ERROR that ends the connection fails the
// exchange. False with failure and reason otherwise.
bool Transact(Transport& t, MsgType type, const std::vector<std::uint8_t>& payload, std::uint16_t version, Clock::time_point deadline,
              std::vector<std::uint32_t>* owed, std::uint32_t awaited, proto::Header& header, std::vector<std::uint8_t>& answer,
              Failure& failure, std::string& reason, const Note& note, bool olderError = false) {
    const auto fail = [&](Failure f, std::string why) {
        failure = f;
        reason = std::move(why);
        return false;
    };
    std::vector<std::uint8_t> frame;
    std::string err;
    if (!proto::EncodeFrame(type, payload, frame, err, version)) return fail(Failure::Encode, "couldn't encode the request (" + err + ")");
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
        // each side answers in the version the other opened with
        if (header.version != version && !(olderError && header.version < version && IsType(header, MsgType::Error)))
            return fail(Failure::Malformed, "the server answered in protocol version " + std::to_string(header.version) + ", not " +
                                                std::to_string(version));
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

// HELLO on t in `version` and its answer, checked against the models it listed (the same, in the same order). A
// version 2 HELLO answered with ERROR 6 framed in version 1 fails with OlderVersion: a version 1 server.
bool SayHello(Transport& t, const proto::Hello& hello, std::uint16_t version, Clock::time_point deadline, std::vector<std::uint32_t>* owed,
              HelloAnswer& out, Failure& failure, std::string& reason, const Note& note) {
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
    if (!Transact(t, MsgType::Hello, payload, version, deadline, owed, 0, header, answer, failure, reason, note, version > proto::kVersion1)) {
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
        if (header.version < version && e.code == static_cast<std::uint16_t>(proto::ErrorCode::VersionNotSupported)) {
            failure = Failure::OlderVersion;
            out.result.detail = "the server speaks protocol version " + std::to_string(header.version) + " only";
            reason = "the server answered the version " + std::to_string(version) + " HELLO with " + ErrorText(e) + ", framed in version " +
                     std::to_string(header.version);
            return false;
        }
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
    // a server's detail on one line, without paths (it may name its Model folder) or addresses: logged in full, shown capped
    for (auto& m : ok.models) m.detail = ServerText(m.detail);
    failure = Failure::None;
    out.result.kind = ProbeResult::Kind::Answered;
    out.result.server = ok.server;
    out.result.device = ok.device;
    out.models = std::move(ok.models);
    return true;
}

bool Early(Failure f) { return f == Failure::SendFailed || f == Failure::Closed || f == Failure::Reset; }
// No frame came back: the connection was reset, or closed before any answer.
bool NoFrame(Failure f) { return f == Failure::Closed || f == Failure::Reset; }
constexpr const char* kNoFrameTwice = "two version 2 HELLOs got no answer: the connection was reset or closed before any";

// The version rule's identity of a server: the address setting's, or the address found, with the port. Never logged.
std::string HostKey(const std::string& host, int port) { return "address " + host + " port " + std::to_string(port); }
std::string FoundKey(const std::vector<Endpoint>& endpoints, int port) {
    static const char* hex = "0123456789abcdef";
    std::string key = "found";
    for (const auto& e : endpoints) {
        key += " " + std::to_string(e.family) + ":";
        for (const std::uint8_t b : e.address) key += {hex[b >> 4], hex[b & 15]};
    }
    return key + " port " + std::to_string(port);
}

static_assert(kNoticeEvery == Monitor::kFallbackEvery, "NOTICE goes at most as often as the fallback note on screen");

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

// A connection of its own, opened with HELLO (OpenWithHello).
struct Client::Opened {
    std::unique_ptr<Transport> transport;  // open after HELLO_OK; null otherwise
    std::uint16_t version = proto::kVersion2;
    bool connected = false;  // the last connect tried succeeded
    std::string connectError;
    HelloAnswer answer;
    ExchangeFailure failure = ExchangeFailure::None;
    std::string reason;
    bool Ok() const { return transport != nullptr; }
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
    bool moved = false;
    {
        std::lock_guard lock(configMutex_);
        moved = settings.serverAddress != pending_.serverAddress || settings.serverPort != pending_.serverPort;
        if (moved) {
            // another address (or the search's, or another port): searched again at once, and spoken to in version 2 first
            lookupFailureLogged_.store(false);
            found_ = Found{};
            anyFind_ = false;
            lastFindError_.clear();
            notFoundLogged_ = false;
            versionKey_.clear();
            serverVersion_ = 0;
        }
        configured_ = true;
        pending_ = settings;
        ++pendingGeneration_;
        pendingGen_.store(pendingGeneration_);
        timeoutMs_.store(settings.timeoutMs);
        detailed_.store(settings.logDetail == LogDetail::Detailed);
    }
    if (moved) {
        std::lock_guard lock(noticeMutex_);  // a NOTICE about the last server isn't for the next
        noticePending_ = false;
        noticeLines_ = 0;
    }
    std::lock_guard lock(bgMutex_);
    active_ = true;  // a server address, or the search for one
    MaybeStartLocked();
    wake_ = true;
    wakeCv_.notify_all();
}

void Client::SetDiscovery(DiscoveryFactory discovery) {
    std::lock_guard lock(configMutex_);
    discovery_ = std::move(discovery);
}

void Client::OnVersion(std::function<void(std::uint16_t)> sink) {
    std::lock_guard lock(configMutex_);
    versionSink_ = std::move(sink);
}

std::uint16_t Client::ServerVersion() const {
    std::lock_guard lock(configMutex_);
    return serverVersion_;
}

int Client::NoticesSent() const {
    std::lock_guard lock(noticeMutex_);
    return noticesSent_;
}

int Client::Searches() const { return searches_.load(); }

std::uint16_t Client::VersionFor(const std::string& key) const {
    std::lock_guard lock(configMutex_);
    return versionKey_ == key && serverVersion_ == proto::kVersion1 ? proto::kVersion1 : proto::kVersion2;
}

// What the server at key answered in: a version 1 server is spoken to in version 1 from now on, logged once.
void Client::LearnVersion(const std::string& key, std::uint16_t version, const char* why) {
    bool first = false, changed = false;
    std::function<void(std::uint16_t)> sink;
    {
        std::lock_guard lock(configMutex_);
        changed = !(versionKey_ == key && serverVersion_ == version);
        first = version == proto::kVersion1 && changed;
        versionKey_ = key;
        serverVersion_ = version;
        if (changed) sink = versionSink_;
    }
    if (sink) {
        try {
            sink(version);
        } catch (...) {
        }
    }
    if (first)
        Log(Level::Info, std::string("The server speaks protocol version 1 only (") +
                             (why ? why : "it answered the bridge's version 2 HELLO with ERROR 6") +
                             "): the bridge speaks version 1 to it until the game restarts or the address changes, and tells it of "
                             "fallbacks by its status checks, not by NOTICE");
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

bool Client::Endpoints(const std::string& host, int port, Target& out, std::string& reason, bool lookUpNow, bool mayFind, bool* notFound) {
    if (notFound) *notFound = false;
    if (host.empty()) {
        if (Discover(port, out, reason, mayFind)) return true;
        if (notFound) *notFound = true;
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
    out.endpoints = lookup_.endpoints;
    out.key = HostKey(host, port);
    out.found = false;
    return true;
}

bool Client::EndpointsLocked(Target& out, std::string& reason, bool lookUpNow) {
    return Endpoints(settings_.serverAddress, settings_.serverPort, out, reason, lookUpNow, false);
}

// The server found on the local network; with mayFind, a search now when there is none (or it went stale), at most one
// every kFindEvery. A search in progress makes another caller wait for its result. Never called with mutex_ held when
// mayFind is set.
bool Client::Discover(int port, Target& out, std::string& reason, bool mayFind) {
    const auto fromFound = [&] {  // under configMutex_
        if (!found_.valid || found_.stale || found_.port != port) return false;
        out.endpoints = found_.endpoints;
        out.key = FoundKey(found_.endpoints, port);
        out.found = true;
        return true;
    };
    const auto moved = [&] { return !pending_.serverAddress.empty() || pending_.serverPort != port; };  // under configMutex_
    {
        std::lock_guard lock(configMutex_);
        if (fromFound()) return true;
        if (!mayFind) {
            reason = found_.valid ? "the server found on the local network stopped answering, and the bridge is searching again"
                                  : "no server has been found on the local network yet";
            return false;
        }
    }
    std::lock_guard round(findMutex_);
    DiscoveryFactory factory;
    {
        std::lock_guard lock(configMutex_);
        if (fromFound()) return true;  // found by the search this caller waited for
        if (moved()) {
            reason = "the settings changed";
            return false;
        }
        if (anyFind_ && now_() < lastFind_ + kFindEvery) {
            reason = lastFindError_.empty() ? std::string("no server answered on the local network yet") : lastFindError_;
            return false;
        }
        anyFind_ = true;
        lastFind_ = now_();
        factory = discovery_;
    }
    std::vector<Endpoint> endpoints;
    std::string server, why;
    const bool ok = Search(factory, port, endpoints, server, why);
    bool fresh = false, tellNotFound = false;
    {
        std::lock_guard lock(configMutex_);
        if (moved()) {
            reason = "the settings changed";
            return false;
        }
        if (!ok) {
            reason = lastFindError_ = why;
            tellNotFound = !notFoundLogged_;
            notFoundLogged_ = true;
        } else {
            fresh = !found_.valid || found_.stale || found_.endpoints != endpoints || found_.port != port;
            if (!found_.valid || found_.endpoints != endpoints || found_.port != port) {
                // another address: spoken to in version 2 first
                versionKey_.clear();
                serverVersion_ = 0;
            }
            found_ = Found{true, false, port, endpoints, server};
            notFoundLogged_ = false;
            fromFound();
        }
    }
    if (tellNotFound) Log(Level::Warn, why + "; the bridge searches again at most every " + std::to_string(kFindEvery.count()) + " s");
    // the server's own name only: an address says where someone's PC is
    if (fresh) Log(Level::Info, "Found the server on the local network: " + ServerText(server) + ", port " + std::to_string(port));
    return ok;
}

// One search: FIND to each IPv4 adapter's broadcast address, then the first FOUND within kFindWait.
bool Client::Search(const DiscoveryFactory& factory, int port, std::vector<Endpoint>& found, std::string& server, std::string& why) {
    auto d = factory ? factory() : nullptr;
    if (!d) {
        why = "no server address is set, and the bridge can't search the local network here";
        return false;
    }
    std::vector<std::uint8_t> payload, datagram;
    std::string err;
    if (!proto::EncodeFind(ClientString(Totals(), CurrentVersions()), payload, err) ||
        !proto::EncodeDatagram(MsgType::Find, payload, datagram, err)) {
        why = "FIND couldn't be encoded (" + err + ")";
        return false;
    }
    ++searches_;
    const int sent = d->Broadcast(datagram, static_cast<std::uint16_t>(port), err);
    if (sent <= 0) {
        d->Close();
        why = "no server was found on the local network: FIND couldn't be sent on any network adapter (" + err + ")";
        return false;
    }
    Detail("FIND sent to " + std::to_string(sent) + " broadcast address" + (sent == 1 ? "" : "es") + " on port " + std::to_string(port));
    const Clock::time_point deadline = Clock::now() + kFindWait;
    for (;;) {
        std::vector<std::uint8_t> bytes;
        Endpoint sender;
        const Transport::Result r = d->Receive(bytes, static_cast<std::uint16_t>(port), sender, deadline, err);
        if (r == Transport::Result::TimedOut) {
            why = "no server answered FIND on the local network (port " + std::to_string(port) + ", " + std::to_string(sent) +
                  " broadcast address" + (sent == 1 ? "" : "es") + ")";
            break;
        }
        if (r != Transport::Result::Ok) {
            why = "no server was found on the local network: the search failed (" + err + ")";
            break;
        }
        proto::Header h;
        std::vector<std::uint8_t> p;
        std::string name;
        if (proto::DecodeDatagram(bytes.data(), bytes.size(), h, p, err) && h.type == static_cast<std::uint16_t>(MsgType::Found) &&
            proto::DecodeFound(p.data(), p.size(), name, err)) {
            d->Close();
            found = {sender};
            server = name;
            return true;
        }
        Detail("Ignored a datagram that isn't FOUND (" + (h.type == static_cast<std::uint16_t>(MsgType::Find) ? std::string("a FIND") : err) + ")");
    }
    d->Close();
    return false;
}

// The address found was lost (a connection failed): the next attempt searches again.
void Client::MarkFoundStale() {
    std::lock_guard lock(configMutex_);
    if (found_.valid) found_.stale = true;
}

// A connection of its own: connected within kConnectTimeout, then HELLO in the server's version, within helloTimeout.
// A version 1 server's ERROR 6 to a version 2 HELLO is learnt, and HELLO goes again on a new connection, in version 1.
Client::Opened Client::OpenWithHello(const Target& target, const proto::Hello& hello, std::chrono::milliseconds helloTimeout, bool notes) {
    Opened o;
    const Note note = [this, notes](const std::string& l) {
        if (notes) Detail(l);
    };
    int noFrame = 0;  // version 2 HELLOs that got no frame back
    for (int round = 0; round < 3; ++round) {
        o.version = VersionFor(target.key);
        auto t = factory_ ? factory_() : nullptr;
        o.connectError = "no network";
        o.connected = t && t->Connect(target.endpoints, static_cast<int>(kConnectTimeout.count()), o.connectError);
        if (!o.connected) {
            if (target.found) MarkFoundStale();
            return o;
        }
        if (SayHello(*t, hello, o.version, Clock::now() + helloTimeout, nullptr, o.answer, o.failure, o.reason, note)) {
            LearnVersion(target.key, o.version);
            o.transport = std::move(t);
            return o;
        }
        t->Close();
        if (o.failure == Failure::OlderVersion) {
            LearnVersion(target.key, proto::kVersion1);
            if (notes) Detail("Connecting again in protocol version 1");
            continue;
        }
        // no frame back (PandorumLLM v3.97-p6 and earlier close with the payload unread, which can reset the
        // connection on Windows): once more in version 2, then as a version 1 server
        if (o.version == proto::kVersion2 && NoFrame(o.failure)) {
            Detail(std::string("The connection was ") + (o.failure == Failure::Reset ? "reset" : "closed") +
                   " before any answer to a version 2 HELLO");
            if (++noFrame == 2) LearnVersion(target.key, proto::kVersion1, kNoFrameTwice);
            continue;
        }
        return o;
    }
    return o;
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
    mainAnswered_ = false;
    owed_.clear();
}

void Client::StartPauseLocked(const std::string& reason) {
    CloseLocked(reason);
    if (settings_.serverAddress.empty()) MarkFoundStale();  // a lost connection: the server is searched for again
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
    Target target;
    if (!EndpointsLocked(target, reason, false)) return false;
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    const Clock::time_point start = Clock::now();
    if (!t || !t->Connect(target.endpoints, timeoutMs, err)) {
        timedOut = Clock::now() >= start + std::chrono::milliseconds(timeoutMs);
        reason = "couldn't connect to the server (" + err + ")";
        if (target.found) MarkFoundStale();
        return false;
    }
    transport_ = std::move(t);
    mainVersion_ = VersionFor(target.key);
    mainKey_ = target.key;
    mainFound_ = target.found;
    mainAnswered_ = false;
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
    const bool ok = SayHello(*transport_, hello, mainVersion_, deadline, &owed_, ans, failure, reason, [this](const std::string& l) { Detail(l); });
    lastActivity_ = now_();
    if (!ok) {
        Detail("HELLO on the main connection: " + reason);
        if (mainVersion_ == proto::kVersion2 && !mainAnswered_ && NoFrame(failure))
            Detail(std::string("The connection was ") + (failure == Failure::Reset ? "reset" : "closed") +
                   " before any answer to a version 2 HELLO");
        if (failure == Failure::OlderVersion) LearnVersion(mainKey_, proto::kVersion1);
        return false;
    }
    mainAnswered_ = true;
    LearnVersion(mainKey_, mainVersion_);
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
    // the address set, or the server found on the local network (searched for now, when none is)
    Target target;
    std::string reason;
    bool notFound = false;
    if (!Endpoints(s.serverAddress, s.serverPort, target, reason, true, true, &notFound)) {
        a.hello.kind = notFound ? ProbeResult::Kind::NotFound : ProbeResult::Kind::NoAnswer;
        a.detail = reason;
        return a;
    }
    SideLoad inFlight(*this);
    proto::Hello hello;
    hello.client = ClientString(Totals(), versions);
    hello.key = s.accessKey;
    hello.ortVersion = versions.ort;
    hello.models = {model};
    ++sideHellos_;
    Opened o = OpenWithHello(target, hello, kHelloTimeout, true);
    if (!o.connected) {
        a.hello.kind = ProbeResult::Kind::NoAnswer;
        a.detail = "couldn't connect to the server (" + o.connectError + ")";
        Detail("Side connection for " + model.name + ": " + a.detail);
        return a;
    }
    if (o.Ok()) o.transport->Close();
    Detail("Side connection for " + model.name + ": closed " + (o.Ok() ? std::string("after HELLO_OK") : "(" + o.reason + ")"));
    a.hello = o.answer.result;
    if (!o.Ok()) {
        a.detail = o.reason;
        return a;
    }
    const HelloAnswer& ans = o.answer;
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
        Target unused;
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
    bool downgraded = false;  // connected again in version 1, for a version 1 server's answer to the version 2 HELLO
    int noFrame = 0;          // version 2 HELLOs on a new connection that got no frame back
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
            ok = Transact(*transport_, MsgType::Run, payload, mainVersion_, deadline, &owed_, request.requestId, header, reply.payload, f, reason,
                          note);
            lastActivity_ = now_();
        }
        if (ok) break;
        // a version 1 server: connect again in version 1 and say HELLO, once, within the same deadline
        if (f == Failure::OlderVersion && !downgraded && Clock::now() < deadline) {
            CloseLocked("the server speaks protocol version 1 only");
            downgraded = true;
            reused = false;
            continue;
        }
        // a new connection's version 2 HELLO with no frame back: once more in version 2, then in version 1 (an older
        // server that closed with the HELLO unread), within the same deadline
        if (inHello && !reused && mainVersion_ == proto::kVersion2 && NoFrame(f) && !downgraded && Clock::now() < deadline) {
            if (++noFrame == 2) {
                LearnVersion(mainKey_, proto::kVersion1, kNoFrameTwice);
                downgraded = true;
            }
            CloseLocked("no answer to the version 2 HELLO");
            continue;
        }
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
        SignalNoticeLocked();
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
    SignalNoticeLocked();
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
    // the address set, or the server found on the local network (searched for now, when none is: at start-up, the
    // first status check's)
    Target target;
    std::string reason;
    bool notFound = false;
    if (!Endpoints(s.serverAddress, s.serverPort, target, reason, true, true, &notFound)) {
        r.kind = notFound ? ProbeResult::Kind::NotFound : ProbeResult::Kind::NoAnswer;
        r.detail = reason;
        return r;
    }
    const Counters sent = Totals();
    proto::Hello hello;
    hello.client = ClientString(sent, versions, fallback);
    hello.key = s.accessKey;
    hello.ortVersion = versions.ort;
    Opened o = OpenWithHello(target, hello, kConnectTimeout, false);
    if (!o.connected) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = "couldn't connect (" + o.connectError + ")";
        return r;
    }
    if (o.Ok()) o.transport->Close();
    r = o.answer.result;
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
    bool looked = false, on = false;
    {
        std::lock_guard lock(configMutex_);
        s = pending_;
        looked = lookup_.done && lookup_.host == s.serverAddress && lookup_.port == s.serverPort;
        on = configured_;  // a server address, or the search for one
    }

    // the name lookup, once per change of the address or port
    if (!s.serverAddress.empty() && !looked && !Resolve()) {
        std::string reason;
        Target unused;
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
    } else if (on && !snap.models.empty()) {
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
        {
            std::lock_guard lock(bgMutex_);
            soon(nextStatus_);
            if (fallbackPending_) soon(fallbackDue_);
        }
        // a NOTICE due, on the main connection
        bool pending = false;
        Clock::time_point due = now;
        {
            std::lock_guard lock(noticeMutex_);
            pending = noticePending_;
            if (anyNotice_) due = std::max(now, lastNotice_ + kNoticeEvery);
        }
        if (pending && due > now) {
            soon(due);
        } else if (pending) {
            const Clock::duration again = NoticeMain();
            if (again > Clock::duration::zero()) soon(now + again);
        }
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
    int attempt = 0;
    {
        std::lock_guard lock(bgMutex_);
        attempt = failedTries_ + 1;
    }
    std::string reason;
    const auto failed = [&](const std::string& why) {
        Detail("Background attempt " + std::to_string(attempt) + " failed: " + why);
        std::lock_guard lock(bgMutex_);
        ++failedTries_;
        nextTry_ = now_() + BackoffDelay(failedTries_);
        return true;
    };
    // the address set, or the server found on the local network: after a lost connection, searched for again
    Target target;
    if (!Endpoints(snap.settings.serverAddress, snap.settings.serverPort, target, reason, false, true)) return failed(reason);
    const MainHello inFlight(*this, true);
    if (inFlight.Held()) {
        Detail("The background HELLO waits for the model load in progress");
        return false;
    }
    Detail("Background attempt " + std::to_string(attempt) + ": connecting to the server and saying HELLO");
    proto::Hello hello;
    const Versions versions = CurrentVersions();
    hello.client = ClientString(Totals(), versions);
    hello.key = snap.settings.accessKey;
    hello.ortVersion = versions.ort;
    hello.models = snap.models;
    Opened o = OpenWithHello(target, hello, kHelloTimeout, false);
    if (!o.connected) return failed("couldn't connect (" + o.connectError + ")");
    if (!o.Ok()) return failed(o.reason);
    std::unique_ptr<Transport> t = std::move(o.transport);
    const HelloAnswer& ans = o.answer;
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
    mainVersion_ = o.version;
    mainKey_ = target.key;
    mainFound_ = target.found;
    mainAnswered_ = true;
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
    bool ok = Transact(*transport_, MsgType::Ping, {}, mainVersion_, Clock::now() + std::chrono::milliseconds(settings_.timeoutMs), &owed_, 0, header,
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

// The first status check, once the settings are given (with an empty address, the start-up search's): logged.
void Client::FirstProbe() {
    {
        std::lock_guard lock(bgMutex_);
        if (!probeWanted_) return;
        probeWanted_ = false;
        nextStatus_ = now_() + kStatusEvery;
    }
    const ProbeResult r = Probe();
    switch (r.kind) {
        case ProbeResult::Kind::Answered:
            Log(Level::Info, "The server answered: " + r.server + " on " + r.device + ", in protocol version " + std::to_string(ServerVersion()));
            break;
        case ProbeResult::Kind::NoAnswer: Log(Level::Warn, "The server didn't answer: " + r.detail); break;
        case ProbeResult::Kind::WrongKey: Log(Level::Warn, "The server refused the access key"); break;
        case ProbeResult::Kind::NotFound: break;  // the search logged it
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

// A line fell back: to a server that answered in version 2, a NOTICE (the background thread's, NoticeMain). Anything
// else (a line to any other server; a model load's refusal or a runtime entry to every server): a status check with
// its reason, within seconds, at most one per kFallbackReportEvery.
void Client::ReportFallback(Reason reason, const std::string& model, const std::string& detail) {
    if (reason == Reason::NoServer) return;
    if (!model.empty()) {
        std::lock_guard lock(noticeMutex_);
        if (noticeLines_ < UINT32_MAX) ++noticeLines_;
        // the detail as the player reads it: one line, no path or address, capped
        notice_ = proto::Notice{model, ReasonKey(reason), ShownServerText(detail), 0};
        noticePending_ = true;
    }
    if (ServerVersion() == proto::kVersion2 && !model.empty()) {
        // a line goes to a version 2 server by NOTICE alone; a model load's refusal or a runtime entry (no line) keeps
        // 1.2.0's status check, sent in the version the server speaks
        Wake();
        return;
    }
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

// The NOTICE due, on the open main connection: only in version 2, and only while no request there awaits an answer
// (after a RUN's answer, before the next request). How soon to look again: zero when there is nothing to retry.
Clock::duration Client::NoticeMain() {
    TryLock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || runsWaiting_.load() > 0) return kBusyRetry;  // a run goes first
    ApplyPendingLocked();
    proto::Notice n;
    {
        std::lock_guard nl(noticeMutex_);
        if (!noticePending_ || (anyNotice_ && now_() < lastNotice_ + kNoticeEvery)) return Clock::duration::zero();
        // nothing of version 2 to a server that hasn't answered in it; and none without a model left to run
        if ((transport_ && mainVersion_ != proto::kVersion2) || ServerVersion() == proto::kVersion1 || (!transport_ && listed_.empty())) {
            noticePending_ = false;
            noticeLines_ = 0;
            return Clock::duration::zero();
        }
        // the connection back, its HELLO answered and current, and no answer owed first: the run that clears the way
        // wakes the thread (SignalNoticeLocked), and a reconnect of its own sends it in the same round
        if (!transport_ || !mainAnswered_ || !helloCurrent_ || !owed_.empty() || paused_.load()) {
            noticeWaiting_ = true;
            return Clock::duration::zero();
        }
        // only a model the connection's last HELLO listed
        if (std::none_of(answered_.begin(), answered_.end(), [&](const Answered& a) { return a.entry.name == notice_.model; })) {
            noticePending_ = false;
            noticeLines_ = 0;
            return Clock::duration::zero();
        }
        noticeWaiting_ = false;
        n = notice_;
        n.lines = noticeLines_;
    }
    std::vector<std::uint8_t> payload, frame;
    std::string err;
    if (!proto::EncodeNotice(n, payload, err) || !proto::EncodeFrame(MsgType::Notice, payload, frame, err, mainVersion_)) {
        Log(Level::Warn, "NOTICE couldn't be encoded (" + err + ")");
        std::lock_guard nl(noticeMutex_);
        noticePending_ = false;
        noticeLines_ = 0;
        return Clock::duration::zero();
    }
    const Transport::Result r = transport_->SendAll(frame.data(), frame.size(), Clock::now() + std::chrono::milliseconds(settings_.timeoutMs), err);
    if (r != Transport::Result::Ok) {
        // the stream's state is unknown: a new connection is made by the next run or the background thread
        Log(Level::Info, "Sending NOTICE failed (" + err + "); reconnecting");
        CloseLocked("sending NOTICE failed");
        noticeWaiting_ = true;  // for the next connection
        return Clock::duration::zero();
    }
    {
        std::lock_guard nl(noticeMutex_);
        noticeLines_ -= std::min(noticeLines_, n.lines);  // lines that fell back meanwhile wait for the next one
        noticePending_ = noticeLines_ > 0;
        anyNotice_ = true;
        lastNotice_ = now_();
        ++noticesSent_;
    }
    Detail("NOTICE: " + n.model + ", " + n.reason + ", " + std::to_string(n.lines) + " line" + (n.lines == 1 ? "" : "s") + " since the last (" +
           n.detail + ")");
    return Clock::duration::zero();
}

// After a RUN's answer: a NOTICE that waited for the connection (owed answers, a reconnect, a HELLO) may go now.
void Client::SignalNoticeLocked() {
    if (!noticeWaiting_ || !transport_ || !mainAnswered_ || !helloCurrent_ || !owed_.empty()) return;
    noticeWaiting_ = false;
    Wake();
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
