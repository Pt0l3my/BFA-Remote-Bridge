#pragma once
// Protocol versions 1, 2 and 3 between the bridge and an A2F server (docs/PROTOCOL.md): framing, every message's
// encoding and decoding, and every limit. Platform-neutral, no I/O. All integers little-endian. Version 2 is version
// 1 with NOTICE (one-way, on TCP) and FIND and FOUND (UDP datagrams) added; every other payload is the same in both.
// Version 3 (docs/AGREEMENT-1.7.md) is version 2 with a line's state kept on the server: HELLO_OK's rows gain `u8 held`
// and RUN gains `u64 stream` and `u8 mode` after its request id; every other payload is version 2's, and FIND and
// FOUND stay version 2 datagrams.
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
constexpr std::uint16_t kVersion3 = 3;  // version 2 with a line's state held on the server
constexpr std::uint16_t kNewestVersion = kVersion3;
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
    Notice = 10,  // version 2 and 3, one-way
};

// Whether a frame of `type` exists in `version` on TCP (1 to 7; 2 and 3 add NOTICE), or as a datagram (FIND and FOUND,
// version 2 only: version 3 changes nothing on UDP).
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
    State = 8,  // version 3: a continue whose stream holds no state, or another model's, or another count; the
                // connection stays open
};

// A RUN's mode, version 3 (mode 0 is version 2's run, exactly).
enum class RunMode : std::uint8_t {
    Carried = 0,   // the run carries its state, if any, as in version 2; the server neither reads nor changes a held state
    Start = 1,     // past_frames 0: the server starts from zeros and holds next_X for the stream
    Continue = 2,  // past_frames above 0: the server fills past_X from its held copy for the stream
};
constexpr std::uint8_t kLastRunMode = 2;

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
    std::uint8_t held = 0;  // version 3 only: 1 when the server keeps this model's line state
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
    std::uint64_t stream = 0;  // version 3 only
    std::uint8_t mode = 0;     // version 3 only (RunMode); a version 1 or 2 RUN is always carried
    std::string model;
    std::vector<TensorView> inputs;
    std::vector<std::string_view> outputs;
};

struct DecodedRun {
    std::uint32_t requestId = 0;
    std::uint64_t stream = 0;
    std::uint8_t mode = 0;
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

// HELLO_OK and RUN take the version of the frame they go in: version 3's rows carry `held`, and its RUN `stream` and
// `mode`. Versions 1 and 2 share one layout.
bool EncodeHelloOk(const HelloOk& m, std::vector<std::uint8_t>& out, std::string& err, std::uint16_t version = kVersion1);
bool DecodeHelloOk(const std::uint8_t* p, std::size_t n, HelloOk& m, std::string& err, std::uint16_t version = kVersion1);

// A RUN in versions 1 and 2 is always carried: a mode or stream other than 0 is refused there. In version 3 a mode
// other than 0, 1 or 2 is refused, and so is a held run the agreement refuses as malformed (HeldRunProblem).
bool EncodeRun(const RunRequest& m, std::vector<std::uint8_t>& out, std::string& err, std::uint16_t version = kVersion1);
// EncodeRun's checks alone, with its answer and err, without making or copying the payload.
bool CheckRun(const RunRequest& m, std::string& err, std::uint16_t version = kVersion1);
bool DecodeRun(const std::uint8_t* p, std::size_t n, DecodedRun& m, std::string& err, std::uint16_t version = kVersion1);

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
std::uint64_t RunPayloadSize(const RunRequest& m, std::uint16_t version = kVersion1) noexcept;

// ---- a line's state (version 3)

constexpr std::string_view kPastFrames = "past_frames";
constexpr std::string_view kPastPrefix = "past_";
constexpr std::string_view kNextPrefix = "next_";

// A run's line state, by its names: inputs past_X paired with outputs next_X (BFA 7.0.0's past_k/next_k and
// past_v/next_v), and the int64 past_frames, one element, 0 or more. Stateful only when every past_X input (but
// past_frames) has its next_X among the outputs, every next_X output its past_X among the inputs, there is at least
// one pair, and past_frames reads; anything else is a run like any other.
struct LineState {
    bool stateful = false;
    std::int64_t pastFrames = 0;
    std::vector<bool> stateInput;  // per input: a past_X of a pair (never past_frames)
    std::vector<int> pairedInput;  // per output: the index of the past_X input its next_X pairs with, else -1
};
LineState DescribeLineState(const RunRequest& m);

// Why the agreement refuses a version 3 RUN as malformed, given what the request alone shows (the server's held 0 for
// the model aside); empty when it doesn't: a mode other than 0, 1 or 2; mode 1 with past_frames other than 0, or none;
// mode 2 with past_frames 0, or none; mode 1 or 2 with a past_X (but past_frames) among the inputs or a next_X among the
// outputs asked for.
std::string HeldRunProblem(std::uint8_t mode, const std::vector<std::string_view>& inputs, const std::vector<std::string_view>& outputs,
                           const std::int64_t* pastFrames);

}  // namespace bfarb::proto
