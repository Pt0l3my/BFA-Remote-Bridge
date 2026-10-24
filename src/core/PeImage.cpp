#include "core/PeImage.h"

#include <cctype>
#include <algorithm>
#include <cstring>

namespace bfarb::pe {
namespace {

class View {
public:
    View(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}

    bool In(std::size_t at, std::size_t len) const { return at <= n_ && len <= n_ - at; }
    bool U16(std::size_t at, std::uint16_t& v) const {
        if (!In(at, 2)) return false;
        v = static_cast<std::uint16_t>(p_[at] | (p_[at + 1] << 8));
        return true;
    }
    bool U32(std::size_t at, std::uint32_t& v) const {
        if (!In(at, 4)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p_[at + i]) << (8 * i);
        return true;
    }
    bool U64(std::size_t at, std::uint64_t& v) const {
        if (!In(at, 8)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p_[at + i]) << (8 * i);
        return true;
    }
    // A NUL-terminated string at `at`, at most 512 bytes.
    bool Cstr(std::size_t at, std::string_view& s) const {
        if (!In(at, 1)) return false;
        const std::size_t limit = std::min<std::size_t>(512, n_ - at);
        const void* end = std::memchr(p_ + at, 0, limit);
        if (!end) return false;
        s = std::string_view(reinterpret_cast<const char*>(p_ + at), static_cast<const std::uint8_t*>(end) - (p_ + at));
        return true;
    }

private:
    const std::uint8_t* p_;
    std::size_t n_;
};

constexpr std::uint16_t kPe32Plus = 0x20B;

// The optional header's offset and its directory `index` (RVA and size).
bool Directory(const View& v, int index, std::uint32_t& rva, std::uint32_t& size) {
    std::uint32_t lfanew = 0, sig = 0, count = 0;
    std::uint16_t magic = 0;
    if (!v.U32(0x3C, lfanew) || !v.U32(lfanew, sig) || sig != 0x00004550) return false;
    const std::size_t opt = static_cast<std::size_t>(lfanew) + 24;
    if (!v.U16(opt, magic) || magic != kPe32Plus || !v.U32(opt + 108, count) || static_cast<std::uint32_t>(index) >= count) return false;
    return v.U32(opt + 112 + 8 * static_cast<std::size_t>(index), rva) && v.U32(opt + 116 + 8 * static_cast<std::size_t>(index), size);
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool EndsWithNoCase(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && EqualsNoCase(s.substr(s.size() - tail.size()), tail);
}

bool SameDll(std::string_view a, std::string_view b) {
    const auto strip = [](std::string_view s) { return EndsWithNoCase(s, ".dll") ? s.substr(0, s.size() - 4) : s; };
    return EqualsNoCase(strip(a), strip(b));
}

}  // namespace

bool FindImportSlot(const std::uint8_t* image, std::size_t size, std::string_view dll, std::string_view function, std::size_t& slotRva) {
    const View v(image, size);
    std::uint32_t dirRva = 0, dirSize = 0;
    if (!Directory(v, 1, dirRva, dirSize) || dirRva == 0) return false;
    for (std::size_t d = dirRva;; d += 20) {
        std::uint32_t lookup = 0, nameRva = 0, iat = 0;
        if (!v.U32(d, lookup) || !v.U32(d + 12, nameRva) || !v.U32(d + 16, iat)) return false;
        if (lookup == 0 && nameRva == 0 && iat == 0) return false;  // the terminator
        std::string_view name;
        if (!v.Cstr(nameRva, name) || !SameDll(name, dll)) continue;
        if (lookup == 0) continue;  // no names to read: the slot can't be told by its name
        for (std::size_t i = 0;; ++i) {
            std::uint64_t thunk = 0;
            if (!v.U64(lookup + 8 * i, thunk)) return false;
            if (thunk == 0) break;
            if (thunk & (1ull << 63)) continue;  // by ordinal
            std::string_view fn;
            if (v.Cstr(static_cast<std::size_t>(thunk & 0x7FFFFFFF) + 2, fn) && fn == function) {
                slotRva = iat + 8 * i;
                return v.In(slotRva, 8);
            }
        }
    }
}

bool FindExport(const std::uint8_t* image, std::size_t size, std::string_view name, std::uint32_t& rva) {
    const View v(image, size);
    std::uint32_t dirRva = 0, dirSize = 0;
    if (!Directory(v, 0, dirRva, dirSize) || dirRva == 0) return false;
    std::uint32_t functions = 0, names = 0, addresses = 0, nameTable = 0, ordinals = 0;
    if (!v.U32(dirRva + 20, functions) || !v.U32(dirRva + 24, names) || !v.U32(dirRva + 28, addresses) || !v.U32(dirRva + 32, nameTable) ||
        !v.U32(dirRva + 36, ordinals))
        return false;
    for (std::uint32_t i = 0; i < names; ++i) {
        std::uint32_t nameRva = 0;
        std::uint16_t ordinal = 0;
        std::string_view s;
        if (!v.U32(nameTable + 4ull * i, nameRva) || !v.Cstr(nameRva, s)) return false;
        if (s != name) continue;
        if (!v.U16(ordinals + 2ull * i, ordinal) || ordinal >= functions || !v.U32(addresses + 4ull * ordinal, rva)) return false;
        if (rva >= dirRva && rva < dirRva + dirSize) return false;  // forwarded
        return rva != 0;
    }
    return false;
}

std::size_t ImageSize(const std::uint8_t* image, std::size_t readable) {
    if (!image) return 0;
    const View v(image, readable);
    std::uint16_t mz = 0, magic = 0;
    std::uint32_t lfanew = 0, sig = 0, sizeOfImage = 0;
    if (!v.U16(0, mz) || mz != 0x5A4D || !v.U32(0x3C, lfanew) || !v.U32(lfanew, sig) || sig != 0x00004550) return 0;
    const std::size_t opt = static_cast<std::size_t>(lfanew) + 24;
    if (!v.U16(opt, magic) || magic != kPe32Plus || !v.U32(opt + 56, sizeOfImage)) return 0;
    return sizeOfImage;
}

bool MapFile(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& image) {
    const View v(file.data(), file.size());
    std::uint32_t lfanew = 0, sig = 0, sizeOfImage = 0, sizeOfHeaders = 0;
    std::uint16_t sections = 0, optSize = 0, magic = 0;
    if (!v.U32(0x3C, lfanew) || !v.U32(lfanew, sig) || sig != 0x00004550) return false;
    if (!v.U16(lfanew + 6, sections) || !v.U16(lfanew + 20, optSize)) return false;
    const std::size_t opt = static_cast<std::size_t>(lfanew) + 24;
    if (!v.U16(opt, magic) || magic != kPe32Plus || !v.U32(opt + 56, sizeOfImage) || !v.U32(opt + 60, sizeOfHeaders)) return false;
    if (sizeOfImage > (1u << 30) || sizeOfHeaders > file.size() || sizeOfHeaders > sizeOfImage) return false;
    image.assign(sizeOfImage, 0);
    std::memcpy(image.data(), file.data(), sizeOfHeaders);
    const std::size_t table = opt + optSize;
    for (std::uint16_t i = 0; i < sections; ++i) {
        const std::size_t h = table + 40ull * i;
        std::uint32_t vsize = 0, vaddr = 0, rawSize = 0, rawAt = 0;
        if (!v.U32(h + 8, vsize) || !v.U32(h + 12, vaddr) || !v.U32(h + 16, rawSize) || !v.U32(h + 20, rawAt)) return false;
        const std::size_t n = std::min<std::size_t>(rawSize, vsize ? vsize : rawSize);
        if (n == 0) continue;
        if (!v.In(rawAt, n) || vaddr > image.size() || n > image.size() - vaddr) return false;
        std::memcpy(image.data() + vaddr, file.data() + rawAt, n);
    }
    return true;
}

std::string UnpackVersion(std::uint32_t packed) {
    const std::uint32_t major = packed >> 24, minor = (packed >> 16) & 0xFF, patch = (packed >> 4) & 0xFFF, build = packed & 0xF;
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (build) s += "." + std::to_string(build);
    return s;
}

bool IsOrtGetApiBaseLookup(const char* nameOrOrdinal, std::string_view moduleFileName) noexcept {
    if (reinterpret_cast<std::uintptr_t>(nameOrOrdinal) <= 0xFFFF) return false;  // an ordinal (or null)
    if (std::strcmp(nameOrOrdinal, "OrtGetApiBase") != 0) return false;
    constexpr std::string_view kFile = "onnxruntime.dll";
    if (!EndsWithNoCase(moduleFileName, kFile)) return false;
    if (moduleFileName.size() == kFile.size()) return true;
    const char before = moduleFileName[moduleFileName.size() - kFile.size() - 1];
    return before == '\\' || before == '/';
}

}  // namespace bfarb::pe
