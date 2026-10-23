#pragma once
// Protocol versions 1 and 2 between the bridge and an A2F server (docs/PROTOCOL.md): framing, every message's
// encoding and decoding, and every limit. Platform-neutral, no I/O. All integers little-endian. Version 2 is version
// 1 with NOTICE (one-way, on TCP) and FIND and FOUND (UDP datagrams) added; every other payload is the same in both.
//
// Decoders never trust a length: each read is bounds-checked, each payload must be consumed exactly, and a
// tensor's byte length must equal its element count times its type's size. Encoders refuse what the decoder
// would refuse (an oversize payload, a string longer than 65,535 bytes, a count above 65,535).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bfarb::proto {

constexpr std::uint8_t kMagic[4] = {'B', 'F', 'A', 'R'};
constexpr std::uint16_t kVersion1 = 1;
constexpr std::uint16_t kVersion2 = 2;  // version 1 with NOTICE, FIND and FOUND
constexpr std::uint16_t kNewestVersion = kVersion2;
constexpr std::size_t kHeaderSize = 12;
constexpr std::uint32_t kMaxPayload = 64u * 1024u * 1024u;  // 64 MiB; larger is refused
constexpr std::size_t kMaxRank = 255;                       // a u8
constexpr std::size_t kMaxDatagram = 1472;                  // FIND or FOUND, header included: one Ethernet frame's UDP payload

enum class MsgType : std::uint16_t {
    Hello = 1,
    HelloOk = 2,
    Run = 3,
    RunOk = 4,
    Error = 5,
    Ping = 6,
    Pong = 7,
    Find = 8,     // version 2, UDP only
    Found = 9,    // version 2, UDP only
    Notice = 10,  // version 2, one-way
};

// Whether a frame of `type` exists in `version` on TCP (1 to 7; 2 adds NOTICE), or as a datagram (FIND and FOUND,
// version 2).
bool KnownType(std::uint16_t version, std::uint16_t type, bool datagram = false) noexcept;

enum class ModelStatus : std::uint8_t {
    Ready = 0,
    NotFound = 1,
    DifferentFile = 2,  // size or SHA-256
    FailedToLoad = 3,
    NotAllowed = 4,
};

enum class ErrorCode : std::uint16_t {
    Malformed = 1,
    WrongKey = 2,
    UnknownModel = 3,
    InferenceFailed = 4,
    TooLarge = 5,
    VersionNotSupported = 6,
    Busy = 7,
};

// ONNX TensorProto codes. BFA uses only float and int64.
enum class DType : std::uint8_t {
    Float = 1,
    Int32 = 6,
    Int64 = 7,
    Float16 = 10,
    Double = 11,
};

// The size of one element of a supported type; 0 for any other code.
std::size_t DTypeSize(std::uint8_t dtype) noexcept;
const char* DTypeName(std::uint8_t dtype) noexcept;
const char* ModelStatusName(std::uint8_t status) noexcept;
const char* ErrorCodeName(std::uint16_t code) noexcept;

// The server closes the connection after these codes (1, 2, 5 and 6).
bool ErrorClosesConnection(std::uint16_t code) noexcept;
// The bridge treats these as the connection's end: the closing codes, and 7 (busy or stopping).
bool ErrorEndsConnection(std::uint16_t code) noexcept;

// A model's base name: not empty, no '/', '\\' or ':' (a drive letter), not "." or "..", no ".." component,
// no control character.
bool ValidModelName(std::string_view name) noexcept;

// The element count of dims, false on a negative dimension or an overflow. Rank 0 is one element.
bool ElementCount(const std::vector<std::int64_t>& dims, std::uint64_t& count) noexcept;
// The byte length of a tensor of dtype and dims, false when unsupported, negative or above kMaxPayload.
bool ByteLength(std::uint8_t dtype, const std::vector<std::int64_t>& dims, std::uint64_t& bytes) noexcept;

struct Header {
    std::uint16_t version = kVersion1;
    std::uint16_t type = 0;
    std::uint32_t length = 0;
};

struct Override {
    std::string name;
    std::int64_t value = 0;
    bool operator==(const Override&) const = default;
};

struct ModelEntry {
    std::string name;  // base name, no path
    std::uint64_t size = 0;
    std::array<std::uint8_t, 32> sha256{};
    std::vector<Override> overrides;
    bool operator==(const ModelEntry&) const = default;
};

struct Hello {
    std::string client;
    std::string key;
    std::string ortVersion;
    std::vector<ModelEntry> models;
};

struct ModelReply {
    std::string name;
    std::uint8_t status = 0;
    std::string detail;
};

