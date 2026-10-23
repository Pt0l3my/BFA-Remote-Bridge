#include "core/Protocol.h"

#include <cstring>
#include <limits>

namespace bfarb::proto {
namespace {

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    void U8(std::uint8_t v) { out_.push_back(v); }
    void U16(std::uint16_t v) { Le(v, 2); }
    void U32(std::uint32_t v) { Le(v, 4); }
    void U64(std::uint64_t v) { Le(v, 8); }
    void I64(std::int64_t v) { Le(static_cast<std::uint64_t>(v), 8); }
    void Bytes(const void* p, std::size_t n) {
        if (n == 0) return;
        const auto* b = static_cast<const std::uint8_t*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    // false when longer than a u16 can say
    bool Str(std::string_view s) {
        if (s.size() > 0xFFFF) return false;
        U16(static_cast<std::uint16_t>(s.size()));
        Bytes(s.data(), s.size());
        return true;
    }

private:
    void Le(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    std::vector<std::uint8_t>& out_;
};

class Reader {
public:
    Reader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}

    bool U8(std::uint8_t& v) {
        if (!Need(1)) return false;
        v = p_[at_++];
        return true;
    }
    bool U16(std::uint16_t& v) {
        std::uint64_t x = 0;
        if (!Le(x, 2)) return false;
        v = static_cast<std::uint16_t>(x);
        return true;
    }
    bool U32(std::uint32_t& v) {
        std::uint64_t x = 0;
        if (!Le(x, 4)) return false;
        v = static_cast<std::uint32_t>(x);
        return true;
    }
    bool U64(std::uint64_t& v) { return Le(v, 8); }
    bool I64(std::int64_t& v) {
        std::uint64_t x = 0;
        if (!Le(x, 8)) return false;
        v = static_cast<std::int64_t>(x);
        return true;
    }
    bool Str(std::string& s) {
        std::uint16_t len = 0;
        if (!U16(len) || !Need(len)) return false;
        s.assign(reinterpret_cast<const char*>(p_ + at_), len);
        at_ += len;
        return true;
    }
    bool Fixed(std::uint8_t* dst, std::size_t n) {
        if (!Need(n)) return false;
        std::memcpy(dst, p_ + at_, n);
        at_ += n;
        return true;
    }
    bool Skip(std::size_t n) {
        if (!Need(n)) return false;
        at_ += n;
        return true;
    }
    std::size_t Offset() const { return at_; }
    bool Done() const { return at_ == n_; }

private:
    bool Need(std::size_t k) const { return k <= n_ - at_; }
    bool Le(std::uint64_t& v, int n) {
        if (!Need(static_cast<std::size_t>(n))) return false;
        v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(p_[at_ + i]) << (8 * i);
        at_ += static_cast<std::size_t>(n);
        return true;
    }
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t at_ = 0;
};

bool Fail(std::string& err, std::string_view what) {
    err.assign(what);
    return false;
}

bool Finish(const std::vector<std::uint8_t>& out, std::string& err) {
    if (out.size() > kMaxPayload) return Fail(err, "payload above 64 MiB");
    return true;
}

// One tensor in RUN or RUN_OK: str name, u8 dtype, u8 rank, i64 dims[rank], u32 byte_length, data.
bool WriteTensor(Writer& w, const TensorView& t, std::string& err) {
    if (!w.Str(t.name)) return Fail(err, "tensor name longer than 65,535 bytes");
    if (DTypeSize(t.dtype) == 0) return Fail(err, "unsupported tensor type " + std::to_string(t.dtype));
    if (t.dims.size() > kMaxRank) return Fail(err, "tensor rank above 255");
    std::uint64_t bytes = 0;
    if (!ByteLength(t.dtype, t.dims, bytes)) return Fail(err, "tensor " + std::string(t.name) + ": bad dimensions");
    if (bytes != t.bytes) return Fail(err, "tensor " + std::string(t.name) + ": byte length doesn't match its shape");
    if (bytes > 0 && !t.data) return Fail(err, "tensor " + std::string(t.name) + ": no data");
    w.U8(t.dtype);
    w.U8(static_cast<std::uint8_t>(t.dims.size()));
    for (const std::int64_t d : t.dims) w.I64(d);
    w.U32(static_cast<std::uint32_t>(bytes));
    w.Bytes(t.data, static_cast<std::size_t>(bytes));
    return true;
}

bool ReadTensor(Reader& r, TensorRef& t, std::string& err) {
    std::uint8_t rank = 0;
    if (!r.Str(t.name) || !r.U8(t.dtype) || !r.U8(rank)) return Fail(err, "truncated tensor");
    if (DTypeSize(t.dtype) == 0) return Fail(err, "tensor " + t.name + ": unsupported type " + std::to_string(t.dtype));
    t.dims.resize(rank);
    for (auto& d : t.dims)
        if (!r.I64(d)) return Fail(err, "truncated tensor dimensions");
    if (!r.U32(t.bytes)) return Fail(err, "truncated tensor length");
    std::uint64_t expected = 0;
    if (!ByteLength(t.dtype, t.dims, expected)) return Fail(err, "tensor " + t.name + ": bad dimensions");
    if (expected != t.bytes) return Fail(err, "tensor " + t.name + ": byte length doesn't match its shape");
    t.offset = r.Offset();
    if (!r.Skip(t.bytes)) return Fail(err, "truncated tensor data");
    return true;
}

std::uint64_t TensorSize(const TensorView& t) noexcept {
    return 2 + t.name.size() + 1 + 1 + 8 * t.dims.size() + 4 + t.bytes;
}

}  // namespace

std::size_t DTypeSize(std::uint8_t dtype) noexcept {
    switch (static_cast<DType>(dtype)) {
        case DType::Float: return 4;
        case DType::Int32: return 4;
        case DType::Int64: return 8;
        case DType::Float16: return 2;
        case DType::Double: return 8;
    }
    return 0;
}

const char* DTypeName(std::uint8_t dtype) noexcept {
    switch (static_cast<DType>(dtype)) {
        case DType::Float: return "float";
        case DType::Int32: return "int32";
        case DType::Int64: return "int64";
        case DType::Float16: return "float16";
        case DType::Double: return "double";
    }
    return "unsupported";
}

const char* ModelStatusName(std::uint8_t status) noexcept {
    switch (static_cast<ModelStatus>(status)) {
        case ModelStatus::Ready: return "ready";
        case ModelStatus::NotFound: return "not found";
        case ModelStatus::DifferentFile: return "different file";
        case ModelStatus::FailedToLoad: return "failed to load";
        case ModelStatus::NotAllowed: return "not allowed";
    }
    return "unknown status";
}

const char* ErrorCodeName(std::uint16_t code) noexcept {
    switch (static_cast<ErrorCode>(code)) {
        case ErrorCode::Malformed: return "malformed frame or payload";
        case ErrorCode::WrongKey: return "wrong access key";
        case ErrorCode::UnknownModel: return "unknown or unready model";
        case ErrorCode::InferenceFailed: return "inference failed";
        case ErrorCode::TooLarge: return "too large";
        case ErrorCode::VersionNotSupported: return "protocol version not supported";
        case ErrorCode::Busy: return "busy or shutting down";
    }
    return "unknown error";
}

bool ErrorClosesConnection(std::uint16_t code) noexcept {
    return code == static_cast<std::uint16_t>(ErrorCode::Malformed) || code == static_cast<std::uint16_t>(ErrorCode::WrongKey) ||
           code == static_cast<std::uint16_t>(ErrorCode::TooLarge) || code == static_cast<std::uint16_t>(ErrorCode::VersionNotSupported);
}

bool ErrorEndsConnection(std::uint16_t code) noexcept {
    return ErrorClosesConnection(code) || code == static_cast<std::uint16_t>(ErrorCode::Busy);
}

bool ValidModelName(std::string_view name) noexcept {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find("..") != std::string_view::npos) return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || u < 0x20 || u == 0x7F) return false;
    }
    return true;
}

