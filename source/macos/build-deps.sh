#!/bin/bash
# Builds AssaultCube's third-party libraries for macOS from source, as universal (arm64 + x86_64) static libraries
# with a minimum macOS version of 11.0, into source/macos/deps/universal.
#
# Homebrew/MacPorts libraries are fine for development builds, but can't be shipped in an app bundle: they're built
# for the macOS version of the build machine only, have many optional dependencies, and are single-architecture.
#
# Usage: source/macos/build-deps.sh        (does nothing if the libraries are already built with the same settings)
# Env:   MACOS_ARCHS ("arm64;x86_64"), MACOSX_DEPLOYMENT_TARGET (11.0)
# Needs: Xcode command line tools, cmake, curl
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="$HERE/deps"
DOWNLOADS="$DEPS/downloads"
BUILD="$DEPS/build"
PREFIX="$DEPS/universal"
ARCHS="${MACOS_ARCHS:-arm64;x86_64}"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
JOBS="$(sysctl -n hw.ncpu)"

# name | url | sha256 (cross-checked against Homebrew formulae and GitHub release digests)
PACKAGES=(
  "SDL2-2.32.10|https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz|5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
  "libogg-1.3.6|https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz|83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638"
  "libvorbis-1.3.7|https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz|b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b"
  "openal-soft-1.25.2|https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2|1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089"
)

for tool in cmake curl shasum; do
  command -v "$tool" >/dev/null || { echo "build-deps: '$tool' is required" >&2; exit 1; }
done

STAMP="$PREFIX/.built-$(printf '%s ' "${PACKAGES[@]}" "$ARCHS" "$MACOSX_DEPLOYMENT_TARGET" | shasum -a 256 | cut -c1-16)"
if [ -f "$STAMP" ]; then
  echo "build-deps: up to date ($PREFIX)"
  exit 0
fi

mkdir -p "$DOWNLOADS" "$BUILD"
rm -rf "$PREFIX"

srcdir() { echo "$BUILD/src/$1"; }

fetch() { # name url sha256 -> unpacks into $BUILD/src/<name>
  local name="$1" url="$2" sha="$3" file="$DOWNLOADS/$(basename "$2")"
  if [ ! -f "$file" ]; then
    echo "build-deps: downloading $url"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  local got
  got="$(shasum -a 256 "$file" | cut -d' ' -f1)"
  if [ "$got" != "$sha" ]; then
    echo "build-deps: checksum mismatch for $file (expected $sha, got $got)" >&2
    exit 1
  fi
  rm -rf "$(srcdir "$name")"
  mkdir -p "$BUILD/src"
  tar -xf "$file" -C "$BUILD/src"
}

cmakebuild() { # name [cmake args...]
  local name="$1"; shift
  echo "build-deps: building $name ($ARCHS, macOS $MACOSX_DEPLOYMENT_TARGET+)"
  cmake -S "$(srcdir "$name")" -B "$BUILD/$name" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_FRAMEWORK=NEVER \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;/opt/local" \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "$@" > "$BUILD/$name.configure.log" 2>&1 || { tail -30 "$BUILD/$name.configure.log"; exit 1; }
  cmake --build "$BUILD/$name" -j "$JOBS" > "$BUILD/$name.build.log" 2>&1 || { tail -30 "$BUILD/$name.build.log"; exit 1; }
  cmake --install "$BUILD/$name" > "$BUILD/$name.install.log" 2>&1 || { tail -30 "$BUILD/$name.install.log"; exit 1; }
}

for p in "${PACKAGES[@]}"; do
  IFS='|' read -r name url sha <<< "$p"
  fetch "$name" "$url" "$sha"
done

cmakebuild SDL2-2.32.10 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF

# (no SDL_image: the game decodes PNG and JPEG textures itself with stb_image, source/src/stbimage.cpp)

cmakebuild libogg-1.3.6 -DBUILD_TESTING=OFF -DINSTALL_DOCS=OFF
cmakebuild libvorbis-1.3.7 -DBUILD_TESTING=OFF

# OpenAL Soft replaces Apple's deprecated OpenAL.framework: HRTF (binaural 3D audio for headphones) with the built-in
# HRTF dataset, plus the same audio engine AssaultCube uses on Linux and Windows. It's LGPL, so it's built as a shared
# library (install name @rpath/libopenal.1.dylib) that make-app.sh ships in Contents/Frameworks with its license.
# CoreAudio output only (plus the wave-file writer for testing); never pick up Homebrew's PortAudio/JACK/PulseAudio.
# (HAVE_WFUNCTION_EFFECTS=OFF: OpenAL Soft builds with -Werror=function-effects, and newer macOS SDKs mark CoreAudio
# render callbacks "nonblocking", which 1.25.2's callback lambdas predate. It's a static real-time-safety lint that
# doesn't change the generated code, so it's turned off rather than patching the source.)
cmakebuild openal-soft-1.25.2 -DLIBTYPE=SHARED -DCMAKE_FIND_FRAMEWORK=LAST -DHAVE_WFUNCTION_EFFECTS=OFF \
  -DALSOFT_REQUIRE_COREAUDIO=ON -DALSOFT_BACKEND_WAVE=ON \
  -DALSOFT_BACKEND_PIPEWIRE=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_JACK=OFF \
  -DALSOFT_BACKEND_PORTAUDIO=OFF -DALSOFT_BACKEND_SDL2=OFF -DALSOFT_BACKEND_SDL3=OFF \
  -DALSOFT_EMBED_HRTF_DATA=ON -DALSOFT_EAX=OFF -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF \
  -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF \
  -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
  -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF

# licenses, shipped with the app (OpenAL Soft contains code under BSD and MIT licenses: its FFT, GSL and {fmt})
mkdir -p "$PREFIX/share/licenses"
cp "$(srcdir libogg-1.3.6)/COPYING" "$PREFIX/share/licenses/libogg-COPYING.txt"
cp "$(srcdir libvorbis-1.3.7)/COPYING" "$PREFIX/share/licenses/libvorbis-COPYING.txt"
cp "$(srcdir openal-soft-1.25.2)/COPYING" "$PREFIX/share/licenses/openal-soft-COPYING.txt"
cp "$(srcdir openal-soft-1.25.2)/BSD-3Clause" "$PREFIX/share/licenses/openal-soft-BSD-3Clause.txt"
cp "$(srcdir openal-soft-1.25.2)/LICENSE-pffft" "$PREFIX/share/licenses/openal-soft-pffft-LICENSE.txt"
cp "$(srcdir openal-soft-1.25.2)/gsl/LICENSE" "$PREFIX/share/licenses/openal-soft-gsl-LICENSE.txt"
cp "$(srcdir openal-soft-1.25.2)/fmt-11.2.0/LICENSE" "$PREFIX/share/licenses/openal-soft-fmt-LICENSE.txt"

# sanity check: every library must contain all requested architectures
for lib in "$PREFIX"/lib/*.a "$PREFIX"/lib/libopenal.1.dylib; do
  for arch in ${ARCHS//;/ }; do
    lipo "$lib" -verify_arch "$arch" || { echo "build-deps: $lib lacks $arch" >&2; exit 1; }
  done
done

touch "$STAMP"
echo "build-deps: done -> $PREFIX"
ls -1 "$PREFIX/lib"/*.a
