// The bridge end to end, as BFA uses ONNX Runtime: the real runtime (onnxruntime 1.24.4's libonnxruntime.so, whose C API
// is the same as the Windows DLL's), the bridge's replacement for OrtGetApiBase, and the reference server
// (tools/reference_server.py) on this machine. Every model load and run goes through the replaced table exactly as
// BFA's calls do: CreateSessionOptions, AddFreeDimensionOverrideByName, CreateSession, CreateTensorWithDataAsOrtValue
// over CPU memory, Run with the outputs left for the runtime to make, GetTensorMutableData.
//
// Needs BFARB_ORT_LIBRARY, BFARB_PYTHON, BFARB_SERVER and BFARB_MODELS (CMakeLists.txt sets them); skipped without.

#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include "PosixTransport.h"
#include "Test.h"
#include "core/Bridge.h"
#include "core/Scrub.h"
#include "core/Status.h"

extern char** environ;

using namespace bfarb;
using namespace bfarb::test;
namespace fs = std::filesystem;

namespace {

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// ---- the real runtime

OrtGetApiBaseFn RealGetApiBase() {
    static OrtGetApiBaseFn fn = [] {
        const std::string lib = Env("BFARB_ORT_LIBRARY");
        if (lib.empty()) return OrtGetApiBaseFn{};
        void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) return OrtGetApiBaseFn{};
        return reinterpret_cast<OrtGetApiBaseFn>(dlsym(h, "OrtGetApiBase"));
    }();
    return fn;
}

const OrtApi* RealApi() { return RealGetApiBase()()->GetApi(ORT_API_VERSION); }

void Need() {
    if (!RealGetApiBase() || Env("BFARB_PYTHON").empty() || Env("BFARB_SERVER").empty() || Env("BFARB_MODELS").empty())
        SKIP("needs onnxruntime 1.24.4 and the reference server (configure with -DBFARB_PYTHON=...)");
}

void Ok(OrtStatus* s) {
    if (!s) return;
    const std::string m = RealApi()->GetErrorMessage(s);
    RealApi()->ReleaseStatus(s);
    throw Failure("runtime error: " + m);
}

// ---- the reference server, a child process

class Server {
public:
    explicit Server(const fs::path& models, std::vector<std::string> extra = {}) {
        int out[2];
        if (pipe(out) != 0) throw Failure("pipe");
        std::vector<std::string> args = {Env("BFARB_PYTHON"), "-I", Env("BFARB_SERVER"), "--models", models.string(), "--port", "0", "--quiet"};
        args.insert(args.end(), extra.begin(), extra.end());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addclose(&fa, out[0]);
        if (posix_spawn(&pid_, argv[0], &fa, nullptr, argv.data(), environ) != 0) throw Failure("couldn't start the reference server");
        posix_spawn_file_actions_destroy(&fa);
        close(out[1]);
        std::string line;
        char c;
        while (read(out[0], &c, 1) == 1 && c != '\n') line += c;
        out_ = out[0];
        if (line.rfind("LISTENING ", 0) != 0) throw Failure("the reference server didn't start: " + line);
        port_ = std::stoi(line.substr(10));
    }
    ~Server() {
        kill(pid_, SIGTERM);
        int status = 0;
        waitpid(pid_, &status, 0);
        close(out_);
    }
    int Port() const { return port_; }

private:
    pid_t pid_ = 0;
    int out_ = -1;
    int port_ = 0;
};

// A port nothing listens on.
int DeadPort() {
    PosixTransport t;
    for (int port = 47000; port < 47100; ++port) {
        std::string err;
        std::vector<Endpoint> where;
        PosixTransport::Resolve("127.0.0.1", static_cast<std::uint16_t>(port), where, err);
        if (!t.Connect(where, 200, err)) return port;
        t.Close();
    }
    throw Failure("no free port");
}

Clock::time_point g_now = Clock::time_point{} + std::chrono::hours(1);
Clock::time_point FakeNow() { return g_now; }

// ---- BFA's side

struct Harness {
    std::mutex mutex;
    std::vector<std::string> log;
    std::vector<std::string> notes;  // every note, as the plugin's ShowNote gets them
    Bridge bridge;
    const OrtApi* api = nullptr;  // what BFA holds: the replaced table
    OrtEnv* env = nullptr;
    OrtMemoryInfo* cpu = nullptr;

    explicit Harness(Settings s, bool apply = true, bool expectSettings = false)
        : bridge([] { return std::make_unique<PosixTransport>(); }, &PosixTransport::Resolve,
                 [this](Level, const std::string& l) {
                     std::lock_guard lock(mutex);
                     log.push_back(l);
                 },
                 &FakeNow) {
        Bridge::SetCurrent(&bridge);
        bridge.ExpectSettings(expectSettings);
        if (apply) bridge.Apply(s);
        bridge.Notes().SetSink([this](const std::string& n) {
            std::lock_guard lock(mutex);
            notes.push_back(n);
        });
        const OrtApiBase* base = Bridge::Hook(RealGetApiBase())();
        api = base->GetApi(ORT_API_VERSION);
        Ok(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "bfarb-tests", &env));
        Ok(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &cpu));
    }
    ~Harness() {
        api->ReleaseMemoryInfo(cpu);
        api->ReleaseEnv(env);
        Bridge::SetCurrent(nullptr);
    }

    // CreateSession as BFA's: options, overrides, the session, the options released. The status is returned (and
    // owned by the caller).
    OrtStatus* Create(const fs::path& model, std::vector<std::pair<const char*, int>> overrides, OrtSession** out, bool gpu = false) {
        OrtSessionOptions* so = nullptr;
        Ok(api->CreateSessionOptions(&so));
        Ok(api->SetIntraOpNumThreads(so, 2));
        Ok(api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_EXTENDED));  // as BFA's CPU sessions
        if (gpu) bridge.OnGpuOptions(so);
        for (auto [name, value] : overrides) Ok(api->AddFreeDimensionOverrideByName(so, name, value));
        *out = nullptr;
        OrtStatus* status = api->CreateSession(env, model.c_str(), so, out);
        api->ReleaseSessionOptions(so);
        return status;
    }

    bool Logged(const std::string& part) {
        std::lock_guard lock(mutex);
        for (const auto& l : log)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
    int CountNotes(const std::string& part) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const auto& l : notes) n += l.find(part) != std::string::npos;
        return n;
    }
    int CountLogged(const std::string& part) {
        std::lock_guard lock(mutex);
        int n = 0;
        for (const auto& l : log) n += l.find(part) != std::string::npos;
        return n;
    }
};

