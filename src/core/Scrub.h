#pragma once
// Paths out of messages the bridge didn't write (ONNX Runtime's, the server's) before they reach the log or the
// player: a log may be shared, and a path names folders on someone's PC. Each path keeps its base name only.

#include <cstddef>
#include <string>
#include <string_view>

namespace bfarb {

// Replaces every occurrence of `known` (a path the caller holds, such as the model's, in UTF-8) and of its folder,
// then every other path-like word (one holding a backslash, or starting with '/', "X:" or "~/"), by its base name.
std::string ScrubPaths(std::string text, std::string_view known = {});

// A server's text (an ERROR message, a model's detail) on one line, for the log: control characters and line breaks
// become spaces; hexadecimal addresses (after "!" or "0x", labelled "caller:" with their brackets, or 16 digits
// long; a "..." or "\u2026" after them too) are dropped, while a shorter unmarked run such as an HRESULT stays; runs of
// spaces collapse; and paths keep their base names (ScrubPaths).
std::string ServerText(std::string text);

// The same for the screen (a corner note, which SkyrimNet sees as an event), capped at about `max` characters: cut at
// a space near the end when there is one, never inside a UTF-8 character, and ended with "..." (plain ASCII, which
// Skyrim's fonts draw).
constexpr std::size_t kShownTextMax = 160;
std::string ShownServerText(const std::string& text, std::size_t max = kShownTextMax);

}  // namespace bfarb
