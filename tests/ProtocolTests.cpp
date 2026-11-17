// Protocol versions 1, 2 and 3: every message's round trip, and the refusals (docs/PROTOCOL.md).

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
    bad[4] = 4;  // version 4
    CHECK(!DecodeHeader(bad.data(), h, err, &check));
    CHECK(check.versionRefused);
    bad[4] = 3;  // version 3 is known
    CHECK(DecodeHeader(bad.data(), h, err, &check));
    bad[4] = 0;
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
    // CheckRun: EncodeRun's answer and words, without the payload
    {
        const auto same = [&](const RunRequest& q, std::uint16_t v) {
            std::vector<std::uint8_t> made;
            std::string e1, e2;
            const bool encoded = EncodeRun(q, made, e1, v);
            const bool checked = CheckRun(q, e2, v);
            CHECK_EQ(encoded, checked);
            CHECK_EQ(e1, e2);
        };
        RunRequest q;
        q.model = "tiny.onnx";
        for (const std::uint16_t v : {kVersion1, kVersion2, kVersion3}) {
            q.inputs = {{"x", 1, {3}, x, sizeof x}};
            same(q, v);
            q.inputs = {{"x", 2, {2}, x, sizeof x}};
            same(q, v);
            q.inputs = {{"x", 1, {2}, nullptr, sizeof x}};
            same(q, v);
            q.inputs = {{"x", 1, {2}, x, sizeof x}};
            q.model = "../tiny.onnx";
            same(q, v);
            q.model = "tiny.onnx";
            same(q, v);
            q.mode = 1;  // a held run: version 3's alone, and refused without past_frames 0
            same(q, v);
            q.mode = 0;
            same(huge, v);
        }
    }
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
    CHECK(ErrorClosesConnection(1) && ErrorClosesConnection(2) && ErrorClosesConnection(5) && ErrorClosesConnection(6));
    CHECK(!ErrorClosesConnection(3) && !ErrorClosesConnection(4) && !ErrorClosesConnection(7));
    CHECK(ErrorEndsConnection(1) && ErrorEndsConnection(2) && ErrorEndsConnection(5) && ErrorEndsConnection(6) && ErrorEndsConnection(7));
    CHECK(!ErrorEndsConnection(3) && !ErrorEndsConnection(4) && !ErrorEndsConnection(0) && !ErrorEndsConnection(8));
    (void)Bytes;
}

// RUN_OK's byte length must equal its shape's size, above or below: Bridge.cpp copies exactly that many bytes.
TEST(ProtocolRunOkByteLengthMustMatchTheShape) {
    const float y[4] = {1, 2, 3, 4};
    RunOk m;
    m.requestId = 1;
    m.outputs = {{"y", 1, {2, 2}, y, sizeof y}};
    std::vector<std::uint8_t> p;
    std::string err;
    CHECK(EncodeRunOk(m, p, err));
    // request id, micros, count, name (2 + 1), dtype, rank, two dims: then the u32 byte length
    const std::size_t at = 4 + 4 + 2 + 3 + 1 + 1 + 16;
    CHECK_EQ(int(p[at]), 16);
    DecodedRunOk back;
    CHECK(DecodeRunOk(p.data(), p.size(), back, err));
    for (std::uint8_t wrong : {12, 20}) {  // below and above the shape's 16 bytes
        auto bad = p;
        bad[at] = wrong;
        if (wrong > 16) bad.insert(bad.end(), wrong - 16, 0);  // the bytes are there too
        CHECK(!DecodeRunOk(bad.data(), bad.size(), back, err));
        CHECK(err.find("byte length") != std::string::npos);
    }
    // more data than the length says is trailing bytes
    auto longer = p;
    longer.push_back(0);
    CHECK(!DecodeRunOk(longer.data(), longer.size(), back, err));
}

// ---- version 2

