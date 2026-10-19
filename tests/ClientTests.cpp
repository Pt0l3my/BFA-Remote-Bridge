// The connection logic against a scripted server: the side connection at a model load and the main connection's
// HELLO after it, RUN, the late answers, the pause and the background thread's work (the backoff, the PING, the held
// HELLO, the status checks), the counters and the summary line, and the probe.

#include <climits>
#include <functional>
#include <thread>

#include "Scripted.h"
#include "Test.h"
#include "core/Client.h"
#include "core/Version.h"

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
    s.logDetail = LogDetail::Detailed;
    return s;
}

proto::Hello DecodedHello(const std::vector<std::uint8_t>& payload) {
    proto::Hello h;
    std::string err;
    proto::DecodeHello(payload.data(), payload.size(), h, err);
    return h;
}

std::vector<std::uint8_t> HelloOkFor(const std::vector<std::uint8_t>& helloPayload, std::uint8_t status, const char* detail = "") {
    const proto::Hello h = DecodedHello(helloPayload);
    proto::HelloOk ok;
    ok.server = "scripted";
    ok.device = "CPU";
    for (const auto& m : h.models) ok.models.push_back({m.name, status, status ? detail : ""});
    std::vector<std::uint8_t> p;
    std::string err;
    proto::EncodeHelloOk(ok, p, err);
    return Frame(proto::MsgType::HelloOk, p);
}

std::vector<std::uint8_t> ErrorFrame(std::uint32_t id, std::uint16_t code) {
    std::vector<std::uint8_t> p;
    std::string err;
    proto::EncodeError({id, code, "scripted"}, p, err);
    return Frame(proto::MsgType::Error, p);
}
std::vector<std::uint8_t> ErrorFrame(std::uint32_t id, proto::ErrorCode code) { return ErrorFrame(id, static_cast<std::uint16_t>(code)); }

std::uint32_t RunId(const std::vector<std::uint8_t>& runPayload) {
    proto::DecodedRun r;
    std::string err;
    proto::DecodeRun(runPayload.data(), runPayload.size(), r, err);
    return r.requestId;
}

// A RUN answered with one float output "y" of two elements.
std::vector<std::uint8_t> RunOkWithId(std::uint32_t id) {
    static const float y[2] = {1.5f, -2.5f};
    proto::RunOk ok;
    ok.requestId = id;
    ok.serverMicros = 7;
    ok.outputs = {{"y", 1, {2}, y, sizeof y}};
    std::vector<std::uint8_t> p;
    std::string err;
    proto::EncodeRunOk(ok, p, err);
    return Frame(proto::MsgType::RunOk, p);
}
std::vector<std::uint8_t> RunOkFor(const std::vector<std::uint8_t>& runPayload, int idDelta = 0) {
    return RunOkWithId(RunId(runPayload) + static_cast<std::uint32_t>(idDelta));
}

std::vector<std::uint8_t> Serve(std::uint16_t type, const std::vector<std::uint8_t>& p) {
    if (type == 1) return HelloOkFor(p, 0);
    if (type == 6) return Frame(proto::MsgType::Pong, {});
    return RunOkFor(p);
}

proto::RunRequest Request(const proto::ModelEntry& m) {
    static const float x[1] = {3};
    proto::RunRequest r;
    r.model = m.name;
    r.inputs = {{"x", 1, {1}, x, sizeof x}};
    r.outputs = {"y"};
    return r;
}

// A client on a scripted server, with its log.
struct Rig {
    std::shared_ptr<Script> script = std::make_shared<Script>();
    std::mutex logMutex;
    std::vector<std::string> log;
    Client c;
    explicit Rig(Settings s = On(), Client::Now now = &FakeNow)
        : c(ScriptedFactory(script), ScriptedResolver(),
            [this](Level, const std::string& l) {
                std::lock_guard lock(logMutex);
                log.push_back(l);
            },
            now) {
        script->answer = &Serve;
        c.Configure(s);
    }
    int Logged(const std::string& part) {
        std::lock_guard lock(logMutex);
        int n = 0;
        for (const auto& l : log) n += l.find(part) != std::string::npos;
        return n;
    }
    void ClearLog() {
        std::lock_guard lock(logMutex);
        log.clear();
    }
    Client::Outcome Run(const proto::ModelEntry& m, Fallback& fb) {
        auto req = Request(m);
        Client::Reply reply;
        return c.Run(m, req, reply, fb);
    }
    Client::Outcome Run(const proto::ModelEntry& m) {
        Fallback fb;
        return Run(m, fb);
    }
    // a model admitted and listed, the main connection made by a first run, and the start-up status check done
    void Serving(const proto::ModelEntry& m) {
        CHECK(c.Admit(m).Ready());
        c.SetModels({m});
        CHECK(Run(m) == Client::Outcome::Forwarded);
        c.Tick();
    }
};

// The frames of one type a connection got.
std::vector<Sent> Frames(const Connection& c, proto::MsgType t) {
    std::vector<Sent> out;
    for (const auto& s : c.sent)
        if (s.type == static_cast<std::uint16_t>(t)) out.push_back(s);
    return out;
}

// A script whose first RUN from now on is answered late: its RUN_OK comes just before the answer to the next frame,
// whatever that frame is.
struct LateOnce {
    std::vector<std::uint8_t> late;
    bool done = false;
    std::vector<std::uint8_t> operator()(std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 3 && !done) {
            done = true;
            late = RunOkFor(p);
            return {};
        }
        auto back = std::move(late);
        late.clear();
        const auto mine = Serve(type, p);
        back.insert(back.end(), mine.begin(), mine.end());
        return back;
    }
};

void WaitFor(const std::function<bool()>& ready) {
    const auto give = Clock::now() + std::chrono::seconds(10);
    while (!ready() && Clock::now() < give) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// A thread joined however the test ends; `release` first lets a scripted server waiting on it go.
struct Joined {
    std::atomic<bool>* release = nullptr;
    std::thread t;
    ~Joined() {
        if (release) *release = true;
        if (t.joinable()) t.join();
    }
};

}  // namespace

// ---- the model load: a side connection, then the main connection

TEST(ClientAdmitsOnASideConnectionThenRuns) {
    Rig r;
    std::vector<std::size_t> listed;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) listed.push_back(DecodedHello(p).models.size());
        return Serve(type, p);
    };
    const auto m = Model("a.onnx");
    const Admission a = r.c.Admit(m);
    CHECK(a.Ready());
    CHECK_EQ(a.hello.server, std::string("scripted"));
    CHECK_EQ(r.script->connects.load(), 1);
    CHECK_EQ(r.script->closes.load(), 1);  // the side connection closes after HELLO_OK
    CHECK_EQ(r.c.SideHellos(), 1);
    CHECK_EQ(r.c.HellosSent(), 0);  // the main connection hasn't been touched
    r.c.SetModels({m});
    CHECK(r.Run(m) == Client::Outcome::Forwarded);  // the run makes the main connection: HELLO, then RUN
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    const auto by = r.script->ByConnection();
    CHECK_EQ(by.size(), std::size_t{2});
    CHECK(by[0] == (std::vector<std::uint16_t>{1}));
    CHECK(by[1] == (std::vector<std::uint16_t>{1, 3, 3}));
    CHECK(listed == (std::vector<std::size_t>{1, 1}));
    CHECK(r.Logged("Side connection for a.onnx: closed after HELLO_OK") == 1);
}

