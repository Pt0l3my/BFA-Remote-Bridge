#pragma once
// BFA 5's files read as BFA 5 reads them. BFA 5.0.6 loads its INI and its installer's choice files with SimpleIni (an
// MIT-licensed library), where Windows' profile calls, which BFA 4.3.2 used, read some files otherwise. This is the
// bridge's own code for the rules that matter to the keys it reads ([General] ForceCPU, GPU, GpuAdapter, EmotionDevice
// and Batch), as BFA 5.0.6's SimpleIni applies them with BFA's settings (its multi-line values off, quotes kept):
//
//   the file      a UTF-8 byte-order mark at its very start is skipped (BFA 5's own saves write one); the text ends at
//                 its first NUL byte
//   lines         end at a line feed or a carriage return; blanks (spaces and tabs) around a section's name, a key and
//                 a value are dropped
//   comments      a line whose first character after its blanks is ; or #; text after a value is part of the value
//   sections      [name]: the name runs to the first ]; anything after the ] is ignored. A [ with no ] on its line
//                 puts the keys after it in a section of no name the bridge reads, until the next section
//   keys          key=value: the key is what comes before the first =, the value the rest of the line. A line with no
//                 =, or with nothing before it, is skipped
//   matching      sections and keys match whatever their ASCII letters' case; a section named twice is one section,
//                 and a key given twice keeps its last value, also across the two
//   quotes        part of the value: "0" is not the number 0
//
// Numbers (core/BfaMode.h, Bfa5Number) are read as BFA's GetLongValue reads them, on Windows, where a long has 32 bits.

#include <optional>
#include <string>
#include <string_view>

namespace bfarb {

// [section] key in an INI file's text, as BFA 5 reads it; empty when the key isn't there.
std::optional<std::string> IniTextValue(std::string_view text, std::string_view section, std::string_view key);

}  // namespace bfarb
