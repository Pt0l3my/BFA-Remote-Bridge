#pragma once
// For tools/wincheck only: a parse-only check of Windows code on Linux (clang for MinGW). MinGW's libstdc++ lacks a
// few C names in std that MSVC's has, and the library's float limits need <cfloat>.
#include <cfloat>
#include <cmath>
#include <cstdlib>
namespace std { using ::logf; using ::expf; using ::powf; using ::sqrtf; using ::fabsf; using ::floorf; using ::ceilf; using ::roundf; using ::fmodf; }