// Every version 1 payload is the same in version 2; only the header says 2. NOTICE is version 2's alone, and FIND and
// FOUND are datagrams, never TCP frames.
TEST(ProtocolVersion2Frames) {
    std::vector<std::uint8_t> p, f;
    std::string err;
    CHECK(EncodeHello(SampleHello(), p, err));
    CHECK(EncodeFrame(MsgType::Hello, p, f, err, kVersion2));
    CHECK_EQ(int(f[4]), 2);
    CHECK_EQ(int(f[5]), 0);
    Header h;
    CHECK(DecodeHeader(f.data(), h, err));
    CHECK_EQ(h.version, 2);
    CHECK_EQ(h.type, 1);
    Hello back;
    CHECK(DecodeHello(f.data() + kHeaderSize, h.length, back, err));
    CHECK_EQ(back.models.size(), std::size_t{2});
    std::vector<std::uint8_t> v1;
    CHECK(EncodeFrame(MsgType::Hello, p, v1, err));
    CHECK(std::equal(f.begin() + kHeaderSize, f.end(), v1.begin() + kHeaderSize));  // the same payload
    CHECK_EQ(int(v1[4]), 1);

    // which types each version has, on TCP and as datagrams
    for (std::uint16_t t = 1; t <= 7; ++t) {
        CHECK(KnownType(1, t));
        CHECK(KnownType(2, t));
        CHECK(!KnownType(2, t, true));
    }
    CHECK(!KnownType(1, 10));
    CHECK(KnownType(2, 10));
    for (std::uint16_t t : {8, 9}) {
        CHECK(!KnownType(1, t));
        CHECK(!KnownType(2, t));
        CHECK(!KnownType(1, t, true));
        CHECK(KnownType(2, t, true));
    }
    CHECK(!KnownType(2, 11));
    CHECK(!KnownType(4, 1));
    CHECK(!EncodeFrame(MsgType::Notice, {}, f, err, kVersion1));  // nothing of version 2 in a version 1 frame
    CHECK(!EncodeFrame(MsgType::Find, {}, f, err, kVersion2));
    CHECK(!EncodeFrame(MsgType::Hello, {}, f, err, 4));
    CHECK(EncodeFrame(MsgType::Pong, {}, f, err, kVersion2));
    auto bad = f;
    bad[6] = 10;  // NOTICE
    CHECK(DecodeHeader(bad.data(), h, err));
    bad[4] = 1;  // ... in version 1
    CHECK(!DecodeHeader(bad.data(), h, err));
    for (std::uint8_t t : {8, 9, 11}) {
        bad = f;
        bad[6] = t;
        CHECK(!DecodeHeader(bad.data(), h, err));
    }
}

TEST(ProtocolNoticeRoundTripAndRefusals) {
    Notice n{"a2f3_fp16.onnx", "timeout", "no answer within 1000 ms", 3};
    std::vector<std::uint8_t> p;
    std::string err;
    CHECK(EncodeNotice(n, p, err));
    CHECK_EQ(p.size(), std::size_t{2 + 14 + 2 + 7 + 2 + 24 + 4});
    Notice back;
    CHECK(DecodeNotice(p.data(), p.size(), back, err));
    CHECK_EQ(back.model, n.model);
    CHECK_EQ(back.reason, n.reason);
    CHECK_EQ(back.detail, n.detail);
    CHECK_EQ(back.lines, 3u);
    auto longer = p;
    longer.push_back(0);
    CHECK(!DecodeNotice(longer.data(), longer.size(), back, err));
    CHECK(!DecodeNotice(p.data(), p.size() - 1, back, err));
    CHECK(!EncodeNotice(Notice{"../a.onnx", "timeout", "", 1}, p, err));
    CHECK(!EncodeNotice(Notice{"", "timeout", "", 1}, p, err));
}

