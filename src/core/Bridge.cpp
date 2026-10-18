#include "core/Bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "core/Half.h"
#include "core/Scrub.h"
#include "core/Sha256.h"
#include "core/Version.h"

static_assert(ORT_API_VERSION == 24, "the vendored header must be ONNX Runtime 1.24.4's");

namespace bfarb {
namespace {

std::atomic<Bridge*> g_current{nullptr};
std::atomic<OrtGetApiBaseFn> g_realGetApiBase{nullptr};
std::atomic<const OrtApiBase*> g_realBase{nullptr};
std::atomic<const OrtApi*> g_realApi{nullptr};
std::atomic<int> g_state{0};
std::atomic<bool> g_unknownTold{false};
std::mutex g_tableMutex;
OrtApi g_table;
OrtApiBase g_base;

// OrtDmlApi's layout in ONNX Runtime 1.24.4's dml_provider_factory.h (MIT): eight entries, of which the three that
// append DirectML to a session's options are wrapped. The others keep the real pointers; their Direct3D types are
// opaque here.
struct DmlApi {
    OrtStatus*(ORT_API_CALL* AppendDml)(OrtSessionOptions* options, int deviceId) noexcept;
    OrtStatus*(ORT_API_CALL* AppendDml1)(OrtSessionOptions* options, void* dmlDevice, void* commandQueue) noexcept;
    void* createGpuAllocationFromD3DResource;
    void* freeGpuAllocation;
    void* getD3D12ResourceFromAllocation;
    OrtStatus*(ORT_API_CALL* AppendDml2)(OrtSessionOptions* options, void* deviceOptions) noexcept;
    void* getDmlDevice;
    void* getDmlCommandQueue;
};
static_assert(sizeof(DmlApi) == 8 * sizeof(void*));
std::atomic<const DmlApi*> g_realDml{nullptr};
DmlApi g_dml;

const OrtApi* Real() noexcept { return g_realApi.load(std::memory_order_acquire); }

// An error status is released, never passed on: these are the bridge's own reads.
bool Ok(const OrtApi* real, OrtStatus* status) noexcept {
    if (!status) return true;
    real->ReleaseStatus(status);
    return false;
}

std::string Utf8Name(const std::filesystem::path& path) {
    const std::u8string s = path.filename().u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string PathText(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string Dims(const std::vector<std::int64_t>& dims) {
    std::string s = "[";
    for (std::size_t i = 0; i < dims.size(); ++i) s += (i ? ", " : "") + std::to_string(dims[i]);
    return s + "]";
}

std::string Ms(std::uint64_t micros) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(micros) / 1000.0);
    return b;
}

// ---- the replaced entries

void MarkGpu(OrtSessionOptions* options) noexcept {
    if (Bridge* b = Bridge::Current()) {
        try {
            b->OnGpuOptions(options);
        } catch (...) {
        }
    }
}

OrtStatus* ORT_API_CALL WrapAppendDml(OrtSessionOptions* options, int deviceId) noexcept {
    OrtStatus* status = g_realDml.load()->AppendDml(options, deviceId);
    if (!status) MarkGpu(options);
    return status;
}

OrtStatus* ORT_API_CALL WrapAppendDml1(OrtSessionOptions* options, void* dmlDevice, void* commandQueue) noexcept {
    OrtStatus* status = g_realDml.load()->AppendDml1(options, dmlDevice, commandQueue);
    if (!status) MarkGpu(options);
    return status;
}

OrtStatus* ORT_API_CALL WrapAppendDml2(OrtSessionOptions* options, void* deviceOptions) noexcept {
    OrtStatus* status = g_realDml.load()->AppendDml2(options, deviceOptions);
    if (!status) MarkGpu(options);
    return status;
}

OrtStatus* ORT_API_CALL WrapGetProviderApi(const char* name, std::uint32_t version, const void** api) noexcept {
    OrtStatus* status = Real()->GetExecutionProviderApi(name, version, api);
    if (status || !api || !*api || !name || std::strcmp(name, "DML") != 0 || version != kOrtApiVersion) return status;
    const auto* real = static_cast<const DmlApi*>(*api);
    std::lock_guard lock(g_tableMutex);
    if (g_realDml.load() != real) {
        std::memcpy(&g_dml, real, sizeof(DmlApi));
        g_dml.AppendDml = real->AppendDml ? &WrapAppendDml : nullptr;
        g_dml.AppendDml1 = real->AppendDml1 ? &WrapAppendDml1 : nullptr;
        g_dml.AppendDml2 = real->AppendDml2 ? &WrapAppendDml2 : nullptr;
        g_realDml.store(real);
    }
    *api = &g_dml;
    return status;
}

OrtStatus* ORT_API_CALL WrapAppendProvider(OrtSessionOptions* options, const char* name, const char* const* keys,
                                           const char* const* values, std::size_t count) noexcept {
    OrtStatus* status = Real()->SessionOptionsAppendExecutionProvider(options, name, keys, values, count);
    if (!status && name && std::strcmp(name, "DML") == 0) MarkGpu(options);
    return status;
}

OrtStatus* ORT_API_CALL WrapOverride(OrtSessionOptions* options, const char* name, std::int64_t value) noexcept {
    OrtStatus* status = Real()->AddFreeDimensionOverrideByName(options, name, value);
    if (!status) {
        if (Bridge* b = Bridge::Current()) {
            try {
                b->OnOverride(options, name, value);
            } catch (...) {
            }
        }
    }
    return status;
}

void ORT_API_CALL WrapReleaseOptions(OrtSessionOptions* options) noexcept {
    if (Bridge* b = Bridge::Current()) {
        try {
            b->OnReleaseOptions(options);
        } catch (...) {
        }
    }
    Real()->ReleaseSessionOptions(options);
}

OrtStatus* ORT_API_CALL WrapCreateSession(const OrtEnv* env, const ORTCHAR_T* path, const OrtSessionOptions* options,
                                          OrtSession** out) noexcept {
    const OrtApi* real = Real();
    if (Bridge* b = Bridge::Current()) return b->CreateSession(real, env, path, options, out);
    return real->CreateSession(env, path, options, out);
}

void ORT_API_CALL WrapReleaseSession(OrtSession* session) noexcept {
    const OrtApi* real = Real();
    if (Bridge* b = Bridge::Current()) return b->ReleaseSession(real, session);
    real->ReleaseSession(session);
}

OrtStatus* ORT_API_CALL WrapRun(OrtSession* session, const OrtRunOptions* runOptions, const char* const* inputNames,
                                const OrtValue* const* inputs, std::size_t inputCount, const char* const* outputNames,
                                std::size_t outputCount, OrtValue** outputs) noexcept {
    const OrtApi* real = Real();
    if (Bridge* b = Bridge::Current())
        return b->DoRun(real, session, runOptions, inputNames, inputs, inputCount, outputNames, outputCount, outputs);
    return real->Run(session, runOptions, inputNames, inputs, inputCount, outputNames, outputCount, outputs);
}

// ---- every other entry that takes a session: a bridge session has nothing behind it in the runtime, so these answer it
// with an error status (BFA calls none of them); a real session's call is the real one

bool IsBridge(const OrtSession* session) noexcept {
    Bridge* b = Bridge::Current();
    try {
        return b && session && b->IsBridgeSession(session);
    } catch (...) {
        return false;
    }
}
inline bool IsBridge(OrtSession* session) noexcept { return IsBridge(static_cast<const OrtSession*>(session)); }
template <typename T>
bool IsBridge(const T&) noexcept {
    return false;
}

OrtStatus* NotOnBridgeSession(const OrtApi* real) noexcept {
    return real->CreateStatus(ORT_FAIL, "BFA Remote Bridge: this call isn't available on a session the server runs");
}

// The entries' types carry the header's warn_unused_result on GCC and Clang, which a template argument drops; harmless.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
#endif
template <typename Table, typename F, F Table::*Member>
struct Guard;
template <typename Table, typename... A, OrtStatus*(ORT_API_CALL* Table::*Member)(A...) noexcept>
struct Guard<Table, OrtStatus*(ORT_API_CALL*)(A...) noexcept, Member> {
    static inline const Table* real = nullptr;
    static OrtStatus* ORT_API_CALL Call(A... a) noexcept {
        if ((IsBridge(a) || ...)) return NotOnBridgeSession(Real());
        return (real->*Member)(a...);
    }
};

#define BFARB_GUARD(TABLE, COPY, REAL, NAME)                                                           \
    do {                                                                                               \
        using G = Guard<TABLE, decltype(TABLE::NAME), &TABLE::NAME>;                                   \
        G::real = (REAL);                                                                              \
        (COPY).NAME = &G::Call;                                                                        \
    } while (false)

// The 22 OrtApi entries of ONNX Runtime 1.24.4 that take an OrtSession* besides Run (wrapped on its own); with
// CreateSession and ReleaseSession, and the model editor's three, every entry that meets a session.
void GuardSessionEntries(OrtApi& table, const OrtApi* real) {
    BFARB_GUARD(OrtApi, table, real, SessionGetInputCount);
    BFARB_GUARD(OrtApi, table, real, SessionGetOutputCount);
    BFARB_GUARD(OrtApi, table, real, SessionGetOverridableInitializerCount);
    BFARB_GUARD(OrtApi, table, real, SessionGetInputTypeInfo);
    BFARB_GUARD(OrtApi, table, real, SessionGetOutputTypeInfo);
    BFARB_GUARD(OrtApi, table, real, SessionGetOverridableInitializerTypeInfo);
    BFARB_GUARD(OrtApi, table, real, SessionGetInputName);
    BFARB_GUARD(OrtApi, table, real, SessionGetOutputName);
    BFARB_GUARD(OrtApi, table, real, SessionGetOverridableInitializerName);
    BFARB_GUARD(OrtApi, table, real, SessionEndProfiling);
    BFARB_GUARD(OrtApi, table, real, SessionGetModelMetadata);
    BFARB_GUARD(OrtApi, table, real, CreateAllocator);
    BFARB_GUARD(OrtApi, table, real, RunWithBinding);
    BFARB_GUARD(OrtApi, table, real, CreateIoBinding);
    BFARB_GUARD(OrtApi, table, real, SessionGetProfilingStartTimeNs);
    BFARB_GUARD(OrtApi, table, real, RunAsync);
    BFARB_GUARD(OrtApi, table, real, SetEpDynamicOptions);
    BFARB_GUARD(OrtApi, table, real, SessionGetMemoryInfoForInputs);
    BFARB_GUARD(OrtApi, table, real, SessionGetMemoryInfoForOutputs);
    BFARB_GUARD(OrtApi, table, real, SessionGetEpDeviceForInputs);
    BFARB_GUARD(OrtApi, table, real, SessionGetEpDeviceForOutputs);
    BFARB_GUARD(OrtApi, table, real, Session_GetEpGraphAssignmentInfo);
}

std::atomic<const OrtModelEditorApi*> g_realEditor{nullptr};
OrtModelEditorApi g_editor;

const OrtModelEditorApi* ORT_API_CALL WrapGetModelEditorApi() noexcept {
    const OrtModelEditorApi* real = Real()->GetModelEditorApi();
    if (!real) return nullptr;
    std::lock_guard lock(g_tableMutex);
    if (g_realEditor.load() != real) {
        std::memcpy(&g_editor, real, sizeof(OrtModelEditorApi));
        BFARB_GUARD(OrtModelEditorApi, g_editor, real, SessionGetOpsetForDomain);
        BFARB_GUARD(OrtModelEditorApi, g_editor, real, ApplyModelToModelEditorSession);
        BFARB_GUARD(OrtModelEditorApi, g_editor, real, FinalizeModelEditorSession);
        g_realEditor.store(real);
    }
    return &g_editor;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

const OrtApi* ORT_API_CALL GetApi(std::uint32_t version) noexcept {
    const OrtApiBase* base = g_realBase.load(std::memory_order_acquire);
    if (!base) return nullptr;
    const OrtApi* real = base->GetApi(version);
    if (!real) return nullptr;
    if (version != kOrtApiVersion) {
        g_state.store(static_cast<int>(Bridge::ApiState::Unknown));
        if (!g_unknownTold.exchange(true)) {
            if (Bridge* b = Bridge::Current()) {
                try {
                    const char* v = base->GetVersionString ? base->GetVersionString() : nullptr;
                    b->LogLine(Level::Warn, "Better Facial Animations asked ONNX Runtime " + std::string(v ? v : "?") + " for API version " +
                                                std::to_string(version) + "; the bridge knows only " + kOrtVersionText +
                                                " (API version 24), so it passes everything through and BFA runs as if it weren't installed");
                    b->Tell(Notice{Notice::Kind::UnknownRuntime, std::string(), v ? v : ""});
                } catch (...) {
                }
            }
        }
        return real;
    }
    std::lock_guard lock(g_tableMutex);
    if (Real() != real) {
        std::memcpy(&g_table, real, sizeof(OrtApi));
        g_table.AddFreeDimensionOverrideByName = &WrapOverride;
        g_table.ReleaseSessionOptions = &WrapReleaseOptions;
        g_table.CreateSession = &WrapCreateSession;
        g_table.ReleaseSession = &WrapReleaseSession;
        g_table.Run = &WrapRun;
        g_table.GetExecutionProviderApi = &WrapGetProviderApi;
        g_table.SessionOptionsAppendExecutionProvider = &WrapAppendProvider;
        g_table.GetModelEditorApi = &WrapGetModelEditorApi;
        GuardSessionEntries(g_table, real);
        g_realApi.store(real, std::memory_order_release);
    }
    g_state.store(static_cast<int>(Bridge::ApiState::Known));
    return &g_table;
}

const OrtApiBase* ORT_API_CALL HookedGetApiBase() noexcept {
    const OrtGetApiBaseFn fn = g_realGetApiBase.load(std::memory_order_acquire);
    if (!fn) return nullptr;
    const OrtApiBase* real = fn();
    if (!real) return nullptr;
    std::lock_guard lock(g_tableMutex);
    g_realBase.store(real, std::memory_order_release);
    g_base.GetApi = &GetApi;
    g_base.GetVersionString = real->GetVersionString;
    if (Bridge* b = Bridge::Current()) {
        try {
            const char* v = real->GetVersionString ? real->GetVersionString() : nullptr;
            b->Connection().SetOrtVersion(v ? v : "");
            static std::atomic<bool> told{false};
            if (!told.exchange(true))
                b->LogLine(Level::Info, std::string("Better Facial Animations loaded ONNX Runtime ") + (v ? v : "?") + "; the bridge answers its OrtGetApiBase");
        } catch (...) {
        }
    }
    return &g_base;
}

struct Tensor {
    std::uint8_t dtype = 0;
    std::vector<std::int64_t> dims;
    void* data = nullptr;
    std::size_t bytes = 0;
};

// A tensor's type, shape and CPU data. False, with why, when it isn't a CPU tensor of a supported type.
bool ReadTensor(const OrtApi* real, const OrtValue* value, Tensor& t, std::string& why) {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (!value || !Ok(real, real->GetTensorTypeAndShape(value, &info)) || !info) {
        why = "isn't a tensor";
        return false;
    }
    ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::size_t rank = 0;
    bool ok = Ok(real, real->GetTensorElementType(info, &type)) && Ok(real, real->GetDimensionsCount(info, &rank)) && rank <= proto::kMaxRank;
    if (ok) {
        t.dims.assign(rank, 0);
        ok = rank == 0 || Ok(real, real->GetDimensions(info, t.dims.data(), rank));
    }
    real->ReleaseTensorTypeAndShapeInfo(info);
    if (!ok) {
        why = "has no readable shape";
        return false;
    }
    std::uint64_t bytes = 0;
    if (static_cast<int>(type) <= 0 || static_cast<int>(type) > 255 || !proto::ByteLength(static_cast<std::uint8_t>(type), t.dims, bytes)) {
        why = "is of a type the protocol doesn't carry (" + std::to_string(static_cast<int>(type)) + ")";
        return false;
    }
    t.dtype = static_cast<std::uint8_t>(type);
    t.bytes = static_cast<std::size_t>(bytes);
    const OrtMemoryInfo* memory = nullptr;
    OrtMemoryInfoDeviceType device = OrtMemoryInfoDeviceType_GPU;
    if (!Ok(real, real->GetTensorMemoryInfo(value, &memory)) || !memory) {
        why = "has no memory information";
        return false;
    }
    real->MemoryInfoGetDeviceType(memory, &device);
    if (device != OrtMemoryInfoDeviceType_CPU) {
        why = "isn't in CPU memory";
        return false;
    }
    if (!Ok(real, real->GetTensorMutableData(const_cast<OrtValue*>(value), &t.data)) || (bytes > 0 && !t.data)) {
        why = "has no readable data";
        return false;
    }
    return true;
}

// The largest absolute difference between two tensors of a floating type, or -1 for any other.
double LargestDifference(std::uint8_t dtype, const std::uint8_t* a, const std::uint8_t* b, std::size_t bytes) {
    double largest = 0.0;
    const auto take = [&](double x, double y) {
        if ((std::isnan(x) && std::isnan(y)) || x == y) return;  // equal infinities too
        const double d = std::fabs(x - y);
        largest = std::isnan(d) ? std::numeric_limits<double>::infinity() : std::max(largest, d);
    };
    switch (static_cast<proto::DType>(dtype)) {
        case proto::DType::Float:
            for (std::size_t i = 0; i + 4 <= bytes; i += 4) {
                float x, y;
                std::memcpy(&x, a + i, 4);
                std::memcpy(&y, b + i, 4);
                take(x, y);
            }
            return largest;
        case proto::DType::Double:
            for (std::size_t i = 0; i + 8 <= bytes; i += 8) {
                double x, y;
                std::memcpy(&x, a + i, 8);
                std::memcpy(&y, b + i, 8);
                take(x, y);
            }
            return largest;
        case proto::DType::Float16:
            for (std::size_t i = 0; i + 2 <= bytes; i += 2) {
                std::uint16_t x, y;
                std::memcpy(&x, a + i, 2);
                std::memcpy(&y, b + i, 2);
                take(HalfToFloat(x), HalfToFloat(y));
            }
            return largest;
        default:
            return -1.0;
    }
}

}  // namespace

Bridge::Bridge(TransportFactory factory, Resolver resolver, LogSink log, Client::Now now)
    : log_(log), client_(std::move(factory), std::move(resolver), std::move(log), now) {}

Bridge::~Bridge() {
    Bridge* self = this;
    g_current.compare_exchange_strong(self, nullptr);
    const OrtApi* real = Real();
    std::lock_guard lock(sessionsMutex_);
    for (auto& [_, s] : sessions_)
        if (s->local && real) real->ReleaseSession(s->local);
}

void Bridge::SetCurrent(Bridge* bridge) noexcept { g_current.store(bridge, std::memory_order_release); }
Bridge* Bridge::Current() noexcept { return g_current.load(std::memory_order_acquire); }

OrtGetApiBaseFn Bridge::Hook(OrtGetApiBaseFn real) noexcept {
    g_realGetApiBase.store(real, std::memory_order_release);
    return &HookedGetApiBase;
}

Bridge::ApiState Bridge::State() noexcept { return static_cast<ApiState>(g_state.load()); }

void Bridge::Log(Level level, const std::string& line) const {
    if (log_) log_(level, line);
}
void Bridge::LogLine(Level level, const std::string& line) const { Log(level, line); }

void Bridge::OnNotice(NoticeSink sink) {
    std::lock_guard lock(noticeMutex_);
    notice_ = std::move(sink);
}

void Bridge::Tell(const Notice& notice) {
    NoticeSink sink;
    {
        std::lock_guard lock(noticeMutex_);
        sink = notice_;
    }
    if (sink) sink(notice);
}

void Bridge::Apply(const Settings& settings) {
    {
        std::lock_guard lock(settingsMutex_);
        settings_ = settings;
    }
    settingsRead_.store(true);
    client_.Configure(settings);
}

Settings Bridge::CurrentSettings() const {
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

Stats Bridge::Statistics() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

std::size_t Bridge::BridgeSessions() const {
    std::lock_guard lock(sessionsMutex_);
    return sessions_.size();
}

bool Bridge::IsBridgeSession(const OrtSession* session) const {
    std::lock_guard lock(sessionsMutex_);
    return sessions_.count(session) > 0;
}

bool Bridge::HasLocalSession(const OrtSession* session) const {
    std::lock_guard lock(sessionsMutex_);
    const auto it = sessions_.find(session);
    return it != sessions_.end() && it->second->local;
}

void Bridge::OnOverride(OrtSessionOptions* options, const char* name, std::int64_t value) {
    if (!options || !name) return;
    std::lock_guard lock(sessionsMutex_);
    options_[options].overrides.push_back(proto::Override{name, value});
}

void Bridge::OnReleaseOptions(const OrtSessionOptions* options) {
    std::lock_guard lock(sessionsMutex_);
    options_.erase(options);
}

void Bridge::OnGpuOptions(const OrtSessionOptions* options) {
    if (!options) return;
    std::lock_guard lock(sessionsMutex_);
    options_[options].gpu = true;
}

std::vector<proto::ModelEntry> Bridge::LiveModelsLocked() const {
    std::vector<const Session*> live;
    for (const auto& [_, s] : sessions_) live.push_back(s.get());
    std::sort(live.begin(), live.end(), [](const Session* a, const Session* b) { return a->order < b->order; });
    std::vector<proto::ModelEntry> models;
    for (const Session* s : live) models.push_back(s->entry);
    return models;
}

void Bridge::PushModelsLocked() { client_.SetModels(LiveModelsLocked()); }

OrtStatus* Bridge::Refuse(const OrtApi* real, const std::string& model, const Admission& a) {
    Notice n;
    n.model = model;
    n.detail = a.detail;
    std::string why;
    using K = ProbeResult::Kind;
    if (a.hello.kind == K::NoAnswer) {
        n.kind = Notice::Kind::NoAnswer;
        why = "the server didn't answer (" + a.detail + ")";
    } else if (a.hello.kind == K::WrongKey) {
        n.kind = Notice::Kind::WrongKey;
        why = "the server refused the access key";
    } else if (a.hello.kind == K::Answered && (a.status == static_cast<std::uint8_t>(proto::ModelStatus::NotFound) ||
                                                a.status == static_cast<std::uint8_t>(proto::ModelStatus::DifferentFile))) {
        n.kind = Notice::Kind::Missing;
        why = "the server's copy: " + std::string(proto::ModelStatusName(a.status)) + (a.detail.empty() ? "" : " (" + a.detail + ")");
    } else {
        n.kind = Notice::Kind::CouldNotLoad;
        why = a.hello.kind == K::Answered
                  ? "the server's copy: " + std::string(proto::ModelStatusName(a.status)) + (a.detail.empty() ? "" : " (" + a.detail + ")")
                  : a.detail;
    }
    return RefuseWith(real, model, why, &n);
}

OrtStatus* Bridge::RefuseWith(const OrtApi* real, const std::string& model, const std::string& why, const Notice* notice) {
    const std::string message = "BFA Remote Bridge: " + model + " isn't loaded, since " + ScrubPaths(why) + "; lines get the game's own lip sync";
    Log(Level::Warn, message.substr(std::strlen("BFA Remote Bridge: ")));
    if (notice) Tell(*notice);
    return real->CreateStatus(ORT_FAIL, message.c_str());
}

void Bridge::ExpectSettings(bool expect) { expectSettings_.store(expect); }

bool Bridge::HashCached(const std::filesystem::path& file, std::uint64_t& size, Sha256::Digest& digest, std::string& err) {
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(file, ec);
    const auto written = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
    const std::string key = PathText(file);
    if (!ec) {
        std::lock_guard lock(hashMutex_);
        const auto it = hashes_.find(key);
        if (it != hashes_.end() && it->second.size == fileSize && it->second.written == written) {
            size = it->second.size;
            digest = it->second.digest;
            return true;
        }
    }
    if (!HashFile(file, size, digest, err)) return false;
    if (!ec) {
        std::lock_guard lock(hashMutex_);
        hashes_[key] = HashEntry{size, written, digest};
    }
    return true;
}

OrtStatus* Bridge::CreateSession(const OrtApi* real, const OrtEnv* env, const ORTCHAR_T* path, const OrtSessionOptions* options,
                                 OrtSession** out) noexcept {
    try {
        const Settings s = CurrentSettings();
        const bool read = settingsRead_.load();
        if (!path || !out) return real->CreateSession(env, path, options, out);
        const std::filesystem::path file(path);
        const std::string modelName = Utf8Name(file);
        if (!read && expectSettings_.load())
            return RefuseWith(real, modelName, "the bridge's settings weren't read yet", nullptr);
        if (!read || !s.enabled || s.serverAddress.empty()) {
            Log(Level::Info, "Model " + modelName + " loads on this PC as usual: " +
                                 (!read ? "SkyrimNet isn't loaded, so the bridge is idle"
                                        : !s.enabled ? "the bridge is off" : "no server address is set"));
            return real->CreateSession(env, path, options, out);
        }

        auto session = std::make_unique<Session>();
        session->entry.name = modelName;
        {
            std::lock_guard lock(sessionsMutex_);
            bool gpu = false;
            if (const auto it = options_.find(options); it != options_.end()) {
                session->entry.overrides = it->second.overrides;
                gpu = it->second.gpu;
            }
            if (gpu && !gpuLogged_) {
                gpuLogged_ = true;
                Log(Level::Info,
                    "Better Facial Animations is set to GPU: with the bridge no lip-sync inference runs on this PC's graphics card, but BFA "
                    "still opens a device on it; its Force CPU setting avoids even that");
            }
        }
        const std::string name = session->entry.name;
        if (!proto::ValidModelName(name)) {
            Admission a;
            a.hello.kind = ProbeResult::Kind::Refused;
            a.detail = "its file name can't be sent";
            return Refuse(real, name, a);
        }
        std::string err;
        if (!HashCached(file, session->entry.size, session->entry.sha256, err)) {
            Admission a;
            a.hello.kind = ProbeResult::Kind::Refused;
            a.detail = "the bridge couldn't read it to hash it (" + err + ")";
            return Refuse(real, name, a);
        }
        std::string overrides;
        for (const auto& o : session->entry.overrides) overrides += (overrides.empty() ? "" : ", ") + o.name + " = " + std::to_string(o.value);
        Log(Level::Info, "Model " + name + ": " + std::to_string(session->entry.size) + " bytes, SHA-256 " + Sha256::Hex(session->entry.sha256) +
                             (overrides.empty() ? ", no overrides" : ", overrides " + overrides) + "; asking the server");

        std::vector<proto::ModelEntry> live;
        {
            std::lock_guard lock(sessionsMutex_);
            live = LiveModelsLocked();
        }
        const Admission a = client_.Admit(live, session->entry);
        if (!a.Ready()) return Refuse(real, name, a);

        if (s.checkCalls > 0) {
            OrtSession* local = nullptr;
            OrtStatus* status = real->CreateSession(env, path, options, &local);
            if (status) {
                Log(Level::Warn, "Check mode: the real session of " + name + " couldn't be made here (" +
                                     ScrubPaths(real->GetErrorMessage(status), PathText(file)) + "), so its runs aren't checked");
                real->ReleaseStatus(status);
            } else {
                session->local = local;
                Log(Level::Info, "Check mode: " + name + " is also loaded on this PC, to check its first " + std::to_string(s.checkCalls) + " runs");
            }
        }
        OrtSession* handle = reinterpret_cast<OrtSession*>(session.get());
        {
            std::lock_guard lock(sessionsMutex_);
            session->order = nextOrder_++;
            // the newest session of a model replaces older ones (BFA can drop a session without releasing it)
            for (auto& [_, other] : sessions_)
                if (other->entry.name == name) other->superseded = true;
            sessions_[handle] = std::move(session);
            PushModelsLocked();
        }
        *out = handle;
        Log(Level::Info, "Model " + name + " is served by " + a.hello.server + " on " + a.hello.device +
                             (a.detail.empty() ? std::string() : " (" + a.detail + ")") + "; nothing of it runs on this PC");
        Tell(Notice{Notice::Kind::Served, name, a.hello.device});
        return nullptr;
    } catch (...) {
        return real->CreateStatus(ORT_FAIL, "BFA Remote Bridge: the model couldn't be set up; lines get the game's own lip sync");
    }
}

void Bridge::ReleaseSession(const OrtApi* real, OrtSession* session) noexcept {
    std::unique_ptr<Session> mine;
    try {
        std::lock_guard lock(sessionsMutex_);
        const auto it = sessions_.find(session);
        if (it != sessions_.end()) {
            mine = std::move(it->second);
            sessions_.erase(it);
            PushModelsLocked();
        }
    } catch (...) {
    }
    if (!mine) {
        real->ReleaseSession(session);
        return;
    }
    if (mine->local) real->ReleaseSession(mine->local);
    try {
        Log(Level::Info, "Model " + mine->entry.name + " released");
    } catch (...) {
    }
}

// Each requested output a one-element float tensor holding NaN (a caller's float value is filled with NaN instead):
// BFA's output check drops the line, and keeps the model.
void Bridge::NanAnswer(const OrtApi* real, std::size_t outputCount, OrtValue** outputs) noexcept {
    {
        std::lock_guard lock(statsMutex_);
        ++stats_.unanswered;
    }
    if (!outputs) return;
    OrtAllocator* allocator = nullptr;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t i = 0; i < outputCount; ++i) {
        if (outputs[i]) {
            Tensor t;
            std::string why;
            if (ReadTensor(real, outputs[i], t, why) && t.dtype == static_cast<std::uint8_t>(proto::DType::Float))
                for (std::size_t k = 0; k < t.bytes / 4; ++k) std::memcpy(static_cast<std::uint8_t*>(t.data) + 4 * k, &nan, 4);
            continue;
        }
        if (!allocator && !Ok(real, real->GetAllocatorWithDefaultOptions(&allocator))) return;
        const std::int64_t shape[1] = {1};
        OrtValue* value = nullptr;
        void* data = nullptr;
        if (!Ok(real, real->CreateTensorAsOrtValue(allocator, shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value)) || !value) continue;
        if (Ok(real, real->GetTensorMutableData(value, &data)) && data) std::memcpy(data, &nan, 4);
        outputs[i] = value;
    }
}

OrtStatus* Bridge::DoRun(const OrtApi* real, OrtSession* session, const OrtRunOptions* runOptions, const char* const* inputNames,
                         const OrtValue* const* inputs, std::size_t inputCount, const char* const* outputNames, std::size_t outputCount,
                         OrtValue** outputs) noexcept {
    proto::ModelEntry entry;
    OrtSession* local = nullptr;
    bool superseded = false;
    try {
        std::lock_guard lock(sessionsMutex_);
        const auto it = sessions_.find(session);
        if (it == sessions_.end()) return real->Run(session, runOptions, inputNames, inputs, inputCount, outputNames, outputCount, outputs);
        entry = it->second->entry;
        local = it->second->local;
        superseded = it->second->superseded;
    } catch (...) {
        NanAnswer(real, outputCount, outputs);
        return nullptr;
    }

    // a bridge session: from here on every path returns success, with the server's outputs or NaN ones
    if (superseded) {  // a newer session of its model replaced it: no backoff, nothing logged
        NanAnswer(real, outputCount, outputs);
        return nullptr;
    }
    std::vector<OrtValue*> created;
    Settings s;
    Client::Reply reply;
    try {
        s = CurrentSettings();
        if (!outputs || outputCount == 0 || !outputNames || (inputCount > 0 && (!inputs || !inputNames))) {
            NanAnswer(real, outputCount, outputs);
            return nullptr;
        }
        std::vector<Tensor> in(inputCount), given(outputCount);
        std::string why;
        bool supported = true;
        for (std::size_t i = 0; supported && i < inputCount; ++i) {
            if (!inputNames[i] || !ReadTensor(real, inputs[i], in[i], why)) {
                why = "input " + std::string(inputNames[i] ? inputNames[i] : "?") + " " + why;
                supported = false;
            }
        }
        for (std::size_t i = 0; supported && i < outputCount; ++i) {
            if (!outputNames[i]) {
                why = "an output has no name";
                supported = false;
            } else if (outputs[i] && !ReadTensor(real, outputs[i], given[i], why)) {
                why = "output " + std::string(outputNames[i]) + " " + why;
                supported = false;
            }
        }
        if (!supported) {
            bool first = false;
            {
                std::lock_guard lock(sessionsMutex_);
                const auto it = sessions_.find(session);
                if (it != sessions_.end() && !it->second->unsupportedLogged) first = it->second->unsupportedLogged = true;
            }
            if (first) Log(Level::Warn, entry.name + ": " + why + ", which the protocol can't carry; those lines get the game's own lip sync");
            NanAnswer(real, outputCount, outputs);
            return nullptr;
        }

        proto::RunRequest request;
        request.model = entry.name;
        for (std::size_t i = 0; i < inputCount; ++i)
            request.inputs.push_back(proto::TensorView{inputNames[i], in[i].dtype, in[i].dims, in[i].data, in[i].bytes});
        for (std::size_t i = 0; i < outputCount; ++i) request.outputs.emplace_back(outputNames[i]);

        std::string reason;
        const Client::Outcome outcome = client_.Run(entry, request, reply, reason);
        if (outcome != Client::Outcome::Forwarded) {
            if (outcome == Client::Outcome::Failed) {
                std::lock_guard lock(statsMutex_);
                ++stats_.failures;
            }
            NanAnswer(real, outputCount, outputs);
            return nullptr;
        }

        // the answer must fit the call exactly before anything is written
        reason.clear();
        const auto& got = reply.ok.outputs;
        if (got.size() != outputCount) reason = "the server returned " + std::to_string(got.size()) + " outputs, not " + std::to_string(outputCount);
        for (std::size_t i = 0; reason.empty() && i < outputCount; ++i) {
            if (got[i].name != outputNames[i])
                reason = "the server's output " + std::to_string(i) + " is " + got[i].name + ", not " + outputNames[i];
            else if (outputs[i] && (got[i].dtype != given[i].dtype || got[i].dims != given[i].dims))
                reason = "the server's " + got[i].name + " is " + proto::DTypeName(got[i].dtype) + " " + Dims(got[i].dims) + ", not " +
                         proto::DTypeName(given[i].dtype) + " " + Dims(given[i].dims) + " as the caller's";
        }

        // new values made by the runtime for the outputs the caller left empty
        created.assign(outputCount, nullptr);
        OrtAllocator* allocator = nullptr;
        for (std::size_t i = 0; reason.empty() && i < outputCount; ++i) {
            if (outputs[i]) continue;
            if (!allocator && !Ok(real, real->GetAllocatorWithDefaultOptions(&allocator))) {
                reason = "the runtime's allocator isn't there";
                break;
            }
            OrtValue* value = nullptr;
            void* data = nullptr;
            if (!Ok(real, real->CreateTensorAsOrtValue(allocator, got[i].dims.data(), got[i].dims.size(),
                                                        static_cast<ONNXTensorElementDataType>(got[i].dtype), &value)) ||
                !value) {
                reason = "the runtime couldn't make " + got[i].name;
                break;
            }
            created[i] = value;
            if (!Ok(real, real->GetTensorMutableData(value, &data)) || (got[i].bytes && !data)) {
                reason = "the runtime's " + got[i].name + " has no data";
                break;
            }
            if (got[i].bytes) std::memcpy(data, reply.payload.data() + got[i].offset, got[i].bytes);
        }
        if (!reason.empty()) {
            for (OrtValue*& v : created)
                if (v) {
                    real->ReleaseValue(v);
                    v = nullptr;
                }
            client_.Fail(reason);
            {
                std::lock_guard lock(statsMutex_);
                ++stats_.failures;
            }
            NanAnswer(real, outputCount, outputs);
            return nullptr;
        }
        for (std::size_t i = 0; i < outputCount; ++i) {
            if (outputs[i]) {
                if (got[i].bytes) std::memcpy(given[i].data, reply.payload.data() + got[i].offset, got[i].bytes);
            } else {
                outputs[i] = created[i];
            }
        }
        created.clear();
    } catch (...) {
        for (OrtValue* v : created)
            if (v) real->ReleaseValue(v);
        NanAnswer(real, outputCount, outputs);
        return nullptr;
    }

    // answered by the server: what follows only reports
    try {
        const std::uint64_t down = proto::kHeaderSize + reply.payload.size();
        Stats snapshot;
        {
            std::lock_guard lock(statsMutex_);
            ++stats_.forwarded;
            stats_.roundTripMicros += static_cast<std::uint64_t>(reply.roundTrip.count());
            stats_.serverMicros += reply.ok.serverMicros;
            stats_.bytesUp += reply.bytesUp;
            stats_.bytesDown += down;
            snapshot = stats_;
        }
        if (s.logDetail == LogDetail::Detailed)
            Log(Level::Info, "RUN " + entry.name + ": round trip " + Ms(static_cast<std::uint64_t>(reply.roundTrip.count())) + " ms, server " +
                                 Ms(reply.ok.serverMicros) + " ms, " + std::to_string(reply.bytesUp) + " bytes up, " + std::to_string(down) +
                                 " bytes back");
        if (snapshot.forwarded % 250 == 0)
            Log(Level::Info, std::to_string(snapshot.forwarded) + " runs answered by the server, " + std::to_string(snapshot.unanswered) +
                                 " not; average round trip " + Ms(snapshot.roundTripMicros / snapshot.forwarded) + " ms, server " +
                                 Ms(snapshot.serverMicros / snapshot.forwarded) + " ms");
        if (local && s.checkCalls > 0) {
            int index = 0;
            {
                std::lock_guard lock(sessionsMutex_);
                const auto it = sessions_.find(session);
                if (it != sessions_.end() && it->second->checks < s.checkCalls) index = ++it->second->checks;
            }
            if (index > 0) Check(real, local, inputNames, inputs, inputCount, outputNames, outputCount, entry.name, index, s.checkCalls, reply);
        }
    } catch (...) {
    }
    return nullptr;
}

// Check mode: the same call on this PC's real session, into separate values, and the largest difference per output.
void Bridge::Check(const OrtApi* real, OrtSession* local, const char* const* inputNames, const OrtValue* const* inputs, std::size_t inputCount,
                   const char* const* outputNames, std::size_t outputCount, const std::string& model, int index, int of,
                   const Client::Reply& reply) {
    std::vector<OrtValue*> mine(outputCount, nullptr);
    std::string line = "Check " + model + " " + std::to_string(index) + "/" + std::to_string(of) + ":";
    OrtStatus* status = real->Run(local, nullptr, inputNames, inputs, inputCount, outputNames, outputCount, mine.data());
    if (status) {
        line += " the run on this PC failed (" + ScrubPaths(real->GetErrorMessage(status)) + ")";
        real->ReleaseStatus(status);
    } else {
        for (std::size_t i = 0; i < outputCount; ++i) {
            const proto::TensorRef& got = reply.ok.outputs[i];
            Tensor here;
            std::string why;
            line += (i ? "," : "") + std::string(" ") + got.name;
            if (!ReadTensor(real, mine[i], here, why)) {
                line += " " + why;
                continue;
            }
            if (here.dtype != got.dtype || here.dims != got.dims) {
                line += " differs in shape or type (" + std::string(proto::DTypeName(here.dtype)) + " " + Dims(here.dims) + " here)";
                continue;
            }
            const auto* a = static_cast<const std::uint8_t*>(here.data);
            const std::uint8_t* b = reply.payload.data() + got.offset;
            const double d = LargestDifference(got.dtype, a, b, got.bytes);
            if (d < 0) {
                line += (got.bytes == 0 || std::memcmp(a, b, got.bytes) == 0) ? " identical" : " differs";
            } else {
                char buf[48];
                std::snprintf(buf, sizeof buf, " largest difference %.3g", d);
                line += buf;
            }
        }
    }
    for (OrtValue* v : mine)
        if (v) real->ReleaseValue(v);
    Log(Level::Info, line);
}

}  // namespace bfarb