Settings On(int port) {
    Settings s;
    s.serverAddress = "127.0.0.1";
    s.serverPort = port;
    s.timeoutMs = 2000;
    return s;
}

// The tiny model's inputs (tests/make_test_model.py), batch 2, as CPU tensors made the way BFA makes them.
struct TinyInputs {
    std::vector<float> x = std::vector<float>(16);
    std::vector<std::int64_t> id = {41};
    std::vector<std::uint16_t> h = {0x3C00, 0x4000, 0xBC00, 0x3555, 0x0001, 0x7BFF, 0x8000, 0x3E00};
    std::vector<double> d = {0.25, -1e300};
    std::vector<std::int32_t> i = {1, -2, 1 << 20};
    std::vector<OrtValue*> values;
    const OrtApi* api;
    TinyInputs(const OrtApi* a, OrtMemoryInfo* cpu, unsigned seed, bool zeros = false) : api(a) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> n(0.f, 1.f);
        for (auto& v : x) v = zeros ? 0.f : n(rng);
        if (zeros) {
            id = {0};
            std::fill(h.begin(), h.end(), 0);
            std::fill(d.begin(), d.end(), 0.0);
            std::fill(i.begin(), i.end(), 0);
        }
        Add(cpu, x.data(), x.size() * 4, {2, 8}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        Add(cpu, id.data(), 8, {1}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        Add(cpu, h.data(), h.size() * 2, {2, 4}, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
        Add(cpu, d.data(), d.size() * 8, {2}, ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE);
        Add(cpu, i.data(), i.size() * 4, {3}, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32);
    }
    void Add(OrtMemoryInfo* cpu, void* p, std::size_t bytes, std::vector<std::int64_t> shape, ONNXTensorElementDataType t) {
        OrtValue* v = nullptr;
        Ok(api->CreateTensorWithDataAsOrtValue(cpu, p, bytes, shape.data(), shape.size(), t, &v));
        values.push_back(v);
    }
    ~TinyInputs() {
        for (auto* v : values) api->ReleaseValue(v);
    }
};

const char* kTinyIn[] = {"x", "id", "h", "d", "i"};
const char* kTinyOut[] = {"y", "z", "hh", "dd", "ii"};

struct Outputs {
    const OrtApi* api;
    std::vector<OrtValue*> v;
    Outputs(const OrtApi* a, std::size_t n) : api(a), v(n, nullptr) {}
    ~Outputs() {
        for (auto* x : v)
            if (x) api->ReleaseValue(x);
    }
};

std::vector<std::uint8_t> Bytes(const OrtApi* api, OrtValue* v, std::vector<std::int64_t>* dims = nullptr, int* type = nullptr) {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    Ok(api->GetTensorTypeAndShape(v, &info));
    std::size_t count = 0, rank = 0;
    ONNXTensorElementDataType t{};
    Ok(api->GetTensorShapeElementCount(info, &count));
    Ok(api->GetTensorElementType(info, &t));
    Ok(api->GetDimensionsCount(info, &rank));
    std::vector<std::int64_t> d(rank);
    if (rank) Ok(api->GetDimensions(info, d.data(), rank));
    api->ReleaseTensorTypeAndShapeInfo(info);
    if (dims) *dims = d;
    if (type) *type = static_cast<int>(t);
    void* data = nullptr;
    Ok(api->GetTensorMutableData(v, &data));
    const std::size_t size = proto::DTypeSize(static_cast<std::uint8_t>(t)) * count;
    const auto* p = static_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(p, p + size);
}

// The same run on a real session of the runtime's own, outside the bridge.
std::vector<std::vector<std::uint8_t>> LocalRun(const fs::path& model, std::vector<std::pair<const char*, int>> overrides, const char* const* in,
                                                const std::vector<OrtValue*>& inputs, const char* const* out, std::size_t outCount) {
    const OrtApi* real = RealApi();
    OrtEnv* env = nullptr;
    Ok(real->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "bfarb-local", &env));
    OrtSessionOptions* so = nullptr;
    Ok(real->CreateSessionOptions(&so));
    Ok(real->SetIntraOpNumThreads(so, 2));
    Ok(real->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_EXTENDED));
    for (auto [n, v] : overrides) Ok(real->AddFreeDimensionOverrideByName(so, n, v));
    OrtSession* s = nullptr;
    Ok(real->CreateSession(env, model.c_str(), so, &s));
    real->ReleaseSessionOptions(so);
    Outputs o(real, outCount);
    Ok(real->Run(s, nullptr, in, inputs.data(), inputs.size(), out, outCount, o.v.data()));
    std::vector<std::vector<std::uint8_t>> result;
    for (auto* v : o.v) result.push_back(Bytes(real, v));
    real->ReleaseSession(s);
    real->ReleaseEnv(env);
    return result;
}

fs::path Models() { return fs::path(Env("BFARB_MODELS")); }

fs::path TempFolder(const char* name) {
    const fs::path p = fs::temp_directory_path() / ("bfarb-" + std::string(name) + "-" + std::to_string(getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

bool IsNanAnswer(const OrtApi* api, OrtValue* v) {
    std::vector<std::int64_t> dims;
    int type = 0;
    const auto b = Bytes(api, v, &dims, &type);
    float f = 0;
    if (b.size() == 4) std::memcpy(&f, b.data(), 4);
    return type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && dims == std::vector<std::int64_t>{1} && std::isnan(f);
}

}  // namespace

TEST(ForwardTinyModelMatchesLocalByteForByte) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    OrtSession* s = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s));
    CHECK(h.bridge.IsBridgeSession(s));
    CHECK(h.Logged("served by"));
    CHECK_EQ(h.CountNotes("tiny.onnx on "), 1);  // the monitor's line: the card, the server, ready
    CHECK_EQ(h.CountNotes(": ready"), 1);

    // BFA's warm-up run (zeros), then real lines
    for (unsigned seed : {0u, 1u, 2u, 3u}) {
        TinyInputs in(h.api, h.cpu, seed, seed == 0);
        Outputs out(h.api, 5);
        Ok(h.api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        const auto local = LocalRun(Models() / "tiny.onnx", {{"batch", 2}}, kTinyIn, in.values, kTinyOut, 5);
        for (int k = 0; k < 5; ++k) {
            std::vector<std::int64_t> dims;
            const auto got = Bytes(h.api, out.v[k], &dims);
            CHECK(got == local[k]);
            CHECK(!got.empty());
        }
    }
    const Stats st = h.bridge.Statistics();
    CHECK_EQ(st.forwarded, 4u);
    CHECK_EQ(st.unanswered, 0u);
    h.api->ReleaseSession(s);
    CHECK_EQ(h.bridge.BridgeSessions(), std::size_t{0});
}