// After a load the background thread says HELLO on the open main connection with every live model; the side HELLO
// names the new model alone.
TEST(ClientMainHelloAfterTheSideLoad) {
    Rig r;
    std::vector<std::size_t> listed;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) listed.push_back(DecodedHello(p).models.size());
        return Serve(type, p);
    };
    const auto a = Model("a.onnx"), b = Model("b.onnx");
    CHECK(r.c.Admit(a).Ready());
    r.c.SetModels({a});
    r.c.Tick();  // the start-up status check (no models), then the main connection: a new one, HELLO with a
    CHECK_EQ(r.c.Reconnects(), 1);
    CHECK(r.c.Admit(b).Ready());
    r.c.SetModels({a, b});
    r.c.Tick();  // HELLO with a and b on the open connection
    CHECK_EQ(r.c.Reconnects(), 1);
    CHECK(listed == (std::vector<std::size_t>{1, 0, 1, 1, 2}));
    const auto before = r.script->Count(proto::MsgType::Hello);
    CHECK(r.Run(a) == Client::Outcome::Forwarded);
    CHECK(r.Run(b) == Client::Outcome::Forwarded);
    CHECK_EQ(r.script->Count(proto::MsgType::Hello), before);  // no HELLO in a run
    const auto by = r.script->ByConnection();
    CHECK(by[2] == (std::vector<std::uint16_t>{1, 1, 3, 3}));  // the main connection
    CHECK(by[3] == (std::vector<std::uint16_t>{1}));           // b's side connection
}

TEST(ClientAdmitRefusals) {
    const auto m = Model("a.onnx");
    // no server
    {
        Rig r;
        r.script->refuseConnect = true;
        const Admission a = r.c.Admit(m);
        CHECK(!a.Ready());
        CHECK(a.hello.kind == ProbeResult::Kind::NoAnswer);
    }
    // no answer in time
    {
        Rig r;
        r.script->answer = nullptr;
        CHECK(r.c.Admit(m).hello.kind == ProbeResult::Kind::NoAnswer);
    }
    // the wrong key
    {
        Rig r;
        r.script->answer = [](std::uint16_t, const std::vector<std::uint8_t>&) { return ErrorFrame(0, proto::ErrorCode::WrongKey); };
        CHECK(r.c.Admit(m).hello.kind == ProbeResult::Kind::WrongKey);
    }
    // each model status but ready
    for (std::uint8_t status : {1, 2, 3, 4}) {
        Rig r;
        r.script->answer = [status](std::uint16_t, const std::vector<std::uint8_t>& p) { return HelloOkFor(p, status, "why"); };
        const Admission a = r.c.Admit(m);
        CHECK(!a.Ready());
        CHECK(a.hello.kind == ProbeResult::Kind::Answered);
        CHECK_EQ(int(a.status), int(status));
        CHECK_EQ(a.detail, std::string("why"));
    }
    // a malformed answer
    {
        Rig r;
        r.script->answer = [](std::uint16_t, const std::vector<std::uint8_t>&) { return Frame(proto::MsgType::HelloOk, {1, 2, 3}); };
        CHECK(r.c.Admit(m).hello.kind == ProbeResult::Kind::Refused);
    }
    // no address
    {
        Rig r(Settings{});
        CHECK(r.c.Admit(m).hello.kind == ProbeResult::Kind::NoAddress);
        CHECK_EQ(r.script->connects.load(), 0);
    }
}

// ---- the pause and the background thread

TEST(ClientBackoffSchedule) {
    const int want[] = {1, 2, 5, 10, 30, 30, 30, 30};
    for (int i = 0; i < 8; ++i) CHECK_EQ(BackoffDelay(i).count(), static_cast<long long>(want[i]));
    CHECK_EQ(BackoffDelay(-1).count(), 1LL);
    CHECK_EQ(BackoffDelay(INT_MAX).count(), 30LL);
}

// A connection failure pauses the bridge: runs fall back at once, without the network, and the background thread
// reconnects after 1, 2, 5, 10, 30 and 30 s; its answered HELLO ends the pause.
TEST(ClientBackgroundRetriesOnTheScheduleAndEndsThePause) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    r.script->refuseConnect = true;
    r.script->closeBeforeAnswer = true;  // the connection is gone, and so is the server
    Fallback fb;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(fb.why == Why::NoConnection);
    CHECK(r.c.Paused());
    CHECK_EQ(r.Logged("until the server answers again"), 1);
    const auto received = r.script->Received().size();
    for (int i = 0; i < 5; ++i) {
        CHECK(r.Run(m, fb) == Client::Outcome::Skipped);
        CHECK(fb.why == Why::Paused);
        CHECK(fb.quiet);
        r.c.RecordFallback(m.name, fb, {});
    }
    CHECK_EQ(r.script->Received().size(), received);  // no network during the pause

    const int connects = r.script->connects.load();
    int tries = 0;
    const auto attempt = [&] { return r.script->connects.load() - connects; };
    const Clock::time_point start = g_now;
    for (int wait : {1, 2, 5, 10, 30, 30}) {
        g_now += std::chrono::seconds(wait) - std::chrono::milliseconds(1);
        r.c.Tick();
        CHECK_EQ(attempt(), tries);  // not yet
        g_now += std::chrono::milliseconds(1);
        const Clock::time_point next = r.c.Tick();
        CHECK_EQ(attempt(), ++tries);
        CHECK(next <= g_now + std::chrono::seconds(30));
        CHECK(r.c.Paused());
    }
    CHECK_EQ(r.Logged("Background attempt 6 failed"), 1);
    CHECK(g_now - start == std::chrono::seconds(78));

    r.script->refuseConnect = false;  // the server is back
    g_now += std::chrono::seconds(30);
    r.c.Tick();
    CHECK(!r.c.Paused());
    CHECK_EQ(r.Logged("The pause ends after 108 s (the server answered the background HELLO); 5 runs got the game's own lip sync meanwhile"), 1);
    const int hellos = r.script->Count(proto::MsgType::Hello);
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    CHECK_EQ(r.script->Count(proto::MsgType::Hello), hellos);  // the background's connection is the main one now
}

