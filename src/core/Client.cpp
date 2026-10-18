#include "core/Client.h"

#include <algorithm>
#include <utility>

#include "core/Version.h"

namespace bfarb {
namespace {

using proto::MsgType;
using Lock = std::lock_guard<std::timed_mutex>;

std::string Join(const std::vector<std::string>& parts) {
    std::string s;
    for (const auto& p : parts) s += (s.empty() ? "" : "; ") + p;
    return s;
}

std::string ErrorText(const proto::Error& e) {
    return std::string(proto::ErrorCodeName(e.code)) + (e.message.empty() ? "" : " (" + e.message + ")");
}

int MillisLeft(Clock::time_point deadline) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(ms, 1, 600000));
}

}  // namespace

std::string ClientName() { return std::string("BFARemoteBridge ") + kVersionText; }

Client::Client(TransportFactory factory, Resolver resolver, LogSink log, Now now)
    : factory_(std::move(factory)), resolver_(std::move(resolver)), log_(std::move(log)), now_(now) {}

Client::~Client() {
    Lock lock(mutex_);
    CloseLocked();
}

void Client::Configure(const Settings& settings) {
    std::lock_guard lock(configMutex_);
    pending_ = settings;
    ++pendingGeneration_;
    timeoutMs_.store(settings.timeoutMs);
}

