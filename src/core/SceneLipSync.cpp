#include "core/SceneLipSync.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace bfarb::scene {

bool AnswersFalse(bool enabled, const void* faction, const Factions& factions, std::uintptr_t caller, Range bfa) noexcept {
    if (!enabled || !faction || !bfa.Contains(caller)) return false;
    for (const void* f : factions)
        if (f && f == faction) return true;
    return false;
}

std::uint32_t FormIdIn(bool light, std::uint8_t compileIndex, std::uint16_t smallIndex, std::uint32_t localId) noexcept {
    if (localId == 0) return 0;
    if (light) {
        if (localId > 0xFFF || smallIndex > 0xFFF) return 0;
        return 0xFE000000u | (static_cast<std::uint32_t>(smallIndex) << 12) | localId;
    }
    if (localId > 0xFFFFFF || compileIndex >= 0xFE) return 0;
    return (static_cast<std::uint32_t>(compileIndex) << 24) | localId;
}

bool SamePlugin(std::string_view fileName, std::string_view plugin) noexcept {
    if (fileName.size() != plugin.size() || plugin.empty()) return false;
    for (std::size_t i = 0; i < plugin.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(fileName[i])) != std::tolower(static_cast<unsigned char>(plugin[i]))) return false;
    return true;
}

bool SlotLooksRight(std::uintptr_t slot, std::uintptr_t entry, Range gameImage, Range gameCode) noexcept {
    if (slot % sizeof(void*) != 0) return false;
    if (!gameImage.Contains(slot) || slot > gameImage.end - sizeof(void*)) return false;
    return gameCode.Contains(entry);
}

std::uintptr_t SiteOf(const std::uintptr_t* frames, std::size_t count, Range bfa) noexcept {
    if (!frames) return 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!bfa.Contains(frames[i])) continue;
        if (i + 1 < count && bfa.Contains(frames[i + 1])) return frames[i + 1];
        return frames[i];
    }
    return 0;
}

KeptLines::Due KeptLines::Kept(std::uint32_t actor, std::uintptr_t site, Clock::time_point now) {
    if (std::find(frameSites_.begin(), frameSites_.end(), site) != frameSites_.end()) return {};
    const auto seen = std::find_if(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.actor == actor && s.site == site; });
    if (seen != seen_.end()) {
        // the same check asking about the actor's next scene faction: its first ask counted already
        if (now >= seen->at && now - seen->at < kSameCheck) return {};
        const bool again = now - seen->at < kFrameGap;
        seen->at = now;
        if (again) {
            // asked every frame: never a line again (when the list is full, the entry above keeps it quiet instead)
            if (frameSites_.size() < kMaxFrameSites) {
                frameSites_.push_back(site);
                seen_.erase(std::remove_if(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.site == site; }), seen_.end());
            }
            return {};
        }
    } else {
        if (seen_.size() >= kMaxSeen)
            seen_.erase(std::min_element(seen_.begin(), seen_.end(), [](const Seen& a, const Seen& b) { return a.at < b.at; }));
        seen_.push_back({actor, site, now});
    }
    const auto line = std::find_if(lines_.begin(), lines_.end(), [&](const Line& l) { return l.actor == actor; });
    if (line != lines_.end()) {
        if (now - line->at < kSameLine) return {};
        line->at = now;
    } else {
        if (lines_.size() >= kMaxLines)
            lines_.erase(std::min_element(lines_.begin(), lines_.end(), [](const Line& a, const Line& b) { return a.at < b.at; }));
        lines_.push_back({actor, now});
    }
    ++since_;
    if (logged_ && now - lastLog_ < kEvery) return {false, since_};
    logged_ = true;
    lastLog_ = now;
    const Due due{true, since_};
    since_ = 0;
    return due;
}

std::string FactionName(std::size_t index) {
    if (index >= kSceneFactions.size()) return {};
    const SceneFaction& f = kSceneFactions[index];
    char id[16];
    std::snprintf(id, sizeof id, "0x%X", static_cast<unsigned>(f.localId));
    return std::string(f.plugin) + " " + id + " (" + f.editorId + ")";
}

std::string InstalledText(const std::array<std::uint32_t, kSceneFactions.size()>& formIds) {
    std::string found, missing;
    for (std::size_t i = 0; i < formIds.size(); ++i) {
        std::string& list = formIds[i] ? found : missing;
        if (!list.empty()) list += ", ";
        list += FactionName(i);
        if (formIds[i]) {
            char id[16];
            std::snprintf(id, sizeof id, "%08X", static_cast<unsigned>(formIds[i]));
            list += std::string(" as ") + id;
        }
    }
    std::string line =
        "Lip sync in scenes: hooked Actor::IsInFaction on Character's and PlayerCharacter's vtables; BFA's own scene "
        "checks are answered \"not in a scene\" while the setting is on, every other caller gets the game's answer. Scene "
        "factions found: " +
        (found.empty() ? std::string("none") : found);
    if (!missing.empty()) line += "; not loaded: " + missing;
    return line;
}

std::string KeptText(std::uint32_t actorFormId, std::size_t factionIndex, std::uint32_t lines) {
    char id[16];
    std::snprintf(id, sizeof id, "%08X", static_cast<unsigned>(actorFormId));
    std::string line = std::string("Lip sync in scenes: kept BFA's lip sync for a line of ") + id + ", who is in a scene (" +
                       FactionName(factionIndex) + ")";
    if (lines > 1) line += "; " + std::to_string(lines) + " lines kept since the last such log line, this one included";
    return line;
}

}  // namespace bfarb::scene
