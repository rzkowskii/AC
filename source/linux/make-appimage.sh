#!/bin/bash
# Builds a self-contained AssaultCube AppImage for Linux, for the machine it runs on (x86_64 or aarch64):
#   source/linux/build/AssaultCube-<version>-<arch>.AppImage
#
# The AppImage contains the game, the standalone server, the game data, and the libraries a distribution may lack
# (SDL3, libogg and libvorbis linked statically, OpenAL Soft as a shared library: see build-deps.sh). The C/C++ runtime,
# libGL/libvulkan, X11/Wayland and zlib come from the system, as usual for AppImages. It runs on distributions with the
# same or a newer glibc than the build machine's (the script prints the version it needs).
#
#   ./AssaultCube-<version>-<arch>.AppImage [game options]      the game; user data in ~/.assaultcube/v1.4
#   ./AssaultCube-<version>-<arch>.AppImage server [options]    the standalone server (see source/linux/AppRun)
#
# Usage: source/linux/make-appimage.sh
# Needs: the build-deps.sh requirements, plus rsync, zip and file. appimagetool and the AppImage runtime are downloaded
#        (pinned releases).
# Env:   CC, CXX     compilers (default: clang, clang++)
#        OUT         output directory (default: source/linux/build)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SRC/.." && pwd)"
OUT="${OUT:-$HERE/build}"
ARCH="$(uname -m)"
APPDIR="$OUT/AssaultCube-$ARCH.AppDir"
APPID=net.cubers.assault.AssaultCube # the FreeDesktop.org ID from source/freedesktop/
export CC="${CC:-clang}" CXX="${CXX:-clang++}"
JOBS="$(nproc)"

step() { echo "make-appimage: $*"; }

# appimagetool and the AppImage runtime (the small program at the start of every AppImage, which mounts or extracts it):
# official releases from github.com/AppImage, pinned by version and SHA-256 (the digests GitHub publishes for them)
APPIMAGETOOL_VERSION=1.9.1
RUNTIME_VERSION=20251108
case "$ARCH" in
  x86_64)
    APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
    RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d ;;
  aarch64)
    APPIMAGETOOL_SHA=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
    RUNTIME_SHA=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 ;;
  *) echo "make-appimage: unsupported architecture $ARCH (x86_64 and aarch64 are supported)" >&2; exit 1 ;;
esac

# version from cube.h, e.g. AC_VERSION 1400 -> 1.4.0.0
ACV="$(sed -n 's/^[[:space:]]*#define AC_VERSION \([0-9][0-9]*\).*/\1/p' "$SRC/src/cube.h" | head -1)"
VERSION="$((ACV / 1000)).$((ACV / 100 % 10)).$((ACV / 10 % 10)).$((ACV % 10))"
APPIMAGE="$OUT/AssaultCube-$VERSION-$ARCH.AppImage"

for tool in rsync zip file readelf strip; do
  command -v "$tool" >/dev/null || { echo "make-appimage: '$tool' is required" >&2; exit 1; }
done

# 1. third-party libraries
"$HERE/build-deps.sh"
DEPS="$HERE/deps/$ARCH"

# 2. compile the client and the server with CMake, against the libraries from step 1 (static SDL3 and Vorbis, so a
#    system copy can't be picked up) and the system's legacy libGL.so.1 (present with every GL driver; libOpenGL.so.0
#    isn't installed everywhere). The client finds the bundled OpenAL Soft in ../lib.
OBJ="$OUT/obj-$ARCH"
LOG="$OUT/build-$ARCH.log"
step "compiling ($ARCH, $CC)"
mkdir -p "$OUT"
GENERATOR="Unix Makefiles"
command -v ninja >/dev/null && GENERATOR=Ninja
buildfailed() {
  grep -E 'error|Error' "$LOG" | head -20
  echo "make-appimage: build failed, see $LOG" >&2
  exit 1
}
# shellcheck disable=SC2016 # ($ORIGIN is for the dynamic linker)
cmake -S "$SRC" -B "$OBJ" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release -DAC_MASTER=OFF \
  -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON -DOpenGL_GL_PREFERENCE=LEGACY \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib' > "$LOG" 2>&1 || buildfailed
