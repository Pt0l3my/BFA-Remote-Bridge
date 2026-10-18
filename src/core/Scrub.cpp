#include "core/Scrub.h"

#include <cctype>

namespace bfarb {
namespace {

bool Separator(char c) { return c == '/' || c == '\\'; }

std::string_view BaseName(std::string_view path) {
    while (!path.empty() && Separator(path.back())) path.remove_suffix(1);
    const auto at = path.find_last_of("/\\");
    return at == std::string_view::npos ? path : path.substr(at + 1);
}

void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

// A word that reads as a path: a backslash anywhere, or a start of '/', "X:" or "~/".
bool PathLike(std::string_view w) {
    if (w.find('\\') != std::string_view::npos) return true;
    if (!w.empty() && w.front() == '/') return true;
    if (w.size() >= 3 && std::isalpha(static_cast<unsigned char>(w[0])) && w[1] == ':' && Separator(w[2])) return true;
    return w.size() >= 2 && w[0] == '~' && w[1] == '/';
}

bool WordBreak(char c) {
    return std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' || c == '(' || c == ')' || c == '[' || c == ']' ||
           c == ',' || c == ';' || c == '<' || c == '>' || c == '`';
}

}  // namespace

std::string ScrubPaths(std::string text, std::string_view known) {
    if (!known.empty()) {
        std::string k(known);
        ReplaceAll(text, k, BaseName(k));
        // the same path with the other separator, and its folder
        std::string flipped = k;
        for (char& c : flipped) c = c == '\\' ? '/' : c == '/' ? '\\' : c;
        ReplaceAll(text, flipped, BaseName(flipped));
        const auto at = k.find_last_of("/\\");
        if (at != std::string::npos && at > 0) {
            const std::string folder = k.substr(0, at + 1);
            ReplaceAll(text, folder, "");
            ReplaceAll(text, k.substr(0, at), BaseName(k.substr(0, at)));
        }
    }
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (WordBreak(text[i])) {
            out.push_back(text[i++]);
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && !WordBreak(text[j])) ++j;
        std::string_view word(text.data() + i, j - i);
        // a trailing ':' or '.' ends a sentence, not the path
        std::string_view tail;
        while (!word.empty() && (word.back() == ':' || word.back() == '.') && PathLike(word.substr(0, word.size() - 1))) {
            tail = std::string_view(word.data() + word.size() - 1, tail.size() + 1);
            word.remove_suffix(1);
        }
        if (PathLike(word)) {
            out.append(BaseName(word));
            out.append(tail);
        } else {
            out.append(text, i, j - i);
        }
        i = j;
    }
    return out;
}

}  // namespace bfarb