TEST(ProtocolFindAndFoundDatagrams) {
    std::vector<std::uint8_t> p, d;
    std::string err;
    CHECK(EncodeFind("BFARemoteBridge 1.3.0 (answered 0, fallbacks 0, timeouts 0)", p, err));
    CHECK(EncodeDatagram(MsgType::Find, p, d, err));
    CHECK(std::memcmp(d.data(), "BFAR", 4) == 0);
    CHECK_EQ(int(d[4]), 2);
    CHECK_EQ(int(d[6]), 8);
    CHECK_EQ(d.size(), kHeaderSize + p.size());
    Header h;
    std::vector<std::uint8_t> payload;
    CHECK(DecodeDatagram(d.data(), d.size(), h, payload, err));
    CHECK_EQ(h.type, 8);
    std::string client;
    CHECK(DecodeFind(payload.data(), payload.size(), client, err));
    CHECK_EQ(client, std::string("BFARemoteBridge 1.3.0 (answered 0, fallbacks 0, timeouts 0)"));
    // FOUND: str server, and no port
    CHECK(EncodeFound("PandorumLLM v3.98 Beta A2F", p, err));
    CHECK_EQ(p.size(), std::size_t{2 + 26});
    CHECK(EncodeDatagram(MsgType::Found, p, d, err));
    CHECK(DecodeDatagram(d.data(), d.size(), h, payload, err));
    std::string server;
    CHECK(DecodeFound(payload.data(), payload.size(), server, err));
    CHECK_EQ(server, std::string("PandorumLLM v3.98 Beta A2F"));
    auto extra = payload;
    extra.push_back(1);
    CHECK(!DecodeFound(extra.data(), extra.size(), server, err));  // a port after it isn't FOUND's
    // refusals: only FIND and FOUND, version 2, the length the datagram's, no more than one Ethernet frame
    CHECK(!EncodeDatagram(MsgType::Hello, p, d, err));
    CHECK(!EncodeDatagram(MsgType::Notice, p, d, err));
    CHECK(!EncodeDatagram(MsgType::Found, std::vector<std::uint8_t>(kMaxDatagram), d, err));
    CHECK(EncodeDatagram(MsgType::Found, p, d, err));
    auto bad = d;
    bad[4] = 1;
    CHECK(!DecodeDatagram(bad.data(), bad.size(), h, payload, err));
    bad = d;
    bad[6] = 1;
    CHECK(!DecodeDatagram(bad.data(), bad.size(), h, payload, err));
    bad = d;
    bad.push_back(0);
    CHECK(!DecodeDatagram(bad.data(), bad.size(), h, payload, err));
    CHECK(!DecodeDatagram(d.data(), d.size() - 1, h, payload, err));
    CHECK(!DecodeDatagram(d.data(), 11, h, payload, err));
    bad = d;
    bad[0] = 'X';
    CHECK(!DecodeDatagram(bad.data(), bad.size(), h, payload, err));
}

// ---- version 3

namespace {

// A run laid out as BFA 7.0.0's Performance model's, the state small.
struct HeldRun {
    float mel[4] = {1, 2, 3, 4};
    std::int64_t identity[1] = {1};
    float k[6] = {0}, v[6] = {0};
    std::int64_t frames[1] = {0};
    RunRequest Request(bool withState = true) const {
        RunRequest r;
        r.requestId = 5;
        r.model = "performance.onnx";
        r.inputs = {{"mel", 1, {1, 4}, mel, sizeof mel}, {"identity", 7, {1}, identity, sizeof identity}};
        if (withState) {
            r.inputs.push_back({"past_k", 1, {1, 6}, k, sizeof k});
            r.inputs.push_back({"past_v", 1, {1, 6}, v, sizeof v});
        }
        r.inputs.push_back({"past_frames", 7, {1}, frames, sizeof frames});
        r.outputs = {"normalized_motion"};
        if (withState) r.outputs = {"normalized_motion", "next_k", "next_v"};
        return r;
    }
};

}  // namespace

