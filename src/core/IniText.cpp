#include "core/IniText.h"

namespace bfarb {
namespace {

bool Blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool LineEnd(char c) { return c == '\r' || c == '\n'; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

std::string_view TrimEnd(std::string_view s) {
    while (!s.empty() && Blank(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace

std::optional<std::string> IniTextValue(std::string_view text, std::string_view section, std::string_view key) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    if (const std::size_t nul = text.find('\0'); nul != std::string_view::npos) text = text.substr(0, nul);

    std::optional<std::string> found;
    // the section the lines are in: "" before the first, none after a [ line without its ]
    std::optional<std::string_view> current = std::string_view();
    std::size_t p = 0;
    const std::size_t n = text.size();
    const auto toLineEnd = [&] {
        while (p < n && !LineEnd(text[p])) ++p;
    };
    while (p < n) {
        while (p < n && Blank(text[p])) ++p;  // blanks and empty lines
        if (p >= n) break;
        const char c = text[p];
        if (c == ';' || c == '#') {
            toLineEnd();
            continue;
        }
        if (c == '[') {
            ++p;
            while (p < n && Blank(text[p])) ++p;  // as SimpleIni: these blanks may run past the line's end
            const std::size_t start = p;
            while (p < n && text[p] != ']' && !LineEnd(text[p])) ++p;
            if (p >= n || text[p] != ']') {
                current.reset();
                continue;
            }
            current = TrimEnd(text.substr(start, p - start));
            toLineEnd();
            continue;
        }
        const std::size_t start = p;
        while (p < n && text[p] != '=' && !LineEnd(text[p])) ++p;
        if (p >= n || text[p] != '=') continue;  // no value: skipped
        if (p == start) {                          // no key: skipped
            toLineEnd();
            continue;
        }
        const std::string_view name = TrimEnd(text.substr(start, p - start));
        ++p;
        while (p < n && (text[p] == ' ' || text[p] == '\t')) ++p;
        const std::size_t valueStart = p;
        toLineEnd();
        const std::string_view value = TrimEnd(text.substr(valueStart, p - valueStart));
        if (current && SameName(*current, section) && SameName(name, key)) found = std::string(value);
    }
    return found;
}

}  // namespace bfarb