bool KnownType(std::uint16_t version, std::uint16_t type, bool datagram) noexcept {
    if (version != kVersion1 && version != kVersion2) return false;
    if (datagram) return version == kVersion2 && (type == static_cast<std::uint16_t>(MsgType::Find) || type == static_cast<std::uint16_t>(MsgType::Found));
    if (type >= static_cast<std::uint16_t>(MsgType::Hello) && type <= static_cast<std::uint16_t>(MsgType::Pong)) return true;
    return version == kVersion2 && type == static_cast<std::uint16_t>(MsgType::Notice);
}

bool ElementCount(const std::vector<std::int64_t>& dims, std::uint64_t& count) noexcept {
    std::uint64_t n = 1;
    for (const std::int64_t d : dims) {
        if (d < 0) return false;
        const auto u = static_cast<std::uint64_t>(d);
        if (u != 0 && n > std::numeric_limits<std::uint64_t>::max() / u) return false;
        n *= u;
    }
    count = n;
    return true;
}

bool ByteLength(std::uint8_t dtype, const std::vector<std::int64_t>& dims, std::uint64_t& bytes) noexcept {
    const std::size_t size = DTypeSize(dtype);
    std::uint64_t count = 0;
    if (size == 0 || !ElementCount(dims, count)) return false;
    if (count > kMaxPayload / size) return false;
    bytes = count * size;
    return true;
}