// A background attempt whose HELLO went before a later failure doesn't end that failure's pause: the next attempt, on
// the schedule, does.
TEST(ClientAnAttemptBeforeAFailureDoesntEndItsPause) {
    Rig r;
    const auto m = Model("a.onnx");
    std::atomic<bool> release{false}, holding{false};
    std::atomic<int> withModels{0};
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1 && !DecodedHello(p).models.empty() && ++withModels == 2) {  // the background's HELLO
            holding = true;
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return Serve(type, p);
    };
    CHECK(r.c.Admit(m).Ready());
    r.c.SetModels({m});
    Joined attempt{&release, std::thread([&] { r.c.Tick(); })};  // the start-up status check, then the main connection
    WaitFor([&] { return holding.load(); });
    r.script->closeBeforeAnswer = true;  // a run's own connection fails meanwhile
    Fallback fb;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(r.c.Paused());
    release = true;
    attempt.t.join();
    CHECK(r.c.Paused());
    CHECK_EQ(r.Logged("a failure came meanwhile"), 1);
    g_now += std::chrono::seconds(1);
    r.c.Tick();
    CHECK(!r.c.Paused());
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
}

// ---- PING

TEST(ClientPingsAfterAMinuteIdle) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    g_now += std::chrono::seconds(59);
    r.c.Tick();
    CHECK_EQ(r.c.Pings(), 0);
    g_now += std::chrono::seconds(1);
    const Clock::time_point next = r.c.Tick();
    CHECK_EQ(r.c.Pings(), 1);
    CHECK(!r.c.Paused());
    CHECK(next == g_now + std::chrono::seconds(60));
    // a run counts as activity
    g_now += std::chrono::seconds(30);
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    g_now += std::chrono::seconds(40);
    r.c.Tick();
    CHECK_EQ(r.c.Pings(), 1);
    // a missing PONG: the connection closes and the background reconnect begins
    r.script->answer = [](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        return type == 6 ? std::vector<std::uint8_t>{} : Serve(type, p);
    };
    g_now += std::chrono::seconds(20);
    r.c.Tick();
    CHECK_EQ(r.c.Pings(), 2);
    CHECK(r.c.Paused());
    CHECK_EQ(r.Logged("PING went unanswered"), 1);
    CHECK_EQ(r.Logged("The server didn't answer PING (the server didn't answer in time); lines get"), 1);
    g_now += std::chrono::seconds(1);
    r.c.Tick();
    CHECK(!r.c.Paused());
}

// PING goes only on a connection whose HELLO was answered and listed models: never on a side connection, a status
// check's, or a connection with no answered HELLO. Here a server that answers a PING before HELLO with ERROR 1, as
// PandorumLLM's does.
TEST(ClientPingsOnlyAfterHello) {
    Rig r;
    const auto m = Model("a.onnx");
    CHECK(r.c.Admit(m).Ready());
    for (int i = 0; i < 5; ++i) {  // no model listed yet: nothing to keep alive
        g_now += std::chrono::seconds(61);
        r.c.Tick();
    }
    CHECK_EQ(r.c.Pings(), 0);
    r.c.SetModels({m});
    for (int i = 0; i < 3; ++i) {
        g_now += std::chrono::seconds(61);
        r.c.Tick();
    }
    CHECK(r.c.Pings() >= 1);
    for (const auto& conn : r.script->ByConnection()) {
        bool hello = false;
        for (auto t : conn) {
            if (t == 1) hello = true;
            if (t == 6) CHECK(hello);
        }
    }
    // a main connection whose HELLO lists no model any more: no PING
    r.c.SetModels({});
    const int pings = r.c.Pings();
    for (int i = 0; i < 3; ++i) {
        g_now += std::chrono::seconds(61);
        r.c.Tick();
    }
    CHECK_EQ(r.c.Pings(), pings);
}

// ---- H: the background HELLO waits for the bridge's own side load; a status check doesn't

TEST(ClientHoldsTheBackgroundHelloDuringASideLoad) {
    Rig r;
    const auto a = Model("a.onnx"), b = Model("b.onnx");
    std::atomic<bool> release{false};
    std::atomic<int> mainHellos{0}, statusChecks{0};
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) {
            const proto::Hello h = DecodedHello(p);
            if (h.models.size() == 1 && h.models[0].name == "b.onnx") {
                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));  // the server loads b
            } else if (h.models.empty()) {
                ++statusChecks;
            } else {
                ++mainHellos;
            }
        }
        return Serve(type, p);
    };
    r.Serving(a);
    const int hellosBefore = mainHellos;
    // the connection fails: paused
    r.script->closeBeforeAnswer = true;
    r.script->refuseConnect = true;
    Fallback fb;
    CHECK(r.Run(a, fb) == Client::Outcome::Failed);
    r.c.RecordFallback(a.name, fb, {});  // the counters change
    CHECK(r.c.Paused());
    r.script->refuseConnect = false;

    Admission admitted;
    Joined load{&release, std::thread([&] { admitted = r.c.Admit(b); })};
    while (r.c.SideLoadsInFlight() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_now += std::chrono::minutes(5);  // the reconnect is due, and so is a status check (the counters changed)
    r.c.Tick();
    CHECK_EQ(mainHellos.load(), hellosBefore);  // held
    CHECK(r.c.Paused());
    CHECK_EQ(statusChecks.load(), 2);  // the start-up one, and this one: not held
    CHECK(r.Logged("waits for the model load in progress") >= 1);
    release = true;
    load.t.join();
    CHECK(admitted.Ready());
    CHECK_EQ(r.c.SideLoadsInFlight(), 0);
    r.c.Tick();
    CHECK_EQ(mainHellos.load(), hellosBefore + 1);
    CHECK(!r.c.Paused());
}

// ---- note 1: the late answers

TEST(ClientOneTimeoutKeepsTheConnection) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    std::vector<std::uint8_t> late;
    int runs = 0;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type != 3) return Serve(type, p);
        if (++runs == 1) {
            late = RunOkFor(p);  // answered after the timeout
            return std::vector<std::uint8_t>{};
        }
        auto back = late;
        late.clear();
        const auto mine = RunOkFor(p);
        back.insert(back.end(), mine.begin(), mine.end());
        return back;
    };
    const int connects = r.script->connects.load(), closes = r.script->closes.load();
    Fallback fb;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(fb.why == Why::RunTimedOut);
    CHECK(!r.c.Paused());
    CHECK_EQ(r.script->closes.load(), closes);
    auto req = Request(m);
    Client::Reply reply;
    CHECK(r.c.Run(m, req, reply, fb) == Client::Outcome::Forwarded);
    CHECK_EQ(reply.ok.requestId, req.requestId);
    CHECK_EQ(r.script->connects.load(), connects);
    CHECK_EQ(r.Logged("Discarded the late answer to request"), 1);
}