// Version 3 adds two fields to RUN (u64 stream, u8 mode, after the request id) and one to HELLO_OK's rows (u8 held);
// every other message keeps version 2's payload, NOTICE included, and only the frame's version says 3. FIND and FOUND
// stay version 2 datagrams.
TEST(ProtocolVersion3Frames) {
    for (std::uint16_t t = 1; t <= 7; ++t) {
        CHECK(KnownType(3, t));
        CHECK(!KnownType(3, t, true));
    }
    CHECK(KnownType(3, 10));
    for (std::uint16_t t : {8, 9, 11}) CHECK(!KnownType(3, t));
    CHECK(!KnownType(3, 8, true));
    CHECK(!KnownType(3, 9, true));
    CHECK_EQ(kNewestVersion, kVersion3);
    CHECK_EQ(std::string(ErrorCodeName(8)), std::string("the line's state isn't held"));
    CHECK(!ErrorClosesConnection(8));
    CHECK(!ErrorEndsConnection(8));  // the connection stays open, as after 3 and 4
    std::vector<std::uint8_t> p, f;
    std::string err;
    CHECK(EncodeHello(SampleHello(), p, err));
    CHECK(EncodeFrame(MsgType::Hello, p, f, err, kVersion3));
    CHECK_EQ(int(f[4]), 3);
    CHECK(EncodeFrame(MsgType::Notice, p, f, err, kVersion3));
    // a datagram is version 2's whatever the connection speaks
    CHECK(EncodeFind("c", p, err));
    CHECK(EncodeDatagram(MsgType::Find, p, f, err));
    CHECK_EQ(int(f[4]), 2);
    f[4] = 3;
    Header h;
    std::vector<std::uint8_t> payload;
    CHECK(!DecodeDatagram(f.data(), f.size(), h, payload, err));

    // HELLO_OK: held after each row's detail, in version 3 alone
    HelloOk m;
    m.server = "s";
    m.device = "d";
    m.models = {{"a.onnx", 0, "", 0}, {"performance.onnx", 0, "x", 1}};
    std::vector<std::uint8_t> v2, v3;
    CHECK(!EncodeHelloOk(m, v2, err, kVersion2));  // held is version 3's
    m.models[1].held = 0;
    CHECK(EncodeHelloOk(m, v2, err, kVersion2));
    m.models[1].held = 1;
    CHECK(EncodeHelloOk(m, v3, err, kVersion3));
    CHECK_EQ(v3.size(), v2.size() + 2);
    CHECK_EQ(int(v3.back()), 1);
    HelloOk back;
    CHECK(DecodeHelloOk(v3.data(), v3.size(), back, err, kVersion3));
    CHECK_EQ(int(back.models[0].held), 0);
    CHECK_EQ(int(back.models[1].held), 1);
    CHECK_EQ(back.models[1].detail, std::string("x"));
    CHECK(!DecodeHelloOk(v3.data(), v3.size(), back, err, kVersion2));  // trailing bytes
    CHECK(!DecodeHelloOk(v2.data(), v2.size(), back, err, kVersion3));  // truncated
    auto two = v3;
    two.back() = 2;
    CHECK(!DecodeHelloOk(two.data(), two.size(), back, err, kVersion3));  // held is 0 or 1
    m.models[1].held = 2;
    CHECK(!EncodeHelloOk(m, v3, err, kVersion3));

    // RUN: stream and mode after the request id, then version 2's layout
    HeldRun run;
    RunRequest r = run.Request();
    std::vector<std::uint8_t> carried2, carried3;
    CHECK(EncodeRun(r, carried2, err, kVersion2));
    CHECK(EncodeRun(r, carried3, err, kVersion3));
    CHECK_EQ(carried3.size(), carried2.size() + 9);
    CHECK_EQ(carried3.size(), static_cast<std::size_t>(RunPayloadSize(r, kVersion3)));
    CHECK(std::equal(carried2.begin(), carried2.begin() + 4, carried3.begin()));
    CHECK(std::equal(carried2.begin() + 4, carried2.end(), carried3.begin() + 13));
    for (int i = 4; i < 13; ++i) CHECK_EQ(int(carried3[static_cast<std::size_t>(i)]), 0);  // mode 0, stream 0
    DecodedRun d;
    CHECK(DecodeRun(carried3.data(), carried3.size(), d, err, kVersion3));
    CHECK_EQ(d.mode, 0);
    CHECK_EQ(d.inputs.size(), std::size_t{5});
    CHECK(!DecodeRun(carried3.data(), carried3.size(), d, err, kVersion2));
    RunRequest start = run.Request(false);
    start.stream = 0x0102030405060708ull;
    start.mode = 1;
    CHECK(!EncodeRun(start, p, err, kVersion2));  // nothing of version 3 in a version 2 RUN
    start.mode = 0;
    CHECK(!EncodeRun(start, p, err, kVersion2));
    start.mode = 1;
    CHECK(EncodeRun(start, p, err, kVersion3));
    CHECK_EQ(int(p[4]), 8);
    CHECK_EQ(int(p[11]), 1);
    CHECK_EQ(int(p[12]), 1);
    CHECK(DecodeRun(p.data(), p.size(), d, err, kVersion3));
    CHECK_EQ(d.stream, 0x0102030405060708ull);
    CHECK_EQ(int(d.mode), 1);
    CHECK_EQ(d.model, std::string("performance.onnx"));
    CHECK_EQ(d.inputs.size(), std::size_t{3});
    CHECK(d.outputs == (std::vector<std::string>{"normalized_motion"}));
    p[12] = 3;
    CHECK(!DecodeRun(p.data(), p.size(), d, err, kVersion3));  // a mode other than 0, 1 or 2
    CHECK(!DecodeRun(p.data(), 12, d, err, kVersion3));
}

