#pragma once
// IEEE 754 half precision to single, for check mode's differences of float16 outputs.

#include <cmath>
#include <cstdint>

namespace bfarb {

inline float HalfToFloat(std::uint16_t h) noexcept {
    const int sign = (h >> 15) & 1;
    const int exponent = (h >> 10) & 0x1F;
    const int mantissa = h & 0x3FF;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? NAN : INFINITY;
    else
        v = std::ldexp(static_cast<float>(mantissa | 0x400), exponent - 25);
    return sign ? -v : v;
}

}  // namespace bfarb