cmake --build "$OBJ" -j "$JOBS" >> "$LOG" 2>&1 || buildfailed

# 3. assemble the AppDir
step "assembling $APPDIR ($VERSION)"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/metainfo" \
  "$APPDIR/usr/share/icons/hicolor/256x256/apps"
install -m 755 "$OBJ/ac_client" "$APPDIR/usr/bin/assaultcube"
install -m 755 "$OBJ/ac_server" "$APPDIR/usr/bin/ac_server"
# OpenAL Soft (LGPL) is the only bundled shared library
install -m 644 "$(readlink -f "$DEPS/lib/libopenal.so.1")" "$APPDIR/usr/lib/libopenal.so.1"
strip --strip-unneeded "$APPDIR/usr/bin/assaultcube" "$APPDIR/usr/bin/ac_server" "$APPDIR/usr/lib/libopenal.so.1"
install -m 755 "$HERE/AppRun" "$APPDIR/AppRun"

# desktop entry, icon and AppStream metadata from source/freedesktop/; the AppImage starts with the player's own display
# settings, so the desktop entry doesn't pass -t (windowed)
FD="$SRC/freedesktop"
sed 's/^Exec=.*/Exec=assaultcube/' "$FD/$APPID.desktop" > "$APPDIR/usr/share/applications/$APPID.desktop"
cp "$FD/$APPID.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APPID.png"
cp "$FD/$APPID.appdata.xml" "$APPDIR/usr/share/metainfo/$APPID.appdata.xml"
ln -s "usr/share/applications/$APPID.desktop" "$APPDIR/$APPID.desktop"
ln -s "usr/share/icons/hicolor/256x256/apps/$APPID.png" "$APPDIR/$APPID.png"
ln -s "$APPID.png" "$APPDIR/.DirIcon"

# game data: only files under version control when building from a git checkout (no local saves, logs, test maps)
RES="$APPDIR/usr/share/assaultcube"
mkdir -p "$RES"
DATA=(bot config demos docs mods packages README.html README.md)
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C "$ROOT" ls-files -z -- "${DATA[@]}" | rsync -a --from0 --files-from=- "$ROOT/" "$RES/"
else
  for d in "${DATA[@]}"; do [ -e "$ROOT/$d" ] && rsync -a "$ROOT/$d" "$RES/"; done
