// Lip sync in scenes: the hook's decision, the factions' form IDs, the slot check, the call's site and the Detailed
// log's lines (core/SceneLipSync.h).

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "Test.h"
#include "core/PeImage.h"
#include "core/SceneLipSync.h"
#include "core/Settings.h"

using namespace bfarb;
using namespace bfarb::scene;

namespace {

// Three factions and one other, as the game's forms would be: only their addresses matter.
struct Forms {
    int sexLab = 0, ostimCount = 0, ostimExcitement = 0, other = 0;
    Factions All() const { return {&sexLab, &ostimCount, &ostimExcitement}; }
};

constexpr Range kBfa{0x7FF800000000, 0x7FF800400000};

}  // namespace

TEST(SceneFactionsAreBfas) {
    CHECK_EQ(kSceneFactions.size(), std::size_t{3});
    CHECK_EQ(std::string(kSceneFactions[0].plugin), std::string("SexLab.esm"));
    CHECK_EQ(kSceneFactions[0].localId, 0xE50Fu);
    CHECK_EQ(std::string(kSceneFactions[1].plugin), std::string("OStim.esp"));
    CHECK_EQ(kSceneFactions[1].localId, 0xECAu);
    CHECK_EQ(std::string(kSceneFactions[2].plugin), std::string("OStim.esp"));
    CHECK_EQ(kSceneFactions[2].localId, 0xD93u);
    CHECK_EQ(kIsInFactionSlot, std::size_t{0xF9});
}

TEST(SceneAnswersFalseForEachFactionFromInsideBfa) {
    Forms f;
    const Factions all = f.All();
    const std::uintptr_t inside = kBfa.begin + 0x26DA;  // where BFA 4.3.2.3's scene check calls IsInFaction
    CHECK(AnswersFalse(true, &f.sexLab, all, inside, kBfa));
    CHECK(AnswersFalse(true, &f.ostimCount, all, inside, kBfa));
    CHECK(AnswersFalse(true, &f.ostimExcitement, all, inside, kBfa));
    // the image's first and last byte are inside it
    CHECK(AnswersFalse(true, &f.ostimCount, all, kBfa.begin, kBfa));
    CHECK(AnswersFalse(true, &f.ostimCount, all, kBfa.end - 1, kBfa));
}

TEST(SceneOtherFactionsGetTheGamesAnswer) {
    Forms f;
    const Factions all = f.All();
    const std::uintptr_t inside = kBfa.begin + 0x1000;
    CHECK(!AnswersFalse(true, &f.other, all, inside, kBfa));
    CHECK(!AnswersFalse(true, nullptr, all, inside, kBfa));
}

TEST(SceneCallersOutsideBfaGetTheGamesAnswer) {
    Forms f;
    const Factions all = f.All();
    // OStim, SexLab, SkyrimNet, the engine: any return address outside BFA's image
    for (const std::uintptr_t caller : {std::uintptr_t{0}, kBfa.begin - 1, kBfa.end, kBfa.end + 0x1000, std::uintptr_t{0x140001000},
                                        std::uintptr_t{0x7FFA12340000}})
        for (const void* faction : all) CHECK(!AnswersFalse(true, faction, all, caller, kBfa));
    // no range read: nobody is BFA
    for (const void* faction : all) CHECK(!AnswersFalse(true, faction, all, kBfa.begin + 0x10, Range{}));
    CHECK(!AnswersFalse(true, &f.sexLab, all, kBfa.begin + 0x10, Range{kBfa.end, kBfa.begin}));
}

TEST(SceneSettingOffPassesEverythingThrough) {
    Forms f;
    const Factions all = f.All();
    for (const void* faction : all) CHECK(!AnswersFalse(false, faction, all, kBfa.begin + 0x26DA, kBfa));
    CHECK(!AnswersFalse(false, &f.other, all, kBfa.begin + 0x26DA, kBfa));
}