// A late ERROR is discarded too; with code 1, 2, 5 or 6 the server closes the connection after it, and 7 says it is
// stopping: those end the connection.
TEST(ClientLateErrorsThatEndTheConnection) {
    for (std::uint16_t code : {1, 2, 3, 4, 5, 6, 7}) {
        Rig r;
        const auto m = Model("a.onnx");
        r.Serving(m);
        std::vector<std::uint8_t> late;
        int runs = 0;
        r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
            if (type != 3) return Serve(type, p);
            if (++runs == 1) {
                late = ErrorFrame(RunId(p), code);
                return std::vector<std::uint8_t>{};
            }
            auto back = late;
            late.clear();
            const auto mine = RunOkFor(p);
            back.insert(back.end(), mine.begin(), mine.end());
            return back;
        };
        Fallback fb;
        CHECK(r.Run(m, fb) == Client::Outcome::Failed);
        CHECK(!r.c.Paused());
        const int closes = r.script->closes.load();
        const bool ends = code != 3 && code != 4;
        CHECK(r.Run(m, fb) == (ends ? Client::Outcome::Failed : Client::Outcome::Forwarded));
        CHECK_EQ(r.c.Paused(), ends);
        CHECK_EQ(r.script->closes.load(), closes + (ends ? 1 : 0));
        if (ends) {
            CHECK(fb.why == Why::ConnectionLost);
            CHECK(fb.text.find("ERROR " + std::to_string(code)) != std::string::npos);
        }
    }
}

TEST(ClientSecondTimeoutInARowPauses) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    bool answer = false;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type != 3 || answer) return Serve(type, p);
        return std::vector<std::uint8_t>{};  // never answered
    };
    Fallback fb;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(!r.c.Paused());
    const int closes = r.script->closes.load();
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(fb.why == Why::RunTimedOut);
    CHECK(r.c.Paused());
    CHECK_EQ(r.script->closes.load(), closes + 1);
    CHECK_EQ(r.Logged("A second RUN in a row timed out; lines get"), 1);
    // an answered run between two timeouts: no pause
    g_now += std::chrono::seconds(1);
    r.c.Tick();
    CHECK(!r.c.Paused());
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    answer = true;
    // the late answer never comes in this script, so the next run's answer is its own: answered
    CHECK(r.Run(m, fb) == Client::Outcome::Forwarded);
    answer = false;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(!r.c.Paused());
}

TEST(ClientTimeoutMidFrameCloses) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    r.script->answer = [](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type != 3) return Serve(type, p);
        auto f = RunOkFor(p);
        f.resize(5);  // the rest never comes
        return f;
    };
    const int closes = r.script->closes.load();
    Fallback fb;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(fb.why == Why::RunTimedOut);
    CHECK(r.c.Paused());
    CHECK_EQ(r.script->closes.load(), closes + 1);
    CHECK_EQ(r.c.Totals().timeouts, 0u);  // recorded by the caller, not by Run
}

TEST(ClientFailureKinds) {
    Rig r;
    int runs = 0;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type != 3) return Serve(type, p);
        ++runs;
        if (runs == 2) return Frame(proto::MsgType::RunOk, {9, 9});  // malformed
        if (runs == 3) return RunOkFor(p, 1);                         // the wrong request id
        if (runs == 4) return ErrorFrame(RunId(p), proto::ErrorCode::InferenceFailed);
        if (runs == 5) return ErrorFrame(RunId(p), proto::ErrorCode::Busy);
        return RunOkFor(p);
    };
    const auto m = Model("a.onnx");
    r.Serving(m);
    struct Want {
        Why why;
        bool paused;
    };
    for (const Want w : {Want{Why::Other, true}, Want{Why::Other, true}, Want{Why::ServerError, false}, Want{Why::ServerError, true}}) {
        Fallback fb;
        CHECK(r.Run(m, fb) == Client::Outcome::Failed);
        CHECK(fb.why == w.why);
        CHECK_EQ(r.c.Paused(), w.paused);
        if (w.paused) {
            g_now += std::chrono::seconds(1);
            r.c.Tick();
            CHECK(!r.c.Paused());
        }
    }
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
}

// Within a RUN, a connection closed or reset, or a send that failed, is opened again once, and the log says which.
TEST(ClientReconnectWithinARunNamesWhich) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    r.script->closeBeforeAnswer = true;
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    CHECK_EQ(r.Logged("The server had closed the connection; reconnecting"), 1);
    r.script->resetBeforeAnswer = true;
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    CHECK_EQ(r.Logged("The server had reset the connection; reconnecting"), 1);
    r.script->failSend = true;
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    CHECK_EQ(r.Logged("Sending to the server failed (couldn't send to the server (send failed)); reconnecting"), 1);
    CHECK(!r.c.Paused());
    CHECK_EQ(r.c.Reconnects(), 4);
}

TEST(ClientHelloAgainWhenModelsChange) {
    Rig r;
    std::vector<std::size_t> listed;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) listed.push_back(DecodedHello(p).models.size());
        return Serve(type, p);
    };
    const auto a = Model("a.onnx"), b = Model("b.onnx");
    CHECK(r.c.Admit(a).Ready());
    r.c.SetModels({a});
    CHECK(r.c.Admit(b).Ready());  // each side HELLO lists its model alone
    r.c.SetModels({a, b});
    CHECK(r.Run(a) == Client::Outcome::Forwarded);  // the main connection: HELLO with both
    r.c.SetModels({a});  // b released: the background says HELLO with a alone
    r.c.Tick();
    CHECK(r.Run(a) == Client::Outcome::Forwarded);
    CHECK(listed == (std::vector<std::size_t>{1, 1, 2, 0, 1}));
    // b is no longer listed: a run of it is answered at once, without a pause
    Fallback fb;
    CHECK(r.Run(b, fb) == Client::Outcome::Skipped);
    CHECK(fb.quiet);
    CHECK(!r.c.Paused());
}

// A model the main connection's HELLO doesn't have ready: its runs fall back at once (the first logged), with no pause,
// and the background asks again on the backoff schedule.
TEST(ClientModelNoLongerReady) {
    Rig r;
    std::atomic<int> status{3};
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1 && !DecodedHello(p).models.empty() && r.c.SideHellos() > 0 && r.script->connects > 1)
            return HelloOkFor(p, static_cast<std::uint8_t>(status.load()), "gone");
        return Serve(type, p);
    };
    const auto m = Model("a.onnx");
    CHECK(r.c.Admit(m).Ready());
    r.c.SetModels({m});
    Fallback fb;
    CHECK(r.Run(m, fb) == Client::Outcome::Failed);
    CHECK(fb.why == Why::NotReady);
    CHECK(!fb.quiet);
    CHECK(fb.text.find("no longer") != std::string::npos);
    const auto received = r.script->Received().size();
    CHECK(r.Run(m, fb) == Client::Outcome::Skipped);
    CHECK(fb.why == Why::NotReady);
    CHECK(fb.quiet);
    CHECK_EQ(r.script->Received().size(), received);
    CHECK(!r.c.Paused());
    status = 0;
    r.c.Tick();  // the start-up status check; the HELLO again is due in 1 s
    CHECK(r.Run(m, fb) == Client::Outcome::Skipped);
    g_now += std::chrono::seconds(1);
    r.c.Tick();
    CHECK(r.Run(m, fb) == Client::Outcome::Forwarded);
}

