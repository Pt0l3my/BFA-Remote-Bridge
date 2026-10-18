#include "core/Client.h"

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

}  // namespace

std::string ClientName() { return std::string("BFARemoteBridge ") + kVersionText; }

Client::Client(TransportFactory factory, LogSink log, Now now) : factory_(std::move(factory)), log_(std::move(log)), now_(now) {}

Client::~Client() {
    Lock lock(mutex_);
    CloseLocked();
}

void Client::Configure(const Settings& settings) {
    Lock lock(mutex_);
    const bool changed = settings.serverAddress != settings_.serverAddress || settings.serverPort != settings_.serverPort ||
                         settings.accessKey != settings_.accessKey || settings.timeoutMs != settings_.timeoutMs;
    settings_ = settings;
    timeoutMs_.store(settings.timeoutMs);
    if (changed) {
        CloseLocked();
        backoffUntil_ = {};
        lastStatusLine_.clear();
    }
}

void Client::SetOrtVersion(const std::string& version) {
    Lock lock(mutex_);
    ortVersion_ = version;
}

std::vector<Client::Listed> Client::Dedupe(const std::vector<proto::ModelEntry>& models) {
    std::vector<Listed> listed;
    for (const auto& m : models) {
        bool seen = false;
        for (const auto& l : listed) seen = seen || l.entry.name == m.name;
        if (seen) continue;
        Listed l;
        l.entry = m;
        listed.push_back(std::move(l));
    }
    return listed;
}

void Client::SetModels(const std::vector<proto::ModelEntry>& models) {
    Lock lock(mutex_);
    std::vector<Listed> listed = Dedupe(models);
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

bool Client::EnsureConnected(std::string& reason) {
    if (transport_) return true;
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(settings_.serverAddress, static_cast<std::uint16_t>(settings_.serverPort), settings_.timeoutMs, err)) {
        reason = "couldn't connect to the server (" + err + ")";
        return false;
    }
    transport_ = std::move(t);
    helloCurrent_ = false;
    if (log_) log_(Level::Info, "Connected to the server on port " + std::to_string(settings_.serverPort));
    return true;
}

bool Client::Exchange(MsgType type, const std::vector<std::uint8_t>& payload, std::chrono::milliseconds wait, proto::Header& header,
                      std::vector<std::uint8_t>& answer, std::string& reason) {
    std::vector<std::uint8_t> frame;
    std::string err;
    if (!proto::EncodeFrame(type, payload, frame, err)) {
        reason = "couldn't encode the request (" + err + ")";
        return false;
    }
    const Clock::time_point deadline = Clock::now() + wait;
    if (!transport_->SendAll(frame.data(), frame.size(), deadline, err)) {
        reason = "couldn't send to the server (" + err + ")";
        return false;
    }
    std::uint8_t head[proto::kHeaderSize];
    if (!transport_->ReceiveAll(head, sizeof head, deadline, err)) {
        reason = "the server didn't answer (" + err + ")";
        return false;
    }
    if (!proto::DecodeHeader(head, header, err)) {
        reason = "the server's answer is malformed (" + err + ")";
        return false;
    }
    answer.resize(header.length);
    if (header.length > 0 && !transport_->ReceiveAll(answer.data(), answer.size(), deadline, err)) {
        reason = "the server's answer was cut short (" + err + ")";
        return false;
    }
    return true;
}