TEST(SceneMissingFactionsMatchNothing) {
    Forms f;
    // OStim alone: SexLab's slot empty
    const Factions ostimOnly{nullptr, &f.ostimCount, &f.ostimExcitement};
    const std::uintptr_t inside = kBfa.begin + 0x26DA;
    CHECK(!AnswersFalse(true, &f.sexLab, ostimOnly, inside, kBfa));
    CHECK(AnswersFalse(true, &f.ostimCount, ostimOnly, inside, kBfa));
    // SexLab alone
    const Factions sexLabOnly{&f.sexLab, nullptr, nullptr};
    CHECK(AnswersFalse(true, &f.sexLab, sexLabOnly, inside, kBfa));
    CHECK(!AnswersFalse(true, &f.ostimExcitement, sexLabOnly, inside, kBfa));
    // none: a null faction never matches an empty slot
    const Factions none{};
    CHECK(!AnswersFalse(true, nullptr, none, inside, kBfa));
    CHECK(!AnswersFalse(true, &f.ostimCount, none, inside, kBfa));
}

TEST(SceneFormIdsByPluginAndLocalId) {
    // a full plugin at index 0x2A: the index in the top byte
    CHECK_EQ(FormIdIn(false, 0x2A, 0, 0xECA), 0x2A000ECAu);
    CHECK_EQ(FormIdIn(false, 0x05, 0, 0xE50F), 0x0500E50Fu);
    CHECK_EQ(FormIdIn(false, 0x00, 0, 0xD93), 0x00000D93u);
    // a light one (ESL flagged) at small index 0x123
    CHECK_EQ(FormIdIn(true, 0xFE, 0x123, 0xECA), 0xFE123ECAu);
    CHECK_EQ(FormIdIn(true, 0xFE, 0x000, 0xD93), 0xFE000D93u);
    // what doesn't fit resolves to nothing: SexLab's 0xE50F in a light plugin, a local ID beyond 24 bits, an index
    // out of range (0xFF: not loaded), no local ID
    CHECK_EQ(FormIdIn(true, 0xFE, 0x001, 0xE50F), 0u);
    CHECK_EQ(FormIdIn(true, 0xFE, 0x1000, 0xECA), 0u);
    CHECK_EQ(FormIdIn(false, 0x01, 0, 0x1000000), 0u);
    CHECK_EQ(FormIdIn(false, 0xFF, 0, 0xECA), 0u);
    CHECK_EQ(FormIdIn(false, 0xFE, 0, 0xECA), 0u);
    CHECK_EQ(FormIdIn(false, 0x01, 0, 0), 0u);
}

TEST(ScenePluginNamesIgnoreCase) {
    CHECK(SamePlugin("OStim.esp", "OStim.esp"));
    CHECK(SamePlugin("oStIm.EsP", "OStim.esp"));
    CHECK(SamePlugin("SEXLAB.ESM", "SexLab.esm"));
    CHECK(!SamePlugin("OStim.esm", "OStim.esp"));
    CHECK(!SamePlugin("OStimX.esp", "OStim.esp"));
    CHECK(!SamePlugin("OStim.esp.bak", "OStim.esp"));  // a longer name starting with the plugin's
    CHECK(!SamePlugin("OStim.es", "OStim.esp"));
    CHECK(!SamePlugin("", "OStim.esp"));
    CHECK(!SamePlugin("", ""));
}

