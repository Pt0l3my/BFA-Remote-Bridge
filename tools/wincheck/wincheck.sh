#!/bin/sh
# A parse-only check of the plugin's Windows files on Linux: clang for MinGW parses each file against the headers of
# CommonLibSSE-NG's MIT line and reports the errors in the repository's own files. It finds API mistakes (a member
# the library doesn't have, a wrong signature), not MSVC's warnings or the layout checks (static_assert is switched
# off, since MSVC's layouts can't hold under MinGW's), and it links nothing: the Windows build in CI decides. Needs
# clang 18 and MinGW's headers (Ubuntu: clang-18, g++-mingw-w64-x86-64), and tools/wincheck/fetch.sh run first.
#
#   sh tools/wincheck/wincheck.sh                 every file in src/plugin
#   sh tools/wincheck/wincheck.sh FILE...         those files
#   WINCHECK_HEADERS=<folder> overrides third_party/wincheck
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
H=${WINCHECK_HEADERS:-$R/third_party/wincheck}
[ -d "$H/commonlibsse-ng/include" ] || { echo "Run tools/wincheck/fetch.sh first" >&2; exit 2; }
[ $# -gt 0 ] || set -- "$R"/src/plugin/*.cpp
rc=0
for f in "$@"; do
  out=$(clang++-18 --target=x86_64-w64-windows-gnu -std=c++23 -fsyntax-only -fms-extensions -Wno-everything \
    '-Dstatic_assert(...)=static_assert(true, "")' -DSPDLOG_COMPILED_LIB -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE \
    -DSPDLOG_FMT_EXTERNAL -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
    -DENABLE_SKYRIM_SE=1 -DENABLE_SKYRIM_AE=1 -DENABLE_SKYRIM_VR=1 -DHAS_SKYRIM_MULTI_TARGETING=1 \
    -DBFARB_VERSION='"0.0.0"' \
    -include "$R/tools/wincheck/shim.h" -include "$R/src/plugin/PCH.h" \
    -isystem "$H/commonlibsse-ng/include" -isystem "$H/spdlog/include" -isystem "$H/fmt/include" -isystem "$H/rapidcsv/src" \
    -isystem "$H/dxmath/Inc" -isystem "$H/dxtk/Inc" -isystem "$R/third_party/onnxruntime" \
    -I"$R/src" \
    -ferror-limit=0 "$(realpath "$f")" 2>&1 | grep -E "^$R/[^:]+:[0-9]+:[0-9]+: (fatal )?error" | grep -v "/third_party/")
  n=$(printf '%s' "$out" | grep -c .)
  echo "$f: $n error(s)"
  if [ -n "$out" ]; then printf '%s\n' "$out"; rc=1; fi
done
exit $rc