struct HelloOk {
    std::string server;
    std::string device;
    std::vector<ModelReply> models;
};

// A tensor to encode: its bytes stay where the caller has them.
struct TensorView {
    std::string_view name;
    std::uint8_t dtype = 0;
    std::vector<std::int64_t> dims;
    const void* data = nullptr;
    std::size_t bytes = 0;
};

// A decoded tensor: its bytes are at `offset` in the payload it came from.
struct TensorRef {
    std::string name;
    std::uint8_t dtype = 0;
    std::vector<std::int64_t> dims;
    std::size_t offset = 0;
    std::uint32_t bytes = 0;
};

struct RunRequest {
    std::uint32_t requestId = 0;
    std::string model;
    std::vector<TensorView> inputs;
    std::vector<std::string_view> outputs;
};

struct DecodedRun {
    std::uint32_t requestId = 0;
    std::string model;
    std::vector<TensorRef> inputs;
    std::vector<std::string> outputs;
};

struct RunOk {
    std::uint32_t requestId = 0;
    std::uint32_t serverMicros = 0;
    std::vector<TensorView> outputs;
};

struct DecodedRunOk {
    std::uint32_t requestId = 0;
    std::uint32_t serverMicros = 0;
    std::vector<TensorRef> outputs;
};

struct Error {
    std::uint32_t requestId = 0;
    std::uint16_t code = 0;
    std::string message;
};

// NOTICE (version 2): a line that fell back, told to the server. One-way: nothing answers it.
struct Notice {
    std::string model;   // a base name, as in HELLO
    std::string reason;  // one word of core/Reasons.h's ReasonKey
    std::string detail;  // one line
    std::uint32_t lines = 0;  // the lines that fell back since the previous NOTICE, the latest included
};

// ---- frames

// A whole TCP frame (header and payload) of `version`. False, with err, when the payload would be refused, or the type
// isn't one of that version's on TCP (NOTICE in version 1, FIND or FOUND in any).
bool EncodeFrame(MsgType type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& frame, std::string& err,
                 std::uint16_t version = kVersion1);
// The 12 header bytes of a TCP frame: false, with err, on a wrong magic, a version other than 1 or 2 (versionRefused
// set), a type that isn't one of its version's on TCP, or a payload above kMaxPayload (tooLarge set).
struct HeaderCheck {
    bool versionRefused = false;
    bool tooLarge = false;
};
bool DecodeHeader(const std::uint8_t* bytes, Header& header, std::string& err, HeaderCheck* check = nullptr);

// A FIND or FOUND datagram (version 2): the header, then the payload, nothing after it, at most kMaxDatagram bytes.
bool EncodeDatagram(MsgType type, const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& datagram, std::string& err);
// A datagram's header and payload: false, with err, unless it is exactly one version 2 frame of FIND or FOUND.
bool DecodeDatagram(const std::uint8_t* p, std::size_t n, Header& header, std::vector<std::uint8_t>& payload, std::string& err);

// ---- payloads (each encoder returns the payload only; EncodeFrame wraps it)

bool EncodeHello(const Hello& m, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeHello(const std::uint8_t* p, std::size_t n, Hello& m, std::string& err);

bool EncodeHelloOk(const HelloOk& m, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeHelloOk(const std::uint8_t* p, std::size_t n, HelloOk& m, std::string& err);

bool EncodeRun(const RunRequest& m, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeRun(const std::uint8_t* p, std::size_t n, DecodedRun& m, std::string& err);

bool EncodeRunOk(const RunOk& m, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeRunOk(const std::uint8_t* p, std::size_t n, DecodedRunOk& m, std::string& err);

bool EncodeError(const Error& m, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeError(const std::uint8_t* p, std::size_t n, Error& m, std::string& err);

bool EncodeNotice(const Notice& m, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeNotice(const std::uint8_t* p, std::size_t n, Notice& m, std::string& err);

// FIND's payload, str client; FOUND's, str server (it carries no port: the client connects on the port FIND reached).
bool EncodeFind(const std::string& client, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeFind(const std::uint8_t* p, std::size_t n, std::string& client, std::string& err);
bool EncodeFound(const std::string& server, std::vector<std::uint8_t>& out, std::string& err);
bool DecodeFound(const std::uint8_t* p, std::size_t n, std::string& server, std::string& err);

// The size of the RUN payload EncodeRun would make, without making it (for the 64 MiB check before copying).
std::uint64_t RunPayloadSize(const RunRequest& m) noexcept;

}  // namespace bfarb::proto