TEST(ForwardIntoCallersOutputs) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    OrtSession* s = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s));
    TinyInputs in(h.api, h.cpu, 7);
    std::vector<float> y(6, 0.f);
    std::vector<std::int64_t> z(1, 0);
    OrtValue* yv = nullptr;
    OrtValue* zv = nullptr;
    const std::int64_t yShape[] = {2, 3}, zShape[] = {1};
    Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, y.data(), 24, yShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &yv));
    Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, z.data(), 8, zShape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &zv));
    OrtValue* outs[5] = {yv, zv, nullptr, nullptr, nullptr};
    Ok(h.api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, outs));
    CHECK(outs[0] == yv && outs[1] == zv);  // the caller's values, filled
    const auto local = LocalRun(Models() / "tiny.onnx", {{"batch", 2}}, kTinyIn, in.values, kTinyOut, 5);
    CHECK(std::memcmp(y.data(), local[0].data(), 24) == 0);
    CHECK_EQ(z[0], 42);
    for (int k = 2; k < 5; ++k) {
        CHECK(outs[k] != nullptr);
        CHECK(Bytes(h.api, outs[k]) == local[k]);
        h.api->ReleaseValue(outs[k]);
    }
    // a caller's value of the wrong shape: the answer doesn't fit, and the line gets NaN
    std::vector<float> wrong(3, 0.f);
    OrtValue* wv = nullptr;
    const std::int64_t wShape[] = {1, 3};
    Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, wrong.data(), 12, wShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &wv));
    OrtValue* outs2[5] = {wv, nullptr, nullptr, nullptr, nullptr};
    Ok(h.api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, outs2));
    CHECK(std::isnan(wrong[0]) && std::isnan(wrong[2]));
    for (int k = 1; k < 5; ++k) {
        CHECK(IsNanAnswer(h.api, outs2[k]));
        h.api->ReleaseValue(outs2[k]);
    }
    CHECK(h.Logged("as the caller's"));
    h.api->ReleaseValue(yv);
    h.api->ReleaseValue(zv);
    h.api->ReleaseValue(wv);
    h.api->ReleaseSession(s);
}

TEST(ForwardCreateSessionRefusedForEachReason) {
    Need();
    struct Case {
        const char* what;
        std::vector<std::string> serverArgs;
        fs::path serverModels;
        fs::path model;
        const char* note;
        bool serverUp = true;
    };
    const fs::path empty = TempFolder("empty");
    const fs::path broken = TempFolder("broken");
    {
        std::ofstream(broken / "broken.onnx") << "not a model";
    }
    std::vector<Case> cases = {
        {"server down", {}, Models(), Models() / "tiny.onnx", "since the server isn't answering", false},
        {"wrong key", {"--key", "k3y-right"}, Models(), Models() / "tiny.onnx", "since the server refused the access key"},
        {"model not found", {}, empty, Models() / "tiny.onnx", "since the server lacks it or has a different copy"},
        {"different file", {}, Models() / "other", Models() / "tiny.onnx", "since the server lacks it or has a different copy"},
        {"failed to load", {}, broken, broken / "broken.onnx", "since the server couldn't load it"},
        {"not allowed", {"--allow", "a2f3_fp16.onnx"}, Models(), Models() / "tiny.onnx", "since the server couldn't load it"},
    };
    for (const Case& c : cases) {
        std::unique_ptr<Server> server;
        int port = DeadPort();
        if (c.serverUp) {
            server = std::make_unique<Server>(c.serverModels, c.serverArgs);
            port = server->Port();
        }
        Settings s = On(port);
        s.accessKey = "k3y-sent";
        Harness h(s);
        OrtSession* session = nullptr;
        OrtStatus* status = h.Create(c.model, {{"batch", 2}}, &session);
        CHECK(status != nullptr);
        const std::string message = h.api->GetErrorMessage(status);
        CHECK_EQ(int(h.api->GetErrorCode(status)), int(ORT_FAIL));
        h.api->ReleaseStatus(status);
        CHECK(message.rfind("BFA Remote Bridge: ", 0) == 0);
        CHECK(message.find(c.model.filename().string()) != std::string::npos);
        CHECK(session == nullptr);
        CHECK_EQ(h.bridge.BridgeSessions(), std::size_t{0});
        CHECK_EQ(h.notes.size(), std::size_t{1});
        CHECK(h.notes[0].find(c.note) != std::string::npos);
        CHECK(h.notes[0].rfind("BFA Remote Bridge: " + c.model.filename().string() + ": lines get the game's own lip sync", 0) == 0);
        CHECK(message.find("k3y") == std::string::npos);
        std::printf("        %s: %s\n", c.what, message.c_str());
    }
    fs::remove_all(empty);
    fs::remove_all(broken);
}

