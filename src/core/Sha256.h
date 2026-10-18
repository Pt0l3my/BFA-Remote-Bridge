#pragma once
// SHA-256 (FIPS 180-4), the bridge's own small portable implementation, and a file's size and digest in one read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace bfarb {

class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() noexcept;
    void Update(const void* data, std::size_t n) noexcept;
    Digest Final() noexcept;

    static Digest Of(const void* data, std::size_t n) noexcept;
    static std::string Hex(const Digest& d);

private:
    void Block(const std::uint8_t* p) noexcept;
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

// The file's size and SHA-256, read once from start to end. False, with err, when it can't be read.
bool HashFile(const std::filesystem::path& path, std::uint64_t& size, Sha256::Digest& digest, std::string& err);

}  // namespace bfarb