TEST(SceneSlotMustHoldAGameFunction) {
    const Range image{0x140000000, 0x143000000};
    const Range code{0x140001000, 0x141600000};
    const std::uintptr_t slot = 0x1416D6DE0 + kIsInFactionSlot * 8;
    CHECK(SlotLooksRight(slot, 0x140612340, image, code));
    // another plugin's function in the slot: left alone
    CHECK(!SlotLooksRight(slot, 0x7FFA12340000, image, code));
    CHECK(!SlotLooksRight(slot, 0, image, code));
    // in the game's data, not its code
    CHECK(!SlotLooksRight(slot, 0x141700000, image, code));
    // a slot outside the image, or not aligned, or hanging over its end
    CHECK(!SlotLooksRight(0x7FF000000000, 0x140612340, image, code));
    CHECK(!SlotLooksRight(slot + 4, 0x140612340, image, code));
    CHECK(!SlotLooksRight(image.end - 4, 0x140612340, image, code));
    CHECK(SlotLooksRight(image.end - 8, 0x140612340, image, code));
    // no ranges read
    CHECK(!SlotLooksRight(slot, 0x140612340, Range{}, code));
    CHECK(!SlotLooksRight(slot, 0x140612340, image, Range{}));
}

TEST(SceneSiteIsTheScenesChecksCaller) {
    const std::uintptr_t hook = 0x7FFB00001000, check = kBfa.begin + 0x26DA, caller = kBfa.begin + 0x5000, game = 0x140612340;
    // [the thunk, BFA's scene check, its caller in BFA, the game]
    const std::uintptr_t full[] = {hook, check, caller, game};
    CHECK_EQ(SiteOf(full, 4, kBfa), caller);
    // the check inlined into its caller: the first frame in BFA
    const std::uintptr_t inlined[] = {hook, check, game};
    CHECK_EQ(SiteOf(inlined, 3, kBfa), check);
    // the walk stopped at the check
    CHECK_EQ(SiteOf(full, 2, kBfa), check);
    // nothing in BFA, nothing captured
    const std::uintptr_t outside[] = {hook, game};
    CHECK_EQ(SiteOf(outside, 2, kBfa), std::uintptr_t{0});
    CHECK_EQ(SiteOf(full, 0, kBfa), std::uintptr_t{0});
    CHECK_EQ(SiteOf(nullptr, 4, kBfa), std::uintptr_t{0});
}

// Per-frame calls are never lines; a line's start is, once per actor, and the log is due at most once a minute.
TEST(SceneKeptLinesSkipPerFrameCalls) {
    using namespace std::chrono_literals;
    KeptLines kept;
    const auto t0 = KeptLines::Clock::time_point{} + 1000s;
    const std::uintptr_t start = 0x1000, provider = 0x2000, frame = 0x3000, morphs = 0x4000;
    const std::uint32_t lydia = 0x000A2C94, player = 0x14;

    // the first line: its provider check, its start, then its face's updates every frame
    auto due = kept.Kept(lydia, provider, t0);
    CHECK(due.log);
    CHECK_EQ(due.lines, 1u);
    CHECK(!kept.Kept(lydia, start, t0 + 5ms).log);  // the same line's start
    auto t = t0 + 20ms;
    for (int i = 0; i < 600; ++i, t += 16ms) {  // ten seconds of frames
        CHECK(!kept.Kept(lydia, frame, t).log);
        CHECK(!kept.Kept(lydia, morphs, t).log);
    }
    // the next line, while the frames go on: a line, counted, but the log waits for its minute
    due = kept.Kept(lydia, start, t);
    CHECK(!due.log);
    CHECK_EQ(due.lines, 1u);
    for (int i = 0; i < 10; ++i, t += 16ms) CHECK(!kept.Kept(lydia, frame, t).log);
    // another speaker's line
    due = kept.Kept(player, provider, t);
    CHECK(!due.log);
    CHECK_EQ(due.lines, 2u);
    // the frame sites are learned: a pause and frames again aren't a line, for anyone
    t += 30s;
    CHECK_EQ(kept.Kept(player, frame, t).lines, 0u);
    CHECK_EQ(kept.Kept(lydia, morphs, t).lines, 0u);
    // a minute after the first log line: the next line logs, counting the lines since
    t = t0 + 61s;
    due = kept.Kept(lydia, start, t);
    CHECK(due.log);
    CHECK_EQ(due.lines, 3u);
    // and the count starts again
    due = kept.Kept(player, start, t + 5s);
    CHECK(!due.log);
    CHECK_EQ(due.lines, 1u);
}