// ---- frames

bool EncodeFrame(MsgType type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& frame, std::string& err,
                 std::uint16_t version) {
    if (payload.size() > kMaxPayload) return Fail(err, "payload above 64 MiB");
    if (!KnownType(version, static_cast<std::uint16_t>(type)))
        return Fail(err, "message type " + std::to_string(static_cast<int>(type)) + " isn't one of protocol version " + std::to_string(version) +
                             "'s on TCP");
    frame.clear();
    frame.reserve(kHeaderSize + payload.size());
    Writer w(frame);
    w.Bytes(kMagic, 4);
    w.U16(version);
    w.U16(static_cast<std::uint16_t>(type));
    w.U32(static_cast<std::uint32_t>(payload.size()));
    w.Bytes(payload.data(), payload.size());
    return true;
}

bool DecodeHeader(const std::uint8_t* bytes, Header& header, std::string& err, HeaderCheck* check) {
    if (check) *check = HeaderCheck{};
    if (std::memcmp(bytes, kMagic, 4) != 0) return Fail(err, "bad magic");
    Reader r(bytes + 4, kHeaderSize - 4);
    r.U16(header.version);
    r.U16(header.type);
    r.U32(header.length);
    if (header.version != kVersion1 && header.version != kVersion2) {
        if (check) check->versionRefused = true;
        return Fail(err, "protocol version " + std::to_string(header.version) + " not supported");
    }
    if (!KnownType(header.version, header.type))
        return Fail(err, "unknown message type " + std::to_string(header.type) + " in protocol version " + std::to_string(header.version));
    if (header.length > kMaxPayload) {
        if (check) check->tooLarge = true;
        return Fail(err, "payload above 64 MiB");
    }
    return true;
}

bool EncodeDatagram(MsgType type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& datagram, std::string& err) {
    if (!KnownType(kVersion2, static_cast<std::uint16_t>(type), true)) return Fail(err, "only FIND and FOUND are datagrams");
    if (kHeaderSize + payload.size() > kMaxDatagram) return Fail(err, "datagram above " + std::to_string(kMaxDatagram) + " bytes");
    datagram.clear();
    Writer w(datagram);
    w.Bytes(kMagic, 4);
    w.U16(kVersion2);
    w.U16(static_cast<std::uint16_t>(type));
    w.U32(static_cast<std::uint32_t>(payload.size()));
    w.Bytes(payload.data(), payload.size());
    return true;
}

bool DecodeDatagram(const std::uint8_t* p, std::size_t n, Header& header, std::vector<std::uint8_t>& payload, std::string& err) {
    if (n < kHeaderSize) return Fail(err, "shorter than a header");
    if (n > kMaxDatagram) return Fail(err, "datagram above " + std::to_string(kMaxDatagram) + " bytes");
    if (std::memcmp(p, kMagic, 4) != 0) return Fail(err, "bad magic");
    Reader r(p + 4, kHeaderSize - 4);
    r.U16(header.version);
    r.U16(header.type);
    r.U32(header.length);
    if (header.version != kVersion2) return Fail(err, "protocol version " + std::to_string(header.version) + ", not 2");
    if (!KnownType(header.version, header.type, true)) return Fail(err, "message type " + std::to_string(header.type) + " isn't FIND or FOUND");
    if (header.length != n - kHeaderSize) return Fail(err, "the length doesn't match the datagram");
    payload.assign(p + kHeaderSize, p + n);
    return true;
}

// ---- HELLO

bool EncodeHello(const Hello& m, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    Writer w(out);
    if (!w.Str(m.client) || !w.Str(m.key) || !w.Str(m.ortVersion)) return Fail(err, "string longer than 65,535 bytes");
    if (m.models.size() > 0xFFFF) return Fail(err, "more than 65,535 models");
    w.U16(static_cast<std::uint16_t>(m.models.size()));
    for (const ModelEntry& e : m.models) {
        if (!ValidModelName(e.name)) return Fail(err, "model name isn't a plain file name");
        w.Str(e.name);
        w.U64(e.size);
        w.Bytes(e.sha256.data(), e.sha256.size());
        if (e.overrides.size() > 0xFFFF) return Fail(err, "more than 65,535 overrides");
        w.U16(static_cast<std::uint16_t>(e.overrides.size()));
        for (const Override& o : e.overrides) {
            if (!w.Str(o.name)) return Fail(err, "override name longer than 65,535 bytes");
            w.I64(o.value);
        }
    }
    return Finish(out, err);
}