// A failure the connection can't survive (a malformed answer, the wrong request id) pauses the bridge: NaN at once,
// nothing more logged, until the background thread's HELLO is answered. An answer that doesn't fit the call falls back
// without a pause. A single timeout keeps the connection; a second in a row pauses.
TEST(ForwardFailuresAnswerNanAndPause) {
    Need();
    for (const auto& [what, args, timeout, pauses] : std::vector<std::tuple<const char*, std::vector<std::string>, int, bool>>{
             {"malformed answer", {"--fault", "malformed"}, 2000, true},
             {"wrong request id", {"--fault", "request-id"}, 2000, true},
             {"wrong output name", {"--fault", "output-name"}, 2000, false},
             {"timeouts", {"--delay-ms", "1500"}, 300, true}}) {
        Server server(Models(), args);
        Settings s = On(server.Port());
        s.timeoutMs = timeout;
        Harness h(s);
        OrtSession* session = nullptr;
        Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session));  // HELLO is fine; RUN isn't
        TinyInputs in(h.api, h.cpu, 3);
        const auto run = [&] {
            Outputs out(h.api, 5);
            CHECK(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()) == nullptr);
            for (auto* v : out.v) CHECK(IsNanAnswer(h.api, v));
        };
        const auto start = Clock::now();
        run();
        if (std::string(what) == "timeouts") {
            CHECK(!h.bridge.Connection().Paused());  // one timeout keeps the connection
            run();
        }
        CHECK_EQ(h.bridge.Connection().Paused(), pauses);
        CHECK_EQ(h.CountLogged("until the server answers again"), pauses ? 1 : 0);
        CHECK(h.CountLogged("tiny.onnx: not answered (") >= 1);
        const auto during = Clock::now();
        for (int k = 0; k < 20; ++k) {
            Outputs out(h.api, 5);
            CHECK(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()) == nullptr);
            CHECK(IsNanAnswer(h.api, out.v[0]));
            if (!pauses) break;
        }
        const auto fast = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - during).count();
        if (pauses) {
            CHECK(fast < 200);  // during the pause: NaN at once, nothing more logged
            CHECK_EQ(h.CountLogged("until the server answers again"), 1);
            // the background thread's HELLO ends the pause, and the next run tries again
            g_now += std::chrono::seconds(1);
            h.bridge.Connection().Tick();
            CHECK(!h.bridge.Connection().Paused());
            CHECK_EQ(h.CountLogged("The pause ends after 1 s"), 1);
        }
        const Stats st = h.bridge.Statistics();
        CHECK_EQ(st.forwarded, 0u);
        const Counters c = h.bridge.Connection().Totals();
        CHECK_EQ(c.answered, 0u);
        CHECK_EQ(c.fallbacks, st.unanswered);
        CHECK(h.bridge.IsBridgeSession(session));  // the model stays
        std::printf("        %s: first answer after %lld ms, the next runs in %lld ms; %llu fallbacks, %llu timeouts\n", what,
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(during - start).count()),
                    static_cast<long long>(fast), static_cast<unsigned long long>(c.fallbacks),
                    static_cast<unsigned long long>(c.timeouts));
        h.api->ReleaseSession(session);
    }
}

// The server answers in order: a late RUN_OK after a single timeout is read and discarded by the next run, which gets
// its own answer on the same connection.
TEST(ForwardLateAnswerIsDiscarded) {
    Need();
    Server server(Models(), {"--delay-ms", "600", "--fault-count", "1"});
    Settings s = On(server.Port());
    s.timeoutMs = 300;
    s.logDetail = LogDetail::Detailed;
    Harness h(s);
    OrtSession* session = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session));
    TinyInputs in(h.api, h.cpu, 3);
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(IsNanAnswer(h.api, out.v[0]));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // the late answer is on its way
    const int connects = h.bridge.Connection().Reconnects();
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    }
    CHECK_EQ(h.bridge.Connection().Reconnects(), connects);
    CHECK(!h.bridge.Connection().Paused());
    CHECK_EQ(h.CountLogged("Discarded the late answer to request"), 1);
    CHECK(h.bridge.Connection().Totals() == (Counters{1, 1, 1}));
    // the monitor: the model's line, the fallback's note, then back on the server
    CHECK_EQ(h.CountNotes("tiny.onnx: lines get the game's own lip sync, since the server didn't answer in time."), 1);
    CHECK_EQ(h.CountNotes("tiny.onnx: lines are back on the server."), 1);
    // the server told, by a status check with the reason
    h.bridge.Connection().Tick();
    CHECK_EQ(h.bridge.Connection().FallbackReports(), 1);
    h.api->ReleaseSession(session);
}

// A late ERROR that ends the connection (here 7, the server stopping), then the background thread's reconnect.
TEST(ForwardLateClosingErrorAndTheReconnect) {
    Need();
    Server server(Models(), {"--delay-ms", "600", "--run-error", "7", "--fault-count", "1"});
    Settings s = On(server.Port());
    s.timeoutMs = 300;
    Harness h(s);
    OrtSession* session = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session));
    TinyInputs in(h.api, h.cpu, 3);
    const auto run = [&] {
        Outputs out(h.api, 5);
        Ok(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        return !IsNanAnswer(h.api, out.v[0]);
    };
    CHECK(!run());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(!run());  // the late ERROR 7 comes first: the connection ends
    CHECK(h.bridge.Connection().Paused());
    CHECK(h.Logged("which ends the connection"));
    g_now += std::chrono::seconds(1);
    h.bridge.Connection().Tick();
    CHECK(!h.bridge.Connection().Paused());
    CHECK(run());
    h.api->ReleaseSession(session);
}

// PING after a minute idle, answered by PONG on the main connection.
TEST(ForwardIdlePing) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    OrtSession* session = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session));
    TinyInputs in(h.api, h.cpu, 3);
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
    }
    g_now += std::chrono::seconds(61);
    h.bridge.Connection().Tick();
    CHECK_EQ(h.bridge.Connection().Pings(), 1);
    CHECK(!h.bridge.Connection().Paused());
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    }
    h.api->ReleaseSession(session);
}

TEST(ForwardServerGoneAfterLoad) {
    Need();
    auto server = std::make_unique<Server>(Models());
    Harness h(On(server->Port()));
    OrtSession* session = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session));
    server.reset();  // the server stops
    TinyInputs in(h.api, h.cpu, 3);
    Outputs out(h.api, 5);
    CHECK(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()) == nullptr);
    CHECK(IsNanAnswer(h.api, out.v[0]));
    CHECK_EQ(h.CountLogged("until the server answers again"), 1);
    CHECK_EQ(h.CountLogged("tiny.onnx: not answered (no connection"), 1);
    h.api->ReleaseSession(session);
}

