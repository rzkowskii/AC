#!/bin/bash
# Builds a self-contained, universal (Apple Silicon + Intel) AssaultCube.app for macOS 11 and later:
#   source/macos/build/AssaultCube.app   (and AssaultCube-<version>-macOS.dmg with --dmg)
#
# The game executable launches straight into the game (no separate launcher). It runs from the bundled game data and
# keeps settings, maps and demos in ~/Library/Application Support/assaultcube/v1.4, next to the old launcher's v1.3.
# A universal standalone server is included as Contents/MacOS/ac_server.
#
# Usage: source/macos/make-app.sh [--dmg]
# Env:   CODESIGN_IDENTITY  "-" (default) signs ad-hoc, which is enough to run it locally. Set it to a
#                           "Developer ID Application: ..." identity to sign with the hardened runtime and a secure
#                           timestamp, as required for notarization (see source/macos/README.md).
#        MACOS_ARCHS        "arm64 x86_64" (default), or just one of them
#        OUT                output directory (default: source/macos/build)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$SRC/.." && pwd)"
OUT="${OUT:-$HERE/build}"
APP="$OUT/AssaultCube.app"
ARCHLIST="${MACOS_ARCHS:-arm64 x86_64}"
SIGNID="${CODESIGN_IDENTITY:--}"
export MACOSX_DEPLOYMENT_TARGET=11.0
JOBS="$(sysctl -n hw.ncpu)"
MAKEDMG=0
[ "${1:-}" = "--dmg" ] && MAKEDMG=1

step() { echo "make-app: $*"; }

# version from cube.h, e.g. AC_VERSION 1400 -> 1.4.0.0
ACV="$(sed -n 's/^[[:space:]]*#define AC_VERSION \([0-9][0-9]*\).*/\1/p' "$SRC/src/cube.h" | head -1)"
VERSION="$((ACV / 1000)).$((ACV / 100 % 10)).$((ACV / 10 % 10)).$((ACV % 10))"

# 1. third-party libraries (universal, static, macOS 11+)
MACOS_ARCHS="${ARCHLIST// /;}" "$HERE/build-deps.sh"
DEPS="$HERE/deps/universal"

# 2. compile the client and server once per architecture, each in its own copy of the source tree, so objects never
#    mix between architectures or with a development build in source/src (rsync keeps timestamps: incremental rebuilds)
mkdir -p "$OUT"
for arch in $ARCHLIST; do
  tree="$OUT/obj-$arch"
  step "compiling $arch"
  mkdir -p "$tree"
  rsync -a --delete \
    --exclude '*.o' --exclude '*.gch' --exclude '.libs/' --exclude 'Makefile_local' \
    --exclude '/src/ac_client' --exclude '/src/ac_server' --exclude '/src/ac_master' \
    "$SRC/src" "$SRC/enet" "$SRC/include" "$tree/"
  if ! make -C "$tree/src" -j"$JOBS" PLATFORM=Darwin MACOS_ARCH="$arch" MACOS_DEPS="$DEPS" client server \
       > "$OUT/build-$arch.log" 2>&1; then
    grep -E 'error|Error' "$OUT/build-$arch.log" | head -20
    echo "make-app: $arch build failed, see $OUT/build-$arch.log" >&2
    exit 1
  fi
done

# 3. assemble the bundle
step "assembling $APP ($VERSION)"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
clients=(); servers=()
for arch in $ARCHLIST; do
  clients+=("$OUT/obj-$arch/src/ac_client")
  servers+=("$OUT/obj-$arch/src/ac_server")
done
lipo -create "${clients[@]}" -output "$APP/Contents/MacOS/assaultcube"
lipo -create "${servers[@]}" -output "$APP/Contents/MacOS/ac_server"
strip -x "$APP/Contents/MacOS/assaultcube" "$APP/Contents/MacOS/ac_server"
# OpenAL Soft (LGPL) is the only shared library the game ships; the executable finds it via @rpath
mkdir -p "$APP/Contents/Frameworks"
cp -L "$DEPS/lib/libopenal.1.dylib" "$APP/Contents/Frameworks/libopenal.1.dylib"
sed "s/@VERSION@/$VERSION/g" "$HERE/Info.plist" > "$APP/Contents/Info.plist"
plutil -lint -s "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cp "$SRC/xcode/assaultcube.icns" "$APP/Contents/Resources/assaultcube.icns"

# game data: only files under version control when building from a git checkout (no local saves, logs, test maps)
RES="$APP/Contents/Resources"
DATA=(bot config demos docs mods packages README.html README.md)
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git -C "$ROOT" ls-files -z -- "${DATA[@]}" | rsync -a --from0 --files-from=- "$ROOT/" "$RES/"
else
  for d in "${DATA[@]}"; do [ -e "$ROOT/$d" ] && rsync -a "$ROOT/$d" "$RES/"; done
fi
# same clean-up as the official packaging script (source/dev_tools/package_assaultcube.sh)
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
Third-party libraries in AssaultCube.app

