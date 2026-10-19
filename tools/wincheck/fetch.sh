#!/bin/sh
# Fetches the headers tools/wincheck/wincheck.sh parses against, at pinned commits, into third_party/wincheck/
# (git-ignored, never committed): CommonLibSSE-NG's MIT line (marthofdoom's 3.7.0 continued, at the commit
# vcpkg-configuration.json's registry port builds) and the headers it includes: spdlog, fmt, rapidcsv,
# DirectXMath and DirectXTK.
#
#   sh tools/wincheck/fetch.sh [folder]
set -eu

DEST=${1:-third_party/wincheck}
mkdir -p "$DEST"
fetch() {   # name url commit
  if [ -d "$DEST/$1/.git" ] && [ "$(git -C "$DEST/$1" rev-parse HEAD)" = "$3" ]; then return; fi
  rm -rf "$DEST/$1"
  git init --quiet "$DEST/$1"
  git -C "$DEST/$1" fetch --quiet --depth 1 "$2" "$3"
  git -C "$DEST/$1" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
  echo "$1 ($3) in $DEST/$1"
}
fetch commonlibsse-ng https://github.com/marthofdoom/CommonLibSSE-NG.git 57be9d67086da3857da1d0171e77966674713aa5
fetch spdlog https://github.com/gabime/spdlog.git 8e5613379f5140fefb0b60412fbf1f5406e7c7f8
fetch fmt https://github.com/fmtlib/fmt.git e69e5f977d458f2650bb346dadf2ad30c5320281
fetch rapidcsv https://github.com/d99kris/rapidcsv.git 8b70667db5d1472fd834f1b0261ee34050c292bf
fetch dxmath https://github.com/microsoft/DirectXMath.git 5bbe2ceeec3236c27ad44c877715e1e0f3b9817a
fetch dxtk https://github.com/microsoft/DirectXTK.git 524c655ee0b69c33f6dfec81878596b131f79671