// No server address: every model load is refused (nothing runs on this PC), lines get the game's own lip sync, and
// the note says so once, however often BFA tries again; the same refusal is logged once.
TEST(ForwardNoServerAddressRefusesEveryLoad) {
    Need();
    Harness h(Settings{});
    for (int attempt = 0; attempt < 3; ++attempt) {
        OrtSession* session = nullptr;
        OrtStatus* st = h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session);
        CHECK(st != nullptr);
        const std::string message = h.api->GetErrorMessage(st);
        h.api->ReleaseStatus(st);
        CHECK(message.find("tiny.onnx isn't loaded, since no server address is set") != std::string::npos);
        CHECK(session == nullptr);
    }
    CHECK_EQ(h.bridge.BridgeSessions(), std::size_t{0});
    CHECK_EQ(h.CountNotes("no server address is set, so lip sync is the game's own"), 1);
    CHECK_EQ(h.notes.size(), std::size_t{1});
    CHECK_EQ(h.CountLogged("tiny.onnx isn't loaded, since no server address is set"), 1);
    const Stats stats = h.bridge.Statistics();
    CHECK_EQ(stats.forwarded + stats.unanswered, 0u);
}

TEST(ForwardHelloAgainWhenSessionsChange) {
    Need();
    const fs::path folder = TempFolder("two");
    fs::copy_file(Models() / "tiny.onnx", folder / "tiny.onnx");
    fs::copy_file(Models() / "other" / "tiny.onnx", folder / "second.onnx");
    Server server(folder);
    Harness h(On(server.Port()));
    OrtSession* a = nullptr;
    OrtSession* b = nullptr;
    Ok(h.Create(folder / "tiny.onnx", {{"batch", 2}}, &a));
    CHECK_EQ(h.bridge.Connection().SideHellos(), 1);
    Ok(h.Create(folder / "second.onnx", {{"batch", 2}}, &b));
    CHECK_EQ(h.bridge.Connection().SideHellos(), 2);
    CHECK_EQ(h.bridge.Connection().HellosSent(), 0);  // each load on a side connection of its own
    TinyInputs in(h.api, h.cpu, 9);
    const auto run = [&](OrtSession* s) {
        Outputs out(h.api, 5);
        Ok(h.api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    };
    run(a);
    run(b);
    CHECK_EQ(h.bridge.Connection().HellosSent(), 1);  // the main connection's, with both
    h.api->ReleaseSession(b);
    run(a);  // a stays ready on the connection: no HELLO in the run
    CHECK_EQ(h.bridge.Connection().HellosSent(), 1);
    h.bridge.Connection().Tick();  // the set changed: the background says HELLO again
    CHECK_EQ(h.bridge.Connection().HellosSent(), 2);
    run(a);
    CHECK_EQ(h.bridge.Connection().HellosSent(), 2);
    h.api->ReleaseSession(a);
    fs::remove_all(folder);
}

TEST(ForwardGpuModeIsLoggedAndPassedThrough) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    // the DirectML provider isn't in the Linux runtime: the wrapped calls pass its refusal on unchanged
    const void* dml = nullptr;
    OrtStatus* st = h.api->GetExecutionProviderApi("DML", ORT_API_VERSION, &dml);
    const OrtApi* real = RealApi();
    const void* realDml = nullptr;
    OrtStatus* realSt = real->GetExecutionProviderApi("DML", ORT_API_VERSION, &realDml);
    CHECK((st == nullptr) == (realSt == nullptr));
    if (st) h.api->ReleaseStatus(st);
    if (realSt) real->ReleaseStatus(realSt);
    OrtSession* s1 = nullptr;
    OrtSession* s2 = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s1, true));
    h.api->ReleaseSession(s1);
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s2, true));
    CHECK_EQ(h.CountLogged("set to GPU"), 1);
    h.api->ReleaseSession(s2);
    // the note on screen: once per session, whatever Status on load says (it asks the player to act)
    CHECK_EQ(h.CountNotes("BFA is set to GPU"), 1);
    CHECK_EQ(h.CountNotes("Tick Force CPU in BFA's menu"), 1);
    Settings quiet = On(server.Port());
    quiet.showStatus = false;
    Harness q(quiet);
    OrtSession* s3 = nullptr;
    Ok(q.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s3, true));
    CHECK_EQ(q.CountNotes("BFA is set to GPU"), 1);
    CHECK_EQ(q.CountNotes(": ready"), 0);  // the ready line follows Status on load
    q.api->ReleaseSession(s3);
    // not in CPU mode
    Harness cpu(On(server.Port()));
    OrtSession* s4 = nullptr;
    Ok(cpu.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s4));
    CHECK_EQ(cpu.CountNotes("BFA is set to GPU"), 0);
    cpu.api->ReleaseSession(s4);
}

namespace {

// A runtime of another version: its OrtApiBase answers GetVersionString with `text` and GetApi (up to `maxApi`) with
// a table of exactly `entries` slots on the heap, so that a read past its end shows under AddressSanitizer. Its first
// entries are the real 1.24.4 runtime's; any past 1.24.4's mark that they were called.
std::atomic<bool> g_extraCalled{false};
OrtStatus* ORT_API_CALL ExtraEntry() noexcept {
    g_extraCalled = true;
    return nullptr;
}

struct FakeRuntime {
    static inline std::string text;
    static inline unsigned maxApi = 0;
    static inline std::unique_ptr<void*[]> slots;
    static inline OrtApiBase base{};
    static const char* ORT_API_CALL Version() noexcept { return text.c_str(); }
    static const OrtApi* ORT_API_CALL Api(std::uint32_t v) noexcept { return v <= maxApi ? reinterpret_cast<const OrtApi*>(slots.get()) : nullptr; }
    static const OrtApiBase* ORT_API_CALL GetBase() noexcept { return &base; }

    // Hooks the fake runtime in and returns the bridge's base for it.
    static const OrtApiBase* Install(const char* version, unsigned api, std::size_t entries) {
        text = version;
        maxApi = api;
        slots = std::make_unique<void*[]>(entries);
        std::memcpy(slots.get(), RealApi(), std::min(entries, kKnownOrtEntries) * sizeof(void*));
        for (std::size_t i = kKnownOrtEntries; i < entries; ++i) {
            OrtStatus* (ORT_API_CALL* extra)() noexcept = &ExtraEntry;
            std::memcpy(&slots[i], &extra, sizeof(void*));
        }
        base.GetApi = &Api;
        base.GetVersionString = &Version;
        g_extraCalled = false;
        return Bridge::Hook(&GetBase)();
    }
};

void* const* Slots(const OrtApi* api) { return reinterpret_cast<void* const*>(api); }

// The tiny model served and run through `api`, as BFA would with that table.
void ServeAndRun(Harness& h, const OrtApi* api, int port) {
    h.api = api;
    OrtSession* s = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s));
    CHECK(h.bridge.IsBridgeSession(s));
    TinyInputs in(api, h.cpu, 3);
    Outputs out(api, 5);
    Ok(api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
    CHECK(!IsNanAnswer(api, out.v[0]));
    const auto local = LocalRun(Models() / "tiny.onnx", {{"batch", 2}}, kTinyIn, in.values, kTinyOut, 5);
    CHECK(Bytes(api, out.v[0]) == local[0]);
    api->ReleaseSession(s);
    (void)port;
}

}  // namespace