// The agreement's refusals as malformed are never encoded: a mode other than 0, 1 or 2; mode 1 with past_frames other
// than 0, mode 2 with 0 (or either without past_frames); mode 1 or 2 carrying past_X or asking for next_X.
TEST(ProtocolHeldRunRefusals) {
    HeldRun run;
    std::vector<std::uint8_t> p;
    std::string err;
    const auto held = [&](std::uint8_t mode, std::int64_t frames, bool withState) {
        run.frames[0] = frames;
        RunRequest r = run.Request(withState);
        r.stream = 1;
        r.mode = mode;
        return EncodeRun(r, p, err, kVersion3);
    };
    CHECK(held(1, 0, false));
    CHECK(held(2, 90, false));
    CHECK(held(0, 90, true));  // carried: as version 2
    CHECK(!held(3, 0, false));
    CHECK(!held(1, 90, false));
    CHECK(!held(2, 0, false));
    CHECK(!held(1, 0, true));
    CHECK(!held(2, 90, true));
    RunRequest r = run.Request(false);
    r.mode = 1;
    r.inputs.pop_back();  // no past_frames
    CHECK(!EncodeRun(r, p, err, kVersion3));
    r = run.Request(false);
    r.mode = 2;
    run.frames[0] = 180;
    r.outputs = {"normalized_motion", "next_v"};
    CHECK(!EncodeRun(r, p, err, kVersion3));
    CHECK_EQ(HeldRunProblem(2, {"mel", "past_frames"}, {"normalized_motion", "next_k"}, run.frames), std::string("a continue (mode 2) asking for next_k"));
    CHECK(HeldRunProblem(0, {"past_k"}, {"next_k"}, nullptr).empty());
}

// A run's line state, by its names: past_X with next_X, and an int64 past_frames of one element.
TEST(ProtocolDescribesALinesState) {
    HeldRun run;
    run.frames[0] = 90;
    LineState s = DescribeLineState(run.Request());
    CHECK(s.stateful);
    CHECK_EQ(s.pastFrames, 90);
    CHECK(s.stateInput == (std::vector<bool>{false, false, true, true, false}));
    CHECK(s.pairedInput == (std::vector<int>{-1, 2, 3}));
    // the outputs in another order pair the same way
    RunRequest r = run.Request();
    r.outputs = {"next_v", "normalized_motion", "next_k"};
    s = DescribeLineState(r);
    CHECK(s.pairedInput == (std::vector<int>{3, -1, 2}));
    // anything else is a run like any other
    r = run.Request();
    r.outputs = {"normalized_motion", "next_k"};  // past_v without next_v
    CHECK(!DescribeLineState(r).stateful);
    r = run.Request();
    r.outputs.push_back("next_q");  // next_q without past_q
    CHECK(!DescribeLineState(r).stateful);
    r = run.Request();
    r.inputs.pop_back();  // no past_frames
    CHECK(!DescribeLineState(r).stateful);
    r = run.Request();
    r.inputs.back().dtype = 1;  // past_frames not int64
    r.inputs.back().bytes = 4;
    CHECK(!DescribeLineState(r).stateful);
    run.frames[0] = -1;
    CHECK(!DescribeLineState(run.Request()).stateful);
    run.frames[0] = 0;
    CHECK(!DescribeLineState(run.Request(false)).stateful);  // no pair
    CHECK(DescribeLineState(run.Request()).stateful);
}