bool DecodeHello(const std::uint8_t* p, std::size_t n, Hello& m, std::string& err) {
    Reader r(p, n);
    std::uint16_t count = 0;
    if (!r.Str(m.client) || !r.Str(m.key) || !r.Str(m.ortVersion) || !r.U16(count)) return Fail(err, "truncated HELLO");
    m.models.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        ModelEntry e;
        std::uint16_t overrides = 0;
        if (!r.Str(e.name) || !r.U64(e.size) || !r.Fixed(e.sha256.data(), e.sha256.size()) || !r.U16(overrides))
            return Fail(err, "truncated HELLO model");
        if (!ValidModelName(e.name)) return Fail(err, "model name isn't a plain file name");
        for (std::uint16_t k = 0; k < overrides; ++k) {
            Override o;
            if (!r.Str(o.name) || !r.I64(o.value)) return Fail(err, "truncated HELLO override");
            e.overrides.push_back(std::move(o));
        }
        m.models.push_back(std::move(e));
    }
    if (!r.Done()) return Fail(err, "trailing bytes after HELLO");
    return true;
}

// ---- HELLO_OK

bool EncodeHelloOk(const HelloOk& m, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    Writer w(out);
    if (!w.Str(m.server) || !w.Str(m.device)) return Fail(err, "string longer than 65,535 bytes");
    if (m.models.size() > 0xFFFF) return Fail(err, "more than 65,535 models");
    w.U16(static_cast<std::uint16_t>(m.models.size()));
    for (const ModelReply& e : m.models) {
        if (!w.Str(e.name)) return Fail(err, "model name longer than 65,535 bytes");
        w.U8(e.status);
        if (!w.Str(e.detail)) return Fail(err, "detail longer than 65,535 bytes");
    }
    return Finish(out, err);
}

bool DecodeHelloOk(const std::uint8_t* p, std::size_t n, HelloOk& m, std::string& err) {
    Reader r(p, n);
    std::uint16_t count = 0;
    if (!r.Str(m.server) || !r.Str(m.device) || !r.U16(count)) return Fail(err, "truncated HELLO_OK");
    m.models.clear();
    for (std::uint16_t i = 0; i < count; ++i) {
        ModelReply e;
        if (!r.Str(e.name) || !r.U8(e.status) || !r.Str(e.detail)) return Fail(err, "truncated HELLO_OK model");
        m.models.push_back(std::move(e));
    }
    if (!r.Done()) return Fail(err, "trailing bytes after HELLO_OK");
    return true;
}

// ---- RUN

std::uint64_t RunPayloadSize(const RunRequest& m) noexcept {
    std::uint64_t n = 4 + 2 + m.model.size() + 2;
    for (const TensorView& t : m.inputs) n += TensorSize(t);
    n += 2;
    for (const std::string_view o : m.outputs) n += 2 + o.size();
    return n;
}

bool EncodeRun(const RunRequest& m, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    if (RunPayloadSize(m) > kMaxPayload) return Fail(err, "payload above 64 MiB");
    if (!ValidModelName(m.model)) return Fail(err, "model name isn't a plain file name");
    if (m.inputs.size() > 0xFFFF || m.outputs.size() > 0xFFFF) return Fail(err, "more than 65,535 tensors");
    out.reserve(static_cast<std::size_t>(RunPayloadSize(m)));
    Writer w(out);
    w.U32(m.requestId);
    w.Str(m.model);
    w.U16(static_cast<std::uint16_t>(m.inputs.size()));
    for (const TensorView& t : m.inputs)
        if (!WriteTensor(w, t, err)) return false;
    w.U16(static_cast<std::uint16_t>(m.outputs.size()));
    for (const std::string_view o : m.outputs)
        if (!w.Str(o)) return Fail(err, "output name longer than 65,535 bytes");
    return Finish(out, err);
}