// An older API version asked of 1.24.4 gets the bridge's table, the same as 24's.
TEST(ForwardOlderApiVersionOfTheSameRuntime) {
    Need();
    Harness h(Settings{});
    const OrtApiBase* base = Bridge::Hook(RealGetApiBase())();
    const OrtApi* t24 = base->GetApi(ORT_API_VERSION);
    CHECK(t24 != RealApi());
    CHECK(base->GetApi(20) == t24);
    CHECK(Bridge::State() == Bridge::ApiState::Served);
    CHECK_EQ(Bridge::ServedApiVersion(), 20u);
    CHECK(std::strcmp(base->GetVersionString(), "1.24.4") == 0);
    CHECK(base->GetApi(ORT_API_VERSION + 1) == nullptr);  // 1.24.4 hasn't 25: BFA's own "wrong version"
    h.api = base->GetApi(ORT_API_VERSION);
}

// An older runtime: only the entries its API version has are copied, never one past its shorter table, and the
// bridge serves BFA through it (MemoryInfoGetDeviceType, since 14, isn't asked of a runtime before it).
TEST(ForwardAnOlderRuntimeIsServedFromItsShorterTable) {
    Need();
    Server server(Models());
    for (const auto& [text, api] : std::vector<std::pair<const char*, unsigned>>{{"1.20.1", 20}, {"1.12.0", 12}}) {
        Harness h(On(server.Port()));
        const std::size_t entries = OrtApiEntries(api);
        const OrtApiBase* base = FakeRuntime::Install(text, api, entries);
        const OrtApi* t = base->GetApi(api);
        CHECK(t != nullptr);
        CHECK(t != reinterpret_cast<const OrtApi*>(FakeRuntime::slots.get()));
        CHECK(Bridge::State() == Bridge::ApiState::Served);
        CHECK(Slots(t)[entries - 1] != nullptr);
        CHECK(Slots(t)[entries] == nullptr);  // past the runtime's own table: nothing copied
        CHECK(Slots(t)[kKnownOrtEntries - 1] == nullptr);
        ServeAndRun(h, t, server.Port());
        CHECK(!h.Logged("can't serve"));
        std::printf("        ONNX Runtime %s: %zu entries copied, the tiny model served\n", text, entries);
    }
}

// A newer API version: 1.24.4's entries wrapped as for 24, and every entry past them answered by the bridge with an
// error status naming it, never the runtime's; the log and the monitor name it, once.
TEST(ForwardANewerRuntimesExtraEntriesRefuse) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    const std::size_t extra = 40;
    const OrtApiBase* base = FakeRuntime::Install("1.26.0", 26, kKnownOrtEntries + extra);
    const OrtApi* t = base->GetApi(26);
    CHECK(t != nullptr);
    CHECK(Bridge::State() == Bridge::ApiState::Served);
    CHECK_EQ(Bridge::ServedApiVersion(), 26u);
    ServeAndRun(h, t, server.Port());
    using Entry = OrtStatus* (ORT_API_CALL*)() noexcept;
    for (std::size_t index : {kKnownOrtEntries, kKnownOrtEntries + 5, kKnownOrtEntries + 5, kKnownOrtEntries + extra - 1}) {
        Entry e = nullptr;
        std::memcpy(&e, &Slots(t)[index], sizeof e);
        CHECK(e != nullptr);
        OrtStatus* st = e();
        CHECK(st != nullptr);
        const std::string message = t->GetErrorMessage(st);
        CHECK(message.find("ONNX Runtime entry " + std::to_string(index)) != std::string::npos);
        CHECK_EQ(int(t->GetErrorCode(st)), int(ORT_NOT_IMPLEMENTED));
        t->ReleaseStatus(st);
    }
    CHECK(!g_extraCalled.load());  // never the runtime's
    CHECK_EQ(h.CountLogged("called ONNX Runtime entry " + std::to_string(kKnownOrtEntries + 5)), 1);  // once per entry
    CHECK_EQ(h.CountLogged("Update the bridge"), 3);
    CHECK(h.CountNotes("runtime entry " + std::to_string(kKnownOrtEntries)) == 1);
    // the model editor's 1.24.4 layout isn't a newer version's: no editor
    CHECK(t->GetModelEditorApi() == nullptr);
}

// What the bridge can't serve gets the real table, untouched: BFA runs as it would without the bridge, and the note
// says to update the bridge.
TEST(ForwardWhatTheBridgeCantServeIsTheRealTable) {
    Need();
    Harness h(Settings{});
    const OrtApi* t24 = h.api;
    const OrtApiBase* base = FakeRuntime::Install("1.9.0", 9, OrtApiEntries(10));
    CHECK(base->GetApi(9) == reinterpret_cast<const OrtApi*>(FakeRuntime::slots.get()));
    CHECK(Bridge::State() == Bridge::ApiState::CantServe);
    CHECK(h.Logged("ONNX Runtime 1.9.0 is older than the bridge serves"));
    base = FakeRuntime::Install("1.31.0", 31, kKnownOrtEntries);
    CHECK(base->GetApi(31) == reinterpret_cast<const OrtApi*>(FakeRuntime::slots.get()));
    CHECK(h.Logged("asked for API version 31, newer than the bridge serves"));
    base = FakeRuntime::Install("unknown", 24, kKnownOrtEntries);
    CHECK(base->GetApi(24) == reinterpret_cast<const OrtApi*>(FakeRuntime::slots.get()));
    CHECK_EQ(h.CountNotes("so BFA runs on this PC as it would without the bridge. Update the bridge."), 1);  // once
    // back to the real runtime, for the harness's own release
    h.api = Bridge::Hook(RealGetApiBase())()->GetApi(ORT_API_VERSION);
    CHECK(h.api == t24);
}

