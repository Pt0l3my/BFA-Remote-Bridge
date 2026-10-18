#pragma once
// Paths out of messages the bridge didn't write (ONNX Runtime's, the server's) before they reach the log or the
// player: a log may be shared, and a path names folders on someone's PC. Each path keeps its base name only.

#include <string>
#include <string_view>

namespace bfarb {

// Replaces every occurrence of `known` (a path the caller holds, such as the model's, in UTF-8) and of its folder,
// then every other path-like word (one holding a backslash, or starting with '/', "X:" or "~/"), by its base name.
std::string ScrubPaths(std::string text, std::string_view known = {});

}  // namespace bfarb
