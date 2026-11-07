// SHA-256, the settings, the status texts, the monitor, the runtime's API versions and the PE reads.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "Test.h"
#include "core/BfaMode.h"
#include "core/BfaSizes.h"
#include "core/IniText.h"
#include "core/Bridge.h"
#include "core/Monitor.h"
#include "core/PeImage.h"
#include "core/Scrub.h"
#include "core/Settings.h"
#include "core/Sha256.h"
#include "core/Status.h"
#include "core/Version.h"

using namespace bfarb;

namespace {

std::string Hex(std::string_view s) { return Sha256::Hex(Sha256::Of(s.data(), s.size())); }

std::string ReadText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* kRepo = BFARB_REPO_ROOT;

}  // namespace

// ---- SHA-256 (FIPS 180-4's examples)

TEST(Sha256KnownAnswers) {
    CHECK_EQ(Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const std::string million(1000000, 'a');
    CHECK_EQ(Hex(million), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    // any split of the input gives the same digest
    for (std::size_t step : {1u, 3u, 63u, 64u, 65u, 1000u}) {
        Sha256 s;
        for (std::size_t i = 0; i < million.size(); i += step) s.Update(million.data() + i, std::min(step, million.size() - i));
        CHECK_EQ(Sha256::Hex(s.Final()), Hex(million));
    }
}

TEST(Sha256File) {
    const auto path = std::filesystem::temp_directory_path() / "bfarb_sha_test.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }
    std::uint64_t size = 0;
    Sha256::Digest d{};
    std::string err;
    CHECK(HashFile(path, size, d, err));
    CHECK_EQ(size, 3u);
    CHECK_EQ(Sha256::Hex(d), Hex("abc"));
    std::filesystem::remove(path);
    CHECK(!HashFile(path, size, d, err));
}

// ---- settings

TEST(SettingsDefaults) {
    // SkyrimNet missing or the keys unsaved: every key gives its default
    const Settings s = ReadSettings([](std::string_view, std::string_view def) { return std::string(def); });
    CHECK(s == Settings{});
    CHECK(s.serverAddress.empty());
    CHECK_EQ(s.serverPort, 7871);
    CHECK(s.accessKey.empty());
    CHECK_EQ(s.timeoutMs, kRunTimeoutMs);
    CHECK_EQ(kRunTimeoutMs, 1000);
    CHECK(s.showStatus);
    CHECK(s.sceneLipSync);
    CHECK(s.logDetail == LogDetail::Normal);
    CHECK(ReadSettings(nullptr) == Settings{});
    // the page holds five keys; 1.1's removed ones are never asked for
    CHECK_EQ(SettingKeys().size(), std::size_t{5});
    std::vector<std::string> asked;
    ReadSettings([&](std::string_view k, std::string_view def) {
        asked.emplace_back(k);
        return std::string(def);
    });
    for (const char* gone : {"enabled", "accessKey", "timeoutMs", "checkCalls"})
        CHECK(std::find(asked.begin(), asked.end(), gone) == asked.end());
}

TEST(SettingsParseAndClamp) {
    // older saved values of the removed keys are there, and ignored
    std::map<std::string, std::string> values = {
        {"enabled", "False"},   {"serverAddress", " \"192.0.2.10\" "}, {"serverPort", "99999"}, {"accessKey", "'k e y'"},
        {"timeoutMs", "10000.6"}, {"checkCalls", "3"},                  {"showStatus", "no"},    {"logDetail", "detailed"},
        {"sceneLipSync", "off"},
    };
    std::vector<std::string> warnings;
    const auto get = [&](std::string_view k, std::string_view def) {
        const auto it = values.find(std::string(k));
        return it == values.end() ? std::string(def) : it->second;
    };
    Settings s = ReadSettings(get, &warnings);
    CHECK_EQ(s.serverAddress, std::string("192.0.2.10"));
    CHECK_EQ(s.serverPort, 65535);
    CHECK(s.accessKey.empty());
    CHECK_EQ(s.timeoutMs, kRunTimeoutMs);
    CHECK(!s.showStatus);
    CHECK(!s.sceneLipSync);
    CHECK(s.logDetail == LogDetail::Detailed);
    CHECK_EQ(warnings.size(), std::size_t{1});  // the port

    values = {{"serverPort", "port"}, {"showStatus", "maybe"}, {"logDetail", "Verbose"}, {"sceneLipSync", "sometimes"}};
    warnings.clear();
    s = ReadSettings(get, &warnings);
    CHECK_EQ(s.serverPort, 7871);
    CHECK(s.showStatus);
    CHECK(s.sceneLipSync);
    CHECK(s.logDetail == LogDetail::Normal);
    CHECK_EQ(warnings.size(), std::size_t{4});

    // a getter that throws gives the default
    s = ReadSettings([](std::string_view, std::string_view) -> std::string { throw 1; });
    CHECK(s == Settings{});
}

TEST(SettingsDescribeHidesTheAddress) {
    Settings s;
    s.serverAddress = "192.0.2.10";
    const std::string line = Describe(s);
    CHECK(line.find("192.0.2.10") == std::string::npos);
    CHECK(line.find("server address set") != std::string::npos);
    CHECK(line.find("access key") == std::string::npos);
    CHECK(line.find("check results") == std::string::npos);
    CHECK(line.find("bridge on") == std::string::npos);
    std::map<std::string, std::string> v = {{"serverAddress", "192.0.2.10"}, {"serverPort", "x"}};
    std::vector<std::string> warnings;
    ReadSettings([&](std::string_view k, std::string_view def) { return v.count(std::string(k)) ? v[std::string(k)] : std::string(def); }, &warnings);
    for (const auto& w : warnings) CHECK(w.find("192.0.2.10") == std::string::npos);
}

// The shipped schema says what the DLL does: every key, its type, default and range, in the same order.
TEST(SettingsSchemaMatchesTheDll) {
    const auto root = std::filesystem::path(kRepo) / "data/SKSE/Plugins/SkyrimNet/external/pt0l3my.bfaremotebridge";
    const std::string yaml = ReadText(root / "settings/BFARemoteBridge.yaml");
    CHECK(!yaml.empty());
    struct Field {
        std::string path, type, def, min, max;
        std::vector<std::string> options;
    };
    std::vector<Field> fields;
    std::istringstream in(yaml);
    std::string line;
    bool inOptions = false;
    const auto value = [](std::string s) {
        s = std::string(Unquote(s));
        return s;
    };
    while (std::getline(in, line)) {
        const auto at = line.find_first_not_of(' ');
        if (at == std::string::npos || line[at] == '#') continue;
        std::string t = line.substr(at);
        if (t.rfind("- name:", 0) == 0) {
            fields.emplace_back();
            inOptions = false;
            continue;
        }
        if (fields.empty()) continue;
        Field& f = fields.back();
        if (inOptions && t.rfind("- ", 0) == 0) {
            f.options.push_back(value(t.substr(2)));
            continue;
        }
        inOptions = false;
        const auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = t.substr(0, colon), rest = t.substr(colon + 1);
        if (key == "path") f.path = value(rest);
        if (key == "type") f.type = value(rest);
        if (key == "defaultValue") f.def = value(rest);
        if (key == "min") f.min = value(rest);
        if (key == "max") f.max = value(rest);
        if (key == "options") inOptions = true;
    }
    const auto& keys = SettingKeys();
    CHECK_EQ(fields.size(), keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const SettingKey& k = keys[i];
        const Field& f = fields[i];
        CHECK_EQ(f.path, std::string(k.path));
        const char* type = k.type == SettingType::Bool ? "bool" : k.type == SettingType::Int ? "int" : k.type == SettingType::String ? "string" : "select";
        CHECK_EQ(f.type, std::string(type));
        CHECK_EQ(f.def, std::string(k.defaultText));
        if (k.type == SettingType::Int) {
            CHECK_EQ(f.min, std::to_string(k.min));
            CHECK_EQ(f.max, std::to_string(k.max));
        }
        if (k.type == SettingType::Select) {
            CHECK_EQ(f.options.size(), k.options.size());
            for (std::size_t o = 0; o < k.options.size(); ++o) CHECK_EQ(f.options[o], std::string(k.options[o]));
        }
    }
    // the manifest's version is the DLL's
    const std::string manifest = ReadText(root / "manifest.json");
    CHECK(manifest.find(std::string("\"version\": \"") + kVersionText + "\"") != std::string::npos);
    CHECK(manifest.find("\"min_skyrimnet_version\": \"0.26.0\"") != std::string::npos);
    CHECK(manifest.find("\"nsfw\": false") != std::string::npos);
    CHECK(yaml.find(std::string("version: \"") + kVersionText + "\"") != std::string::npos);
}

// ---- the status texts and the monitor

TEST(StatusTexts) {
    StatusFacts f;
    CHECK_EQ(LoadStatusText(f), std::string("BFA Remote Bridge: Better Facial Animations wasn't found, so the bridge is idle."));
    f.bfaFound = true;
    f.bfaVersion = "4.4.0";
    f.problem = "it doesn't look ONNX Runtime up the way the bridge knows";
    CHECK_EQ(LoadStatusText(f), std::string("BFA Remote Bridge: couldn't attach to Better Facial Animations 4.4.0 (it doesn't look ONNX "
                                            "Runtime up the way the bridge knows), so BFA runs on this PC as it would without the "
                                            "bridge. Update the bridge."));
    f.hooked = true;
    CHECK(LoadStatusText(f).empty());  // the monitor's lines follow
    CHECK_EQ(CantServeText("4.5.0", "1.31.0", true), std::string("BFA Remote Bridge: Better Facial Animations 4.5.0 uses ONNX Runtime 1.31.0, "
                                                               "which the bridge can't serve, so lip sync is the game's own. Update the bridge."));
    CHECK(CantServeText("3.0.0", "1.9.0", false).find("Update Better Facial Animations.") != std::string::npos);
    CHECK(NoServerText().find("SkyrimNet wasn't found") != std::string::npos);
    CHECK_EQ(NotFoundText(7871), std::string("BFA Remote Bridge: no server answered on the local network, so lip sync is the game's own. In "
                                             "PandorumLLM, set the A2F server's Remote IP to this PC and allow port 7871 (UDP and TCP) through "
                                             "that PC's firewall, or enter its address on SkyrimNet's Settings > Plugins page."));
    CHECK_EQ(DeviceText("DirectML 3: NVIDIA GeForce RTX 5090"), std::string("NVIDIA GeForce RTX 5090 (DirectML 3)"));
    CHECK_EQ(DeviceText("CPU"), std::string("CPU"));
    CHECK_EQ(DeviceText("CPU: "), std::string("CPU: "));
    CHECK_EQ(ModelLine("a2f3_fp16.onnx", "PandorumLLM v3.97-p6 Beta A2F", "DirectML 3: NVIDIA GeForce RTX 5090", "ready"),
             std::string("BFA Remote Bridge: a2f3_fp16.onnx on NVIDIA GeForce RTX 5090 (DirectML 3), PandorumLLM v3.97-p6 Beta A2F: ready"));
    CHECK_EQ(FallbackLine("a2f3_fp16.onnx", ReasonWords(Reason::TimedOut)),
             std::string("BFA Remote Bridge: a2f3_fp16.onnx: lines get the game's own lip sync, since the server didn't answer in time."));
    CHECK_EQ(BackLine("a2f3_fp16.onnx"), std::string("BFA Remote Bridge: a2f3_fp16.onnx: lines are back on the server."));
    // every note starts so, for a SkyrimNet trigger to tell them apart; plain words, no emoji
    for (std::size_t i = 0; i < kReasonCount; ++i) {
        const std::string w = ReasonWords(static_cast<Reason>(i));
        CHECK(!w.empty());
        for (unsigned char c : FallbackLine("", w)) CHECK(c < 0x80);
    }
    for (const std::string& t : {NoServerText(), NotFoundText(7871), CantAttachText("", ""), CantServeText("", "", true), BackLine("")}) {
        CHECK(t.rfind("BFA Remote Bridge: ", 0) == 0);
        for (unsigned char c : t) CHECK(c < 0x80);
    }
}

namespace {
Clock::time_point g_monitorNow = Clock::time_point{} + std::chrono::hours(1);
Clock::time_point MonitorNow() { return g_monitorNow; }
}  // namespace

TEST(MonitorLinesAndTheirRate) {
    Monitor m(&MonitorNow);
    std::vector<std::string> notes;
    m.SetSink([&](const std::string& t) { notes.push_back(t); });  // the one function every note goes through
    const std::string server = "PandorumLLM v3.97-p6 Beta A2F", card = "DirectML 3: NVIDIA GeForce RTX 5090";
    m.ModelState("a2f3_fp16.onnx", server, card, "ready", true);
    CHECK_EQ(notes.size(), std::size_t{1});
    CHECK_EQ(notes[0], std::string("BFA Remote Bridge: a2f3_fp16.onnx on NVIDIA GeForce RTX 5090 (DirectML 3), PandorumLLM v3.97-p6 Beta A2F: ready"));
    m.ModelState("a2f3_fp16.onnx", server, card, "ready", true);  // unchanged: nothing
    CHECK_EQ(notes.size(), std::size_t{1});
    m.ModelState("a2f3_fp16.onnx", server, "DirectML 0: Another card", "ready", true);  // the card changed
    CHECK_EQ(notes.size(), std::size_t{2});
    // a fallback: the first note, then at most one a minute while it lasts
    m.Fallback(Reason::TimedOut, "a2f3_fp16.onnx");
    m.Fallback(Reason::Paused, "a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{3});
    CHECK(notes.back().find("since the server didn't answer in time") != std::string::npos);
    g_monitorNow += std::chrono::seconds(59);
    m.Fallback(Reason::Paused, "a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{3});
    g_monitorNow += std::chrono::seconds(1);
    m.Fallback(Reason::Paused, "a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{4});
    CHECK(notes.back().find("since the bridge is reconnecting to the server") != std::string::npos);
    CHECK(m.InFallback());
    // back on the server: once
    m.Answered("a2f3_fp16.onnx");
    m.Answered("a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{5});
    CHECK_EQ(notes.back(), std::string("BFA Remote Bridge: a2f3_fp16.onnx: lines are back on the server."));
    // lines that alternate (one in five missed): still at most one fallback note and one back note a minute
    for (int i = 0; i < 20; ++i) {
        g_monitorNow += std::chrono::seconds(2);
        m.Fallback(Reason::TimedOut, "a2f3_fp16.onnx");
        m.Answered("a2f3_fp16.onnx");
    }
    CHECK_EQ(notes.size(), std::size_t{5});  // 40 s since the last of each: none
    g_monitorNow += std::chrono::seconds(20);
    m.Fallback(Reason::TimedOut, "a2f3_fp16.onnx");
    m.Answered("a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{7});  // a minute on: one of each
    for (int i = 0; i < 20; ++i) {
        g_monitorNow += std::chrono::seconds(2);
        m.Fallback(Reason::TimedOut, "a2f3_fp16.onnx");
        m.Answered("a2f3_fp16.onnx");
    }
    CHECK_EQ(notes.size(), std::size_t{7});
    // a back line names its model only when the fallback note named the same one
    g_monitorNow += std::chrono::seconds(60);
    m.Fallback(Reason::ServerError, "emotion_fp16.onnx", "the server couldn't run it");
    m.Answered("a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{9});
    CHECK_EQ(notes.back(), std::string("BFA Remote Bridge: lines are back on the server."));
    // a back note held by the minute comes with a later answer
    g_monitorNow += std::chrono::seconds(60);
    m.Fallback(Reason::TimedOut, "a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{10});
    g_monitorNow += std::chrono::seconds(1);
    m.Answered("a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{11});
    g_monitorNow += std::chrono::seconds(59);
    m.Fallback(Reason::TimedOut, "a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{12});
    m.Answered("a2f3_fp16.onnx");  // within the minute of the last back note: held
    CHECK_EQ(notes.size(), std::size_t{12});
    g_monitorNow += std::chrono::seconds(1);
    m.Answered("a2f3_fp16.onnx");
    CHECK_EQ(notes.size(), std::size_t{13});
    CHECK(!m.InFallback());
    notes.resize(6);
    // once per session
    m.Once("gpu", "BFA Remote Bridge: gpu");
    m.Once("gpu", "BFA Remote Bridge: gpu");
    CHECK_EQ(notes.size(), std::size_t{7});
    // status on load: the lines at a load, and a ready line, only while it allows them; a problem always
    m.Loaded();
    CHECK_EQ(notes.size(), std::size_t{8});  // the one model it knows
    m.SetShowStatus(false);
    m.Loaded();
    m.ModelState("emotion_fp16.onnx", server, card, "ready", true);
    CHECK_EQ(notes.size(), std::size_t{8});
    m.ModelState("emotion_fp16.onnx", server, card, "not ready (different file)", false);
    CHECK_EQ(notes.size(), std::size_t{9});
    m.Forget("emotion_fp16.onnx");
    m.SetShowStatus(true);
    notes.clear();
    m.Loaded();
    CHECK_EQ(notes.size(), std::size_t{1});
    for (const auto& n : notes) CHECK(n.rfind("BFA Remote Bridge: ", 0) == 0);
}

// ---- the runtime's API versions

// The first entry of each API version, as the bridge lists it, is the vendored header's: the entry right after the
// first "\since Version 1.N" note inside struct OrtApi, and no entry before it carries that version.
TEST(OrtApiStartsMatchTheHeader) {
    const std::string header = ReadText(std::filesystem::path(kRepo) / "third_party/onnxruntime/onnxruntime_c_api.h");
    const auto begin = header.find("\nstruct OrtApi {");
    CHECK(begin != std::string::npos);
    const auto end = header.find("\n};", begin);
    std::istringstream in(header.substr(begin, end - begin));
    std::map<unsigned, std::string> firstOf;
    unsigned since = 0, pending = 0;
    std::string line;
    const auto entryName = [](const std::string& l) -> std::string {
        // ORT_API_T(RETURN_TYPE, NAME, ...): the name is the second argument
        if (const auto at = l.find("ORT_API_T("); at != std::string::npos) {
            const auto comma = l.find(',', at);
            if (comma == std::string::npos) return {};
            const auto from = l.find_first_not_of(' ', comma + 1);
            return l.substr(from, l.find_first_of(",)", from) - from);
        }
        for (const char* m : {"ORT_API2_STATUS(", "ORT_CLASS_RELEASE("}) {
            const auto at = l.find(m);
            if (at == std::string::npos) continue;
            const auto from = at + std::strlen(m);
            const auto to = l.find_first_of(",)", from);
            const std::string n = l.substr(from, to - from);
            return std::string(m) == "ORT_CLASS_RELEASE(" ? "Release" + n : n;
        }
        const auto star = l.find("ORT_API_CALL* ");
        if (star == std::string::npos) return {};
        const auto from = star + std::strlen("ORT_API_CALL* ");
        return l.substr(from, l.find(')', from) - from);
    };
    while (std::getline(in, line)) {
        const auto at = line.find("\\since Version 1.");
        if (at != std::string::npos) {
            const unsigned v = static_cast<unsigned>(std::stoul(line.substr(at + 17)));
            if (v > since) pending = v;
            since = std::max(since, v);
            continue;
        }
        const std::string name = entryName(line);
        if (name.empty() || line.find("//") < line.find(name)) continue;
        if (pending) {
            firstOf[pending] = name;
            pending = 0;
        }
    }
    const auto& starts = OrtApiStarts();
    CHECK_EQ(starts.size(), firstOf.size());
    std::size_t last = 0;
    for (const OrtApiStart& s : starts) {
        CHECK_EQ(firstOf[s.version], std::string(s.name));
        CHECK(s.index > last);
        last = s.index;
    }
    CHECK_EQ(kKnownOrtEntries, std::size_t{415});
    // each version's count pinned to ONNX Runtime's own static_asserts (onnxruntime_c_api.cc, "Size of version N"),
    // not to the bridge's list
    const std::map<unsigned, std::size_t> published = {{10, 204}, {11, 210}, {12, 219}, {13, 225}, {14, 239}, {15, 255},
                                                       {16, 266}, {17, 276}, {18, 280}, {19, 280}, {20, 285}, {21, 285},
                                                       {22, 318}, {23, 390}, {24, 415}};
    for (const auto& [version, count] : published) CHECK_EQ(OrtApiEntries(version), count);
    CHECK_EQ(OrtApiEntries(9), std::size_t{0});
    CHECK_EQ(OrtApiEntries(10), starts.front().index);
    CHECK_EQ(OrtApiEntries(18), OrtApiEntries(19));  // 19 and 21 added nothing
    CHECK_EQ(OrtApiEntries(20), OrtApiEntries(21));
    CHECK(OrtApiEntries(23) < OrtApiEntries(24));
    CHECK_EQ(OrtApiEntries(24), kKnownOrtEntries);
    CHECK_EQ(OrtApiEntries(26), kKnownOrtEntries);
    CHECK_EQ(OrtApiVersionOf("1.24.4"), 24u);
    CHECK_EQ(OrtApiVersionOf("1.9.0"), 9u);
    CHECK_EQ(OrtApiVersionOf("1.26.0-dev"), 26u);
    CHECK_EQ(OrtApiVersionOf("2.0.0"), 0u);
    CHECK_EQ(OrtApiVersionOf("1."), 0u);
    CHECK_EQ(OrtApiVersionOf("1.2x"), 0u);
    CHECK_EQ(OrtApiVersionOf(nullptr), 0u);
}

// ---- PE reads

namespace {

// A tiny PE32+ image laid out as the loader would: headers, an import directory naming KERNEL32.dll with
// LoadLibraryW, GetProcAddress and an ordinal, and an export directory with SKSEPlugin_Version.
struct FakeImage {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x3000, 0);
    void U16(std::size_t at, std::uint16_t v) {
        bytes[at] = v & 0xFF;
        bytes[at + 1] = v >> 8;
    }
    void U32(std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = (v >> (8 * i)) & 0xFF;
    }
    void U64(std::size_t at, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes[at + i] = (v >> (8 * i)) & 0xFF;
    }
    void Str(std::size_t at, const char* s) { std::memcpy(bytes.data() + at, s, std::strlen(s) + 1); }

    FakeImage(const char* dllName, bool withLookup) {
        bytes[0] = 'M';
        bytes[1] = 'Z';
        U32(0x3C, 0x80);
        U32(0x80, 0x00004550);
        const std::size_t opt = 0x80 + 24;
        U16(opt, 0x20B);
        U32(opt + 108, 16);
        U32(opt + 112, 0x2000);  // export directory
        U32(opt + 116, 0x100);
        U32(opt + 120, 0x1000);  // import directory
        U32(opt + 124, 40);
        // import descriptor 0: lookup 0x1100, name 0x1200, IAT 0x1300; then a zero descriptor
        U32(0x1000, withLookup ? 0x1100 : 0);
        U32(0x1000 + 12, 0x1200);
        U32(0x1000 + 16, 0x1300);
        Str(0x1200, dllName);
        U64(0x1100, 0x1400);
        U64(0x1108, (1ull << 63) | 5);
        U64(0x1110, 0x1420);
        U64(0x1118, 0);
        Str(0x1402, "LoadLibraryW");
        Str(0x1422, "GetProcAddress");
        // exports: two functions, two names
        U32(0x2000 + 20, 2);
        U32(0x2000 + 24, 2);
        U32(0x2000 + 28, 0x2040);
        U32(0x2000 + 32, 0x2050);
        U32(0x2000 + 36, 0x2060);
        U32(0x2040, 0x2800);
        U32(0x2044, 0x2900);
        U32(0x2050, 0x2070);
        U32(0x2054, 0x2080);
        U16(0x2060, 0);
        U16(0x2062, 1);
        Str(0x2070, "SKSEPlugin_Load");
        Str(0x2080, "SKSEPlugin_Version");
    }
};

}  // namespace

TEST(PeFindsTheImportSlotAndTheExport) {
    FakeImage img("KERNEL32.dll", true);
    std::size_t slot = 0;
    CHECK(pe::FindImportSlot(img.bytes.data(), img.bytes.size(), "kernel32.dll", "GetProcAddress", slot));
    CHECK_EQ(slot, std::size_t{0x1310});  // the third IAT entry
    CHECK(pe::FindImportSlot(img.bytes.data(), img.bytes.size(), "KERNEL32", "LoadLibraryW", slot));
    CHECK_EQ(slot, std::size_t{0x1300});
    CHECK(!pe::FindImportSlot(img.bytes.data(), img.bytes.size(), "kernel32.dll", "LoadLibraryExW", slot));
    CHECK(!pe::FindImportSlot(img.bytes.data(), img.bytes.size(), "user32.dll", "GetProcAddress", slot));
    std::uint32_t rva = 0;
    CHECK(pe::FindExport(img.bytes.data(), img.bytes.size(), "SKSEPlugin_Version", rva));
    CHECK_EQ(rva, 0x2900u);
    CHECK(!pe::FindExport(img.bytes.data(), img.bytes.size(), "SKSEPlugin_Query", rva));

    FakeImage noLookup("kernel32.dll", false);
    CHECK(!pe::FindImportSlot(noLookup.bytes.data(), noLookup.bytes.size(), "kernel32.dll", "GetProcAddress", slot));
    // a truncated image is refused, never read past
    for (std::size_t n : {std::size_t{0}, std::size_t{0x40}, std::size_t{0x200}, std::size_t{0x1210}, std::size_t{0x1428}})
        CHECK(!pe::FindImportSlot(img.bytes.data(), n, "kernel32.dll", "GetProcAddress", slot));
}

TEST(PeVersionAndLookup) {
    CHECK_EQ(pe::UnpackVersion(0x04030020), std::string("4.3.2"));
    CHECK_EQ(pe::UnpackVersion(0x03000011), std::string("3.0.1.1"));
    CHECK(pe::IsOrtGetApiBaseLookup("OrtGetApiBase", "C:\\Games\\Data\\SKSE\\Plugins\\BetterFacialAnimations\\onnxruntime.dll"));
    CHECK(pe::IsOrtGetApiBaseLookup("OrtGetApiBase", "ONNXRUNTIME.DLL"));
    CHECK(!pe::IsOrtGetApiBaseLookup("OrtGetApiBase", "C:\\x\\myonnxruntime.dll"));
    CHECK(!pe::IsOrtGetApiBaseLookup("OrtGetApiBase", "C:\\x\\DirectML.dll"));
    CHECK(!pe::IsOrtGetApiBaseLookup("OrtGetApiBaseX", "onnxruntime.dll"));
    CHECK(!pe::IsOrtGetApiBaseLookup(reinterpret_cast<const char*>(std::uintptr_t{12}), "onnxruntime.dll"));
    CHECK(!pe::IsOrtGetApiBaseLookup(nullptr, "onnxruntime.dll"));
}

// BFA's own DLL (never committed): BFARB_BFA_DLL names it.
TEST(PeReadsBetterFacialAnimationsDll) {
    const char* path = std::getenv("BFARB_BFA_DLL");
    if (!path || !*path) SKIP("BFARB_BFA_DLL isn't set");
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> image;
    CHECK(pe::MapFile(file, image));
    std::size_t slot = 0;
    CHECK(pe::FindImportSlot(image.data(), image.size(), "KERNEL32.dll", "GetProcAddress", slot));
    std::uint32_t rva = 0;
    CHECK(pe::FindExport(image.data(), image.size(), "SKSEPlugin_Version", rva));
    pe::SksePluginVersion v{};
    CHECK(rva + sizeof v <= image.size());
    std::memcpy(&v, image.data() + rva, sizeof v);
    CHECK_EQ(v.dataVersion, 1u);
    CHECK(pe::UnpackVersion(v.pluginVersion).rfind("4.3.2", 0) == 0);  // the shipped DLL says 4.3.2.3
    CHECK_EQ(std::string(v.pluginName), std::string("BetterFacialAnimations"));
    std::printf("        BFA's DLL: GetProcAddress's IAT slot at RVA 0x%zx, SKSEPlugin_Version at RVA 0x%x, version %s\n", slot, rva,
                pe::UnpackVersion(v.pluginVersion).c_str());
}

// BFA 5.0.6's DLL (never committed): BFARB_BFA5_DLL names it. It still imports GetProcAddress from KERNEL32 by name,
// which the bridge's hook patches, and reports 5.0.6.3, which the bridge's BFA 5 rules take as 5.0.6.
TEST(PeReadsBetterFacialAnimations506Dll) {
    const char* path = std::getenv("BFARB_BFA5_DLL");
    if (!path || !*path) SKIP("BFARB_BFA5_DLL isn't set");
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> image;
    CHECK(pe::MapFile(file, image));
    std::size_t slot = 0;
    CHECK(pe::FindImportSlot(image.data(), image.size(), "KERNEL32.dll", "GetProcAddress", slot));
    std::uint32_t rva = 0;
    CHECK(pe::FindExport(image.data(), image.size(), "SKSEPlugin_Version", rva));
    pe::SksePluginVersion v{};
    CHECK(rva + sizeof v <= image.size());
    std::memcpy(&v, image.data() + rva, sizeof v);
    CHECK_EQ(v.dataVersion, 1u);
    const std::string version = pe::UnpackVersion(v.pluginVersion);
    CHECK_EQ(version, std::string("5.0.6.3"));
    CHECK(IsBfa506(version));
    CHECK(ListsSizesFor(version));
    CHECK_EQ(std::string(v.pluginName), std::string("BetterFacialAnimations"));
    CHECK_EQ(pe::ImageSize(image.data(), 0x1000), image.size());
    std::printf("        BFA 5's DLL: GetProcAddress's IAT slot at RVA 0x%zx, SKSEPlugin_Version at RVA 0x%x, version %s\n", slot, rva,
                version.c_str());
}

// ---- paths out of other programs' messages

// A server's detail as PandorumLLM sent it in the owner's Force CPU test: ONNX Runtime's raw message, with addresses
// and a line break. Logged on one line, without the addresses; shown capped.
TEST(ServerTextsGoOnOneLine) {
    const std::string raw =
        "Non-zero status code returned while running Concat node. Name:'Concat_7' Status Message: "
        "MLOperatorAuthorImpl.cpp(2508)onnxruntime.dll!00007FFC19A9F444: (caller: 00007FFC\xE2\x80\xA6\n"
        "Exception(1) tid(2f40) 80070057 The parameter is incorrect.\r\n";
    const std::string line = ServerText(raw);
    CHECK_EQ(line, std::string("Non-zero status code returned while running Concat node. Name:'Concat_7' Status Message: "
                               "MLOperatorAuthorImpl.cpp(2508)onnxruntime.dll: Exception(1) tid(2f40) 80070057 The parameter is incorrect."));
    for (unsigned char c : line) CHECK(c >= 0x20 && c != 0x7F);
    CHECK(line.find("00007FFC") == std::string::npos);
    CHECK(line.find("  ") == std::string::npos);
    const std::string shown = ShownServerText(raw);
    CHECK(shown.size() <= kShownTextMax);
    CHECK(shown.size() >= kShownTextMax - 30);
    CHECK(shown.rfind("...") == shown.size() - 3);
    CHECK(line.rfind(shown.substr(0, shown.size() - 3), 0) == 0);  // the line's own start
    CHECK(shown.find('\n') == std::string::npos);
    // short texts are shown whole; other shapes of address go too; a path keeps its base name; UTF-8 isn't cut inside
    CHECK_EQ(ShownServerText("the file here has another size or SHA-256"), std::string("the file here has another size or SHA-256"));
    CHECK_EQ(ServerText("at 0x00007ffc19a9f444 and 00007FFC19A9F444 in\tC:\\Users\\someone\\m.onnx"), std::string("at and in m.onnx"));
    CHECK_EQ(ServerText("\x01\x02 a \x7F  b \n"), std::string("a b"));
    const std::string accents(200, 'a');
    std::string utf = accents.substr(0, 156) + "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    const std::string cut = ShownServerText(utf, 160);
    CHECK(cut.size() <= 160);
    CHECK((static_cast<unsigned char>(cut[cut.size() - 4]) & 0xC0) != 0xC0);  // no lead byte left without its tail
    // the monitor's line for a refused model: one line, capped
    const std::string note = ModelLine("a2f3_fp16.onnx", "PandorumLLM v3.97-p7 Beta A2F", "DirectML 3: NVIDIA GeForce RTX 5090",
                                       "not ready (failed to load) (" + ShownServerText(raw) + ")");
    CHECK(note.find('\n') == std::string::npos);
    CHECK(note.find("00007FFC") == std::string::npos);
}

TEST(ScrubPathsKeepsBaseNamesOnly) {
    const std::string win = "C:\\Games\\Skyrim Special Edition\\Data\\SKSE\\Plugins\\BetterFacialAnimations\\a2f3_fp16.onnx";
    CHECK_EQ(ScrubPaths("Load model from " + win + " failed: no such file", win),
             std::string("Load model from a2f3_fp16.onnx failed: no such file"));
    // a path not given, with spaces: each word that reads as a path keeps its last part, so no folder survives whole
    CHECK_EQ(ScrubPaths("Load model from " + win + " failed"), std::string("Load model from Skyrim Special a2f3_fp16.onnx failed"));
    CHECK_EQ(ScrubPaths("Load model from /tmp/some folder/x/broken.onnx failed:Protobuf parsing failed.", "/tmp/some folder/x/broken.onnx"),
             std::string("Load model from broken.onnx failed:Protobuf parsing failed."));
    CHECK_EQ(ScrubPaths("open(/home/someone/m.onnx): denied"), std::string("open(m.onnx): denied"));
    CHECK_EQ(ScrubPaths("in D:/mods/BFA/net.onnx."), std::string("in net.onnx."));
    CHECK_EQ(ScrubPaths("Check tiny.onnx 1/2: y largest difference 0"), std::string("Check tiny.onnx 1/2: y largest difference 0"));
    CHECK_EQ(ScrubPaths("ratio 3/4 and http-ish a/b"), std::string("ratio 3/4 and http-ish a/b"));
}

// ---- BFA's mode, set by the bridge (core/BfaMode.h)

namespace {

// BFA's INI and the bridge's state, in memory, with every write counted. BFA's own INI holds ForceCPU=forceCpu while
// forceCpuInIni; its other keys, and BFA 5's installer choice files, are in `values` ("choice/section/key", choice
// empty for BFA's own INI); the bridge's records in `records`. A key in failWrites can't be written or removed in BFA's
// INI (read-only, say).
struct FakeIni {
    bool present = true;
    bool readable = true;  // false: there, but another program holds it (its reads answer nothing)
    int forceCpu = 0;
    bool forceCpuInIni = true;
    int iniWrites = 0;
    int recordWrites = 0;
    bool failRecord = false;
    std::set<std::string> failWrites;
    std::map<std::string, std::string> values;
    std::map<std::string, std::string> records;
    void Set(const std::string& choice, const std::string& key, const std::string& value) { values[choice + "/General/" + key] = value; }
    std::string Own(const std::string& key) const {
        if (key == "ForceCPU") return forceCpuInIni ? std::to_string(forceCpu) : "(absent)";
        const auto it = values.find("/General/" + key);
        return it == values.end() ? "(absent)" : it->second;
    }
    std::string Record(const char* key) const {
        const auto it = records.find(key);
        return it == records.end() ? "(none)" : it->second;
    }
    bool SetByBridge() const { return records.count(kForceCpuRecord) > 0; }
    // BFA's SaveIni: every setting written back into its own INI, as BFA holds it
    void BfaSaves(int force, long emotion) {
        forceCpu = force;
        forceCpuInIni = true;
        Set("", "EmotionDevice", std::to_string(emotion));
    }
};

class FakeIniStore final : public IniStore {
public:
    explicit FakeIniStore(std::shared_ptr<FakeIni> ini) : ini_(std::move(ini)) {}
    BfaIniState BfaIni() override {
        if (!ini_->present) return BfaIniState::Missing;
        return ini_->readable ? BfaIniState::Readable : BfaIniState::Unreadable;
    }
    // missing or unreadable: the profile call's default, as on Windows
    int ReadForceCpu() override { return ini_->present && ini_->readable && ini_->forceCpuInIni ? ini_->forceCpu : 0; }
    bool WriteForceCpu(int value, std::string& err) override {
        CHECK(ini_->present);  // never created
        CHECK(ini_->readable);  // never written while unreadable
        if (Fails("ForceCPU", err)) return false;
        ini_->forceCpu = value;
        ini_->forceCpuInIni = true;
        ++ini_->iniWrites;
        return true;
    }
    std::optional<std::string> ReadBfaValue(const char* choice, const char* section, const char* key) override {
        if (!choice && (!ini_->present || !ini_->readable)) return std::nullopt;
        if (!choice && std::string(section) == "General" && std::string(key) == "ForceCPU")
            return ini_->forceCpuInIni ? std::optional<std::string>(std::to_string(ini_->forceCpu)) : std::nullopt;
        const auto it = ini_->values.find(std::string(choice ? choice : "") + "/" + section + "/" + key);
        if (it == ini_->values.end()) return std::nullopt;
        return it->second;
    }
    bool WriteBfaValue(const char* key, const std::string& value, std::string& err) override {
        CHECK(ini_->present);  // never created
        CHECK(ini_->readable);
        CHECK(std::string(key) != "IndependentGpuDevice");  // left as it is
        if (Fails(key, err)) return false;
        if (std::string(key) == "ForceCPU") {
            ini_->forceCpu = std::stoi(value);
            ini_->forceCpuInIni = true;
        } else {
            ini_->Set("", key, value);
        }
        ++ini_->iniWrites;
        return true;
    }
    bool RemoveBfaValue(const char* key, std::string& err) override {
        CHECK(ini_->present);
        CHECK(ini_->readable);
        if (Fails(key, err)) return false;
        if (std::string(key) == "ForceCPU")
            ini_->forceCpuInIni = false;
        else
            ini_->values.erase(std::string("/General/") + key);
        ++ini_->iniWrites;
        return true;
    }
    std::string ReadRecord(const char* key) override {
        const auto it = ini_->records.find(key);
        return it == ini_->records.end() ? std::string() : it->second;
    }
    bool WriteRecord(const char* key, const char* value, std::string& err) override {
        if (ini_->failRecord) {
            err = "read-only";
            return false;
        }
        if (value)
            ini_->records[key] = value;
        else
            ini_->records.erase(key);
        ++ini_->recordWrites;
        return true;
    }

private:
    bool Fails(const std::string& key, std::string& err) const {
        if (!ini_->failWrites.count(key)) return false;
        err = "read-only";
        return true;
    }
    std::shared_ptr<FakeIni> ini_;
};

struct ModeRig {
    std::shared_ptr<FakeIni> ini = std::make_shared<FakeIni>();
    std::vector<std::string> log;
    BfaCpuMode mode{std::make_unique<FakeIniStore>(ini), [this](Level, const std::string& l) { log.push_back(l); }};
    explicit ModeRig(const char* bfa = kCpuModeBfaVersion) { mode.SetBfaVersion(bfa); }
    int Logged(const std::string& part) const {
        int n = 0;
        for (const auto& l : log) n += l.find(part) != std::string::npos;
        return n;
    }
    // The next game start: a new bridge over the same files, after BFA loaded them.
    std::unique_ptr<BfaCpuMode> NextStart(const char* bfa) {
        auto next = std::make_unique<BfaCpuMode>(std::make_unique<FakeIniStore>(ini), [this](Level, const std::string& l) { log.push_back(l); });
        next->SetBfaVersion(bfa);
        next->OnStart();
        return next;
    }
};

// BFA 5.0.6 as its installer leaves it: the choices in Installer\Startup.ini and Installer\EmotionDevice.ini (both
// on graphics card 0, the default), ForceCPU and EmotionDevice commented out in BFA's own INI.
struct Mode5Rig : ModeRig {
    explicit Mode5Rig(const char* bfa = "5.0.6.3") : ModeRig(bfa) {
        ini->forceCpuInIni = false;
        ini->Set("Startup", "ForceCPU", "0");
        ini->Set("Startup", "GpuAdapter", "0");
        ini->Set("EmotionDevice", "EmotionDevice", "0");
        ini->Set("GraphicsRoute", "IndependentGpuDevice", "0");
        ini->Set("", "IniVersion", "9");
    }
};

// BFA 5.0.6 once its menu has saved: every key in its own INI too (Audio2Face on a card, Audio2Emotion on card 2).
struct Mode5Saved : Mode5Rig {
    Mode5Saved() { ini->BfaSaves(0, 2); }
};

}  // namespace

// Nothing while no server has answered; version 2 sets ForceCPU=1, recording that the bridge set it; logged once.
// Each game save writes it again.
TEST(BfaModeSetOnVersionTwo) {
    ModeRig r;
    r.mode.OnStart();
    r.mode.OnGameSave();
    r.mode.OnServerVersion(0);
    CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, 0);
    CHECK(r.log.empty());
    r.mode.OnServerVersion(2);
    CHECK(r.mode.Set());
    CHECK_EQ(r.ini->forceCpu, 1);
    CHECK_EQ(r.ini->Record(kForceCpuRecord), std::string("1"));
    CHECK_EQ(r.Logged("Set BFA's [General] ForceCPU to 1 in BetterFacialAnimations.ini (it was 0)"), 1);
    CHECK_EQ(r.Logged("from the next game start"), 1);
    // BFA's menu saves its file with ForceCPU=0 in this session: the next game save writes 1 again, silently
    r.ini->forceCpu = 0;
    r.mode.OnGameSave();
    CHECK_EQ(r.ini->forceCpu, 1);
    CHECK_EQ(r.ini->iniWrites, 2);
    CHECK_EQ(r.log.size(), std::size_t{1});
    // already 1: nothing written
    r.mode.OnGameSave();
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->iniWrites, 2);
    // the next start loads the bridge's 1: the record stays
    r.NextStart(kCpuModeBfaVersion);
    CHECK(r.ini->SetByBridge());
}

// The record is written once, and only when the bridge changes a 0: a 1 it finds (the player's, or its own whose record
// was lost) is never recorded, so nothing is put back for it.
TEST(BfaModeRecordsTheValueFoundOnce) {
    ModeRig r;
    r.mode.OnServerVersion(2);
    r.ini->forceCpu = 0;
    r.mode.OnGameSave();
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->recordWrites, 1);
    CHECK(r.ini->SetByBridge());
    // the next start: the file says 1 (the bridge's), the record stays
    BfaCpuMode again(std::make_unique<FakeIniStore>(r.ini), nullptr);
    again.SetBfaVersion(kCpuModeBfaVersion);
    again.OnStart();
    again.OnServerVersion(2);
    CHECK_EQ(r.ini->recordWrites, 1);
    // BFA already in its CPU mode: nothing recorded, nothing written, nothing put back at version 1
    ModeRig cpu;
    cpu.ini->forceCpu = 1;
    cpu.mode.OnServerVersion(2);
    CHECK(!cpu.ini->SetByBridge());
    CHECK_EQ(cpu.ini->iniWrites + cpu.ini->recordWrites, 0);
    cpu.mode.OnServerVersion(1);
    CHECK_EQ(cpu.ini->forceCpu, 1);
    CHECK_EQ(cpu.ini->iniWrites, 0);
    // the bridge's record lost after it set 1: its own 1 isn't taken for the player's, and nothing is put back
    ModeRig lost;
    lost.mode.OnServerVersion(2);
    lost.ini->records.erase(kForceCpuRecord);
    BfaCpuMode later(std::make_unique<FakeIniStore>(lost.ini), nullptr);
    later.SetBfaVersion(kCpuModeBfaVersion);
    later.OnServerVersion(2);
    CHECK(!lost.ini->SetByBridge());
    later.OnServerVersion(1);
    CHECK_EQ(lost.ini->forceCpu, 1);
    // a record that doesn't read as the bridge wrote it is none
    for (const char* damaged : {"0", "1 ", "yes", "Added"}) {
        ModeRig d;
        d.mode.OnServerVersion(2);
        d.ini->records[kForceCpuRecord] = damaged;
        d.mode.OnServerVersion(1);
        CHECK_EQ(d.ini->forceCpu, 1);
    }
    // the record can't be written: BFA's file is left as it is
    ModeRig ro;
    ro.ini->failRecord = true;
    ro.mode.OnServerVersion(2);
    CHECK_EQ(ro.ini->forceCpu, 0);
    CHECK_EQ(ro.ini->iniWrites, 0);
    CHECK_EQ(ro.Logged("BFA's mode is left as it is"), 1);
}

// A version 1 server: the value the bridge set goes back; its record stays until a start has loaded the 0; saves then
// leave BFA's file alone while it holds the 0.
TEST(BfaModeRestoredOnVersionOne) {
    ModeRig r;
    r.mode.OnServerVersion(1);  // nothing recorded: nothing to put back
    CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, 0);
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->forceCpu, 1);
    r.mode.OnServerVersion(1);
    CHECK(!r.mode.Set());
    CHECK_EQ(r.ini->forceCpu, 0);
    CHECK(r.ini->SetByBridge());
    CHECK_EQ(r.Logged("Put BFA's [General] ForceCPU back to 0 in BetterFacialAnimations.ini, as it was: the server speaks protocol "
                      "version 1 only, and the bridge sets BFA's CPU mode only for a server that speaks version 2. It applies from "
                      "the next game start."),
             1);
    r.mode.OnGameSave();
    CHECK_EQ(r.ini->forceCpu, 0);
    const int writes = r.ini->iniWrites;
    r.mode.OnServerVersion(1);
    CHECK_EQ(r.ini->iniWrites, writes);
    // the next start loads the 0: the record goes, and nothing more is written
    auto next = r.NextStart(kCpuModeBfaVersion);
    CHECK(!r.ini->SetByBridge());
    next->OnServerVersion(1);
    next->OnGameSave();
    CHECK_EQ(r.ini->iniWrites, writes);
}

// BFA's menu, saved after the put-back in the same session, writes the bridge's value back (it holds what it loaded
// at its start): each game save while the answer is version 1, or the BFA version isn't served, puts it back again,
// logged once; a start that loads the bridge's value again keeps the record, so the next version 1 answer puts it back.
TEST(BfaModePutBackSurvivesBfasSave) {
    ModeRig r;
    r.mode.OnServerVersion(2);
    r.mode.OnServerVersion(1);
    CHECK_EQ(r.ini->forceCpu, 0);
    // a game save (an autosave, say) before BFA's own: the 0 is there already, and the record stays for what follows
    r.mode.OnGameSave();
    CHECK(r.ini->SetByBridge());
    r.ini->forceCpu = 1;  // BFA's SaveIni
    r.mode.OnGameSave();
    CHECK_EQ(r.ini->forceCpu, 0);
    CHECK_EQ(r.Logged("Put BFA's [General] ForceCPU back to 0"), 1);
    // BFA saves again after the last game save, and the game quits: the next start loads 1, the record stays
    r.ini->forceCpu = 1;
    auto next = r.NextStart(kCpuModeBfaVersion);
    CHECK(r.ini->SetByBridge());
    next->OnServerVersion(1);
    CHECK_EQ(r.ini->forceCpu, 0);
    // BFA 5.0.6, both keys, each written back as BFA's own INI held it
    Mode5Saved five;
    five.mode.OnServerVersion(2);
    CHECK_EQ(five.ini->Own("ForceCPU"), std::string("1"));
    CHECK_EQ(five.ini->Own("EmotionDevice"), std::string("-1"));
    five.mode.OnServerVersion(1);
    CHECK_EQ(five.ini->Own("ForceCPU"), std::string("0"));
    CHECK_EQ(five.ini->Own("EmotionDevice"), std::string("2"));
    five.mode.OnGameSave();  // before BFA's save: both records stay
    CHECK_EQ(five.ini->Record(kForceCpuRecord), std::string("1"));
    CHECK_EQ(five.ini->Record(kEmotionDeviceRecord), std::string("2"));
    five.ini->BfaSaves(1, -1);
    five.mode.OnGameSave();
    CHECK_EQ(five.ini->Own("ForceCPU"), std::string("0"));
    CHECK_EQ(five.ini->Own("EmotionDevice"), std::string("2"));
    CHECK_EQ(five.Logged("Put BFA's [General] EmotionDevice back to 2"), 1);
    CHECK_EQ(five.ini->Record(kEmotionDeviceRecord), std::string("2"));
    five.NextStart("5.0.6.3");
    CHECK(!five.ini->SetByBridge());
    CHECK_EQ(five.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
    // BFA updated to a version the bridge doesn't serve: the same at each save
    Mode5Saved up;
    up.mode.OnServerVersion(2);
    auto updated = up.NextStart("5.1.0.1");
    updated->OnServerVersion(2);
    CHECK_EQ(up.ini->Own("ForceCPU"), std::string("0"));
    CHECK_EQ(up.ini->Own("EmotionDevice"), std::string("2"));
    up.ini->BfaSaves(1, -1);
    updated->OnGameSave();
    CHECK_EQ(up.ini->Own("ForceCPU"), std::string("0"));
    CHECK_EQ(up.ini->Own("EmotionDevice"), std::string("2"));
    // nothing answered this session: nothing written at a save
    Mode5Saved quiet;
    quiet.mode.OnServerVersion(2);
    auto none = quiet.NextStart("5.0.6.3");
    quiet.ini->BfaSaves(0, 2);
    const int writes = quiet.ini->iniWrites;
    none->OnGameSave();
    CHECK_EQ(quiet.ini->iniWrites, writes);
}

// BFA's INI missing: never created. Nothing recorded or written at version 2, at saves or at version 1, one log
// line; a record left with nothing to put back is cleared.
TEST(BfaModeNeverCreatesBfasIni) {
    ModeRig r;
    r.ini->present = false;
    r.mode.OnStart();
    r.mode.OnServerVersion(2);
    r.mode.OnGameSave();
    r.mode.OnServerVersion(1);
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, 0);
    CHECK_EQ(r.Logged("BFA's mode is left as it is: Data\\SKSE\\Plugins\\BetterFacialAnimations.ini isn't there, and the bridge never "
                      "creates it"),
             1);
    // set earlier, then the file went: the record is cleared, and the file isn't made again
    ModeRig gone;
    gone.mode.OnServerVersion(2);
    CHECK(gone.ini->SetByBridge());
    gone.ini->present = false;
    gone.mode.OnServerVersion(1);
    CHECK(!gone.ini->SetByBridge());
    CHECK_EQ(gone.ini->iniWrites, 1);
    // found gone at a start (BFA's mod disabled for a launch, say): BFA loaded nothing of it, so the record stays
    ModeRig start;
    start.mode.OnServerVersion(2);
    start.ini->present = false;
    const int records = start.ini->recordWrites;
    start.NextStart(kCpuModeBfaVersion);
    CHECK(start.ini->SetByBridge());
    CHECK_EQ(start.ini->recordWrites, records);
    // the file back, with the bridge's 1: kept, and a version 1 server gets the 0 back
    start.ini->present = true;
    auto back = start.NextStart(kCpuModeBfaVersion);
    CHECK(start.ini->SetByBridge());
    back->OnServerVersion(1);
    CHECK_EQ(start.ini->forceCpu, 0);
    // the file back without the bridge's value: a stale record, cleared at that start
    ModeRig stale;
    stale.mode.OnServerVersion(2);
    stale.ini->present = false;
    stale.NextStart(kCpuModeBfaVersion);
    stale.ini->present = true;
    stale.ini->forceCpu = 0;
    stale.NextStart(kCpuModeBfaVersion);
    CHECK(!stale.ini->SetByBridge());
    // BFA 5.0.6: both records stay through a start without the file; at its return, each goes when its value isn't the
    // bridge's, and stays when it is
    Mode5Rig five;
    five.mode.OnServerVersion(2);
    five.ini->present = false;
    five.NextStart("5.0.6.3");
    CHECK_EQ(five.ini->Record(kForceCpuRecord), std::string("added"));
    CHECK_EQ(five.ini->Record(kEmotionDeviceRecord), std::string("added"));
    five.ini->present = true;
    auto returned = five.NextStart("5.0.6.3");
    CHECK_EQ(five.ini->Record(kForceCpuRecord), std::string("added"));
    CHECK_EQ(five.ini->Record(kEmotionDeviceRecord), std::string("added"));
    returned->OnServerVersion(1);
    CHECK_EQ(five.ini->Own("ForceCPU"), std::string("(absent)"));
    CHECK_EQ(five.ini->Own("EmotionDevice"), std::string("(absent)"));
    Mode5Rig reinstalled;
    reinstalled.mode.OnServerVersion(2);
    reinstalled.ini->present = false;
    reinstalled.NextStart("5.0.6.3");
    reinstalled.ini->present = true;  // reinstalled: its own INI holds neither key again
    reinstalled.ini->forceCpuInIni = false;
    reinstalled.ini->values.erase("/General/EmotionDevice");
    reinstalled.NextStart("5.0.6.3");
    CHECK(reinstalled.ini->records.empty());
    // the same with only EmotionDevice gone from BFA's own INI: its record goes, ForceCPU's stays
    Mode5Rig half;
    half.mode.OnServerVersion(2);
    half.ini->present = false;
    half.NextStart("5.0.6.3");
    half.ini->present = true;
    half.ini->Set("", "EmotionDevice", "1");
    half.NextStart("5.0.6.3");
    CHECK_EQ(half.ini->Record(kForceCpuRecord), std::string("added"));
    CHECK_EQ(half.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
}

// BFA's INI there but unreadable (another program holding it): nothing cleared or written, at a start, an answer or a
// save, logged once; once it reads again, the records do their work.
TEST(BfaModeLeavesAnUnreadableIni) {
    Mode5Rig r;
    r.mode.OnServerVersion(2);
    r.ini->readable = false;
    const int writes = r.ini->iniWrites + r.ini->recordWrites;
    auto next = r.NextStart("5.0.6.3");
    CHECK_EQ(r.Logged("couldn't be read"), 1);  // said at the start itself
    next->OnServerVersion(1);
    next->OnGameSave();
    next->OnServerVersion(2);
    next->OnGameSave();
    CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, writes);
    CHECK_EQ(r.ini->Record(kForceCpuRecord), std::string("added"));
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    CHECK_EQ(r.Logged("BFA's mode is left as it is for now: Data\\SKSE\\Plugins\\BetterFacialAnimations.ini is there but couldn't be "
                      "read (another program may hold it), so the bridge changes nothing in it and keeps its records"),
             1);
    CHECK_EQ(r.Logged("isn't there"), 0);
    // readable again in that session: the version 1 answer puts back
    r.ini->readable = true;
    next->OnServerVersion(1);
    CHECK_EQ(r.ini->Own("ForceCPU"), std::string("(absent)"));
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("(absent)"));
    // BFA 4.3.2.3: a start that can't read ForceCPU keeps the record, and a version 2 answer writes nothing
    ModeRig four;
    four.mode.OnServerVersion(2);
    four.ini->readable = false;
    auto later = four.NextStart(kCpuModeBfaVersion);
    CHECK(four.ini->SetByBridge());
    later->OnServerVersion(1);
    CHECK_EQ(four.ini->forceCpu, 1);
    CHECK(four.ini->SetByBridge());
    ModeRig fresh;
    fresh.ini->readable = false;
    fresh.mode.OnServerVersion(2);
    fresh.mode.OnGameSave();
    CHECK_EQ(fresh.ini->iniWrites + fresh.ini->recordWrites, 0);
    CHECK_EQ(fresh.Logged("couldn't be read"), 1);
}

// The EmotionDevice record is BFA 5.0.6's: a start, an answer or a save with BFA 4.3.2.3 (another Mod Organizer 2
// profile, its own BFA INI, the same Overwrite) leaves it, and back on BFA 5.0.6 it is put back as before.
TEST(BfaModeKeepsBfa5sRecordUnder4323) {
    Mode5Rig r;
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    // BFA 4.3.2.3's INI: ForceCPU=1, no EmotionDevice
    r.ini->values.erase("/General/EmotionDevice");
    auto four = r.NextStart(kCpuModeBfaVersion);
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    four->OnServerVersion(1);
    four->OnGameSave();
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("(absent)"));
    CHECK_EQ(r.Logged("EmotionDevice from"), 0);
    // another BFA 4 (4.4.0): no EmotionDevice in its INI either, and its answers and saves leave the record
    auto other4 = r.NextStart("4.4.0");
    other4->OnServerVersion(1);
    other4->OnGameSave();
    other4->OnServerVersion(2);
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    // another version's start leaves it too (its put-back runs at an answer)
    r.NextStart("5.1.0.1");
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    // back on BFA 5.0.6, its INI with the bridge's -1: kept at the start, removed at a version 1 answer
    r.ini->Set("", "EmotionDevice", "-1");
    auto five = r.NextStart("5.0.6.3");
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    five->OnServerVersion(1);
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("(absent)"));
}

// Another BFA version (or one unknown): its mode is left as it is, and a value the bridge set earlier is put back.
TEST(BfaModeOnlyForBfa4323) {
    for (const char* other : {"4.3.2.2", "4.4.0", ""}) {
        ModeRig r(other);
        r.mode.OnServerVersion(2);
        r.mode.OnGameSave();
        CHECK_EQ(r.ini->forceCpu, 0);
        CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, 0);
        CHECK_EQ(r.Logged("BFA's mode is left as it is: this is Better Facial Animations"), 1);
        CHECK_EQ(r.Logged(", the versions it is made for"), 1);
    }
    // set under 4.3.2.3, then BFA updated: put back
    ModeRig r;
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->forceCpu, 1);
    BfaCpuMode updated(std::make_unique<FakeIniStore>(r.ini), nullptr);
    updated.SetBfaVersion("4.4.0");
    updated.OnServerVersion(2);
    CHECK_EQ(r.ini->forceCpu, 0);
}