// BFA's own Performance and Old models (never committed): BFARB_BFA_MODELS names their folder.
TEST(ForwardBfaModels) {
    Need();
    const std::string dir = Env("BFARB_BFA_MODELS");
    if (dir.empty()) SKIP("BFARB_BFA_MODELS isn't set");
    const char* threads = std::getenv("BFARB_SERVER_THREADS");  // 2, BFA's own, by default
    Server server(dir, {"--threads", threads ? threads : "2"});
    Settings s = On(server.Port());
    s.timeoutMs = 5000;
    Harness h(s);
    std::mt19937 rng(11);
    std::normal_distribution<float> n(0.f, 1.f);

    // Performance (Thin120): mel [1, F, 80], identity [1] int64, emotion [1, F, 10]; BFA's window is fixed per run
    {
        OrtSession* session = nullptr;
        const auto t0 = Clock::now();
        Ok(h.Create(fs::path(dir) / "performance.onnx", {{"batch", 1}}, &session));
        const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        const int frames = 120;
        for (int call = 0; call < 4; ++call) {
            std::vector<float> mel(frames * 80), emo(frames * 10);
            std::int64_t id = call % 2;  // the model holds two identities
            for (auto& v : mel) v = call ? n(rng) : 0.f;
            for (auto& v : emo) v = call ? std::fabs(n(rng)) * 0.1f : 0.f;
            const std::int64_t melShape[] = {1, frames, 80}, idShape[] = {1}, emoShape[] = {1, frames, 10};
            OrtValue* vals[3] = {};
            Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, mel.data(), mel.size() * 4, melShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vals[0]));
            Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, &id, 8, idShape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &vals[1]));
            Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, emo.data(), emo.size() * 4, emoShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vals[2]));
            const char* in[] = {"mel", "identity", "emotion"};
            const char* out[] = {"normalized_motion"};
            Outputs o(h.api, 1);
            Ok(h.api->Run(session, nullptr, in, vals, 3, out, 1, o.v.data()));
            CHECK(!IsNanAnswer(h.api, o.v[0]));
            for (auto* v : vals) h.api->ReleaseValue(v);
        }
        std::printf("        performance.onnx: admitted (hash, HELLO, server load and warm-up) in %lld ms\n", static_cast<long long>(loadMs));
        h.api->ReleaseSession(session);
    }
    // Old (Audio2Face-3D v2.3 Mark): input [b, 1, 8320], emotion [b, 1, 26]; BFA's batch 2
    {
        OrtSession* session = nullptr;
        const auto t0 = Clock::now();
        Ok(h.Create(fs::path(dir) / "network_fp16.onnx", {{"input_dynamic_axes_1", 2}, {"emotion_dynamic_axes_1", 2}}, &session));
        const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        for (int call = 0; call < 4; ++call) {
            std::vector<float> x(2 * 8320), emo(2 * 26);
            for (auto& v : x) v = call ? n(rng) * 0.1f : 0.f;
            for (auto& v : emo) v = call ? std::fabs(n(rng)) * 0.1f : 0.f;
            const std::int64_t xShape[] = {2, 1, 8320}, eShape[] = {2, 1, 26};
            OrtValue* vals[2] = {};
            Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, x.data(), x.size() * 4, xShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vals[0]));
            Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, emo.data(), emo.size() * 4, eShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vals[1]));
            const char* in[] = {"input", "emotion"};
            const char* out[] = {"result"};
            Outputs o(h.api, 1);
            Ok(h.api->Run(session, nullptr, in, vals, 2, out, 1, o.v.data()));
            std::vector<std::int64_t> dims;
            Bytes(h.api, o.v[0], &dims);
            CHECK(dims == (std::vector<std::int64_t>{2, 1, 301}));
            for (auto* v : vals) h.api->ReleaseValue(v);
        }
        std::printf("        network_fp16.onnx: admitted (hash, HELLO, server load and warm-up) in %lld ms\n", static_cast<long long>(loadMs));
        h.api->ReleaseSession(session);
    }
    const Stats st = h.bridge.Statistics();
    std::printf("        %llu runs answered, %llu not; average round trip %.2f ms, server %.2f ms\n",
                static_cast<unsigned long long>(st.forwarded), static_cast<unsigned long long>(st.unanswered),
                st.forwarded ? st.roundTripMicros / 1000.0 / st.forwarded : 0.0, st.forwarded ? st.serverMicros / 1000.0 / st.forwarded : 0.0);
    CHECK_EQ(st.forwarded, 8u);
    std::lock_guard lock(h.mutex);
    for (const auto& l : h.log)
        if (l.rfind("Model ", 0) == 0) std::printf("        log: %s\n", l.c_str());
}