OpenAL Soft 1.25.2 - GNU LGPL 2 or later (openal-soft-COPYING.txt)
  Contents/Frameworks/libopenal.1.dylib, unmodified, built from
  https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2 (see source/macos/build-deps.sh in the
  AssaultCube source code). It's a separate shared library: it can be replaced with any other build of OpenAL Soft.

Statically linked: SDL3 3.4.20 (zlib license), stb_image 2.30 (public domain/MIT), libogg 1.3.6 and
libvorbis 1.3.7 (BSD license). See README.html and docs/package_copyrights.txt for AssaultCube itself.

The license texts in this folder:
  SDL3-LICENSE.txt                            SDL3
  libogg-COPYING.txt, libvorbis-COPYING.txt   libogg and libvorbis
  enet-LICENSE.txt                            ENet, the network library (MIT license), also statically linked
  stb_image-LICENSE.txt                       stb_image (from the end of source/src/stb_image.h)
  openal-soft-BSD-3Clause.txt, openal-soft-pffft-LICENSE.txt, openal-soft-gsl-LICENSE.txt, openal-soft-fmt-LICENSE.txt
                                              code in OpenAL Soft under BSD and MIT licenses
  AssaultCube-README.txt, Cube-README.txt     AssaultCube and the Cube engine: source/README.txt and
                                              source/README_CUBEENGINE.txt of the AssaultCube source code
EOF
# (a library folder built by an older build-deps.sh lacks some of them: remove source/macos/deps/universal then)
for f in SDL3-LICENSE.txt libogg-COPYING.txt libvorbis-COPYING.txt openal-soft-COPYING.txt \
         openal-soft-BSD-3Clause.txt openal-soft-pffft-LICENSE.txt openal-soft-gsl-LICENSE.txt \
         openal-soft-fmt-LICENSE.txt enet-LICENSE.txt stb_image-LICENSE.txt; do
  [ -s "$RES/licenses/$f" ] || { echo "make-app: licenses/$f is missing (remove source/macos/deps/universal and run again)" >&2; exit 1; }
done

# 4. checks: architectures, minimum macOS version, no links to libraries outside the OS (except the bundled OpenAL Soft),
#    and no library search paths other than the bundle's own Frameworks folder
OPENAL="$APP/Contents/Frameworks/libopenal.1.dylib"
for bin in "$APP/Contents/MacOS/assaultcube" "$APP/Contents/MacOS/ac_server" "$OPENAL"; do
  for arch in $ARCHLIST; do
    lipo "$bin" -verify_arch "$arch" || { echo "make-app: $bin lacks $arch" >&2; exit 1; }
    minos="$(vtool -arch "$arch" -show-build "$bin" | awk '/minos/ {print $2; exit}')"
    [ "$minos" = "$MACOSX_DEPLOYMENT_TARGET" ] || { echo "make-app: $bin ($arch) has minos $minos" >&2; exit 1; }
    libs="$(otool -arch "$arch" -L "$bin" | tail -n +2 | awk '{print $1}')"
    [ "$bin" = "$OPENAL" ] && libs="$(echo "$libs" | tail -n +2)" # first entry is the library's own install name
    bad="$(echo "$libs" | grep -vE '^(/usr/lib/|/System/Library/|@rpath/libopenal\.1\.dylib$)' || true)"
    [ -z "$bad" ] || { echo "make-app: $bin ($arch) links libraries outside the OS:" >&2; echo "$bad" >&2; exit 1; }
    rpaths="$(otool -arch "$arch" -l "$bin" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' | sort -u | tr '\n' ' ')"
    case "$bin" in
      */assaultcube) [ "$rpaths" = "@executable_path/../Frameworks " ] || { echo "make-app: unexpected rpaths in $bin ($arch): $rpaths" >&2; exit 1; } ;;
      *) [ -z "$rpaths" ] || { echo "make-app: unexpected rpaths in $bin ($arch): $rpaths" >&2; exit 1; } ;;
    esac
  done
done

# 5. code signing (inside out: the bundled library and the server, then the bundle, whose main executable is signed with it)
signargs=(--force --sign "$SIGNID")
[ "$SIGNID" != "-" ] && signargs+=(--options runtime --timestamp)
codesign "${signargs[@]}" "$OPENAL"
codesign "${signargs[@]}" "$APP/Contents/MacOS/ac_server"
codesign "${signargs[@]}" "$APP"
codesign --verify --strict "$APP"

step "done: $APP"
lipo -info "$APP/Contents/MacOS/assaultcube"
du -sh "$APP"

# 6. optional disk image
if [ "$MAKEDMG" = 1 ]; then
  DMG="$OUT/AssaultCube-$VERSION-macOS.dmg"
  STAGE="$OUT/dmg-stage"
  rm -rf "$STAGE" "$DMG"
  mkdir -p "$STAGE"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -quiet -volname "AssaultCube $VERSION" -srcfolder "$STAGE" -format UDZO -ov "$DMG"
  rm -rf "$STAGE"
  [ "$SIGNID" != "-" ] && codesign --force --sign "$SIGNID" --timestamp "$DMG"
  step "disk image: $DMG"
fi