TEST(ClientProbe) {
    Rig r(Settings{});
    CHECK(r.c.Probe().kind == ProbeResult::Kind::NoAddress);
    r.c.Configure(On());
    std::string client;
    r.script->answer = [&](std::uint16_t, const std::vector<std::uint8_t>& p) {
        client = DecodedHello(p).client;
        return HelloOkFor(p, 0);
    };
    const ProbeResult p = r.c.Probe();
    CHECK(p.kind == ProbeResult::Kind::Answered);
    CHECK_EQ(p.device, std::string("CPU"));
    CHECK_EQ(client, ClientString(Counters{}));
    r.script->answer = [](std::uint16_t, const std::vector<std::uint8_t>&) { return ErrorFrame(0, proto::ErrorCode::WrongKey); };
    CHECK(r.c.Probe().kind == ProbeResult::Kind::WrongKey);
    r.script->answer = nullptr;
    CHECK(r.c.Probe().kind == ProbeResult::Kind::NoAnswer);
    auto bad = Frame(proto::MsgType::HelloOk, {});
    bad[4] = 2;  // version 2
    r.script->answer = [&](std::uint16_t, const std::vector<std::uint8_t>&) { return bad; };
    CHECK(r.c.Probe().kind == ProbeResult::Kind::Refused);
}

// Configure is called on the game's main thread: it never waits, even while a load is in flight; nor does a run.
TEST(ClientConfigureAndRunsNeverWaitForALoad) {
    Rig r;
    const auto a = Model("a.onnx"), b = Model("b.onnx");
    r.Serving(a);
    std::atomic<bool> release{false};
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1 && DecodedHello(p).models.size() == 1 && DecodedHello(p).models[0].name == "b.onnx")
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));  // a server loading the model
        return Serve(type, p);
    };
    Admission admitted;
    Joined admit{&release, std::thread([&] { admitted = r.c.Admit(b); })};
    while (r.c.SideLoadsInFlight() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Settings changed = On();
    changed.timeoutMs = 400;
    const auto start = Clock::now();
    r.c.Configure(changed);
    r.c.SetOrtVersion("1.24.4");
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    const auto runStart = Clock::now();
    CHECK(r.Run(a) == Client::Outcome::Forwarded);  // a's runs go on during b's load (on a new connection: the settings changed)
    const auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - runStart).count();
    release = true;
    admit.t.join();
    CHECK(took < 50);
    CHECK(ran < 200);
    CHECK(admitted.Ready());
    CHECK_EQ(r.c.Reconnects(), 2);
}

TEST(ClientHelloOkOutOfOrderIsRefused) {
    Rig r;
    r.script->answer = [](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type != 1) return Serve(type, p);
        const proto::Hello h = DecodedHello(p);
        proto::HelloOk ok;
        ok.server = "scripted";
        ok.device = "CPU";
        for (auto it = h.models.rbegin(); it != h.models.rend(); ++it) ok.models.push_back({it->name, 0, ""});
        std::vector<std::uint8_t> out;
        std::string err;
        proto::EncodeHelloOk(ok, out, err);
        return Frame(proto::MsgType::HelloOk, out);
    };
    const auto a = Model("a.onnx"), b = Model("b.onnx");
    CHECK(r.c.Admit(a).Ready());  // one model: no order to get wrong
    CHECK(r.c.Admit(b).Ready());
    r.c.SetModels({a, b});
    Fallback fb;
    CHECK(r.Run(a, fb) == Client::Outcome::Failed);
    CHECK(fb.text.find("out of order") != std::string::npos);
    CHECK(r.c.Paused());
}

// The name is looked up once per settings change; a failed lookup is logged once and runs fall back until the
// settings change.
TEST(ClientNameLookup) {
    auto script = std::make_shared<Script>();
    script->answer = &Serve;
    auto calls = std::make_shared<std::atomic<int>>(0);
    std::vector<std::string> log;
    Client c(ScriptedFactory(script), ScriptedResolver(calls), [&](Level, const std::string& l) { log.push_back(l); }, &FakeNow);
    Settings s = On();
    s.serverAddress = "server.invalid";
    c.Configure(s);
    CHECK(!c.Resolve());
    const auto m = Model("a.onnx");
    const Admission a = c.Admit(m);
    CHECK(a.hello.kind == ProbeResult::Kind::NoAnswer);
    CHECK(a.detail.find("couldn't be looked up") != std::string::npos);
    c.SetModels({m});
    auto req = Request(m);
    Client::Reply reply;
    Fallback fb;
    for (int i = 0; i < 3; ++i) {
        CHECK(c.Run(m, req, reply, fb) == Client::Outcome::Skipped);
        CHECK(fb.why == Why::NoConnection);
        CHECK(fb.quiet);
    }
    c.Tick();
    int lines = 0;
    for (const auto& l : log) lines += l.find("until the settings change") != std::string::npos;
    CHECK_EQ(lines, 1);
    CHECK_EQ(calls->load(), 1);  // runs and the background never look a failed name up again
    CHECK_EQ(script->connects.load(), 0);
    s.serverAddress = "server.example";
    c.Configure(s);
    CHECK(c.Resolve());
    CHECK(c.Admit(m).Ready());
    CHECK_EQ(calls->load(), 2);
    CHECK(c.Run(m, req, reply, fb) == Client::Outcome::Forwarded);
}

// BFA can drop a session without releasing it: the newest session of a model name is the one HELLO lists.
TEST(ClientNewestSessionOfAModelWins) {
    Rig r;
    std::vector<std::size_t> listed;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) listed.push_back(DecodedHello(p).models.size());
        return Serve(type, p);
    };
    const auto old = Model("a.onnx");
    auto fresh = Model("a.onnx");
    fresh.overrides = {{"batch", 2}};
    CHECK(r.c.Admit(old).Ready());
    CHECK(r.c.Admit(fresh).Ready());  // not refused: it replaces the old one
    r.c.SetModels({old, fresh});
    CHECK(r.Run(fresh) == Client::Outcome::Forwarded);
    Fallback fb;
    CHECK(r.Run(old, fb) == Client::Outcome::Skipped);
    CHECK(fb.quiet);
    CHECK(!r.c.Paused());
    CHECK(listed == (std::vector<std::size_t>{1, 1, 1}));
}

// ---- the timeouts: 3000 ms for a connect off the speech path, 60 s for a HELLO that may load a model, and the run's
// own timeout for everything in a run and for the background thread's exchanges on the main connection

