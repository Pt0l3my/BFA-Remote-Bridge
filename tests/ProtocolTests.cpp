// Protocol version 1: every message's round trip, and the refusals (docs/PROTOCOL.md).

#include <cstring>

#include "Test.h"
#include "core/Protocol.h"

using namespace bfarb;
using namespace bfarb::proto;

namespace {

std::vector<std::uint8_t> Bytes(const char* s) { return std::vector<std::uint8_t>(s, s + std::strlen(s)); }

Hello SampleHello() {
    Hello h;
    h.client = "BFARemoteBridge 1.0.0";
    h.key = "secret";
    h.ortVersion = "1.24.4";
    ModelEntry a;
    a.name = "a2f3_fp16.onnx";
    a.size = 123456789;
    for (int i = 0; i < 32; ++i) a.sha256[i] = static_cast<std::uint8_t>(i * 7);
    a.overrides = {{"window_dynamic_axes_1", 1}, {"identity_dynamic_axes_1", 1}, {"neg", -5}};
    ModelEntry b;
    b.name = "performance.onnx";
    b.size = 329557;
    b.overrides = {{"batch", 1}};
    h.models = {a, b};
    return h;
}

}  // namespace

TEST(ProtocolHelloRoundTrip) {
    const Hello h = SampleHello();
    std::vector<std::uint8_t> p;
    std::string err;
    CHECK(EncodeHello(h, p, err));
    Hello back;
    CHECK(DecodeHello(p.data(), p.size(), back, err));
    CHECK_EQ(back.client, h.client);
    CHECK_EQ(back.key, h.key);
    CHECK_EQ(back.ortVersion, h.ortVersion);
    CHECK(back.models == h.models);

    // the exact layout of an empty HELLO: three strings and a zero count
    Hello empty;
    empty.client = "c";
    CHECK(EncodeHello(empty, p, err));
    const std::vector<std::uint8_t> expected = {1, 0, 'c', 0, 0, 0, 0, 0, 0};
    CHECK(p == expected);
}

TEST(ProtocolHelloOkRoundTrip) {
    HelloOk m;
    m.server = "PandorumLLM A2F";
    m.device = "DirectML 0";
    m.models = {{"a.onnx", 0, ""}, {"b.onnx", 2, "different file"}};
    std::vector<std::uint8_t> p;
    std::string err;
    CHECK(EncodeHelloOk(m, p, err));
    HelloOk back;
    CHECK(DecodeHelloOk(p.data(), p.size(), back, err));
    CHECK_EQ(back.server, m.server);
    CHECK_EQ(back.device, m.device);
    CHECK_EQ(back.models.size(), std::size_t{2});
    CHECK_EQ(back.models[1].name, std::string("b.onnx"));
    CHECK_EQ(int(back.models[1].status), 2);
    CHECK_EQ(back.models[1].detail, std::string("different file"));
}

TEST(ProtocolRunRoundTrip) {
    const float x[6] = {1, 2, 3, 4, 5, 6};
    const std::int64_t id[1] = {-3};
    const std::uint16_t h[2] = {0x3C00, 0xC000};
    const double d[1] = {0.25};
    const std::int32_t i32[2] = {7, -7};
    RunRequest r;
    r.requestId = 42;
    r.model = "tiny.onnx";
    r.inputs = {{"x", 1, {2, 3}, x, sizeof x}, {"id", 7, {1}, id, sizeof id}, {"h", 10, {2}, h, sizeof h},
                {"d", 11, {}, d, sizeof d},     {"i", 6, {2}, i32, sizeof i32}};
    r.outputs = {"y", "z"};
    std::vector<std::uint8_t> p;
    std::string err;
    CHECK(EncodeRun(r, p, err));
    CHECK_EQ(p.size(), static_cast<std::size_t>(RunPayloadSize(r)));
    DecodedRun back;
    CHECK(DecodeRun(p.data(), p.size(), back, err));
    CHECK_EQ(back.requestId, 42u);
    CHECK_EQ(back.model, std::string("tiny.onnx"));
    CHECK_EQ(back.inputs.size(), std::size_t{5});
    CHECK_EQ(back.inputs[0].name, std::string("x"));
    CHECK(back.inputs[0].dims == (std::vector<std::int64_t>{2, 3}));
    CHECK_EQ(back.inputs[0].bytes, 24u);
    CHECK(std::memcmp(p.data() + back.inputs[0].offset, x, sizeof x) == 0);
    CHECK(std::memcmp(p.data() + back.inputs[1].offset, id, sizeof id) == 0);
    CHECK(back.inputs[3].dims.empty());
    CHECK(std::memcmp(p.data() + back.inputs[4].offset, i32, sizeof i32) == 0);
    CHECK(back.outputs == (std::vector<std::string>{"y", "z"}));
}