bool DecodeRun(const std::uint8_t* p, std::size_t n, DecodedRun& m, std::string& err) {
    Reader r(p, n);
    std::uint16_t inputs = 0;
    if (!r.U32(m.requestId) || !r.Str(m.model) || !r.U16(inputs)) return Fail(err, "truncated RUN");
    if (!ValidModelName(m.model)) return Fail(err, "model name isn't a plain file name");
    m.inputs.clear();
    for (std::uint16_t i = 0; i < inputs; ++i) {
        TensorRef t;
        if (!ReadTensor(r, t, err)) return false;
        m.inputs.push_back(std::move(t));
    }
    std::uint16_t outputs = 0;
    if (!r.U16(outputs)) return Fail(err, "truncated RUN outputs");
    m.outputs.clear();
    for (std::uint16_t i = 0; i < outputs; ++i) {
        std::string name;
        if (!r.Str(name)) return Fail(err, "truncated RUN output name");
        m.outputs.push_back(std::move(name));
    }
    if (!r.Done()) return Fail(err, "trailing bytes after RUN");
    return true;
}

// ---- RUN_OK

bool EncodeRunOk(const RunOk& m, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    if (m.outputs.size() > 0xFFFF) return Fail(err, "more than 65,535 tensors");
    Writer w(out);
    w.U32(m.requestId);
    w.U32(m.serverMicros);
    w.U16(static_cast<std::uint16_t>(m.outputs.size()));
    for (const TensorView& t : m.outputs)
        if (!WriteTensor(w, t, err)) return false;
    return Finish(out, err);
}

bool DecodeRunOk(const std::uint8_t* p, std::size_t n, DecodedRunOk& m, std::string& err) {
    Reader r(p, n);
    std::uint16_t outputs = 0;
    if (!r.U32(m.requestId) || !r.U32(m.serverMicros) || !r.U16(outputs)) return Fail(err, "truncated RUN_OK");
    m.outputs.clear();
    for (std::uint16_t i = 0; i < outputs; ++i) {
        TensorRef t;
        if (!ReadTensor(r, t, err)) return false;
        m.outputs.push_back(std::move(t));
    }
    if (!r.Done()) return Fail(err, "trailing bytes after RUN_OK");
    return true;
}

// ---- ERROR

bool EncodeError(const Error& m, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    Writer w(out);
    w.U32(m.requestId);
    w.U16(m.code);
    if (!w.Str(m.message)) return Fail(err, "message longer than 65,535 bytes");
    return Finish(out, err);
}

bool DecodeError(const std::uint8_t* p, std::size_t n, Error& m, std::string& err) {
    Reader r(p, n);
    if (!r.U32(m.requestId) || !r.U16(m.code) || !r.Str(m.message)) return Fail(err, "truncated ERROR");
    if (!r.Done()) return Fail(err, "trailing bytes after ERROR");
    return true;
}

// ---- NOTICE

bool EncodeNotice(const Notice& m, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    if (!ValidModelName(m.model)) return Fail(err, "model name isn't a plain file name");
    Writer w(out);
    if (!w.Str(m.model) || !w.Str(m.reason) || !w.Str(m.detail)) return Fail(err, "string longer than 65,535 bytes");
    w.U32(m.lines);
    return Finish(out, err);
}

bool DecodeNotice(const std::uint8_t* p, std::size_t n, Notice& m, std::string& err) {
    Reader r(p, n);
    if (!r.Str(m.model) || !r.Str(m.reason) || !r.Str(m.detail) || !r.U32(m.lines)) return Fail(err, "truncated NOTICE");
    if (!ValidModelName(m.model)) return Fail(err, "model name isn't a plain file name");
    if (!r.Done()) return Fail(err, "trailing bytes after NOTICE");
    return true;
}

// ---- FIND and FOUND

namespace {

bool EncodeOneString(const std::string& s, std::vector<std::uint8_t>& out, std::string& err) {
    out.clear();
    Writer w(out);
    if (!w.Str(s)) return Fail(err, "string longer than 65,535 bytes");
    return true;
}

bool DecodeOneString(const std::uint8_t* p, std::size_t n, std::string& s, const char* what, std::string& err) {
    Reader r(p, n);
    if (!r.Str(s)) return Fail(err, std::string("truncated ") + what);
    if (!r.Done()) return Fail(err, std::string("trailing bytes after ") + what);
    return true;
}

}  // namespace

bool EncodeFind(const std::string& client, std::vector<std::uint8_t>& out, std::string& err) { return EncodeOneString(client, out, err); }
bool DecodeFind(const std::uint8_t* p, std::size_t n, std::string& client, std::string& err) {
    return DecodeOneString(p, n, client, "FIND", err);
}
bool EncodeFound(const std::string& server, std::vector<std::uint8_t>& out, std::string& err) { return EncodeOneString(server, out, err); }
bool DecodeFound(const std::uint8_t* p, std::size_t n, std::string& server, std::string& err) {
    return DecodeOneString(p, n, server, "FOUND", err);
}

}  // namespace bfarb::proto