TEST(ClientTimeoutsOfEachConnectAndExchange) {
    Rig r;  // a 300 ms run timeout
    const auto a = Model("a.onnx");
    // the side load: 3000 ms to connect, 60 s for HELLO
    CHECK(r.c.Admit(a).Ready());
    auto conns = r.script->Connections();
    CHECK_EQ(conns.size(), std::size_t{1});
    CHECK_EQ(conns[0].connectTimeoutMs, 3000);
    const auto side = Frames(conns[0], proto::MsgType::Hello);
    CHECK_EQ(side.size(), std::size_t{1});
    CHECK(side[0].BudgetMs() > 59000 && side[0].BudgetMs() <= 60000);

    // a run makes the main connection: its connect, HELLO and RUN within the run's 300 ms, one deadline for the three
    r.c.SetModels({a});
    CHECK(r.Run(a) == Client::Outcome::Forwarded);
    conns = r.script->Connections();
    CHECK_EQ(conns.size(), std::size_t{2});
    CHECK(conns[1].connectTimeoutMs > 0 && conns[1].connectTimeoutMs <= 300);
    auto hello = Frames(conns[1], proto::MsgType::Hello);
    auto run = Frames(conns[1], proto::MsgType::Run);
    CHECK(hello.size() == 1 && run.size() == 1);
    CHECK(hello[0].BudgetMs() > 200 && hello[0].BudgetMs() <= 300);
    CHECK(run[0].deadline == hello[0].deadline);
    CHECK(r.Run(a) == Client::Outcome::Forwarded);
    run = Frames(r.script->Connections()[1], proto::MsgType::Run);
    CHECK(run.size() == 2 && run[1].BudgetMs() > 200 && run[1].BudgetMs() <= 300);

    // the start-up status check: 3000 ms to connect, and 3000 ms for its answer
    r.c.Tick();
    conns = r.script->Connections();
    CHECK_EQ(conns.size(), std::size_t{3});
    CHECK_EQ(conns[2].connectTimeoutMs, 3000);
    const auto status = Frames(conns[2], proto::MsgType::Hello);
    CHECK(status.size() == 1 && status[0].BudgetMs() > 2000 && status[0].BudgetMs() <= 3000);

    // PING after 60 s idle, not before; its PONG within the run timeout
    g_now += std::chrono::seconds(59);
    r.c.Tick();
    CHECK(Frames(r.script->Connections()[1], proto::MsgType::Ping).empty());
    g_now += std::chrono::seconds(1);
    r.c.Tick();
    const auto ping = Frames(r.script->Connections()[1], proto::MsgType::Ping);
    CHECK(ping.size() == 1 && ping[0].BudgetMs() > 200 && ping[0].BudgetMs() <= 300);

    // a connect inside a run (the reconnect after a closed connection) stays within the run's deadline
    r.script->closeBeforeAnswer = true;
    r.script->refuseConnect = true;
    CHECK(r.Run(a) == Client::Outcome::Failed);
    CHECK(r.c.Paused());
    const auto tried = r.script->ConnectTimeouts();
    CHECK(tried.back() > 0 && tried.back() <= 300);

    // the background reconnect: 3000 ms to connect, 60 s for HELLO
    r.script->refuseConnect = false;
    g_now += std::chrono::seconds(1);
    r.c.Tick();
    CHECK(!r.c.Paused());
    conns = r.script->Connections();
    CHECK_EQ(conns.size(), std::size_t{4});
    CHECK_EQ(conns[3].connectTimeoutMs, 3000);
    hello = Frames(conns[3], proto::MsgType::Hello);
    CHECK(hello.size() == 1 && hello[0].BudgetMs() > 59000 && hello[0].BudgetMs() <= 60000);
}

// The background thread's PING on the main connection never makes a run wait past the run's own timeout: the run's
// clock starts when it is called, its wait for the connection included. A slow PONG, then a lost one, with a run
// arriving while the PING is out; the server takes 280 ms over each answer.
TEST(ClientRunNeverWaitsPastItsTimeoutForThePing) {
    for (const bool lost : {false, true}) {
        Rig r;  // 300 ms
        const auto m = Model("a.onnx");
        r.Serving(m);
        if (lost)
            r.script->answer = [](std::uint16_t type, const std::vector<std::uint8_t>& p) {
                return type == 6 ? std::vector<std::uint8_t>{} : Serve(type, p);
            };
        r.script->answerDelayMs = 280;
        r.script->timed = true;
        g_now += std::chrono::seconds(60);
        Joined ping{nullptr, std::thread([&] { r.c.Tick(); })};
        WaitFor([&] { return r.script->Count(proto::MsgType::Ping) == 1; });
        const auto start = Clock::now();
        Fallback fb;
        const Client::Outcome outcome = r.Run(m, fb);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        ping.t.join();
        CHECK(took <= 300 + 100);
        CHECK(outcome != Client::Outcome::Forwarded);
        if (lost) {
            CHECK(outcome == Client::Outcome::Skipped);
            CHECK(r.c.Paused());
        } else {
            CHECK(fb.why == Why::RunTimedOut || fb.why == Why::Other);
            CHECK(!r.c.Paused());
        }
    }
}

// No address (cleared mid-game), or one not looked up yet: runs fall back at once, and only the first of a series is
// logged.
TEST(ClientNoAddressIsLoggedOnce) {
    Rig r;
    const auto m = Model("a.onnx");
    r.Serving(m);
    Settings s = On();
    s.serverAddress.clear();
    const auto series = [&](const std::string& text) {
        for (int i = 0; i < 20; ++i) {
            Fallback fb;
            CHECK(r.Run(m, fb) == Client::Outcome::Skipped);
            CHECK(fb.why == Why::NoConnection);
            CHECK_EQ(fb.text, text);
            CHECK_EQ(fb.quiet, i > 0);
            r.c.RecordFallback(m.name, fb, {});
        }
        CHECK_EQ(r.Logged("a.onnx: not answered (no connection: " + text + ")"), 1);
    };
    r.c.Configure(s);
    series("no server address is set");
    s.serverAddress = "server.example";  // a new address, not looked up yet: a new series
    r.c.Configure(s);
    series("the server's address hasn't been looked up yet");
    CHECK(r.c.Resolve());
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    CHECK(!r.c.Paused());
}