void Client::SetOrtVersion(const std::string& version) {
    std::lock_guard lock(configMutex_);
    ortVersion_ = version;
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

// Under mutex_: the settings Configure left, applied.
void Client::ApplyPendingLocked() {
    Settings s;
    {
        std::lock_guard lock(configMutex_);
        if (pendingGeneration_ == appliedGeneration_) return;
        appliedGeneration_ = pendingGeneration_;
        s = pending_;
    }
    const bool changed = s.serverAddress != settings_.serverAddress || s.serverPort != settings_.serverPort ||
                         s.accessKey != settings_.accessKey || s.timeoutMs != settings_.timeoutMs;
    settings_ = s;
    if (changed) {
        CloseLocked();
        backoffUntil_ = {};
        lastStatusLine_.clear();
        lookupFailureLogged_ = false;
    }
}

// Under mutex_: the addresses looked up for the settings in force. lookUpNow looks the name up when it hasn't been.
bool Client::EndpointsLocked(std::vector<Endpoint>& out, std::string& reason, bool lookUpNow) {
    const auto matches = [this] {
        std::lock_guard lock(configMutex_);
        return lookup_.done && lookup_.host == settings_.serverAddress && lookup_.port == settings_.serverPort;
    };
    if (!matches() && lookUpNow) Resolve();
    std::lock_guard lock(configMutex_);
    if (!(lookup_.done && lookup_.host == settings_.serverAddress && lookup_.port == settings_.serverPort)) {
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

std::vector<Client::Listed> Client::Newest(const std::vector<proto::ModelEntry>& models) {
    std::vector<Listed> listed;
    for (const auto& m : models) {
        const auto same = std::find_if(listed.begin(), listed.end(), [&](const Listed& l) { return l.entry.name == m.name; });
        if (same != listed.end()) listed.erase(same);  // the newer session of that name replaces the older
        Listed l;
        l.entry = m;
        listed.push_back(std::move(l));
    }
    return listed;
}

void Client::SetModels(const std::vector<proto::ModelEntry>& models) {
    Lock lock(mutex_);
    std::vector<Listed> listed = Newest(models);
    bool same = listed.size() == listed_.size();
    for (std::size_t i = 0; same && i < listed.size(); ++i) same = listed[i].entry == listed_[i].entry;
    if (same) return;
    // keep what the server said of models still listed, until the next HELLO
    for (auto& l : listed)
        for (const auto& old : listed_)
            if (old.entry == l.entry) {
                l.status = old.status;
                l.detail = old.detail;
            }
    listed_ = std::move(listed);
    helloCurrent_ = false;
}

bool Client::BackingOff() const {
    Lock lock(mutex_);
    return now_() < backoffUntil_;
}

int Client::HellosSent() const {
    Lock lock(mutex_);
    return hellosSent_;
}

int Client::Reconnects() const {
    Lock lock(mutex_);
    return connects_;
}

void Client::CloseLocked() noexcept {
    if (transport_) {
        transport_->Close();
        transport_.reset();
    }
    helloCurrent_ = false;
}

void Client::FailLocked(const std::string& reason, bool keepConnection) {
    if (!keepConnection) CloseLocked();
    helloCurrent_ = false;
    backoffUntil_ = now_() + kBackoff;
    if (log_) log_(Level::Warn, reason + "; lines get the game's own lip sync for the next 10 seconds");
}

void Client::Fail(const std::string& reason) {
    Lock lock(mutex_);
    FailLocked(reason, false);
}

bool Client::EnsureConnected(Clock::time_point deadline, std::string& reason) {
    if (transport_) return true;
    std::vector<Endpoint> endpoints;
    if (!EndpointsLocked(endpoints, reason, false)) return false;
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(endpoints, std::min(settings_.timeoutMs, MillisLeft(deadline)), err)) {
        reason = "couldn't connect to the server (" + err + ")";
        return false;
    }
    transport_ = std::move(t);
    helloCurrent_ = false;
    ++connects_;
    if (log_) log_(Level::Info, "Connected to the server on port " + std::to_string(settings_.serverPort));
    return true;
}

// One request and its answer by the deadline. False on a transport failure or an answer that isn't a frame of
// version 1 (the connection is then unusable); failure says when it failed.
bool Client::Exchange(MsgType type, const std::vector<std::uint8_t>& payload, Clock::time_point deadline, proto::Header& header,
                      std::vector<std::uint8_t>& answer, std::string& reason, Failure* failure) {
    const auto fail = [&](Failure f, std::string why) {
        if (failure) *failure = f;
        reason = std::move(why);
        return false;
    };
    std::vector<std::uint8_t> frame;
    std::string err;
    if (!proto::EncodeFrame(type, payload, frame, err)) return fail(Failure::Late, "couldn't encode the request (" + err + ")");
    Transport::Result r = transport_->SendAll(frame.data(), frame.size(), deadline, err);
    if (r != Transport::Result::Ok)
        return fail(r == Transport::Result::TimedOut ? Failure::TimedOut : Failure::Early, "couldn't send to the server (" + err + ")");
    std::uint8_t head[proto::kHeaderSize];
    // the first byte apart: a connection closed before it is one the server had closed already
    r = transport_->ReceiveAll(head, 1, deadline, err);
    if (r != Transport::Result::Ok)
        return fail(r == Transport::Result::TimedOut ? Failure::TimedOut : Failure::Early, "the server didn't answer (" + err + ")");
    if (transport_->ReceiveAll(head + 1, sizeof head - 1, deadline, err) != Transport::Result::Ok)
        return fail(Failure::Late, "the server's answer was cut short (" + err + ")");
    if (!proto::DecodeHeader(head, header, err)) return fail(Failure::Late, "the server's answer is malformed (" + err + ")");
    answer.resize(header.length);
    if (header.length > 0 && transport_->ReceiveAll(answer.data(), answer.size(), deadline, err) != Transport::Result::Ok)
        return fail(Failure::Late, "the server's answer was cut short (" + err + ")");
    if (failure) *failure = Failure::None;
    return true;
}

bool Client::Hello(Clock::time_point deadline, ProbeResult& result, std::string& reason, Failure* failure) {
    proto::Hello hello;
    hello.client = ClientName();
    hello.key = settings_.accessKey;
    {
        std::lock_guard lock(configMutex_);
        hello.ortVersion = ortVersion_;
    }
    for (const auto& l : listed_) hello.models.push_back(l.entry);
    std::vector<std::uint8_t> payload, answer;
    std::string err;
    if (failure) *failure = Failure::Late;
    if (!proto::EncodeHello(hello, payload, err)) {
        result.kind = ProbeResult::Kind::Refused;
        reason = "couldn't encode HELLO (" + err + ")";
        return false;
    }
    proto::Header header;
    ++hellosSent_;
    if (!Exchange(MsgType::Hello, payload, deadline, header, answer, reason, failure)) {
        result.kind = ProbeResult::Kind::NoAnswer;
        return false;
    }
    if (header.type == static_cast<std::uint16_t>(MsgType::Error)) {
        proto::Error e;
        if (!proto::DecodeError(answer.data(), answer.size(), e, err)) {
            result.kind = ProbeResult::Kind::Refused;
            reason = "the server's answer to HELLO is malformed (" + err + ")";
            return false;
        }
        result.kind = e.code == static_cast<std::uint16_t>(proto::ErrorCode::WrongKey) ? ProbeResult::Kind::WrongKey : ProbeResult::Kind::Refused;
        reason = "the server refused HELLO: " + ErrorText(e);
        return false;
    }
    proto::HelloOk ok;
    result.kind = ProbeResult::Kind::Refused;
    if (header.type != static_cast<std::uint16_t>(MsgType::HelloOk) || !proto::DecodeHelloOk(answer.data(), answer.size(), ok, err)) {
        reason = "the server's answer to HELLO is malformed" + (err.empty() ? std::string() : " (" + err + ")");
        return false;
    }
    if (ok.models.size() != listed_.size()) {
        reason = "the server's answer to HELLO lists " + std::to_string(ok.models.size()) + " models, not " + std::to_string(listed_.size());
        return false;
    }
    std::vector<std::string> parts;
    for (std::size_t i = 0; i < listed_.size(); ++i) {
        if (ok.models[i].name != listed_[i].entry.name) {
            reason = "the server's answer to HELLO names its models out of order";
            return false;
        }
    }
    for (std::size_t i = 0; i < listed_.size(); ++i) {
        listed_[i].status = ok.models[i].status;
        listed_[i].detail = ok.models[i].detail;
        // a ready model may carry a detail too (another model running in its place): it is always logged
        std::string part = listed_[i].entry.name + " " + proto::ModelStatusName(ok.models[i].status);
        if (!ok.models[i].detail.empty()) part += " (" + ok.models[i].detail + ")";
        parts.push_back(std::move(part));
    }
    result.kind = ProbeResult::Kind::Answered;
    result.server = ok.server;
    result.device = ok.device;
    helloCurrent_ = true;
    const std::string line = "Server " + ok.server + " on " + ok.device + ": " + (parts.empty() ? std::string("no models asked") : Join(parts));
    if (line != lastStatusLine_ && log_) log_(Level::Info, line);
    lastStatusLine_ = line;
    return true;
}

Admission Client::Admit(const std::vector<proto::ModelEntry>& live, const proto::ModelEntry& model) {
    Lock lock(mutex_);
    ApplyPendingLocked();
    Admission a;
    std::vector<proto::ModelEntry> all = live;
    all.push_back(model);
    const auto restore = [&] {
        listed_ = Newest(live);
        helloCurrent_ = false;  // the server's list may still name the refused model: the next run says HELLO again
    };
    if (settings_.serverAddress.empty()) {
        a.hello.kind = ProbeResult::Kind::NoAddress;
        a.detail = "no server address is set";
        return a;
    }
    std::string reason;
    // a fresh connection for a model load: a backoff doesn't stop it, and an old connection may be stale
    CloseLocked();
    listed_ = Newest(all);
    const Clock::time_point deadline = Clock::now() + kHelloTimeout;
    std::vector<Endpoint> endpoints;
    if (!EndpointsLocked(endpoints, reason, true) || !EnsureConnected(deadline, reason)) {
        a.hello.kind = ProbeResult::Kind::NoAnswer;
        a.detail = reason;
        restore();
        return a;
    }
    if (!Hello(deadline, a.hello, reason)) {
        a.detail = reason;
        CloseLocked();
        restore();
        return a;
    }
    for (const auto& l : listed_)
        if (l.entry.name == model.name) {
            a.status = l.status;
            a.detail = l.detail;
        }
    backoffUntil_ = {};
    if (!a.Ready()) restore();
    return a;
}

Client::Outcome Client::Run(const proto::ModelEntry& model, proto::RunRequest& request, Reply& reply, std::string& reason) {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    // another run (or a model load) holding the connection past this call's timeout: this line goes without
    if (!lock.try_lock_for(std::chrono::milliseconds(timeoutMs_.load()))) return Outcome::Skipped;
    ApplyPendingLocked();
    if (now_() < backoffUntil_) return Outcome::Skipped;
    const auto listedNow = [&]() -> const Listed* {
        for (const auto& l : listed_)
            if (l.entry.name == model.name) return &l;
        return nullptr;
    };
    const Listed* listed = listedNow();
    if (!listed || !(listed->entry == model)) return Outcome::Skipped;  // a session a newer one of its model replaced
    {
        std::vector<Endpoint> unused;
        if (!transport_ && !EndpointsLocked(unused, reason, false)) {
            if (!lookupFailureLogged_ && reason.find("couldn't be looked up") != std::string::npos) {
                lookupFailureLogged_ = true;
                if (log_) log_(Level::Warn, reason + "; lines get the game's own lip sync until the settings change");
            }
            return Outcome::Skipped;
        }
    }

    request.requestId = nextRequest_++;
    if (nextRequest_ == 0) nextRequest_ = 1;
    std::vector<std::uint8_t> payload;
    std::string err;
    if (!proto::EncodeRun(request, payload, err)) {
        reason = "couldn't encode RUN for " + model.name + " (" + err + ")";
        FailLocked(reason, true);
        return Outcome::Failed;
    }

    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline = start + std::chrono::milliseconds(settings_.timeoutMs);
    proto::Header header;
    bool reused = transport_ != nullptr;
    for (int attempt = 0;; ++attempt) {
        Failure failure = Failure::Late;
        bool ok = EnsureConnected(deadline, reason);
        if (ok && !helloCurrent_) {
            ProbeResult r;
            ok = Hello(deadline, r, reason, &failure);
            const Listed* again = ok ? listedNow() : nullptr;
            if (ok && (!again || again->status != static_cast<std::uint8_t>(proto::ModelStatus::Ready))) {
                reason = "the server no longer has " + model.name + " ready (" +
                         (again ? std::string(proto::ModelStatusName(again->status)) + (again->detail.empty() ? "" : ": " + again->detail)
                                : std::string("not listed")) +
                         ")";
                FailLocked(reason, true);
                return Outcome::Failed;
            }
        }
        if (ok) ok = Exchange(MsgType::Run, payload, deadline, header, reply.payload, reason, &failure);
        if (ok) break;
        // the server had closed this (idle) connection: open it again and send again, once, within the same deadline
        if (attempt == 0 && reused && failure == Failure::Early && Clock::now() < deadline) {
            CloseLocked();
            reused = false;
            if (log_) log_(Level::Info, "The server had closed the connection; reconnecting");
            continue;
        }
        FailLocked(reason, false);
        return Outcome::Failed;
    }
    reply.roundTrip = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start);
    reply.bytesUp = payload.size() + proto::kHeaderSize;
    if (header.type == static_cast<std::uint16_t>(MsgType::Error)) {
        proto::Error e;
        if (!proto::DecodeError(reply.payload.data(), reply.payload.size(), e, err)) {
            reason = "the server's answer to RUN is malformed (" + err + ")";
            FailLocked(reason, false);
            return Outcome::Failed;
        }
        reason = "the server couldn't run " + model.name + ": " + ErrorText(e);
        FailLocked(reason, !proto::ErrorClosesConnection(e.code));
        return Outcome::Failed;
    }
    if (header.type != static_cast<std::uint16_t>(MsgType::RunOk) ||
        !proto::DecodeRunOk(reply.payload.data(), reply.payload.size(), reply.ok, err)) {
        reason = "the server's answer to RUN is malformed" + (err.empty() ? std::string() : " (" + err + ")");
        FailLocked(reason, false);
        return Outcome::Failed;
    }
    if (reply.ok.requestId != request.requestId) {
        reason = "the server answered request " + std::to_string(reply.ok.requestId) + ", not " + std::to_string(request.requestId);
        FailLocked(reason, false);
        return Outcome::Failed;
    }
    reason.clear();  // a first attempt's failure, retried, isn't this call's
    return Outcome::Forwarded;
}

ProbeResult Client::Probe() {
    Settings s;
    std::string ortVersion;
    std::vector<Endpoint> endpoints;
    bool looked = false;
    std::string lookupError;
    {
        std::lock_guard lock(configMutex_);
        s = pending_;
        ortVersion = ortVersion_;
        looked = lookup_.done && lookup_.ok && lookup_.host == s.serverAddress && lookup_.port == s.serverPort;
        if (looked) endpoints = lookup_.endpoints;
    }
    ProbeResult r;
    if (s.serverAddress.empty()) {
        r.kind = ProbeResult::Kind::NoAddress;
        return r;
    }
    if (!looked) {
        Resolve();
        std::lock_guard lock(configMutex_);
        looked = lookup_.done && lookup_.ok && lookup_.host == s.serverAddress && lookup_.port == s.serverPort;
        if (looked) endpoints = lookup_.endpoints;
        lookupError = lookup_.error;
    }
    if (!looked) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = "the address couldn't be looked up (" + lookupError + ")";
        return r;
    }
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(endpoints, s.timeoutMs, err)) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = "couldn't connect (" + err + ")";
        return r;
    }
    proto::Hello hello;
    hello.client = ClientName();
    hello.key = s.accessKey;
    hello.ortVersion = ortVersion;
    std::vector<std::uint8_t> payload, frame;
    proto::EncodeHello(hello, payload, err);
    proto::EncodeFrame(MsgType::Hello, payload, frame, err);
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(s.timeoutMs);
    std::uint8_t head[proto::kHeaderSize];
    proto::Header header;
    if (t->SendAll(frame.data(), frame.size(), deadline, err) != Transport::Result::Ok ||
        t->ReceiveAll(head, sizeof head, deadline, err) != Transport::Result::Ok) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = "no answer (" + err + ")";
    } else if (!proto::DecodeHeader(head, header, err)) {
        r.kind = ProbeResult::Kind::Refused;
        r.detail = "the answer isn't protocol version 1 (" + err + ")";
    } else {
        std::vector<std::uint8_t> answer(header.length);
        if (header.length > 0 && t->ReceiveAll(answer.data(), answer.size(), deadline, err) != Transport::Result::Ok) {
            r.kind = ProbeResult::Kind::NoAnswer;
            r.detail = "the answer was cut short (" + err + ")";
        } else if (header.type == static_cast<std::uint16_t>(MsgType::HelloOk)) {
            proto::HelloOk ok;
            if (proto::DecodeHelloOk(answer.data(), answer.size(), ok, err) && ok.models.empty()) {
                r.kind = ProbeResult::Kind::Answered;
                r.server = ok.server;
                r.device = ok.device;
            } else {
                r.kind = ProbeResult::Kind::Refused;
                r.detail = "malformed HELLO_OK" + (err.empty() ? std::string() : " (" + err + ")");
            }
        } else if (header.type == static_cast<std::uint16_t>(MsgType::Error)) {
            proto::Error e;
            if (proto::DecodeError(answer.data(), answer.size(), e, err)) {
                r.kind = e.code == static_cast<std::uint16_t>(proto::ErrorCode::WrongKey) ? ProbeResult::Kind::WrongKey : ProbeResult::Kind::Refused;
                r.detail = ErrorText(e);
            } else {
                r.kind = ProbeResult::Kind::Refused;
                r.detail = "malformed ERROR (" + err + ")";
            }
        } else {
            r.kind = ProbeResult::Kind::Refused;
            r.detail = "unexpected answer type " + std::to_string(header.type);
        }
    }
    t->Close();
    return r;
}

}  // namespace bfarb