TEST(SceneKeptLinesOncePerLine) {
    using namespace std::chrono_literals;
    KeptLines kept;
    const auto t0 = KeptLines::Clock::time_point{} + 50s;
    // one line's checks within two seconds are one line
    CHECK(kept.Kept(7, 0x10, t0).log);
    CHECK_EQ(kept.Kept(7, 0x20, t0 + 1s).lines, 0u);
    // a line's start asked twice within a frame's gap is a per-frame site from then on
    CHECK_EQ(kept.Kept(7, 0x30, t0 + 3s).lines, 1u);
    CHECK_EQ(kept.Kept(7, 0x30, t0 + 3s + 10ms).lines, 0u);
    CHECK_EQ(kept.Kept(7, 0x30, t0 + 30s).lines, 0u);
    // lines seconds apart are each counted
    CHECK_EQ(kept.Kept(7, 0x10, t0 + 6s).lines, 2u);
    CHECK_EQ(kept.Kept(7, 0x10, t0 + 9s).lines, 3u);
    // many actors and sites: the tables stay bounded, and lines are still counted
    std::uint32_t lines = 3;
    for (std::uint32_t a = 100; a < 400; ++a) {
        const auto due = kept.Kept(a, 0x100 + a, t0 + 10s + std::chrono::milliseconds(a));
        CHECK_EQ(due.lines, ++lines);
    }
}

// OStim puts everyone in a scene in both of its factions, so BFA's one check asks twice, microseconds apart, from the
// same place: one line still counts once, and every line is counted, not only the first.
TEST(SceneKeptLinesActorInBothFactions) {
    using namespace std::chrono_literals;
    KeptLines kept;
    const auto t0 = KeptLines::Clock::time_point{} + 100s;
    const std::uintptr_t start = 0x1000, provider = 0x2000, frame = 0x3000, morphs = 0x4000;
    const std::uint32_t speaker = 0x000A2C94;
    int logs = 0;
    std::uint32_t loggedLines = 0;
    const auto check = [&](std::uintptr_t site, KeptLines::Clock::time_point t) {
        // OStimActorCountFaction, then OStimExcitementFaction
        for (const auto at : {t, t + 3us}) {
            const auto due = kept.Kept(speaker, site, at);
            if (due.log) {
                ++logs;
                loggedLines += due.lines;
            }
        }
    };
    // 20 lines, 30 s apart: each a provider check, its start, and two seconds of its face's frames
    for (int line = 0; line < 20; ++line) {
        const auto t = t0 + std::chrono::seconds(30 * line);
        check(provider, t);
        check(start, t + 4ms);
        auto f = t + 20ms;
        for (int i = 0; i < 120; ++i, f += 16ms) {
            check(frame, f);
            check(morphs, f);
        }
    }
    // a log line a minute: at 0 s, 60 s, ... 540 s; each after the first counts the two lines since the last
    CHECK_EQ(logs, 10);
    CHECK_EQ(loggedLines, 19u);  // every line but the last, still waiting for its minute
    // the line sites were never learned as per-frame: a line ten minutes on still logs, counting itself and the last
    const auto due = kept.Kept(speaker, start, t0 + 700s);
    CHECK(due.log);
    CHECK_EQ(due.lines, 2u);
}