// H with the main connection open: the HELLO a change of the live models calls for waits while the bridge's own side
// load is in flight, and goes when it ends.
TEST(ClientHoldsTheReHelloDuringASideLoad) {
    Rig r;
    const auto a = Model("a.onnx"), b = Model("b.onnx"), c = Model("c.onnx");
    std::atomic<bool> release{false};
    std::atomic<int> hellos{0};  // every HELLO with models but b's side HELLO
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) {
            const proto::Hello h = DecodedHello(p);
            if (h.models.size() == 1 && h.models[0].name == "b.onnx") {
                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));  // the server loads b
            } else if (!h.models.empty()) {
                ++hellos;
            }
        }
        return Serve(type, p);
    };
    r.Serving(a);
    r.c.SetModels({a, c});
    r.c.Tick();  // HELLO with a and c on the open connection
    const int before = hellos, reconnects = r.c.Reconnects();
    Admission admitted;
    Joined load{&release, std::thread([&] { admitted = r.c.Admit(b); })};
    WaitFor([&] { return r.c.SideLoadsInFlight() > 0; });
    r.c.SetModels({a});  // c released: the connection's HELLO is out of date
    r.c.Tick();
    CHECK_EQ(hellos.load(), before);  // held
    CHECK(r.Logged("waits for the model load in progress") >= 1);
    CHECK(!r.c.Paused());
    release = true;
    load.t.join();
    CHECK(admitted.Ready());
    r.c.Tick();
    CHECK_EQ(hellos.load(), before + 1);
    CHECK_EQ(r.c.Reconnects(), reconnects);  // on the open connection
    CHECK(r.Run(a) == Client::Outcome::Forwarded);
}

// H's gate: a side load waits for a HELLO already in flight on the main connection before its own HELLO goes, so that
// HELLO is never caught waiting for the load.
TEST(ClientSideLoadWaitsForAMainHelloInFlight) {
    Rig r;
    const auto a = Model("a.onnx"), b = Model("b.onnx"), c = Model("c.onnx");
    r.Serving(a);
    std::atomic<bool> release{false};
    std::mutex eventsMutex;
    std::vector<std::string> events;
    const auto note = [&](const char* e) {
        std::lock_guard lock(eventsMutex);
        events.push_back(e);
    };
    const auto seen = [&] {
        std::lock_guard lock(eventsMutex);
        return events;
    };
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) {
            const proto::Hello h = DecodedHello(p);
            if (h.models.size() == 2) {
                note("main");
                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                note("main answered");
            } else if (h.models.size() == 1 && h.models[0].name == "b.onnx") {
                note("side");
            }
        }
        return Serve(type, p);
    };
    r.c.SetModels({a, c});
    Joined hello{&release, std::thread([&] { r.c.Tick(); })};  // HELLO with a and c on the main connection
    WaitFor([&] { return !seen().empty(); });
    Admission admitted;
    Joined load{&release, std::thread([&] { admitted = r.c.Admit(b); })};
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(seen() == (std::vector<std::string>{"main"}));
    CHECK_EQ(r.c.SideLoadsInFlight(), 0);
    release = true;
    hello.t.join();
    load.t.join();
    CHECK(seen() == (std::vector<std::string>{"main", "main answered", "side"}));
    CHECK(admitted.Ready());
    CHECK(!r.c.Paused());
}

// A late answer pending when the background thread's HELLO, or its PING, goes out on the main connection: it is read
// and discarded first, and the connection stays.
TEST(ClientDiscardsALateAnswerBeforeAReHelloOrAPing) {
    for (const bool ping : {false, true}) {
        Rig r;
        const auto a = Model("a.onnx");
        r.Serving(a);
        r.script->answer = LateOnce{};
        Fallback fb;
        CHECK(r.Run(a, fb) == Client::Outcome::Failed);
        CHECK(fb.why == Why::RunTimedOut);
        CHECK(!r.c.Paused());
        const int reconnects = r.c.Reconnects(), hellos = r.c.HellosSent();
        if (ping) {
            g_now += std::chrono::seconds(60);
            r.c.Tick();
            CHECK_EQ(r.c.Pings(), 1);
        } else {
            r.c.SetModels({a, Model("c.onnx")});
            r.c.Tick();
            CHECK_EQ(r.c.HellosSent(), hellos + 1);
        }
        CHECK(!r.c.Paused());
        CHECK_EQ(r.c.Reconnects(), reconnects);
        CHECK_EQ(r.Logged("Discarded the late answer to request"), 1);
        CHECK(r.Run(a) == Client::Outcome::Forwarded);
    }
}

// A model the server keeps not ready: the background asks again on a new connection every 30 s, and the log stays
// quiet: that connect is logged at Detailed only (it ends no pause), and the status line and the fallback once.
TEST(ClientModelKeptNotReadyStaysQuiet) {
    Settings s = On();
    s.logDetail = LogDetail::Normal;
    Rig r(s);
    r.script->answer = [](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1 && !DecodedHello(p).models.empty()) return HelloOkFor(p, 3, "not loaded");
        return Serve(type, p);
    };
    const auto m = Model("a.onnx");
    r.c.SetModels({m});
    r.c.Tick();  // the start-up status check, and the main connection
    for (int i = 0; i < 10; ++i) {
        Fallback fb;
        CHECK(r.Run(m, fb) == Client::Outcome::Skipped);
        CHECK(fb.why == Why::NotReady);
        r.c.RecordFallback(m.name, fb, {});
        g_now += std::chrono::seconds(30);
        r.c.Tick();
    }
    CHECK(r.c.Reconnects() >= 10);
    CHECK(!r.c.Paused());
    CHECK_EQ(r.Logged("Connected to the server"), 0);
    CHECK_EQ(r.Logged("a.onnx: not answered (not ready"), 1);
    CHECK_EQ(r.Logged("Server scripted on CPU: a.onnx"), 1);
}

// ---- the counters, the client string and the summary line

TEST(ClientStringAndItsEightyCharacters) {
    const std::string name = std::string("BFARemoteBridge ") + kVersionText;
    CHECK_EQ(ClientName(), name);
    CHECK_EQ(ClientString(Counters{1250, 0, 0}), name + " (answered 1250, fallbacks 0, timeouts 0)");
    if (std::string(kVersionText) == "1.1.0") CHECK_EQ(ClientString(Counters{1250, 0, 0}).size(), std::size_t{62});
    // the longest that fits whole, then one digit more: the trailing key goes first, then the next
    const std::size_t fixed = (name + " (answered , fallbacks , timeouts )").size();
    std::uint64_t big = 1;
    std::size_t digits = 1;
    while (fixed + 3 * (digits + 1) <= 80) {
        big = big * 10 + 1;
        ++digits;
    }
    const std::string whole = ClientString(Counters{big, big, big});
    CHECK(whole.size() <= 80);
    CHECK(whole.find("timeouts") != std::string::npos);
    const std::string cut = ClientString(Counters{big * 100, big * 100, big * 100});
    CHECK(cut.size() <= 80);
    CHECK(cut.find("timeouts") == std::string::npos);
    CHECK(cut.rfind(name + " (answered ", 0) == 0);
    const std::string most = ClientString(Counters{UINT64_MAX, UINT64_MAX, UINT64_MAX});
    CHECK(most.size() <= 80);
    CHECK(most.find("fallbacks") == std::string::npos || most.find("timeouts") == std::string::npos);
    // exactly 80 keeps every key; 81 drops the last
    const auto ofDigits = [](std::size_t n) {
        std::uint64_t v = 1;
        for (std::size_t i = 1; i < n; ++i) v *= 10;
        return v;
    };
    const std::size_t room = 80 - fixed;  // the digits the three counts may have between them
    const std::size_t a = std::min<std::size_t>(19, room - 2), f = room - a - 1;
    const std::string exact = ClientString(Counters{ofDigits(a), ofDigits(f), 7});
    CHECK_EQ(exact.size(), std::size_t{80});
    CHECK(exact.find("timeouts 7)") != std::string::npos);
    const std::string over = ClientString(Counters{ofDigits(a), ofDigits(f), 17});
    CHECK(over.find("timeouts") == std::string::npos);
    CHECK(over.find("fallbacks") != std::string::npos);
    CHECK(over.size() <= 80);
    CHECK_EQ(ClientString(Counters{1, 2, 3}, 10), name.substr(0, 10));
    CHECK_EQ(ClientString(Counters{1, 2, 3}, name.size()), name);
}

