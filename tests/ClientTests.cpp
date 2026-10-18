// The connection logic against a scripted server: HELLO at a model load and its answers, RUN, the failures and the
// 10-second backoff, HELLO again when the live models change, and the probe.

#include "Scripted.h"
#include "Test.h"
#include "core/Client.h"

using namespace bfarb;
using namespace bfarb::test;

namespace {

Clock::time_point g_now = Clock::time_point{} + std::chrono::hours(1);
Clock::time_point FakeNow() { return g_now; }

proto::ModelEntry Model(const char* name) {
    proto::ModelEntry m;
    m.name = name;
    m.size = 10;
    m.sha256[0] = 1;
    m.overrides = {{"batch", 1}};
    return m;
}

Settings On() {
    Settings s;
    s.serverAddress = "192.0.2.1";
    s.timeoutMs = 300;
    return s;
}

std::vector<std::uint8_t> HelloOkFor(const std::vector<std::uint8_t>& helloPayload, std::uint8_t status, const char* detail = "") {
    proto::Hello h;
    std::string err;
    proto::DecodeHello(helloPayload.data(), helloPayload.size(), h, err);
    proto::HelloOk ok;
    ok.server = "scripted";
    ok.device = "CPU";
    for (const auto& m : h.models) ok.models.push_back({m.name, status, status ? detail : ""});
    std::vector<std::uint8_t> p;
    proto::EncodeHelloOk(ok, p, err);
    return Frame(proto::MsgType::HelloOk, p);
}

std::vector<std::uint8_t> ErrorFrame(std::uint32_t id, proto::ErrorCode code) {
    std::vector<std::uint8_t> p;
    std::string err;
    proto::EncodeError({id, static_cast<std::uint16_t>(code), "scripted"}, p, err);
    return Frame(proto::MsgType::Error, p);
}

// A RUN answered with one float output "y" of two elements.
std::vector<std::uint8_t> RunOkFor(const std::vector<std::uint8_t>& runPayload, int idDelta = 0) {
    proto::DecodedRun r;
    std::string err;
    proto::DecodeRun(runPayload.data(), runPayload.size(), r, err);
    static const float y[2] = {1.5f, -2.5f};
    proto::RunOk ok;
    ok.requestId = r.requestId + static_cast<std::uint32_t>(idDelta);
    ok.serverMicros = 7;
    ok.outputs = {{"y", 1, {2}, y, sizeof y}};
    std::vector<std::uint8_t> p;
    proto::EncodeRunOk(ok, p, err);
    return Frame(proto::MsgType::RunOk, p);
}

proto::RunRequest Request(const proto::ModelEntry& m) {
    static const float x[1] = {3};
    proto::RunRequest r;
    r.model = m.name;
    r.inputs = {{"x", 1, {1}, x, sizeof x}};
    r.outputs = {"y"};
    return r;
}

int Count(const std::vector<std::uint16_t>& v, proto::MsgType t) {
    int n = 0;
    for (auto x : v) n += x == static_cast<std::uint16_t>(t);
    return n;
}

}  // namespace

TEST(ClientAdmitsAndRuns) {
    auto script = std::make_shared<Script>();
    script->answer = [](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        return type == 1 ? HelloOkFor(p, 0) : RunOkFor(p);
    };
    std::vector<std::string> log;
    Client c(ScriptedFactory(script), [&](Level, const std::string& l) { log.push_back(l); }, &FakeNow);
    c.Configure(On());
    const auto m = Model("a.onnx");
    const Admission a = c.Admit({}, m);
    CHECK(a.Ready());
    CHECK_EQ(a.hello.server, std::string("scripted"));
    c.SetModels({m});  // as the bridge does once the session is made: no change, no new HELLO
    auto req = Request(m);
    Client::Reply reply;
    std::string reason;
    CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Forwarded);
    CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Forwarded);
    CHECK_EQ(reply.ok.outputs.size(), std::size_t{1});
    CHECK_EQ(reply.ok.serverMicros, 7u);
    CHECK_EQ(Count(script->received, proto::MsgType::Hello), 1);
    CHECK_EQ(Count(script->received, proto::MsgType::Run), 2);
    CHECK_EQ(script->connects, 1);
}