// ---- BFA 5.0.6's mode: ForceCPU and EmotionDevice, read through BFA 5's layers

// Version 2 with BFA 5.0.6, as its installer leaves it (both keys only in the installer's files): ForceCPU=1 and
// EmotionDevice=-1 in BFA's own INI, which wins over the installer's files, each recorded first as added; one log
// line naming both; IndependentGpuDevice and the installer's files left as they are. Each game save writes them again;
// a card picked in BFA's menu since is recorded in place of the first.
TEST(Bfa5ModeSetsBothKeysOnVersionTwo) {
    Mode5Rig r;
    const auto installer = r.ini->values;
    r.mode.OnServerVersion(0);
    CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, 0);
    r.mode.OnServerVersion(2);
    CHECK(r.mode.Set());
    CHECK_EQ(r.ini->forceCpu, 1);
    CHECK(r.ini->forceCpuInIni);
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("-1"));
    CHECK_EQ(r.ini->Record(kForceCpuRecord), std::string("added"));
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("added"));
    CHECK_EQ(r.Logged("Set BFA's [General] ForceCPU to 1 (it was 0) and EmotionDevice to -1 (it was 0) in BetterFacialAnimations.ini"), 1);
    CHECK_EQ(r.Logged("Audio2Face and Audio2Emotion in its CPU mode from the next game start and creates no graphics device on this PC"), 1);
    for (const auto& [k, v] : installer)
        if (k.rfind("/", 0) != 0) CHECK_EQ(r.ini->values[k], v);  // the installer's files: never written
    CHECK_EQ(r.ini->Own("IndependentGpuDevice"), std::string("(absent)"));
    FakeIniStore reader(r.ini);
    CHECK_EQ(ReadBfa5Devices(reader).forceCpu, 1);
    CHECK_EQ(ReadBfa5Devices(reader).emotionDevice, -1L);
    // BFA's menu picks graphics card 1 for Audio2Emotion in this session (BFA's save writes every key): the next save
    // writes -1 again, silently, and the record names card 1, as BFA's own INI held it
    r.ini->BfaSaves(0, 1);
    r.mode.OnGameSave();
    CHECK_EQ(r.ini->Own("ForceCPU"), std::string("1"));
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("-1"));
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("1"));
    CHECK_EQ(r.ini->Record(kForceCpuRecord), std::string("1"));
    CHECK_EQ(r.log.size(), std::size_t{1});
    // both on the CPU already: nothing written
    const int writes = r.ini->iniWrites + r.ini->recordWrites;
    r.mode.OnGameSave();
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, writes);
    // the card picked is the one put back
    r.mode.OnServerVersion(1);
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("1"));
    CHECK_EQ(r.ini->Own("ForceCPU"), std::string("0"));
}

