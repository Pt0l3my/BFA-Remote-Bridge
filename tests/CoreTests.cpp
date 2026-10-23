// SHA-256, the settings, the status texts, the monitor, the runtime's API versions and the PE reads.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "Test.h"
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
    CHECK(s.logDetail == LogDetail::Normal);
    CHECK(ReadSettings(nullptr) == Settings{});
    // the page holds four keys; 1.1's removed ones are never asked for
    CHECK_EQ(SettingKeys().size(), std::size_t{4});
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
    CHECK(s.logDetail == LogDetail::Detailed);
    CHECK_EQ(warnings.size(), std::size_t{1});  // the port

    values = {{"serverPort", "port"}, {"showStatus", "maybe"}, {"logDetail", "Verbose"}};
    warnings.clear();
    s = ReadSettings(get, &warnings);
    CHECK_EQ(s.serverPort, 7871);
    CHECK(s.showStatus);
    CHECK(s.logDetail == LogDetail::Normal);
    CHECK_EQ(warnings.size(), std::size_t{3});

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
    CHECK(NoServerText(true).find("no server address is set, so lip sync is the game's own") != std::string::npos);
    CHECK(NoServerText(false).find("SkyrimNet wasn't found") != std::string::npos);
    CHECK(GpuText().find("Tick Force CPU in BFA's menu") != std::string::npos);
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
    for (const std::string& t : {NoServerText(true), NoServerText(false), GpuText(), CantAttachText("", ""), BackLine("")}) {
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
    // a new fallback after that is said at once
    m.Fallback(Reason::ServerError, "emotion_fp16.onnx", "the server couldn't run it");
    CHECK_EQ(notes.size(), std::size_t{6});
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

// ---- paths out of other programs' messages

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