TEST(ClientAdmitRefusals) {
    const auto m = Model("a.onnx");
    // no server
    {
        auto script = std::make_shared<Script>();
        script->refuseConnect = true;
        Client c(ScriptedFactory(script), nullptr, &FakeNow);
        c.Configure(On());
        const Admission a = c.Admit({}, m);
        CHECK(!a.Ready());
        CHECK(a.hello.kind == ProbeResult::Kind::NoAnswer);
    }
    // no answer in time
    {
        auto script = std::make_shared<Script>();
        Client c(ScriptedFactory(script), nullptr, &FakeNow);
        c.Configure(On());
        CHECK(c.Admit({}, m).hello.kind == ProbeResult::Kind::NoAnswer);
    }
    // the wrong key
    {
        auto script = std::make_shared<Script>();
        script->answer = [](std::uint16_t, const std::vector<std::uint8_t>&) { return ErrorFrame(0, proto::ErrorCode::WrongKey); };
        Client c(ScriptedFactory(script), nullptr, &FakeNow);
        c.Configure(On());
        CHECK(c.Admit({}, m).hello.kind == ProbeResult::Kind::WrongKey);
    }
    // each model status but ready
    for (std::uint8_t status : {1, 2, 3, 4}) {
        auto script = std::make_shared<Script>();
        script->answer = [status](std::uint16_t, const std::vector<std::uint8_t>& p) { return HelloOkFor(p, status, "why"); };
        Client c(ScriptedFactory(script), nullptr, &FakeNow);
        c.Configure(On());
        const Admission a = c.Admit({}, m);
        CHECK(!a.Ready());
        CHECK(a.hello.kind == ProbeResult::Kind::Answered);
        CHECK_EQ(int(a.status), int(status));
        CHECK_EQ(a.detail, std::string("why"));
    }
    // a malformed answer
    {
        auto script = std::make_shared<Script>();
        script->answer = [](std::uint16_t, const std::vector<std::uint8_t>&) { return Frame(proto::MsgType::HelloOk, {1, 2, 3}); };
        Client c(ScriptedFactory(script), nullptr, &FakeNow);
        c.Configure(On());
        CHECK(c.Admit({}, m).hello.kind == ProbeResult::Kind::Refused);
    }
    // no address
    {
        auto script = std::make_shared<Script>();
        Client c(ScriptedFactory(script), nullptr, &FakeNow);
        CHECK(c.Admit({}, m).hello.kind == ProbeResult::Kind::NoAddress);
        CHECK_EQ(script->connects, 0);
    }
}

TEST(ClientFailureStartsTheBackoff) {
    auto script = std::make_shared<Script>();
    int runs = 0;
    script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) return HelloOkFor(p, 0);
        ++runs;
        if (runs == 2) return Frame(proto::MsgType::RunOk, {9, 9});  // malformed
        if (runs == 3) return RunOkFor(p, 1);                         // the wrong request id
        if (runs == 4) return std::vector<std::uint8_t>{};            // a timeout
        if (runs == 5) return ErrorFrame(0, proto::ErrorCode::InferenceFailed);
        return RunOkFor(p);
    };
    std::vector<std::string> log;
    Client c(ScriptedFactory(script), [&](Level, const std::string& l) { log.push_back(l); }, &FakeNow);
    c.Configure(On());
    const auto m = Model("a.onnx");
    CHECK(c.Admit({}, m).Ready());
    auto req = Request(m);
    Client::Reply reply;
    std::string reason;
    CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Forwarded);
    for (int failure = 0; failure < 4; ++failure) {
        log.clear();
        CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Failed);
        CHECK(c.BackingOff());
        const auto backoffLines = [&] {
            int n = 0;
            for (const auto& l : log) n += l.find("for the next 10 seconds") != std::string::npos;
            return n;
        };
        CHECK_EQ(backoffLines(), 1);  // one line per backoff
        const auto before = script->received.size();
        for (int i = 0; i < 5; ++i) CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Skipped);
        CHECK_EQ(script->received.size(), before);  // no network during the backoff
        CHECK_EQ(backoffLines(), 1);
        g_now += std::chrono::seconds(9);
        CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Skipped);
        g_now += std::chrono::seconds(1);
        CHECK(!c.BackingOff());
    }
    CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Forwarded);
    // each failure closed the connection (the server's code 4 keeps it open) and said HELLO again after it
    CHECK_EQ(script->connects, 4);
    CHECK_EQ(Count(script->received, proto::MsgType::Hello), 5);
}

