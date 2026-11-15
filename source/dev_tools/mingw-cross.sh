#!/bin/bash
# mingw-cross.sh: builds the Windows client and server with MinGW-w64 on Linux or macOS, as a check that the code still
# builds for Windows (the release builds use the Visual Studio project in source/vcpp; this is the same source with
# GCC, close to the Code::Blocks project). It uses the headers and libraries the repository has for Windows
# (source/include, source/lib, bin_win32), ENet from source, and SDL3's official MinGW package (pinned, checked).
#
#   source/dev_tools/mingw-cross.sh [output directory]
#
# 32-bit, like the Visual Studio project (the libraries in source/lib and bin_win32 are 32-bit). Needs
# i686-w64-mingw32-g++: Debian/Ubuntu g++-mingw-w64-i686, Fedora mingw32-gcc-c++, Homebrew mingw-w64. Output:
# <output directory>/ac_client.exe and ac_server.exe (default build-mingw/). Running them needs Windows (or Wine)
# and the DLLs in bin_win32.
set -eu
ARCH=i686
OUT=${1:-build-mingw}
SDLVER=3.4.20
SDLSHA=d2d3bf3fb97a90b22e908a306d7362631ef1bab229d988fe6669595dc29cd9ed # SDL3-devel-3.4.20-mingw.tar.gz
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CXX=$ARCH-w64-mingw32-g++
CC=$ARCH-w64-mingw32-gcc
command -v "$CXX" > /dev/null || { echo "mingw-cross: $CXX not found" >&2; exit 2; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# SDL3 (headers and import library for MinGW)
SDLPKG="$OUT/SDL3-devel-$SDLVER-mingw.tar.gz"
if [ ! -f "$SDLPKG" ]; then
  curl -fsSL --retry 3 -o "$SDLPKG.part" "https://github.com/libsdl-org/SDL/releases/download/release-$SDLVER/SDL3-devel-$SDLVER-mingw.tar.gz"
  mv "$SDLPKG.part" "$SDLPKG"
fi
if command -v sha256sum > /dev/null; then sum=$(sha256sum "$SDLPKG" | cut -d' ' -f1); else sum=$(shasum -a 256 "$SDLPKG" | cut -d' ' -f1); fi
[ "$sum" = "$SDLSHA" ] || { echo "mingw-cross: $SDLPKG has the wrong checksum" >&2; rm -f "$SDLPKG"; exit 2; }
[ -d "$OUT/SDL3-$SDLVER" ] || tar -xzf "$SDLPKG" -C "$OUT"
SDL="$OUT/SDL3-$SDLVER/$ARCH-w64-mingw32"

FLAGS="-O2 -fsigned-char -DWIN32 -D_WINDOWS -I$ROOT/source/enet/include"
CXXFLAGS="$FLAGS -std=gnu++17 -I$ROOT/source/src -I$ROOT/source/src/bot -I$SDL/include -idirafter $ROOT/source/include"
JOBS=$( (nproc || sysctl -n hw.ncpu) 2> /dev/null || echo 4)

compile() # <object dir> <extra flags> <files...>: a fresh build of each file, in parallel (through a generated makefile)
{
  local dir=$1 extra=$2 mk objs="" f o
  shift 2
  rm -rf "$dir"; mkdir -p "$dir"
  mk="$dir/build.mk"
  for f in "$@"; do
    o="$dir/$(basename "$f").o"
    objs="$objs $o"
    case "$f" in
      *.c) printf '%s: %s\n\t%s -c $< -o $@\n' "$o" "$f" "$CC $FLAGS $extra" ;;
      *)   printf '%s: %s\n\t%s -c $< -o $@\n' "$o" "$f" "$CXX $CXXFLAGS $extra" ;;
    esac
  done > "$mk.rules"
  { printf 'all:%s\n' "$objs"; cat "$mk.rules"; } > "$mk"
  make -s -j"$JOBS" -f "$mk" all
}

ENET=$(for c in callbacks compress host list packet peer protocol win32; do echo "$ROOT/source/enet/$c.c"; done)
CLIENT=$(ls "$ROOT"/source/src/*.cpp "$ROOT"/source/src/bot/*.cpp | grep -v -E '/(master|pch)\.cpp$')
SERVER=$(for f in crypto log protocol serverms server stream tools wizard; do echo "$ROOT/source/src/$f.cpp"; done)

echo "mingw-cross: client ($ARCH)"
# shellcheck disable=SC2086 # (lists of files)
compile "$OUT/obj-client" "" $ENET $CLIENT
"$CXX" -mwindows -o "$OUT/ac_client.exe" "$OUT"/obj-client/*.o -L"$SDL/lib" -lSDL3 -lopengl32 -lws2_32 -lwinmm -ldbghelp \
  "$ROOT/bin_win32/libvorbisfile.dll" "$ROOT/bin_win32/zlib1.dll" "$ROOT/source/lib/OpenAL32.lib" -static-libgcc -static-libstdc++
echo "mingw-cross: server ($ARCH)"
# shellcheck disable=SC2086
compile "$OUT/obj-server" "-DSTANDALONE" $ENET $SERVER
"$CXX" -o "$OUT/ac_server.exe" "$OUT"/obj-server/*.o -L"$SDL/lib" -lSDL3 -lws2_32 -lwinmm -ldbghelp "$ROOT/bin_win32/zlib1.dll" \
  -static-libgcc -static-libstdc++
echo "mingw-cross: built $OUT/ac_client.exe and $OUT/ac_server.exe"