// The counters go in every HELLO: the main connection's, a side load's and the status checks' (every 5 minutes, only
// when they changed).
TEST(ClientCountersGoInEveryHello) {
    Rig r;
    std::vector<std::string> clients;
    r.script->answer = [&](std::uint16_t type, const std::vector<std::uint8_t>& p) {
        if (type == 1) clients.push_back(DecodedHello(p).client);
        return Serve(type, p);
    };
    const auto m = Model("a.onnx");
    r.Serving(m);  // side, main and the start-up status check
    Client::Reply reply;
    r.c.RecordAnswered(reply);
    r.c.RecordFallback("a.onnx", Fallback{Why::RunTimedOut, "", true}, std::chrono::microseconds(0));
    r.c.RecordFallback("a.onnx", Fallback{Why::Paused, "", true}, std::chrono::microseconds(0));
    CHECK(r.c.Totals() == (Counters{1, 2, 1}));
    CHECK(r.c.Admit(Model("b.onnx")).Ready());
    CHECK_EQ(clients.back(), ClientString(Counters{1, 2, 1}));
    const auto n = clients.size();
    g_now += std::chrono::minutes(5);
    r.c.Tick();
    CHECK_EQ(clients.size(), n + 1);
    CHECK_EQ(clients.back(), ClientString(Counters{1, 2, 1}));
    g_now += std::chrono::minutes(5);  // nothing changed: no status check
    r.c.Tick();
    CHECK_EQ(clients.size(), n + 1);
    for (const auto& c : clients) CHECK(c.size() <= 80);
}

TEST(ClientSummaryLineByReason) {
    Rig r;
    Client::Reply reply;
    reply.ok.serverMicros = 500;
    for (int i = 0; i < 244; ++i) {
        reply.roundTrip = std::chrono::microseconds(i == 7 ? 21000 : 1000);
        r.c.RecordAnswered(reply);
    }
    for (Why w : {Why::RunTimedOut, Why::ServerError, Why::NotReady, Why::NoConnection, Why::Paused}) r.c.RecordFallback("a.onnx", Fallback{w, "", true}, {});
    CHECK_EQ(r.Logged("runs since the last summary"), 0);
    r.c.RecordFallback("a.onnx", Fallback{Why::ConnectTimedOut, "", true}, {});
    CHECK_EQ(r.Logged("250 runs since the last summary: 244 answered, 6 not (1 timed out, 1 refused, 1 not ready, 2 no connection, 1 paused, 0 other); "
                      "round trip 1.1 ms, server 0.5 ms, slowest 21.0 ms"),
             1);
    // every 10 minutes when anything happened
    r.c.RecordFallback("a.onnx", Fallback{Why::Other, "", true}, {});
    r.c.RecordFallback("a.onnx", Fallback{Why::ConnectionLost, "", true}, {});
    g_now += std::chrono::minutes(9);
    r.c.Tick();
    CHECK_EQ(r.Logged("runs since the last summary"), 1);
    g_now += std::chrono::minutes(1);
    r.c.Tick();
    CHECK_EQ(r.Logged("2 runs since the last summary: 0 answered, 2 not (0 timed out, 0 refused, 0 not ready, 1 no connection, 0 paused, 1 other)"), 1);
    g_now += std::chrono::minutes(30);
    r.c.Tick();
    CHECK_EQ(r.Logged("runs since the last summary"), 2);  // nothing happened since
}

TEST(ClientEachFallbackIsLogged) {
    Rig r;
    r.c.RecordFallback("a2f3_fp16.onnx", Fallback{Why::RunTimedOut, "no answer within 1000 ms", false}, std::chrono::microseconds(1001200));
    CHECK_EQ(r.Logged("a2f3_fp16.onnx: not answered (RUN timed out: no answer within 1000 ms) after 1001.2 ms; its line gets the game's own lip sync"), 1);
    r.c.RecordFallback("a2f3_fp16.onnx", Fallback{Why::ServerError, "ERROR 4, inference failed: scripted", false}, std::chrono::microseconds(3000));
    CHECK_EQ(r.Logged("a2f3_fp16.onnx: not answered (ERROR 4, inference failed: scripted) after 3.0 ms"), 1);
    CHECK_EQ(r.Logged("ERROR: ERROR"), 0);
    r.c.RecordFallback("a2f3_fp16.onnx", Fallback{Why::ServerError, "HELLO: the server refused HELLO: ERROR 2, wrong key", false}, {});
    CHECK_EQ(r.Logged("not answered (ERROR: HELLO: the server refused HELLO: ERROR 2, wrong key) after 0.0 ms"), 1);
    r.c.RecordFallback("a2f3_fp16.onnx", Fallback{Why::Paused, "the bridge is paused", true}, {});
    CHECK_EQ(r.Logged("not answered (paused"), 0);
    CHECK(r.c.Totals() == (Counters{0, 4, 1}));
}

// ---- the thread itself, on the real clock

TEST(ClientBackgroundThreadEndsThePause) {
    Rig r(On(), &Clock::now);
    r.c.StartBackground();
    const auto m = Model("a.onnx");
    CHECK(r.c.Admit(m).Ready());
    r.c.SetModels({m});
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    r.script->closeBeforeAnswer = true;
    r.script->refuseConnect = true;
    const auto start = Clock::now();
    CHECK(r.Run(m) == Client::Outcome::Failed);
    CHECK(r.c.Paused());
    r.script->refuseConnect = false;
    while (r.c.Paused() && Clock::now() - start < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    CHECK(!r.c.Paused());
    CHECK(took >= 1000 && took < 3000);  // the first try after 1 s
    CHECK(r.Run(m) == Client::Outcome::Forwarded);
    r.c.StopBackground();
}