TEST(ClientHelloAgainWhenModelsChange) {
    auto script = std::make_shared<Script>();
    std::vector<std::size_t> listed;
    script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) {
            proto::Hello h;
            std::string err;
            proto::DecodeHello(p.data(), p.size(), h, err);
            listed.push_back(h.models.size());
            return HelloOkFor(p, 0);
        }
        return RunOkFor(p);
    };
    Client c(ScriptedFactory(script), nullptr, &FakeNow);
    c.Configure(On());
    const auto a = Model("a.onnx"), b = Model("b.onnx");
    CHECK(c.Admit({}, a).Ready());
    c.SetModels({a});
    CHECK(c.Admit({a}, b).Ready());  // the second model: HELLO lists both
    c.SetModels({a, b});
    auto req = Request(a);
    Client::Reply reply;
    std::string reason;
    CHECK(c.Run(a, req, reply, reason) == Client::Outcome::Forwarded);
    c.SetModels({a});  // b released: the next run says HELLO with a alone
    CHECK(c.Run(a, req, reply, reason) == Client::Outcome::Forwarded);
    CHECK(c.Run(a, req, reply, reason) == Client::Outcome::Forwarded);
    CHECK(listed == (std::vector<std::size_t>{1, 2, 1}));
    // b is no longer listed: a run of it fails
    auto reqB = Request(b);
    CHECK(c.Run(b, reqB, reply, reason) == Client::Outcome::Failed);
}

TEST(ClientModelNoLongerReady) {
    auto script = std::make_shared<Script>();
    int hellos = 0;
    script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) return HelloOkFor(p, ++hellos == 1 ? 0 : 3, "gone");
        return ErrorFrame(0, proto::ErrorCode::UnknownModel);
    };
    Client c(ScriptedFactory(script), nullptr, &FakeNow);
    c.Configure(On());
    const auto m = Model("a.onnx");
    CHECK(c.Admit({}, m).Ready());
    c.SetModels({m});
    auto req = Request(m);
    Client::Reply reply;
    std::string reason;
    CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Failed);  // ERROR 3
    g_now += std::chrono::seconds(10);
    CHECK(c.Run(m, req, reply, reason) == Client::Outcome::Failed);  // HELLO again: not ready
    CHECK(reason.find("no longer") != std::string::npos);
}

TEST(ClientProbe) {
    auto script = std::make_shared<Script>();
    script->answer = [](std::uint16_t, const std::vector<std::uint8_t>& p) { return HelloOkFor(p, 0); };
    Client c(ScriptedFactory(script), nullptr, &FakeNow);
    CHECK(c.Probe().kind == ProbeResult::Kind::NoAddress);
    c.Configure(On());
    const ProbeResult r = c.Probe();
    CHECK(r.kind == ProbeResult::Kind::Answered);
    CHECK_EQ(r.device, std::string("CPU"));
    script->answer = [](std::uint16_t, const std::vector<std::uint8_t>&) { return ErrorFrame(0, proto::ErrorCode::WrongKey); };
    CHECK(c.Probe().kind == ProbeResult::Kind::WrongKey);
    script->answer = nullptr;
    CHECK(c.Probe().kind == ProbeResult::Kind::NoAnswer);
    auto bad = Frame(proto::MsgType::HelloOk, {});
    bad[4] = 2;  // version 2
    script->answer = [&](std::uint16_t, const std::vector<std::uint8_t>&) { return bad; };
    CHECK(c.Probe().kind == ProbeResult::Kind::Refused);
}