TEST(SceneLogTexts) {
    const std::string installed = InstalledText({0, 0x2A000ECA, 0x2A000D93});
    CHECK(installed.find("OStim.esp 0xECA (OStimActorCountFaction) as 2A000ECA") != std::string::npos);
    CHECK(installed.find("OStim.esp 0xD93 (OStimExcitementFaction) as 2A000D93") != std::string::npos);
    CHECK(installed.find("not loaded: SexLab.esm 0xE50F (SexLabAnimatingFaction)") != std::string::npos);
    CHECK(installed.find("Character's and PlayerCharacter's") != std::string::npos);
    const std::string all = InstalledText({0x0500E50F, 0xFE123ECA, 0xFE123D93});
    CHECK(all.find("not loaded") == std::string::npos);
    CHECK(all.find("SexLab.esm 0xE50F (SexLabAnimatingFaction) as 0500E50F") != std::string::npos);
    CHECK_EQ(FactionName(3), std::string());

    const std::string one = KeptText(0x000A2C94, 1, 1);
    CHECK(one.find("000A2C94") != std::string::npos);
    CHECK(one.find("OStim.esp 0xECA") != std::string::npos);
    CHECK(one.find("lines kept since") == std::string::npos);
    CHECK(KeptText(0x14, 0, 4).find("4 lines kept since the last such log line") != std::string::npos);
}

TEST(SceneSettingIsOnByDefaultAndRead) {
    const Settings defaults = ReadSettings([](std::string_view, std::string_view def) { return std::string(def); });
    CHECK(defaults.sceneLipSync);
    const Settings off = ReadSettings([](std::string_view k, std::string_view def) {
        return k == "sceneLipSync" ? std::string("false") : std::string(def);
    });
    CHECK(!off.sceneLipSync);
    CHECK(Describe(defaults).find("lip sync in scenes on") != std::string::npos);
    CHECK(Describe(off).find("lip sync in scenes off") != std::string::npos);
}

// A loaded module's range from its headers: the image size, or 0 when the headers aren't a PE32+ image's.
TEST(SceneImageRangeFromTheHeaders) {
    std::vector<std::uint8_t> h(0x1000, 0);
    const auto u16 = [&](std::size_t at, std::uint16_t v) {
        h[at] = v & 0xFF;
        h[at + 1] = v >> 8;
    };
    const auto u32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) h[at + i] = (v >> (8 * i)) & 0xFF;
    };
    u16(0, 0x5A4D);
    u32(0x3C, 0x100);
    u32(0x100, 0x00004550);
    u16(0x100 + 24, 0x20B);
    u32(0x100 + 24 + 56, 0x01C9A000);
    CHECK_EQ(pe::ImageSize(h.data(), h.size()), std::size_t{0x01C9A000});
    CHECK_EQ(pe::ImageSize(nullptr, h.size()), std::size_t{0});
    CHECK_EQ(pe::ImageSize(h.data(), 0x100 + 24 + 56), std::size_t{0});  // the size beyond what is readable
    u32(0x100, 0x00004551);  // not the PE signature
    CHECK_EQ(pe::ImageSize(h.data(), h.size()), std::size_t{0});
    u32(0x100, 0x00004550);
    CHECK_EQ(pe::ImageSize(h.data(), h.size()), std::size_t{0x01C9A000});
    u16(0x100 + 24, 0x10B);  // PE32
    CHECK_EQ(pe::ImageSize(h.data(), h.size()), std::size_t{0});
    u16(0x100 + 24, 0x20B);
    u16(0, 0x4D5A);
    CHECK_EQ(pe::ImageSize(h.data(), h.size()), std::size_t{0});
    u16(0, 0x5A4D);
    u32(0x3C, 0xFFFFFFF0);  // e_lfanew beyond the page
    CHECK_EQ(pe::ImageSize(h.data(), h.size()), std::size_t{0});

    // BFA's own DLL (never committed): BFARB_BFA_DLL names it
    const char* path = std::getenv("BFARB_BFA_DLL");
    if (!path || !*path) SKIP("BFARB_BFA_DLL isn't set");
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> image;
    CHECK(pe::MapFile(file, image));
    CHECK_EQ(pe::ImageSize(image.data(), 0x1000), image.size());
    std::printf("        BFA's DLL: image size 0x%zx\n", image.size());
}