// An input the protocol can't carry (a bool tensor): the line gets NaN, logged once per session, and no backoff.
TEST(ForwardUnsupportedInputAnswersNan) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    OrtSession* session = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &session));
    TinyInputs in(h.api, h.cpu, 1);
    bool flags[2] = {true, false};
    OrtValue* b = nullptr;
    const std::int64_t shape[] = {2};
    Ok(h.api->CreateTensorWithDataAsOrtValue(h.cpu, flags, sizeof flags, shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL, &b));
    std::vector<OrtValue*> values = in.values;
    values[4] = b;
    for (int k = 0; k < 3; ++k) {
        Outputs out(h.api, 5);
        CHECK(h.api->Run(session, nullptr, kTinyIn, values.data(), 5, kTinyOut, 5, out.v.data()) == nullptr);
        CHECK(IsNanAnswer(h.api, out.v[0]));
    }
    CHECK_EQ(h.CountLogged("which the protocol can't carry"), 1);
    CHECK(!h.bridge.Connection().Paused());
    {
        Outputs out(h.api, 5);  // the next proper line goes through
        Ok(h.api->Run(session, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    }
    h.api->ReleaseValue(b);
    h.api->ReleaseSession(session);
}

// Every other runtime entry that takes a session answers a bridge session with an error, without the runtime; a real
// session's call is the real one.
TEST(ForwardOtherSessionEntriesRefuseBridgeSessions) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    OrtSession* bridged = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &bridged));
    const auto refused = [&](OrtStatus* st) {
        const bool ok = st && std::string(h.api->GetErrorMessage(st)).rfind("BFA Remote Bridge: ", 0) == 0;
        if (st) h.api->ReleaseStatus(st);
        return ok;
    };
    std::size_t count = 99;
    CHECK(refused(h.api->SessionGetInputCount(bridged, &count)));
    CHECK_EQ(count, std::size_t{99});
    OrtAllocator* allocator = nullptr;
    Ok(h.api->GetAllocatorWithDefaultOptions(&allocator));
    char* name = nullptr;
    CHECK(refused(h.api->SessionGetOutputName(bridged, 0, allocator, &name)));
    CHECK(name == nullptr);
    OrtModelMetadata* metadata = nullptr;
    CHECK(refused(h.api->SessionGetModelMetadata(bridged, &metadata)));
    OrtIoBinding* binding = nullptr;
    CHECK(refused(h.api->CreateIoBinding(bridged, &binding)));
    if (const OrtModelEditorApi* editor = h.api->GetModelEditorApi()) {
        int opset = 0;
        CHECK(refused(editor->SessionGetOpsetForDomain(bridged, "", &opset)));
    }
    h.api->ReleaseSession(bridged);

    // a real session (made outside the bridge: the bridge makes none): the real answers through the same table
    const OrtApi* rt = RealApi();
    OrtSessionOptions* so = nullptr;
    Ok(rt->CreateSessionOptions(&so));
    OrtSession* real = nullptr;
    Ok(rt->CreateSession(h.env, (Models() / "tiny.onnx").c_str(), so, &real));
    rt->ReleaseSessionOptions(so);
    Ok(h.api->SessionGetInputCount(real, &count));
    CHECK_EQ(count, std::size_t{5});
    Ok(h.api->SessionGetOutputName(real, 0, allocator, &name));
    CHECK_EQ(std::string(name), std::string("y"));
    allocator->Free(allocator, name);
    Ok(h.api->CreateIoBinding(real, &binding));
    h.api->ReleaseIoBinding(binding);
    h.api->ReleaseSession(real);
}

// BFA can drop a session without releasing it: a newer session of the same model with other overrides is admitted,
// and the old one answers NaN, without a backoff.
TEST(ForwardNewestSessionReplacesALeakedOne) {
    Need();
    Server server(Models());
    Harness h(On(server.Port()));
    OrtSession* leaked = nullptr;
    OrtSession* fresh = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &leaked));
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}, {"unused_dimension", 1}}, &fresh));
    TinyInputs in(h.api, h.cpu, 4);
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(leaked, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(IsNanAnswer(h.api, out.v[0]));
    }
    CHECK(!h.bridge.Connection().Paused());
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(fresh, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    }
    CHECK_EQ(h.CountLogged("until the server answers again"), 0);
    h.api->ReleaseSession(fresh);
    h.api->ReleaseSession(leaked);
}

// Before the first settings read: refused, as "not read yet" while SkyrimNet is loaded (its config may not be
// registered yet; BFA tries again), and as having no server address when it isn't. Nothing loads on this PC.
TEST(ForwardBeforeTheSettingsAreRead) {
    Need();
    {
        Harness h(Settings{}, false, true);
        OrtSession* s = nullptr;
        OrtStatus* st = h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s);
        CHECK(st != nullptr);
        CHECK_EQ(std::string(h.api->GetErrorMessage(st)),
                 std::string("BFA Remote Bridge: tiny.onnx isn't loaded, since the bridge's settings weren't read yet; lines get the game's own lip sync"));
        h.api->ReleaseStatus(st);
        CHECK(s == nullptr);
        CHECK(h.notes.empty());  // a moment of start-up, not worth a note
    }
    {
        Harness h(Settings{}, false, false);
        OrtSession* s = nullptr;
        OrtStatus* st = h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s);
        CHECK(st != nullptr);
        CHECK(std::string(h.api->GetErrorMessage(st)).find("SkyrimNet isn't loaded, so the bridge has no server address") != std::string::npos);
        h.api->ReleaseStatus(st);
        CHECK(s == nullptr);
        CHECK_EQ(h.CountNotes("SkyrimNet wasn't found"), 1);
    }
}

// A connection the server closed while idle (here after a second) is opened again in the same call.
TEST(ForwardReconnectsAfterTheServerClosesAnIdleConnection) {
    Need();
    Server server(Models(), {"--idle-seconds", "1"});
    Harness h(On(server.Port()));
    OrtSession* s = nullptr;
    Ok(h.Create(Models() / "tiny.onnx", {{"batch", 2}}, &s));
    TinyInputs in(h.api, h.cpu, 2);
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
    {
        Outputs out(h.api, 5);
        Ok(h.api->Run(s, nullptr, kTinyIn, in.values.data(), 5, kTinyOut, 5, out.v.data()));
        CHECK(!IsNanAnswer(h.api, out.v[0]));
    }
    CHECK(h.Logged("had closed the connection; reconnecting"));
    CHECK_EQ(h.CountLogged("until the server answers again"), 0);
    CHECK_EQ(h.bridge.Statistics().forwarded, 2u);
    h.api->ReleaseSession(s);
}

// ONNX Runtime's own messages name the model's full path: what the bridge logs keeps the base name only.
TEST(ForwardRuntimeMessagesLoseTheirPaths) {
    Need();
    const fs::path folder = TempFolder("secret folder name");
    const fs::path missing = folder / "absent.onnx";
    const OrtApi* real = RealApi();
    OrtEnv* env = nullptr;
    Ok(real->CreateEnv(ORT_LOGGING_LEVEL_FATAL, "bfarb-scrub", &env));
    OrtSessionOptions* so = nullptr;
    Ok(real->CreateSessionOptions(&so));
    OrtSession* s = nullptr;
    OrtStatus* st = real->CreateSession(env, missing.c_str(), so, &s);
    CHECK(st != nullptr);
    const std::string message = real->GetErrorMessage(st);
    real->ReleaseStatus(st);
    real->ReleaseSessionOptions(so);
    real->ReleaseEnv(env);
    CHECK(message.find("secret folder name") != std::string::npos);  // the runtime's own text has it
    const std::string scrubbed = ScrubPaths(message, missing.string());
    CHECK(scrubbed.find("secret folder name") == std::string::npos);
    CHECK(scrubbed.find(fs::temp_directory_path().string()) == std::string::npos);
    CHECK(scrubbed.find("absent.onnx") != std::string::npos);
    std::printf("        %s\n", scrubbed.c_str());
    fs::remove_all(folder);
}