fi
# same clean-up as the official packaging script (source/dev_tools/package_assaultcube.sh) and source/macos/make-app.sh
(
  cd "$RES"
  find . \( -name '.gitignore' -o -name '.DS_Store' -o -name '*~' -o -name 'shadows.dat' \) -delete
  rm -f config/init*.cfg config/saved*.cfg config/servervita*.cfg config/servers.cfg config/history clientlog*.txt
  rm -f packages/maps/dev_*.cgz packages/maps/dev_*.cfg packages/maps/servermaps/incoming/*.cgz packages/maps/servermaps/incoming/*.cfg
  find demos -type f ! -name 'tutorial_demo.dmo' -delete 2>/dev/null || true
  rm -rf docs/autogen
  # user-editable configs are shipped as templates, which the game copies into the user folder if they're missing
  cd config
  rm -f configtemplates.zip
  zip -q -X configtemplates.zip autoexec.cfg favourites.cfg pcksources.cfg
  rm -f autoexec.cfg favourites.cfg pcksources.cfg
)
mkdir -p "$RES/licenses"
cp "$DEPS"/share/licenses/*.txt "$RES/licenses/"
cp "$SRC/enet/LICENSE" "$RES/licenses/enet-LICENSE.txt"
cp "$SRC/README.txt" "$RES/licenses/AssaultCube-README.txt"
cp "$SRC/README_CUBEENGINE.txt" "$RES/licenses/Cube-README.txt"
sed -n '/^This software is available under 2 licenses/,/^\*\/$/p' "$SRC/src/stb_image.h" | sed '$d' > "$RES/licenses/stb_image-LICENSE.txt"
cat > "$RES/licenses/README.txt" <<EOF
Third-party libraries in the AssaultCube AppImage

OpenAL Soft 1.25.2 - GNU LGPL 2 or later (openal-soft-COPYING.txt)
  usr/lib/libopenal.so.1, unmodified, built from https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2
  (see source/linux/build-deps.sh in the AssaultCube source code). It's a separate shared library: it can be replaced
  with any other build of OpenAL Soft (extract the AppImage with --appimage-extract and run squashfs-root/AppRun).

Statically linked: SDL3 3.4.20 (zlib license, SDL3-LICENSE.txt), stb_image 2.30 (public domain/MIT,
stb_image-LICENSE.txt, from the end of source/src/stb_image.h), libogg 1.3.6 and libvorbis 1.3.7 (BSD license,
libogg-COPYING.txt and libvorbis-COPYING.txt), ENet 1.3.18, the network library (MIT license, enet-LICENSE.txt).
OpenAL Soft contains code under BSD and MIT licenses: openal-soft-BSD-3Clause.txt, openal-soft-pffft-LICENSE.txt,
openal-soft-gsl-LICENSE.txt, openal-soft-fmt-LICENSE.txt.

AssaultCube-README.txt and Cube-README.txt are source/README.txt and source/README_CUBEENGINE.txt of the AssaultCube
source code: the licenses of AssaultCube and the Cube engine. See also README.html and docs/package_copyrights.txt.

The file starts with the AppImage runtime, release $RUNTIME_VERSION of https://github.com/AppImage/type2-runtime,
unmodified: see there for its source code and licenses.
EOF
# (a library folder built by an older build-deps.sh lacks some of them: remove $DEPS then)
for f in SDL3-LICENSE.txt libogg-COPYING.txt libvorbis-COPYING.txt openal-soft-COPYING.txt \
         openal-soft-BSD-3Clause.txt openal-soft-pffft-LICENSE.txt openal-soft-gsl-LICENSE.txt \
         openal-soft-fmt-LICENSE.txt enet-LICENSE.txt stb_image-LICENSE.txt; do
  [ -s "$RES/licenses/$f" ] || { echo "make-appimage: licenses/$f is missing (remove $DEPS and run again)" >&2; exit 1; }
done

# 4. checks: the executables and OpenAL Soft link only to system libraries every desktop Linux has (and the client to
#    the bundled OpenAL Soft, found through its run path), and the bundled library is the one that gets loaded
ALLOWED='^(libc|libm|libstdc\+\+|libgcc_s|libpthread|libdl|librt|libX11|libGL|libz)\.so\.[0-9]+$|^ld-linux[-a-z0-9_.]*\.so\.[0-9]+$'
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
for bin in "$APPDIR/usr/bin/assaultcube" "$APPDIR/usr/bin/ac_server" "$APPDIR/usr/lib/libopenal.so.1"; do
  file -L "$bin" | grep -q "ELF 64-bit" || { echo "make-appimage: $bin isn't a 64-bit ELF file" >&2; exit 1; }
  libs="$(needed "$bin")"
  [ "$bin" = "$APPDIR/usr/bin/assaultcube" ] && libs="$(echo "$libs" | grep -vx 'libopenal\.so\.1')"
  bad="$(echo "$libs" | grep -vE "$ALLOWED" || true)"
  [ -z "$bad" ] || { echo "make-appimage: $bin links unexpected libraries:" >&2; echo "$bad" >&2; exit 1; }
done
needed "$APPDIR/usr/bin/assaultcube" | grep -qx 'libopenal\.so\.1' || { echo "make-appimage: the client doesn't link OpenAL Soft" >&2; exit 1; }
# shellcheck disable=SC2016
readelf -d "$APPDIR/usr/bin/assaultcube" | grep -Eq '(RUNPATH|RPATH).*\[\$ORIGIN/\.\./lib\]' \
  || { echo "make-appimage: the client's run path isn't \$ORIGIN/../lib" >&2; exit 1; }
openal="$(ldd "$APPDIR/usr/bin/assaultcube" | sed -n 's/^[[:space:]]*libopenal\.so\.1 => \(.*\) (0x.*/\1/p')"
[ -n "$openal" ] && [ "$(readlink -f "$openal")" = "$APPDIR/usr/lib/libopenal.so.1" ] \
  || { echo "make-appimage: the client doesn't load the bundled OpenAL Soft" >&2; ldd "$APPDIR/usr/bin/assaultcube" >&2; exit 1; }
# the oldest C and C++ runtime it runs with
symver() { for bin in "$APPDIR"/usr/bin/* "$APPDIR"/usr/lib/*.so*; do readelf -V "$bin"; done | grep -o "$1_[0-9.]*" | sort -u -V | tail -1; }
step "needs $(symver GLIBC) and $(symver GLIBCXX) (libstdc++) or later"

# 5. pack it, with the pinned appimagetool (extracted, so it runs without FUSE) and runtime
TOOLS="$HERE/deps/tools-$ARCH"
fetchtool() { # url sha256 file
  if [ ! -f "$3" ] || [ "$(sha256sum "$3" | cut -d' ' -f1)" != "$2" ]; then
    echo "make-appimage: downloading $1"
    curl -fsSL --retry 3 -o "$3.part" "$1"
    mv "$3.part" "$3"
  fi
  local got
  got="$(sha256sum "$3" | cut -d' ' -f1)"
  [ "$got" = "$2" ] || { echo "make-appimage: checksum mismatch for $3 (expected $2, got $got)" >&2; exit 1; }
}
mkdir -p "$TOOLS"
fetchtool "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-$ARCH.AppImage" \
  "$APPIMAGETOOL_SHA" "$TOOLS/appimagetool-$APPIMAGETOOL_VERSION.AppImage"
fetchtool "https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-$ARCH" \
  "$RUNTIME_SHA" "$TOOLS/runtime-$RUNTIME_VERSION"
if [ ! -x "$TOOLS/appimagetool-$APPIMAGETOOL_VERSION/AppRun" ]; then
  # (extracted by a copy without the AppImage signature in its ELF header, bytes 8-10, which would keep it from running
  # in a container that emulates the architecture with qemu or Rosetta)
  rm -rf "$TOOLS/squashfs-root" "$TOOLS/appimagetool-$APPIMAGETOOL_VERSION"
  cp "$TOOLS/appimagetool-$APPIMAGETOOL_VERSION.AppImage" "$TOOLS/appimagetool-extract"
  printf '\0\0\0' | dd of="$TOOLS/appimagetool-extract" bs=1 seek=8 count=3 conv=notrunc status=none
  chmod +x "$TOOLS/appimagetool-extract"
  (cd "$TOOLS" && ./appimagetool-extract --appimage-extract >/dev/null)
  rm "$TOOLS/appimagetool-extract"
  mv "$TOOLS/squashfs-root" "$TOOLS/appimagetool-$APPIMAGETOOL_VERSION"
fi
# (appimagetool validates the desktop entry. --no-appstream: the AppStream metadata from upstream pull request #453 is
# included as it is, but predates the AppStream 1.0 validator, which wants <launchable> and <image> in <screenshot>.)
rm -f "$APPIMAGE"
ARCH="$ARCH" VERSION="$VERSION" "$TOOLS/appimagetool-$APPIMAGETOOL_VERSION/AppRun" --no-appstream \
  --runtime-file "$TOOLS/runtime-$RUNTIME_VERSION" "$APPDIR" "$APPIMAGE"

step "done: $APPIMAGE"
du -h "$APPIMAGE"
