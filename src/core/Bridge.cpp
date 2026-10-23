#include "core/Bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "core/Scrub.h"
#include "core/Sha256.h"
#include "core/Status.h"
#include "core/Version.h"

static_assert(ORT_API_VERSION == 24, "the vendored header must be ONNX Runtime 1.24.4's");

namespace bfarb {
namespace {

std::atomic<Bridge*> g_current{nullptr};
std::atomic<OrtGetApiBaseFn> g_realGetApiBase{nullptr};
std::atomic<const OrtApiBase*> g_realBase{nullptr};
std::atomic<const OrtApi*> g_realApi{nullptr};
std::atomic<int> g_state{0};
std::atomic<unsigned> g_servedVersion{0};  // the API version BFA asked for and got the bridge's table
std::atomic<unsigned> g_runtimeApi{0};     // the runtime's own API version
std::mutex g_tableMutex;
OrtApiBase g_base;

// The table BFA gets: OrtApi's 1.24.4 entries, then the stubs answering the entries of newer versions. BFA reaches
// an entry past 1.24.4's by its index, which lands in `newer`.
using NewerFn = OrtStatus*(ORT_API_CALL*)() noexcept;
struct Table {
    OrtApi api;
    NewerFn newer[kNewerOrtSlots];
};
static_assert(offsetof(Table, newer) == sizeof(OrtApi), "the stubs must follow 1.24.4's entries without a gap");
Table g_table;
struct Built {
    const OrtApi* real = nullptr;
    std::size_t entries = 0;
    bool newer = false;
} g_built;

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

// The DirectML table is wrapped only at API version 24 from a runtime at least that new: its layout is 1.24.4's.
// Any other version gets the real table, and BFA's GPU route is then seen only through
// SessionOptionsAppendExecutionProvider.
OrtStatus* ORT_API_CALL WrapGetProviderApi(const char* name, std::uint32_t version, const void** api) noexcept {
    OrtStatus* status = Real()->GetExecutionProviderApi(name, version, api);
    if (status || !api || !*api || !name || std::strcmp(name, "DML") != 0 || version != kOrtApiVersion ||
        g_runtimeApi.load() < kOrtApiVersion)
        return status;
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

// ---- the entries past 1.24.4's: each answers with an error status naming it, without the runtime. They take no
// parameters: whatever BFA passes is left where it is (both x64 calling conventions leave the arguments to the
// caller), and nothing is read of them.

OrtStatus* RefuseNewer(std::size_t index) noexcept {
    if (Bridge* b = Bridge::Current()) {
        try {
            b->OnNewerEntry(index);
        } catch (...) {
        }
    }
    const OrtApi* real = Real();
    if (!real) return nullptr;
    const std::string text = "BFA Remote Bridge: ONNX Runtime entry " + std::to_string(index) +
                             " is newer than the runtime the bridge knows (" + kOrtVersionText + "), so it isn't served";
    return real->CreateStatus(ORT_NOT_IMPLEMENTED, text.c_str());
}

template <std::size_t I>
OrtStatus* ORT_API_CALL NewerEntry() noexcept {
    return RefuseNewer(kKnownOrtEntries + I);
}

template <std::size_t... I>
constexpr std::array<NewerFn, sizeof...(I)> MakeNewer(std::index_sequence<I...>) {
    return {&NewerEntry<I>...};
}
constexpr std::array<NewerFn, kNewerOrtSlots> kNewer = MakeNewer(std::make_index_sequence<kNewerOrtSlots>{});

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

// An OrtApi entry the runtime's table has (its index below the entries copied).
#define BFARB_HAS(NAME) (offsetof(OrtApi, NAME) / sizeof(void*) < entries)

#define BFARB_GUARD(TABLE, COPY, REAL, NAME)                                                           \
    do {                                                                                               \
        using G = Guard<TABLE, decltype(TABLE::NAME), &TABLE::NAME>;                                   \
        G::real = (REAL);                                                                              \
        (COPY).NAME = &G::Call;                                                                        \
    } while (false)

#define BFARB_GUARD_IF_HAS(NAME)                                                                       \
    do {                                                                                               \
        if (BFARB_HAS(NAME)) BFARB_GUARD(OrtApi, table, real, NAME);                                   \
    } while (false)

// The 22 OrtApi entries of ONNX Runtime 1.24.4 that take an OrtSession* besides Run (wrapped on its own); with
// CreateSession and ReleaseSession, and the model editor's three, every entry that meets a session. Each only when
// the runtime's table has it.
void GuardSessionEntries(OrtApi& table, const OrtApi* real, std::size_t entries) {
    BFARB_GUARD_IF_HAS(SessionGetInputCount);
    BFARB_GUARD_IF_HAS(SessionGetOutputCount);
    BFARB_GUARD_IF_HAS(SessionGetOverridableInitializerCount);
    BFARB_GUARD_IF_HAS(SessionGetInputTypeInfo);
    BFARB_GUARD_IF_HAS(SessionGetOutputTypeInfo);
    BFARB_GUARD_IF_HAS(SessionGetOverridableInitializerTypeInfo);
    BFARB_GUARD_IF_HAS(SessionGetInputName);
    BFARB_GUARD_IF_HAS(SessionGetOutputName);
    BFARB_GUARD_IF_HAS(SessionGetOverridableInitializerName);
    BFARB_GUARD_IF_HAS(SessionEndProfiling);
    BFARB_GUARD_IF_HAS(SessionGetModelMetadata);
    BFARB_GUARD_IF_HAS(CreateAllocator);
    BFARB_GUARD_IF_HAS(RunWithBinding);
    BFARB_GUARD_IF_HAS(CreateIoBinding);
    BFARB_GUARD_IF_HAS(SessionGetProfilingStartTimeNs);
    BFARB_GUARD_IF_HAS(RunAsync);
    BFARB_GUARD_IF_HAS(SetEpDynamicOptions);
    BFARB_GUARD_IF_HAS(SessionGetMemoryInfoForInputs);
    BFARB_GUARD_IF_HAS(SessionGetMemoryInfoForOutputs);
    BFARB_GUARD_IF_HAS(SessionGetEpDeviceForInputs);
    BFARB_GUARD_IF_HAS(SessionGetEpDeviceForOutputs);
    BFARB_GUARD_IF_HAS(Session_GetEpGraphAssignmentInfo);
}

std::atomic<const OrtModelEditorApi*> g_realEditor{nullptr};
OrtModelEditorApi g_editor;

// The model editor's table is copied with 1.24.4's layout, so only for BFA asking at most API version 24 of a
// runtime at least that new; otherwise no editor (its documented answer in a minimal build). BFA calls none of it.
const OrtModelEditorApi* ORT_API_CALL WrapGetModelEditorApi() noexcept {
    if (g_servedVersion.load() > kOrtApiVersion || g_runtimeApi.load() < kOrtApiVersion) return nullptr;
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

void CantServe(const std::string& why) {
    g_state.store(static_cast<int>(Bridge::ApiState::CantServe));
    if (Bridge* b = Bridge::Current()) {
        try {
            b->OnCantServe(why);
        } catch (...) {
        }
    }
}

const OrtApi* ORT_API_CALL GetApi(std::uint32_t version) noexcept {
    const OrtApiBase* base = g_realBase.load(std::memory_order_acquire);
    if (!base) return nullptr;
    const OrtApi* real = base->GetApi(version);
    if (!real) return nullptr;  // the runtime doesn't have it: BFA's own error ("wrong version")
    const char* text = base->GetVersionString ? base->GetVersionString() : nullptr;
    const unsigned runtime = OrtApiVersionOf(text);
    const std::string runtimeText = std::string("ONNX Runtime ") + (text ? text : "of an unknown version");
    try {
        if (runtime < kOldestOrtApi) {
            CantServe(runtimeText + " is older than the bridge serves");
            return real;
        }
        if (version > kNewestOrtApi) {
            CantServe(runtimeText + " was asked for API version " + std::to_string(version) + ", newer than the bridge serves");
            return real;
        }
    } catch (...) {
        return real;
    }
    // the runtime's entries the bridge copies: as many as its own version has, at most 1.24.4's
    const std::size_t entries = OrtApiEntries(std::min<unsigned>(runtime, kOrtApiVersion));
    const bool newer = version > kOrtApiVersion;
    std::lock_guard lock(g_tableMutex);
    g_runtimeApi.store(runtime);
    g_servedVersion.store(version);
    if (g_built.real != real || g_built.entries != entries || g_built.newer != newer) {
        std::memset(static_cast<void*>(&g_table), 0, sizeof g_table);
        std::memcpy(static_cast<void*>(&g_table.api), real, entries * sizeof(void*));
        OrtApi& table = g_table.api;
        // the entries of API version 1 (all the runtimes served have them)
        table.AddFreeDimensionOverrideByName = &WrapOverride;
        table.ReleaseSessionOptions = &WrapReleaseOptions;
        table.CreateSession = &WrapCreateSession;
        table.ReleaseSession = &WrapReleaseSession;
        table.Run = &WrapRun;
        table.GetExecutionProviderApi = &WrapGetProviderApi;
        if (BFARB_HAS(SessionOptionsAppendExecutionProvider)) table.SessionOptionsAppendExecutionProvider = &WrapAppendProvider;
        if (BFARB_HAS(GetModelEditorApi)) table.GetModelEditorApi = &WrapGetModelEditorApi;
        GuardSessionEntries(table, real, entries);
        if (newer)
            for (std::size_t i = 0; i < kNewerOrtSlots; ++i) g_table.newer[i] = kNewer[i];
        g_built = Built{real, entries, newer};
        g_realApi.store(real, std::memory_order_release);
    }
    g_state.store(static_cast<int>(Bridge::ApiState::Served));
    return &g_table.api;
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

// A tensor's type, shape and CPU data. False, with why, when it isn't a CPU tensor of a supported type. Its memory's
// device is read only from a runtime whose API has MemoryInfoGetDeviceType (14 on); BFA's tensors are CPU ones.
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
    if (g_runtimeApi.load() >= 14) {
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
    }
    if (!Ok(real, real->GetTensorMutableData(const_cast<OrtValue*>(value), &t.data)) || (bytes > 0 && !t.data)) {
        why = "has no readable data";
        return false;
    }
    return true;
}

}  // namespace

// ---- the runtime's API versions

const std::vector<OrtApiStart>& OrtApiStarts() {
#define BFARB_START(V, NAME) OrtApiStart{V, offsetof(OrtApi, NAME) / sizeof(void*), #NAME}
    static const std::vector<OrtApiStart> starts = {
        BFARB_START(11, SessionOptionsAppendExecutionProvider_CUDA_V2),
        BFARB_START(12, AddExternalInitializers),
        BFARB_START(13, GetTrainingApi),
        BFARB_START(14, MemoryInfoGetDeviceType),
        BFARB_START(15, SessionOptionsAppendExecutionProvider_Dnnl),
        BFARB_START(16, CreateROCMProviderOptions),
        BFARB_START(17, SetUserLoggingFunction),
        BFARB_START(18, SessionOptionsAppendExecutionProvider_VitisAI),
        BFARB_START(20, CreateLoraAdapter),
        BFARB_START(22, ReleaseValueInfo),
        BFARB_START(23, GetTensorSizeInBytes),
        BFARB_START(24, KernelInfo_GetConfigEntries),
    };
#undef BFARB_START
    return starts;
}

std::size_t OrtApiEntries(unsigned version) noexcept {
    if (version < kOldestOrtApi) return 0;
    if (version >= kOrtApiVersion) return kKnownOrtEntries;
    for (const OrtApiStart& s : OrtApiStarts())
        if (s.version > version) return s.index;
    return kKnownOrtEntries;
}

unsigned OrtApiVersionOf(const char* text) noexcept {
    if (!text || text[0] != '1' || text[1] != '.') return 0;
    unsigned v = 0;
    const char* p = text + 2;
    if (*p < '0' || *p > '9') return 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        v = v * 10 + static_cast<unsigned>(*p - '0');
        if (v > 1000) return 0;
    }
    return (*p == '\0' || *p == '.' || *p == '-' || *p == '+') ? v : 0;
}

// ---- the bridge

Bridge::Bridge(TransportFactory factory, Resolver resolver, LogSink log, Client::Now now)
    : log_(log), client_(std::move(factory), std::move(resolver), std::move(log), now), monitor_(now) {
    client_.OnStates([this](const std::string& server, const std::string& device, const std::vector<ModelState>& models) {
        OnStates(server, device, models);
    });
}

Bridge::~Bridge() {
    Bridge* self = this;
    g_current.compare_exchange_strong(self, nullptr);
    client_.OnStates(nullptr);
}

void Bridge::SetCurrent(Bridge* bridge) noexcept { g_current.store(bridge, std::memory_order_release); }
Bridge* Bridge::Current() noexcept { return g_current.load(std::memory_order_acquire); }

OrtGetApiBaseFn Bridge::Hook(OrtGetApiBaseFn real) noexcept {
    g_realGetApiBase.store(real, std::memory_order_release);
    return &HookedGetApiBase;
}

Bridge::ApiState Bridge::State() noexcept { return static_cast<ApiState>(g_state.load()); }
unsigned Bridge::ServedApiVersion() noexcept { return g_servedVersion.load(); }

void Bridge::Log(Level level, const std::string& line) const {
    if (log_) log_(level, line);
}
void Bridge::LogLine(Level level, const std::string& line) const { Log(level, line); }

void Bridge::Apply(const Settings& settings) {
    {
        std::lock_guard lock(settingsMutex_);
        settings_ = settings;
    }
    settingsRead_.store(true);
    monitor_.SetShowStatus(settings.showStatus);
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

void Bridge::OnNewerEntry(std::size_t index) {
    {
        std::lock_guard lock(sessionsMutex_);
        if (std::find(newerLogged_.begin(), newerLogged_.end(), index) != newerLogged_.end()) return;
        newerLogged_.push_back(index);
    }
    Log(Level::Warn, "Better Facial Animations called ONNX Runtime entry " + std::to_string(index) + " (API version " +
                         std::to_string(ServedApiVersion()) + "), which is newer than " + kOrtVersionText +
                         "'s; the bridge answers it with an error status, so what needs it gets the game's own lip sync. Update the bridge.");
    monitor_.Fallback(Reason::Incompatible, std::string(),
                      "this Better Facial Animations called runtime entry " + std::to_string(index) + ", which the bridge doesn't know. Update the bridge");
    client_.ReportFallback(Reason::Incompatible);
}

void Bridge::OnCantServe(const std::string& why) {
    Log(Level::Warn, "The bridge can't serve this Better Facial Animations: " + why +
                         ". BFA runs on this PC as it would without the bridge. Update the bridge.");
    monitor_.Once("cantserve", CantAttachText(client_.CurrentVersions().bfa, why));
}

void Bridge::OnStates(const std::string& server, const std::string& device, const std::vector<ModelState>& models) {
    for (const ModelState& m : models) {
        const bool ready = m.status == static_cast<std::uint8_t>(proto::ModelStatus::Ready);
        std::string state = ready ? "ready" : std::string("not ready (") + proto::ModelStatusName(m.status) + ")";
        if (!m.detail.empty()) state += " (" + m.detail + ")";
        monitor_.ModelState(m.name, server, device, state, ready);
    }
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
    std::string why, words;
    Reason reason = Reason::ModelRefused;
    using K = ProbeResult::Kind;
    if (a.hello.kind == K::NoAnswer) {
        reason = Reason::Unreachable;
        why = "the server didn't answer (" + a.detail + ")";
    } else if (a.hello.kind == K::WrongKey) {
        why = "the server refused the access key";
        words = "the server refused the access key";
    } else if (a.hello.kind == K::Answered && (a.status == static_cast<std::uint8_t>(proto::ModelStatus::NotFound) ||
                                                a.status == static_cast<std::uint8_t>(proto::ModelStatus::DifferentFile))) {
        why = "the server's copy: " + std::string(proto::ModelStatusName(a.status)) + (a.detail.empty() ? "" : " (" + a.detail + ")");
        words = "the server lacks it or has a different copy";
    } else if (a.hello.kind == K::Answered) {
        why = "the server's copy: " + std::string(proto::ModelStatusName(a.status)) + (a.detail.empty() ? "" : " (" + a.detail + ")");
        words = "the server couldn't load it";
    } else {
        reason = Reason::Incompatible;
        why = a.detail;
        words = "the bridge couldn't send it (" + a.detail + ")";
    }
    OrtStatus* status = RefuseWith(real, model, why);
    monitor_.Fallback(reason, model, words);
    client_.ReportFallback(reason);
    return status;
}

OrtStatus* Bridge::RefuseWith(const OrtApi* real, const std::string& model, const std::string& why) {
    const std::string message = "BFA Remote Bridge: " + model + " isn't loaded, since " + ScrubPaths(why) + "; lines get the game's own lip sync";
    bool fresh = true;
    {
        std::lock_guard lock(sessionsMutex_);
        std::string& last = lastRefusal_[model];
        fresh = last != message;
        last = message;
    }
    // BFA tries a failed load again: the same refusal is logged once, until it changes
    if (fresh)
        Log(Level::Warn, message.substr(std::strlen("BFA Remote Bridge: ")));
    else if (CurrentSettings().logDetail == LogDetail::Detailed)
        Log(Level::Info, message.substr(std::strlen("BFA Remote Bridge: ")) + " (again)");
    return real->CreateStatus(ORT_FAIL, message.c_str());
}

void Bridge::ExpectSettings(bool skyrimNet) { skyrimNet_.store(skyrimNet); }

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
        if (!path || !out) return real->CreateSession(env, path, options, out);  // the runtime's own refusal
        const Settings s = CurrentSettings();
        const std::filesystem::path file(path);
        const std::string name = Utf8Name(file);

        // BFA's GPU route is its own setting: reported, once per session
        bool gpu = false, tellGpu = false;
        std::vector<proto::Override> overrides;
        {
            std::lock_guard lock(sessionsMutex_);
            if (const auto it = options_.find(options); it != options_.end()) {
                overrides = it->second.overrides;
                gpu = it->second.gpu;
            }
            tellGpu = gpu && !gpuLogged_;
            if (tellGpu) gpuLogged_ = true;
        }
        if (tellGpu) {
            Log(Level::Info,
                "Better Facial Animations is set to GPU: no lip-sync model runs on this PC's graphics card with the bridge, but BFA "
                "keeps a device open on it; its Force CPU setting avoids that, and the model runs on the server either way");
            monitor_.Once("gpu", GpuText());
        }

        if (!settingsRead_.load() && skyrimNet_.load()) return RefuseWith(real, name, "the bridge's settings weren't read yet");
        if (!settingsRead_.load() || s.serverAddress.empty()) {
            const bool skyrimNet = settingsRead_.load() || skyrimNet_.load();  // the settings come from SkyrimNet
            OrtStatus* status = RefuseWith(real, name,
                                           skyrimNet ? "no server address is set (finding the server by itself needs a newer PandorumLLM)"
                                                     : "SkyrimNet isn't loaded, so the bridge has no server address");
            monitor_.Once("noserver", NoServerText(skyrimNet));
            return status;
        }

        auto session = std::make_unique<Session>();
        session->entry.name = name;
        session->entry.overrides = std::move(overrides);
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
        std::string list;
        for (const auto& o : session->entry.overrides) list += (list.empty() ? "" : ", ") + o.name + " = " + std::to_string(o.value);
        Log(Level::Info, "Model " + name + ": " + std::to_string(session->entry.size) + " bytes, SHA-256 " + Sha256::Hex(session->entry.sha256) +
                             (list.empty() ? ", no overrides" : ", overrides " + list) + "; asking the server");

        // the new model alone, on a side connection; the main connection gets the full list once the session is in
        const Admission a = client_.Admit(session->entry);
        if (!a.Ready()) return Refuse(real, name, a);

        OrtSession* handle = reinterpret_cast<OrtSession*>(session.get());
        {
            std::lock_guard lock(sessionsMutex_);
            session->order = nextOrder_++;
            // the newest session of a model replaces older ones (BFA can drop a session without releasing it)
            for (auto& [_, other] : sessions_)
                if (other->entry.name == name) other->superseded = true;
            sessions_[handle] = std::move(session);
            lastRefusal_.erase(name);
            PushModelsLocked();
        }
        *out = handle;
        Log(Level::Info, "Model " + name + " is served by " + a.hello.server + " on " + a.hello.device +
                             (a.detail.empty() ? std::string() : " (" + a.detail + ")") + "; nothing of it runs on this PC");
        monitor_.ModelState(name, a.hello.server, a.hello.device, a.detail.empty() ? "ready" : "ready (" + a.detail + ")", true);
        return nullptr;
    } catch (...) {
        return real->CreateStatus(ORT_FAIL, "BFA Remote Bridge: the model couldn't be set up; lines get the game's own lip sync");
    }
}

void Bridge::ReleaseSession(const OrtApi* real, OrtSession* session) noexcept {
    std::unique_ptr<Session> mine;
    bool lastOfModel = false;
    try {
        std::lock_guard lock(sessionsMutex_);
        const auto it = sessions_.find(session);
        if (it != sessions_.end()) {
            mine = std::move(it->second);
            sessions_.erase(it);
            lastOfModel = std::none_of(sessions_.begin(), sessions_.end(),
                                       [&](const auto& kv) { return kv.second->entry.name == mine->entry.name; });
            PushModelsLocked();
        }
    } catch (...) {
    }
    if (!mine) {
        real->ReleaseSession(session);
        return;
    }
    try {
        if (lastOfModel) monitor_.Forget(mine->entry.name);
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

void Bridge::FallBack(const OrtApi* real, const std::string& model, const Fallback& fallback, Reason reason, Clock::time_point start,
                      std::size_t outputCount, OrtValue** outputs) noexcept {
    try {
        client_.RecordFallback(model, fallback, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start));
        monitor_.Fallback(reason, model);
        client_.ReportFallback(reason);
    } catch (...) {
    }
    NanAnswer(real, outputCount, outputs);
}

OrtStatus* Bridge::DoRun(const OrtApi* real, OrtSession* session, const OrtRunOptions* runOptions, const char* const* inputNames,
                         const OrtValue* const* inputs, std::size_t inputCount, const char* const* outputNames, std::size_t outputCount,
                         OrtValue** outputs) noexcept {
    const Clock::time_point start = Clock::now();
    proto::ModelEntry entry;
    bool superseded = false;
    try {
        std::lock_guard lock(sessionsMutex_);
        const auto it = sessions_.find(session);
        if (it == sessions_.end()) return real->Run(session, runOptions, inputNames, inputs, inputCount, outputNames, outputCount, outputs);
        entry = it->second->entry;
        superseded = it->second->superseded;
    } catch (...) {
        NanAnswer(real, outputCount, outputs);
        return nullptr;
    }

    // a bridge session: from here on every path returns success, with the server's outputs or NaN ones, and every run
    // is counted once
    const auto fallBack = [&](Why why, std::string text, bool quiet, Reason reason) -> OrtStatus* {
        Fallback f;
        f.why = why;
        f.text = std::move(text);
        f.quiet = quiet;
        FallBack(real, entry.name, f, reason, start, outputCount, outputs);
        return nullptr;
    };
    if (superseded) {  // a newer session of its model replaced it: counted, nothing logged or told
        try {
            Fallback f{Why::Other, "a newer session of the model replaced this one", true};
            client_.RecordFallback(entry.name, f, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start));
        } catch (...) {
        }
        NanAnswer(real, outputCount, outputs);
        return nullptr;
    }
    std::vector<OrtValue*> created;
    Settings s;
    Client::Reply reply;
    try {
        s = CurrentSettings();
        if (!outputs || outputCount == 0 || !outputNames || (inputCount > 0 && (!inputs || !inputNames)))
            return fallBack(Why::Other, "the call names no outputs or lacks its inputs", false, Reason::Incompatible);
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
            return fallBack(Why::Other, why + ", which the protocol can't carry", true, Reason::Incompatible);
        }

        proto::RunRequest request;
        request.model = entry.name;
        for (std::size_t i = 0; i < inputCount; ++i)
            request.inputs.push_back(proto::TensorView{inputNames[i], in[i].dtype, in[i].dims, in[i].data, in[i].bytes});
        for (std::size_t i = 0; i < outputCount; ++i) request.outputs.emplace_back(outputNames[i]);

        Fallback fallback;
        const Client::Outcome outcome = client_.Run(entry, request, reply, fallback);
        if (outcome != Client::Outcome::Forwarded) {
            FallBack(real, entry.name, fallback, ReasonOf(fallback), start, outputCount, outputs);
            return nullptr;
        }

        // the answer must fit the call exactly before anything is written
        std::string reason;
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
            // the server answered, but its answer doesn't fit the call: counted, and the connection stays
            return fallBack(Why::Other, reason, false, Reason::ServerError);
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
        return fallBack(Why::Other, "the bridge failed unexpectedly", false, Reason::ServerError);
    }

    // answered by the server: what follows only reports
    try {
        const std::uint64_t down = proto::kHeaderSize + reply.payload.size();
        {
            std::lock_guard lock(statsMutex_);
            ++stats_.forwarded;
            stats_.roundTripMicros += static_cast<std::uint64_t>(reply.roundTrip.count());
            stats_.serverMicros += reply.ok.serverMicros;
            stats_.bytesUp += reply.bytesUp;
            stats_.bytesDown += down;
        }
        client_.RecordAnswered(reply);
        monitor_.Answered(entry.name);
        if (s.logDetail == LogDetail::Detailed)
            Log(Level::Info, "RUN " + entry.name + ": round trip " + Ms(static_cast<std::uint64_t>(reply.roundTrip.count())) + " ms, server " +
                                 Ms(reply.ok.serverMicros) + " ms, " + std::to_string(reply.bytesUp) + " bytes up, " + std::to_string(down) +
                                 " bytes back");
    } catch (...) {
    }
    return nullptr;
}

}  // namespace bfarb