bool Client::Hello(std::chrono::milliseconds wait, ProbeResult& result, std::string& reason) {
    proto::Hello hello;
    hello.client = ClientName();
    hello.key = settings_.accessKey;
    hello.ortVersion = ortVersion_;
    for (const auto& l : listed_) hello.models.push_back(l.entry);
    std::vector<std::uint8_t> payload, answer;
    std::string err;
    if (!proto::EncodeHello(hello, payload, err)) {
        result.kind = ProbeResult::Kind::Refused;
        reason = "couldn't encode HELLO (" + err + ")";
        return false;
    }
    proto::Header header;
    ++hellosSent_;
    if (!Exchange(MsgType::Hello, payload, wait, header, answer, reason)) {
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
        listed_[i].status = ok.models[i].status;
        listed_[i].detail = ok.models[i].detail;
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
    Admission a;
    std::vector<proto::ModelEntry> all = live;
    all.push_back(model);
    listed_ = Dedupe(all);
    helloCurrent_ = false;
    const Listed* asked = nullptr;
    for (const auto& l : listed_)
        if (l.entry.name == model.name) asked = &l;
    if (!(asked->entry == model)) {
        a.hello.kind = ProbeResult::Kind::Refused;
        a.detail = "another session of " + model.name + " with other overrides or another file is live";
        listed_ = Dedupe(live);
        return a;
    }
    if (settings_.serverAddress.empty()) {
        a.hello.kind = ProbeResult::Kind::NoAddress;
        a.detail = "no server address is set";
        listed_ = Dedupe(live);
        return a;
    }
    std::string reason;
    // a fresh connection for a model load: a backoff doesn't stop it, and an old connection may be stale
    CloseLocked();
    if (!EnsureConnected(reason)) {
        a.hello.kind = ProbeResult::Kind::NoAnswer;
        a.detail = reason;
        listed_ = Dedupe(live);
        return a;
    }
    if (!Hello(std::chrono::duration_cast<std::chrono::milliseconds>(kHelloTimeout), a.hello, reason)) {
        a.detail = reason;
        CloseLocked();
        listed_ = Dedupe(live);
        return a;
    }
    for (const auto& l : listed_)
        if (l.entry.name == model.name) {
            a.status = l.status;
            a.detail = l.detail;
        }
    backoffUntil_ = {};
    if (!a.Ready()) {
        listed_ = Dedupe(live);
        helloCurrent_ = false;  // the server's list still names the refused model: the next run says HELLO again
    }
    return a;
}

Client::Outcome Client::Run(const proto::ModelEntry& model, proto::RunRequest& request, Reply& reply, std::string& reason) {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    // another run (or a model load) holding the connection past this call's timeout: this line goes without
    if (!lock.try_lock_for(std::chrono::milliseconds(timeoutMs_.load()))) return Outcome::Skipped;
    if (now_() < backoffUntil_) return Outcome::Skipped;
    const Listed* listed = nullptr;
    for (const auto& l : listed_)
        if (l.entry.name == model.name) listed = &l;
    if (!listed || !(listed->entry == model)) {
        reason = model.name + " isn't among the server's models";
        FailLocked(reason, true);
        return Outcome::Failed;
    }
    const auto wait = std::chrono::milliseconds(settings_.timeoutMs);
    if (!EnsureConnected(reason)) {
        FailLocked(reason, false);
        return Outcome::Failed;
    }
    if (!helloCurrent_) {
        ProbeResult r;
        if (!Hello(wait, r, reason)) {
            FailLocked(reason, false);
            return Outcome::Failed;
        }
    }
    for (const auto& l : listed_)
        if (l.entry.name == model.name) listed = &l;
    if (listed->status != static_cast<std::uint8_t>(proto::ModelStatus::Ready)) {
        reason = "the server no longer has " + model.name + " ready (" + proto::ModelStatusName(listed->status) +
                 (listed->detail.empty() ? "" : ": " + listed->detail) + ")";
        FailLocked(reason, true);
        return Outcome::Failed;
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
    proto::Header header;
    if (!Exchange(MsgType::Run, payload, wait, header, reply.payload, reason)) {
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
    return Outcome::Forwarded;
}

ProbeResult Client::Probe() {
    Settings s;
    std::string ortVersion;
    {
        Lock lock(mutex_);
        s = settings_;
        ortVersion = ortVersion_;
    }
    ProbeResult r;
    if (s.serverAddress.empty()) {
        r.kind = ProbeResult::Kind::NoAddress;
        return r;
    }
    auto t = factory_ ? factory_() : nullptr;
    std::string err = "no network";
    if (!t || !t->Connect(s.serverAddress, static_cast<std::uint16_t>(s.serverPort), s.timeoutMs, err)) {
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
    if (!t->SendAll(frame.data(), frame.size(), deadline, err) || !t->ReceiveAll(head, sizeof head, deadline, err)) {
        r.kind = ProbeResult::Kind::NoAnswer;
        r.detail = "no answer (" + err + ")";
    } else if (!proto::DecodeHeader(head, header, err)) {
        r.kind = ProbeResult::Kind::Refused;
        r.detail = "the answer isn't protocol version 1 (" + err + ")";
    } else {
        std::vector<std::uint8_t> answer(header.length);
        if (header.length > 0 && !t->ReceiveAll(answer.data(), answer.size(), deadline, err)) {
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