// BFA 5's layers: its own INI wins over the installer's files, the last file wins among those, and a key missing
// everywhere takes BFA's default (ForceCPU from the legacy GPU=0; EmotionDevice -1 with ForceCPU=1, else GpuAdapter
// at least 0). Numbers read as BFA's GetLongValue reads them on Windows.
TEST(Bfa5ModeReadsBfasLayers) {
    const auto devices = [](const std::function<void(FakeIni&)>& setUp) {
        auto ini = std::make_shared<FakeIni>();
        ini->forceCpuInIni = false;
        setUp(*ini);
        FakeIniStore store(ini);
        const Bfa5Devices d = ReadBfa5Devices(store);
        return std::make_pair(d.forceCpu, d.emotionDevice);
    };
    using P = std::pair<int, long>;
    CHECK(devices([](FakeIni&) {}) == P(0, 0));  // nothing anywhere: graphics card 0 for both
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "ForceCPU", "1"); i.Set("EmotionDevice", "EmotionDevice", "-1"); }) == P(1, -1));
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "ForceCPU", "1"); }) == P(1, -1));  // EmotionDevice follows ForceCPU
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "GpuAdapter", "3"); }) == P(0, 3));  // or GpuAdapter
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "GpuAdapter", "-1"); }) == P(0, 0));  // which BFA keeps at 0 at least
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "ForceCPU", "1"); i.forceCpuInIni = true; i.forceCpu = 0; }) == P(0, 0));
    CHECK(devices([](FakeIni& i) { i.Set("EmotionDevice", "EmotionDevice", "0"); i.Set("", "EmotionDevice", "2"); }) == P(0, 2));
    CHECK(devices([](FakeIni& i) { i.Set("", "GPU", "0"); }) == P(1, -1));       // the legacy key
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "GPU", "0"); }) == P(1, -1));
    CHECK(devices([](FakeIni& i) { i.Set("", "GPU", "0"); i.forceCpuInIni = true; i.forceCpu = 0; }) == P(0, 0));
    CHECK(devices([](FakeIni& i) { i.Set("Startup", "ForceCPU", "x"); i.Set("", "GPU", "0"); }) == P(1, -1));  // not a number
    // the later choice file wins (GraphicsRoute comes first, EmotionDevice last)
    CHECK(devices([](FakeIni& i) { i.Set("GraphicsRoute", "EmotionDevice", "1"); i.Set("EmotionDevice", "EmotionDevice", "2"); }) == P(0, 2));
    // numbers: hexadecimal, and anything not wholly a number is the default
    CHECK(devices([](FakeIni& i) { i.Set("", "EmotionDevice", "0x2"); }) == P(0, 2));
    CHECK(devices([](FakeIni& i) { i.Set("", "EmotionDevice", "0X2"); }) == P(0, 2));
    CHECK(devices([](FakeIni& i) { i.Set("", "EmotionDevice", "1x"); i.Set("Startup", "GpuAdapter", "1"); }) == P(0, 1));
    CHECK(devices([](FakeIni& i) { i.Set("", "EmotionDevice", ""); }) == P(0, 0));
    CHECK(devices([](FakeIni& i) { i.Set("", "EmotionDevice", "\"2\""); }) == P(0, 0));  // quotes are part of the value
    CHECK(devices([](FakeIni& i) { i.Set("", "EmotionDevice", "-7"); }) == P(0, -1));  // BFA keeps -1 at least
    CHECK_EQ(Bfa5Long(std::string("0x"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("0X"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("12"), 5), 12L);
    CHECK_EQ(Bfa5Long(std::string(" 12"), 5), 12L);  // strtol's leading blanks
    CHECK_EQ(Bfa5Long(std::string("+12"), 5), 12L);
    CHECK_EQ(Bfa5Long(std::string("-12"), 5), -12L);
    CHECK_EQ(Bfa5Long(std::string("12 "), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("1.0"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("-"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("0x1f"), 5), 31L);
    CHECK_EQ(Bfa5Long(std::string("0x-1"), 5), -1L);  // strtol's sign after SimpleIni's 0x
    CHECK_EQ(Bfa5Long(std::string("0x0x1f"), 5), 31L);  // and its own 0x
    CHECK_EQ(Bfa5Long(std::string("0x0x"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("0xg"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("2147483647"), 5), 2147483647L);  // a long on Windows: 32 bits
    CHECK_EQ(Bfa5Long(std::string("2147483648"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("-2147483648"), 5), -2147483647L - 1);
    CHECK_EQ(Bfa5Long(std::string("-2147483649"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("0x7fffffff"), 5), 2147483647L);
    CHECK_EQ(Bfa5Long(std::string("0x80000000"), 5), 5L);
    CHECK_EQ(Bfa5Long(std::string("99999999999999999999999"), 5), 5L);
    CHECK(!Bfa5Number(std::string("18446744073709551617")).has_value());  // 2^64 + 1: no wrap to 1
    CHECK(!Bfa5Number(std::string("0x100000001")).has_value());
    CHECK_EQ(Bfa5Long(std::string(63, '0'), 5), 0L);
    CHECK_EQ(Bfa5Long(std::string(64, '0'), 5), 5L);  // SimpleIni's 64-byte buffer
    CHECK_EQ(Bfa5Long(std::nullopt, 5), 5L);
    CHECK(!Bfa5Number(std::string("")).has_value());
    CHECK(Bfa5Number(std::string("0")) == 0L);
    // BFA 5's Batch: 2 by default, at least 1, from its layers too
    auto ini = std::make_shared<FakeIni>();
    FakeIniStore store(ini);
    CHECK_EQ(Bfa5Batch(store), 2);
    ini->Set("FaceModel", "Batch", "3");
    CHECK_EQ(Bfa5Batch(store), 3);
    ini->Set("", "Batch", "4");
    CHECK_EQ(Bfa5Batch(store), 4);
    ini->Set("", "Batch", "0");
    CHECK_EQ(Bfa5Batch(store), 1);
    ini->Set("", "Batch", "-3");
    CHECK_EQ(Bfa5Batch(store), 1);
    ini->Set("", "Batch", "\"4\"");
    CHECK_EQ(Bfa5Batch(store), 2);
}

// BFA 5's own choices are kept: both on the CPU already, nothing is recorded or written; one of them, only the other.
TEST(Bfa5ModeKeepsWhatIsOnTheCpuAlready) {
    Mode5Rig both;
    both.ini->Set("Startup", "ForceCPU", "1");
    both.ini->Set("EmotionDevice", "EmotionDevice", "-1");
    both.mode.OnServerVersion(2);
    CHECK_EQ(both.ini->iniWrites + both.ini->recordWrites, 0);
    CHECK(both.log.empty());
    both.mode.OnServerVersion(1);
    CHECK_EQ(both.ini->iniWrites + both.ini->recordWrites, 0);
    Mode5Rig face;  // Audio2Face on the CPU, Audio2Emotion on graphics card 2 (BFA's own INI)
    face.ini->Set("Startup", "ForceCPU", "1");
    face.ini->Set("", "EmotionDevice", "2");
    face.mode.OnServerVersion(2);
    CHECK(!face.ini->SetByBridge());
    CHECK(!face.ini->forceCpuInIni);
    CHECK_EQ(face.ini->Own("EmotionDevice"), std::string("-1"));
    CHECK_EQ(face.ini->Record(kEmotionDeviceRecord), std::string("2"));
    CHECK_EQ(face.Logged("Set BFA's [General] EmotionDevice to -1 (it was 2) in"), 1);
    CHECK_EQ(face.Logged("so BFA runs Audio2Face and Audio2Emotion in its CPU mode"), 1);
    // BFA's own INI holds one of the two keys: each record asks about its own key
    Mode5Rig card;
    card.ini->Set("", "EmotionDevice", "1");
    card.mode.OnServerVersion(2);
    CHECK_EQ(card.ini->Record(kForceCpuRecord), std::string("added"));
    CHECK_EQ(card.ini->Record(kEmotionDeviceRecord), std::string("1"));
    Mode5Rig gpu;
    gpu.ini->forceCpuInIni = true;
    gpu.ini->forceCpu = 0;
    gpu.mode.OnServerVersion(2);
    CHECK_EQ(gpu.ini->Record(kForceCpuRecord), std::string("1"));
    CHECK_EQ(gpu.ini->Record(kEmotionDeviceRecord), std::string("added"));
    gpu.mode.OnServerVersion(1);
    CHECK_EQ(gpu.ini->Own("ForceCPU"), std::string("0"));
    CHECK_EQ(gpu.ini->Own("EmotionDevice"), std::string("(absent)"));
    Mode5Rig emotion;  // Audio2Emotion on the CPU only
    emotion.ini->Set("", "EmotionDevice", "-1");
    emotion.mode.OnServerVersion(2);
    CHECK(emotion.ini->SetByBridge());
    CHECK_EQ(emotion.ini->forceCpu, 1);
    CHECK_EQ(emotion.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
    CHECK_EQ(emotion.Logged("Set BFA's [General] ForceCPU to 1 (it was 0) in"), 1);
}

// A version 1 server: both values the bridge set go back, each as its record says: a key BFA's own INI didn't hold
// is removed, so BFA's installer choice applies again (even one changed since), and a value it held is written back;
// one line each. A value the player changed since is left as it is, its record cleared; a damaged record puts nothing
// back.
TEST(Bfa5ModePutBackOnVersionOne) {
    Mode5Rig r;
    r.mode.OnServerVersion(2);
    // the installer's choice changed since: Audio2Emotion on the CPU (Installer\EmotionDevice.ini)
    r.ini->Set("EmotionDevice", "EmotionDevice", "-1");
    r.mode.OnServerVersion(1);
    CHECK(!r.mode.Set());
    CHECK_EQ(r.ini->Own("ForceCPU"), std::string("(absent)"));
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("(absent)"));
    FakeIniStore reader(r.ini);
    CHECK_EQ(ReadBfa5Devices(reader).forceCpu, 0);
    CHECK_EQ(ReadBfa5Devices(reader).emotionDevice, -1L);  // the installer's choice
    CHECK_EQ(r.Logged("Removed the bridge's [General] ForceCPU from BetterFacialAnimations.ini, which had none before the bridge set "
                      "it, so BFA's installer choice applies again: the server speaks protocol version 1"),
             1);
    CHECK_EQ(r.Logged("Removed the bridge's [General] EmotionDevice from BetterFacialAnimations.ini"), 1);
    CHECK_EQ(r.Logged("back to"), 0);
    r.NextStart("5.0.6.3");
    CHECK(r.ini->records.empty());
    // BFA's own INI held both: written back as they were
    Mode5Rig held;
    held.ini->forceCpuInIni = true;
    held.ini->forceCpu = 0;
    held.ini->Set("", "EmotionDevice", "1");
    held.mode.OnServerVersion(2);
    CHECK_EQ(held.ini->Record(kForceCpuRecord), std::string("1"));
    CHECK_EQ(held.ini->Record(kEmotionDeviceRecord), std::string("1"));
    held.mode.OnServerVersion(1);
    CHECK_EQ(held.ini->Own("ForceCPU"), std::string("0"));
    CHECK_EQ(held.ini->Own("EmotionDevice"), std::string("1"));
    CHECK_EQ(held.Logged("Put BFA's [General] ForceCPU back to 0 in BetterFacialAnimations.ini, as it was: the server speaks protocol version 1"),
             1);
    CHECK_EQ(held.Logged("Put BFA's [General] EmotionDevice back to 1 in BetterFacialAnimations.ini, as it was: the server speaks protocol "
                         "version 1 only, and the bridge sets BFA's CPU mode only for a server that speaks version 2. It applies from the "
                         "next game start."),
             1);
    const int writes = held.ini->iniWrites;
    held.mode.OnGameSave();
    held.mode.OnServerVersion(1);
    CHECK_EQ(held.ini->iniWrites, writes);
    // the player chose graphics card 3 in BFA's menu since: left as it is, the record goes
    Mode5Rig changed;
    changed.mode.OnServerVersion(2);
    changed.ini->Set("", "EmotionDevice", "3");
    changed.mode.OnServerVersion(1);
    CHECK_EQ(changed.ini->Own("EmotionDevice"), std::string("3"));
    CHECK_EQ(changed.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
    CHECK_EQ(changed.Logged("EmotionDevice from"), 0);
    // BFA reinstalled since: its own INI holds no EmotionDevice, and the installer's file says the CPU; nothing is
    // written over that choice, and the record goes
    Mode5Rig reinstalled;
    reinstalled.ini->Set("", "EmotionDevice", "0");
    reinstalled.mode.OnServerVersion(2);
    CHECK_EQ(reinstalled.ini->Record(kEmotionDeviceRecord), std::string("0"));
    reinstalled.ini->values.erase("/General/EmotionDevice");
    reinstalled.ini->Set("EmotionDevice", "EmotionDevice", "-1");
    reinstalled.mode.OnServerVersion(1);
    CHECK_EQ(reinstalled.ini->Own("EmotionDevice"), std::string("(absent)"));
    CHECK_EQ(reinstalled.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
    // a damaged record: nothing put back, and it stays
    for (const char* damaged : {"x", "-1", "1 ", "1234", "", "Added"}) {
        Mode5Rig d;
        d.mode.OnServerVersion(2);
        if (*damaged)
            d.ini->records[kEmotionDeviceRecord] = damaged;
        else
            d.ini->records.erase(kEmotionDeviceRecord);
        d.mode.OnServerVersion(1);
        CHECK_EQ(d.ini->Own("EmotionDevice"), std::string("-1"));
        CHECK_EQ(d.Logged("EmotionDevice from"), 0);
    }
}

// A card over 999 can't be recorded, so BFA's file is left as it is; a write to BFA's INI that fails: no line claims
// it, the record stays and the next answer tries again.
TEST(Bfa5ModeFailuresKeepWhatTheyCant) {
    Mode5Rig big;
    big.ini->Set("", "EmotionDevice", "1000");
    big.mode.OnServerVersion(2);
    CHECK_EQ(big.ini->Own("EmotionDevice"), std::string("1000"));
    CHECK_EQ(big.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
    CHECK_EQ(big.Logged("BFA's EmotionDevice is left as it is: the bridge couldn't record that it sets it (card 1000 is out of range)"), 1);
    CHECK_EQ(big.Logged("Set BFA's [General] ForceCPU to 1 (it was 0) in BetterFacialAnimations.ini: the server speaks protocol version 2, "
                        "so BFA runs Audio2Face in its CPU mode from the next game start. The model"),
             1);
    // EmotionDevice=-1 can't be written: the line names ForceCPU and Audio2Face alone
    Mode5Rig set;
    set.ini->failWrites.insert("EmotionDevice");
    set.mode.OnServerVersion(2);
    set.mode.OnGameSave();
    CHECK_EQ(set.Logged("BFA's [General] EmotionDevice couldn't be set to -1 (read-only)"), 1);
    CHECK_EQ(set.Logged("EmotionDevice to -1"), 0);
    CHECK_EQ(set.Logged("so BFA runs Audio2Face in its CPU mode from the next game start. The model"), 1);
    CHECK_EQ(set.Logged("graphics device"), 0);
    // its put-back can't be written: the record stays, a warning, and the next answer puts it back
    Mode5Rig back;
    back.mode.OnServerVersion(2);
    back.ini->failWrites.insert("EmotionDevice");
    back.mode.OnServerVersion(1);
    CHECK_EQ(back.ini->Own("EmotionDevice"), std::string("-1"));
    CHECK_EQ(back.ini->Record(kEmotionDeviceRecord), std::string("added"));
    CHECK_EQ(back.Logged("BFA's [General] EmotionDevice couldn't be removed (read-only); the bridge keeps its record"), 1);
    CHECK_EQ(back.Logged("EmotionDevice from"), 0);
    back.ini->failWrites.clear();
    back.mode.OnServerVersion(1);
    CHECK_EQ(back.ini->Own("EmotionDevice"), std::string("(absent)"));
    CHECK_EQ(back.Logged("Removed the bridge's [General] EmotionDevice"), 1);
    Mode5Rig written;
    written.ini->Set("", "EmotionDevice", "2");
    written.mode.OnServerVersion(2);
    written.ini->failWrites.insert("EmotionDevice");
    written.mode.OnServerVersion(1);
    written.mode.OnGameSave();
    CHECK_EQ(written.ini->Own("EmotionDevice"), std::string("-1"));
    CHECK_EQ(written.ini->Record(kEmotionDeviceRecord), std::string("2"));
    CHECK_EQ(written.Logged("couldn't be put back to 2 (read-only)"), 1);
    written.ini->failWrites.clear();
    written.mode.OnGameSave();
    CHECK_EQ(written.ini->Own("EmotionDevice"), std::string("2"));
    // a ForceCPU failure is logged on its own, beside an EmotionDevice one
    Mode5Rig two;
    two.ini->failWrites = {"ForceCPU", "EmotionDevice"};
    two.mode.OnServerVersion(2);
    CHECK_EQ(two.Logged("ForceCPU couldn't be set to 1"), 1);
    CHECK_EQ(two.Logged("EmotionDevice couldn't be set to -1"), 1);
    CHECK_EQ(two.Logged("Set BFA's"), 0);
}

// Only BFA 5.0.6 and its builds: another 5.x is left as it is, and the values the bridge set under 5.0.6 go back once
// BFA is updated. BFA's INI missing: nothing for 5.0.6 either. A record that can't be written leaves that key alone.
TEST(Bfa5ModeOnlyFor506) {
    for (const char* v : {"5.0.6", "5.0.6.3", "5.0.6.15"}) {
        CHECK(IsBfa506(v));
        Mode5Rig r(v);
        r.mode.OnServerVersion(2);
        CHECK_EQ(r.ini->forceCpu, 1);
        CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("-1"));
    }
    for (const char* v : {"5.0.5.2", "5.0.7", "5.0.60", "5.1.0.1", "5", "4.3.2.3x", ""}) {
        CHECK(!IsBfa506(v));
        Mode5Rig r(v);
        r.mode.OnServerVersion(2);
        CHECK_EQ(r.ini->iniWrites + r.ini->recordWrites, 0);
        CHECK_EQ(r.Logged("BFA's mode is left as it is: this is Better Facial Animations"), 1);
    }
    Mode5Rig r;
    r.mode.OnServerVersion(2);
    BfaCpuMode updated(std::make_unique<FakeIniStore>(r.ini), nullptr);
    updated.SetBfaVersion("5.1.0.1");
    updated.OnServerVersion(2);
    CHECK_EQ(r.ini->Own("ForceCPU"), std::string("(absent)"));
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("(absent)"));
    FakeIniStore reader(r.ini);
    CHECK_EQ(ReadBfa5Devices(reader).emotionDevice, 0L);
    // BFA's INI missing: never created, nothing recorded
    Mode5Rig absent;
    absent.ini->present = false;
    absent.mode.OnServerVersion(2);
    absent.mode.OnGameSave();
    absent.mode.OnServerVersion(1);
    CHECK_EQ(absent.ini->iniWrites + absent.ini->recordWrites, 0);
    // set, then the file went: both records cleared at version 1
    Mode5Rig gone;
    gone.mode.OnServerVersion(2);
    gone.ini->present = false;
    gone.mode.OnServerVersion(1);
    CHECK(gone.ini->records.empty());
    // the records can't be written: BFA's file is left as it is, logged once each
    Mode5Rig ro;
    ro.ini->failRecord = true;
    ro.mode.OnServerVersion(2);
    ro.mode.OnGameSave();
    CHECK_EQ(ro.ini->iniWrites, 0);
    CHECK_EQ(ro.Logged("ForceCPU is left as it is: the bridge couldn't record that it sets it"), 1);
    CHECK_EQ(ro.Logged("EmotionDevice is left as it is: the bridge couldn't record that it sets it"), 1);
}

// BFA 4.3.2.3 is as before: ForceCPU from its own INI alone, EmotionDevice and the installer's files never read or
// written.
TEST(BfaModeFor4323IgnoresBfa5sKeys) {
    ModeRig r;
    r.ini->Set("Startup", "ForceCPU", "1");  // not BFA 4's: ignored
    r.ini->Set("", "EmotionDevice", "0");
    r.mode.OnServerVersion(2);
    CHECK_EQ(r.ini->forceCpu, 1);
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("0"));
    CHECK_EQ(r.ini->Record(kEmotionDeviceRecord), std::string("(none)"));
    CHECK_EQ(r.ini->iniWrites, 1);
    CHECK_EQ(r.ini->recordWrites, 1);
    CHECK_EQ(r.Logged("Set BFA's [General] ForceCPU to 1 in BetterFacialAnimations.ini (it was 0): the server speaks protocol version 2, "
                      "so BFA uses its CPU mode from the next game start"),
             1);
    r.mode.OnServerVersion(1);
    CHECK_EQ(r.ini->forceCpu, 0);
    CHECK_EQ(r.ini->Own("EmotionDevice"), std::string("0"));
}

// ---- BFA 5's files read as BFA 5 reads them (core/IniText.h)

TEST(IniTextReadsAsSimpleIni) {
    const auto v = [](std::string_view text, const char* key = "EmotionDevice", const char* section = "General") {
        return IniTextValue(text, section, key).value_or("(none)");
    };
    CHECK_EQ(v("[General]\nEmotionDevice=1\n"), std::string("1"));
    CHECK_EQ(v("[General]\r\nEmotionDevice = 1 \r\n"), std::string("1"));  // blanks around
    CHECK_EQ(v("[General]\rEmotionDevice=\t1\t\r"), std::string("1"));       // a carriage return ends a line
    CHECK_EQ(v("[General]\nEmotionDevice=1"), std::string("1"));             // no line end at the file's end
    CHECK_EQ(v("\xEF\xBB\xBF[General]\nEmotionDevice=1\n"), std::string("1"));  // BFA 5's own saves' byte-order mark
    CHECK_EQ(v("x\xEF\xBB\xBF[General]\nEmotionDevice=1\n"), std::string("(none)"));
    CHECK_EQ(v("[General]\nEmotionDevice=1\nEmotionDevice=-1\n"), std::string("-1"));  // the last one counts
    CHECK_EQ(v("[General]\nEmotionDevice=1\n[Other]\nEmotionDevice=4\n[General]\nEmotionDevice=-1\n[Other]\n"), std::string("-1"));
    CHECK_EQ(v("[general]\nemotiondevice=2\n"), std::string("2"));  // ASCII case
    CHECK_EQ(v("[ General ]  trailing\nEmotionDevice=2\n"), std::string("2"));
    CHECK_EQ(v("[\nGeneral]\nEmotionDevice=2\n"), std::string("2"));  // SimpleIni's blanks after [ run on
    CHECK_EQ(v("[General\nEmotionDevice=2\n"), std::string("(none)"));  // no ]: no section the bridge reads
    CHECK_EQ(v("[General]\nEmotionDevice=1\n[Gen\nEmotionDevice=2\n"), std::string("1"));
    CHECK_EQ(v("EmotionDevice=1\n"), std::string("(none)"));  // before any section
    CHECK_EQ(v("[General]\nEmotionDevice=\"1\"\n"), std::string("\"1\""));  // quotes kept
    CHECK_EQ(v("[General]\nEmotionDevice=1 ; card one\n"), std::string("1 ; card one"));
    CHECK_EQ(v("[General]\n; EmotionDevice=1\n  # EmotionDevice=2\nEmotionDevice=3\n"), std::string("3"));
    CHECK_EQ(v("[General]\n;EmotionDevice=1\n"), std::string("(none)"));
    CHECK_EQ(v("[General]\nEmotionDevice\n=4\n"), std::string("(none)"));  // no = on its line; no key on the next
    CHECK_EQ(v("[General]\nEmotionDevice=\n"), std::string(""));
    CHECK_EQ(v("[General]\nEmotionDevice=\nForceCPU=1\n", "ForceCPU"), std::string("1"));  // a value's blanks stop at its line's end
    CHECK_EQ(v("[General]\nEmotionDevice=\nForceCPU=1\n"), std::string(""));
    CHECK_EQ(v("[General] EmotionDevice=2\n"), std::string("(none)"));  // the rest of a section's line is no key
    CHECK_EQ(v("[Gen\nForceCPU=1\n[General]\nEmotionDevice=2\n"), std::string("2"));  // a section's name ends with its line
    CHECK_EQ(v("[General]\nEmotionDevice==2\n"), std::string("=2"));
    CHECK_EQ(v("[General]\nEmotion Device=2\n", "Emotion Device"), std::string("2"));
    constexpr char withNul[] = "[General]\nEmotionDevice=1\0\nEmotionDevice=2\n";
    CHECK_EQ(v(std::string_view(withNul, sizeof withNul - 1)), std::string("1"));  // a NUL ends the text
    CHECK_EQ(v("", "Batch"), std::string("(none)"));
    CHECK_EQ(v("; Better Facial Animations\n\n[General]\nBatch=3\n", "Batch"), std::string("3"));
}

// A store over BFA 5's files' text, read with IniTextValue as the plugin reads them: a key given twice in BFA's own
// INI counts by its last value, so a CPU choice before a card the player added later doesn't keep the bridge out.
TEST(Bfa5ModeReadsTheFilesText) {
    struct TextStore final : IniStore {
        std::map<std::string, std::string> files;  // "" BFA's own INI, else the choice's name
        std::map<std::string, std::string> records;
        BfaIniState BfaIni() override { return files.count("") ? BfaIniState::Readable : BfaIniState::Missing; }
        int ReadForceCpu() override { return 0; }
        bool WriteForceCpu(int, std::string&) override { return false; }
        std::optional<std::string> ReadBfaValue(const char* choice, const char* section, const char* key) override {
            const auto it = files.find(choice ? choice : "");
            if (it == files.end()) return std::nullopt;
            return IniTextValue(it->second, section, key);
        }
        bool WriteBfaValue(const char* key, const std::string& value, std::string&) override {
            files[""] += std::string("\n[General]\n") + key + "=" + value + "\n";  // as a profile call past a hidden section
            return true;
        }
        bool RemoveBfaValue(const char*, std::string&) override { return false; }
        std::string ReadRecord(const char* key) override { return records.count(key) ? records[key] : std::string(); }
        bool WriteRecord(const char* key, const char* value, std::string&) override {
            if (value)
                records[key] = value;
            else
                records.erase(key);
            return true;
        }
    };
    auto store = std::make_unique<TextStore>();
    TextStore* files = store.get();
    files->files[""] = "\xEF\xBB\xBF[General]\nForceCPU=1\nEmotionDevice=-1\nEmotionDevice=0\n";
    files->files["EmotionDevice"] = "[General]\nEmotionDevice=-1\n";
    CHECK_EQ(ReadBfa5Devices(*files).emotionDevice, 0L);
    BfaCpuMode mode(std::move(store), nullptr);
    mode.SetBfaVersion("5.0.6.3");
    mode.OnServerVersion(2);
    CHECK_EQ(files->records[kEmotionDeviceRecord], std::string("0"));
    CHECK_EQ(ReadBfa5Devices(*files).emotionDevice, -1L);
    CHECK_EQ(ReadBfa5Devices(*files).forceCpu, 1);
    CHECK(!files->records.count(kForceCpuRecord));
}

// ---- the sizes the bridge lists for BFA 5.0.6 (core/BfaSizes.h)

TEST(Bfa506GpuRouteSizes) {
    const auto sizes = [](const std::string& bfa, const std::string& file, int batch = 2) {
        std::string s;
        for (const auto& o : GpuRouteSizes(bfa, file, batch)) s += (s.empty() ? "" : ",") + o.name + "=" + std::to_string(o.value);
        return s;
    };
    for (const char* v : {"5.0.6", "5.0.6.3"}) {
        CHECK(ListsSizesFor(v));
        CHECK_EQ(sizes(v, "a2f3_fp16.onnx"),
                 std::string("window_dynamic_axes_1=1,identity_dynamic_axes_1=1,emotion_dynamic_axes_1=1,input_latents_dynamic_axes_1=1"));
        CHECK_EQ(sizes(v, "network_fp16.onnx"), std::string("input_dynamic_axes_1=2,emotion_dynamic_axes_1=2"));
        CHECK_EQ(sizes(v, "network_fp16.onnx", 4), std::string("input_dynamic_axes_1=4,emotion_dynamic_axes_1=4"));
        CHECK_EQ(sizes(v, "network_fp16.onnx", 0), std::string("input_dynamic_axes_1=1,emotion_dynamic_axes_1=1"));
        CHECK_EQ(sizes(v, "performance.onnx"), std::string("batch=1"));
        CHECK_EQ(sizes(v, "emotion_fp16.onnx"), std::string("batch_size=1,seq_len=60000,batch=1"));
        CHECK_EQ(sizes(v, "emotion_performance.onnx"), std::string("batch_size=1,seq_len=60000,batch=1"));
        CHECK_EQ(sizes(v, "tiny.onnx"), std::string());
        CHECK_EQ(sizes(v, "Performance.onnx"), std::string());  // BFA's names, exactly
    }
    // BFA 4.3.2.3's HELLO lists none (the server fills its sizes in); no other version gets any
    for (const char* v : {"4.3.2.3", "4.3.2", "5.0.5.2", "5.1.0", ""}) {
        CHECK(!ListsSizesFor(v));
        CHECK_EQ(sizes(v, "a2f3_fp16.onnx"), std::string());
        CHECK_EQ(sizes(v, "performance.onnx"), std::string());
    }
}