TEST(ProtocolRunOkRoundTrip) {
    const float y[3] = {0.5f, -0.5f, 1e-7f};
    RunOk m;
    m.requestId = 9;
    m.serverMicros = 1234;
    m.outputs = {{"y", 1, {1, 3}, y, sizeof y}, {"empty", 1, {0, 3}, nullptr, 0}};
    std::vector<std::uint8_t> p;
    std::string err;
    CHECK(EncodeRunOk(m, p, err));
    DecodedRunOk back;
    CHECK(DecodeRunOk(p.data(), p.size(), back, err));
    CHECK_EQ(back.requestId, 9u);
    CHECK_EQ(back.serverMicros, 1234u);
    CHECK_EQ(back.outputs.size(), std::size_t{2});
    CHECK(std::memcmp(p.data() + back.outputs[0].offset, y, sizeof y) == 0);
    CHECK_EQ(back.outputs[1].bytes, 0u);
}

TEST(ProtocolErrorAndFrameRoundTrip) {
    Error e{7, 4, "the runtime's message"};
    std::vector<std::uint8_t> p, f;
    std::string err;
    CHECK(EncodeError(e, p, err));
    CHECK(EncodeFrame(MsgType::Error, p, f, err));
    CHECK_EQ(f.size(), kHeaderSize + p.size());
    CHECK(std::memcmp(f.data(), "BFAR", 4) == 0);
    CHECK_EQ(int(f[4]), 1);  // version 1, little-endian
    CHECK_EQ(int(f[5]), 0);
    CHECK_EQ(int(f[6]), 5);  // ERROR
    Header h;
    CHECK(DecodeHeader(f.data(), h, err));
    CHECK_EQ(h.type, 5);
    CHECK_EQ(h.length, static_cast<std::uint32_t>(p.size()));
    Error back;
    CHECK(DecodeError(f.data() + kHeaderSize, h.length, back, err));
    CHECK_EQ(back.requestId, 7u);
    CHECK_EQ(back.code, 4);
    CHECK_EQ(back.message, e.message);

    // PING and PONG carry nothing
    CHECK(EncodeFrame(MsgType::Ping, {}, f, err));
    CHECK_EQ(f.size(), kHeaderSize);
    CHECK(DecodeHeader(f.data(), h, err));
    CHECK_EQ(h.type, 6);
    CHECK_EQ(h.length, 0u);
}

TEST(ProtocolHeaderRefusals) {
    std::vector<std::uint8_t> f;
    std::string err;
    CHECK(EncodeFrame(MsgType::Pong, {}, f, err));
    Header h;
    HeaderCheck check;
    auto bad = f;
    bad[0] = 'X';
    CHECK(!DecodeHeader(bad.data(), h, err, &check));  // bad magic
    bad = f;
    bad[4] = 2;  // version 2
    CHECK(!DecodeHeader(bad.data(), h, err, &check));
    CHECK(check.versionRefused);
    bad = f;
    bad[6] = 8;  // unknown type
    CHECK(!DecodeHeader(bad.data(), h, err, &check));
    bad = f;
    bad[6] = 0;
    CHECK(!DecodeHeader(bad.data(), h, err, &check));
    bad = f;
    const std::uint32_t big = kMaxPayload + 1;
    std::memcpy(bad.data() + 8, &big, 4);  // oversize (little-endian host)
    CHECK(!DecodeHeader(bad.data(), h, err, &check));
    CHECK(check.tooLarge);
    const std::uint32_t max = kMaxPayload;
    std::memcpy(bad.data() + 8, &max, 4);
    CHECK(DecodeHeader(bad.data(), h, err, &check));  // exactly 64 MiB is allowed
}

TEST(ProtocolPayloadRefusals) {
    std::vector<std::uint8_t> p;
    std::string err;
    // names with path parts, in HELLO either way
    for (const char* name : {"../a.onnx", "a/b.onnx", "a\\b.onnx", "C:a.onnx", "..", ".", "", "a..b.onnx", "a\nb"}) {
        Hello h = SampleHello();
        h.models[0].name = name;
        CHECK(!EncodeHello(h, p, err));
        h.models[0].name = "x.onnx";
        CHECK(EncodeHello(h, p, err));
        // patch "x" into a '/' in the encoded bytes: the decoder refuses it too
        for (auto& b : p)
            if (b == 'x') {
                b = '/';
                break;
            }
        Hello back;
        CHECK(!DecodeHello(p.data(), p.size(), back, err));
    }
    CHECK(ValidModelName("a2f3_fp16.onnx"));
    CHECK(ValidModelName("network_fp16.onnx"));

    // truncation and trailing bytes
    Hello h = SampleHello();
    CHECK(EncodeHello(h, p, err));
    Hello back;
    for (std::size_t n = 0; n < p.size(); ++n) CHECK(!DecodeHello(p.data(), n, back, err));
    p.push_back(0);
    CHECK(!DecodeHello(p.data(), p.size(), back, err));

    // a tensor whose length doesn't match its shape, an unsupported type, a negative dimension
    const float x[2] = {1, 2};
    RunRequest r;
    r.model = "tiny.onnx";
    r.inputs = {{"x", 1, {3}, x, sizeof x}};
    CHECK(!EncodeRun(r, p, err));
    r.inputs = {{"x", 2, {2}, x, sizeof x}};  // uint8 isn't carried
    CHECK(!EncodeRun(r, p, err));
    r.inputs = {{"x", 1, {-2}, x, sizeof x}};
    CHECK(!EncodeRun(r, p, err));
    r.inputs = {{"x", 1, {2}, x, sizeof x}};
    r.model = "../tiny.onnx";
    CHECK(!EncodeRun(r, p, err));
    r.model = "tiny.onnx";
    CHECK(EncodeRun(r, p, err));
    DecodedRun run;
    CHECK(DecodeRun(p.data(), p.size(), run, err));
    // corrupt the byte length (u32 after name, model, count, tensor name, dtype, rank, one dim)
    const std::size_t lengthAt = 4 + 2 + 9 + 2 + 2 + 1 + 1 + 1 + 8;
    CHECK_EQ(int(p[lengthAt]), 8);
    p[lengthAt] = 12;
    CHECK(!DecodeRun(p.data(), p.size(), run, err));
    p[lengthAt] = 8;
    p[4 + 2 + 9 + 2 + 2 + 1] = 99;  // dtype 99
    CHECK(!DecodeRun(p.data(), p.size(), run, err));

    // too large: a tensor of more than 64 MiB is refused before anything is copied
    RunRequest huge;
    huge.model = "tiny.onnx";
    huge.inputs = {{"x", 1, {static_cast<std::int64_t>(kMaxPayload / 4 + 1)}, x, kMaxPayload + 4}};
    CHECK(!EncodeRun(huge, p, err));
    std::uint64_t bytes = 0;
    CHECK(!ByteLength(1, {static_cast<std::int64_t>(kMaxPayload / 4 + 1)}, bytes));
    CHECK(!ByteLength(1, {INT64_MAX, INT64_MAX}, bytes));  // overflow
    CHECK(ByteLength(1, {0, 5}, bytes) && bytes == 0);

    // the codes and their names
    CHECK_EQ(DTypeSize(1), std::size_t{4});
    CHECK_EQ(DTypeSize(7), std::size_t{8});
    CHECK_EQ(DTypeSize(6), std::size_t{4});
    CHECK_EQ(DTypeSize(10), std::size_t{2});
    CHECK_EQ(DTypeSize(11), std::size_t{8});
    CHECK_EQ(DTypeSize(16), std::size_t{0});
    CHECK(ErrorClosesConnection(1) && ErrorClosesConnection(2) && ErrorClosesConnection(6));
    CHECK(!ErrorClosesConnection(3) && !ErrorClosesConnection(4) && !ErrorClosesConnection(5) && !ErrorClosesConnection(7));
    (void)Bytes;
}
