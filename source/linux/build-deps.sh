#!/bin/bash
# Builds the third-party libraries that the AssaultCube AppImage bundles, from source, for the machine it runs on
# (x86_64 or aarch64), into source/linux/deps/<arch>: SDL3, libogg and libvorbis as static libraries, and OpenAL Soft
# as a shared library (it's LGPL, so it stays a separate, replaceable file).
#
# Distribution packages are fine for development builds (see .github/workflows/build.yml), but an AppImage must also run
# on distributions that don't package SDL3 at all, or that ship other versions of these libraries. Everything else comes
# from the system, where it matches the graphics driver and the desktop: the C/C++ runtime, libGL and libvulkan, X11 and
# Wayland, zlib, and the sound servers. SDL3 and OpenAL Soft load the X11, Wayland, D-Bus, PipeWire, PulseAudio and ALSA
# libraries at run time (dlopen), so here they're only needed as headers. On Debian/Ubuntu:
#
#   sudo apt-get install build-essential clang cmake ninja-build pkg-config curl ca-certificates \
#     libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
#     libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libdrm-dev libgbm-dev \
#     libdbus-1-dev libudev-dev libibus-1.0-dev libpipewire-0.3-dev libpulse-dev libasound2-dev zlib1g-dev
#
# Usage: source/linux/build-deps.sh        (does nothing if the libraries are already built with the same settings)
# Env:   DEPS_CC, DEPS_CXX  compilers (default: gcc, g++; clang 18 with libstdc++ 13 can't compile OpenAL Soft 1.25)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"
DEPS="$HERE/deps"
DOWNLOADS="$DEPS/downloads"
BUILD="$DEPS/build-$ARCH"
PREFIX="$DEPS/$ARCH"
export CC="${DEPS_CC:-gcc}" CXX="${DEPS_CXX:-g++}"
JOBS="$(nproc)"

# name | url | sha256 (the same releases and checksums as source/macos/build-deps.sh)
PACKAGES=(
  "SDL3-3.4.20|https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz|f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f"
  "libogg-1.3.6|https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz|83e6704730683d004d20e21b8f7f55dcb3383cdf84c0daedf30bde175f774638"
  "libvorbis-1.3.7|https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz|b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b"
  "openal-soft-1.25.2|https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2|1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089"
)

for tool in cmake curl sha256sum pkg-config "$CC" "$CXX"; do
  command -v "$tool" >/dev/null || { echo "build-deps: '$tool' is required" >&2; exit 1; }
done
GENERATOR="Unix Makefiles"
command -v ninja >/dev/null && GENERATOR=Ninja

STAMP="$PREFIX/.built-$(printf '%s ' "${PACKAGES[@]}" "$ARCH" "$("$CC" --version | head -1)" | sha256sum | cut -c1-16)"
if [ -f "$STAMP" ]; then
  echo "build-deps: up to date ($PREFIX)"
  exit 0
fi

mkdir -p "$DOWNLOADS" "$BUILD"
rm -rf "$PREFIX"

srcdir() { echo "$BUILD/src/$1"; }

fetch() { # name url sha256 -> unpacks into $BUILD/src/<name>
  local name="$1" url="$2" sha="$3" file
  file="$DOWNLOADS/$(basename "$2")"
  if [ ! -f "$file" ]; then
    echo "build-deps: downloading $url"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  local got
  got="$(sha256sum "$file" | cut -d' ' -f1)"
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
  echo "build-deps: building $name ($ARCH, $CC)"
  rm -rf "${BUILD:?}/${name:?}"
  cmake -S "$(srcdir "$name")" -B "$BUILD/$name" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    "$@" > "$BUILD/$name.configure.log" 2>&1 || { tail -30 "$BUILD/$name.configure.log"; exit 1; }
  cmake --build "$BUILD/$name" -j "$JOBS" > "$BUILD/$name.build.log" 2>&1 || { tail -30 "$BUILD/$name.build.log"; exit 1; }
  cmake --install "$BUILD/$name" > "$BUILD/$name.install.log" 2>&1 || { tail -30 "$BUILD/$name.install.log"; exit 1; }
}

for p in "${PACKAGES[@]}"; do
  IFS='|' read -r name url sha <<< "$p"
  fetch "$name" "$url" "$sha"
done

# SDL3 loads X11, Wayland (with libdecor window decorations), D-Bus and libvulkan when the game starts; the game itself
# only uses SDL's video and input
cmakebuild SDL3-3.4.20 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
# a missing -dev package silently drops a feature from SDL: make sure everything the game relies on is there
SDLCONFIG="$(find "$BUILD/SDL3-3.4.20" -name SDL_build_config.h | head -1)"
for feature in SDL_VIDEO_DRIVER_X11 SDL_VIDEO_DRIVER_X11_XINPUT2 SDL_VIDEO_DRIVER_WAYLAND HAVE_LIBDECOR_H \
               SDL_VIDEO_OPENGL_GLX SDL_VIDEO_OPENGL_EGL SDL_VIDEO_VULKAN SDL_GPU_VULKAN HAVE_DBUS_DBUS_H; do
  grep -Eq "^#define $feature( 1)?$" "$SDLCONFIG" || { echo "build-deps: SDL3 was built without $feature" >&2; exit 1; }
done

# (no SDL_image: the game decodes PNG and JPEG textures itself with stb_image, source/src/stbimage.cpp)

cmakebuild libogg-1.3.6 -DBUILD_TESTING=OFF -DINSTALL_DOCS=OFF
cmakebuild libvorbis-1.3.7 -DBUILD_TESTING=OFF

# OpenAL Soft, with the built-in HRTF dataset (binaural 3D audio for headphones, see the hrtf variable). Its sound
# backends are loaded at run time: PipeWire, PulseAudio and ALSA (all three required here), OSS, plus the wave-file
# writer for testing. It reads the usual configuration files (/etc/openal/alsoft.conf, ~/.config/alsoft.conf).
cmakebuild openal-soft-1.25.2 -DLIBTYPE=SHARED \
  -DALSOFT_REQUIRE_PIPEWIRE=ON -DALSOFT_REQUIRE_PULSEAUDIO=ON -DALSOFT_REQUIRE_ALSA=ON -DALSOFT_BACKEND_OSS=ON \
  -DALSOFT_BACKEND_WAVE=ON -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_PORTAUDIO=OFF -DALSOFT_BACKEND_SNDIO=OFF \
  -DALSOFT_BACKEND_SDL2=OFF -DALSOFT_BACKEND_SDL3=OFF \
  -DALSOFT_EMBED_HRTF_DATA=ON -DALSOFT_EAX=OFF -DALSOFT_ENABLE_MODULES=OFF -DALSOFT_UPDATE_BUILD_VERSION=OFF \
  -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF \
  -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
  -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF

# licenses, shipped with the AppImage (OpenAL Soft contains code under BSD and MIT licenses: its FFT, GSL and {fmt})
mkdir -p "$PREFIX/share/licenses"
cp "$(srcdir SDL3-3.4.20)/LICENSE.txt" "$PREFIX/share/licenses/SDL3-LICENSE.txt"
cp "$(srcdir libogg-1.3.6)/COPYING" "$PREFIX/share/licenses/libogg-COPYING.txt"
cp "$(srcdir libvorbis-1.3.7)/COPYING" "$PREFIX/share/licenses/libvorbis-COPYING.txt"
cp "$(srcdir openal-soft-1.25.2)/COPYING" "$PREFIX/share/licenses/openal-soft-COPYING.txt"
cp "$(srcdir openal-soft-1.25.2)/BSD-3Clause" "$PREFIX/share/licenses/openal-soft-BSD-3Clause.txt"
cp "$(srcdir openal-soft-1.25.2)/LICENSE-pffft" "$PREFIX/share/licenses/openal-soft-pffft-LICENSE.txt"
cp "$(srcdir openal-soft-1.25.2)/gsl/LICENSE" "$PREFIX/share/licenses/openal-soft-gsl-LICENSE.txt"
cp "$(srcdir openal-soft-1.25.2)/fmt-11.2.0/LICENSE" "$PREFIX/share/licenses/openal-soft-fmt-LICENSE.txt"

# sanity check: static libraries for SDL3/Ogg/Vorbis (so the game can't pick up a system copy), a shared OpenAL Soft
for lib in libSDL3.a libogg.a libvorbis.a libvorbisfile.a libopenal.so.1; do
  [ -e "$PREFIX/lib/$lib" ] || { echo "build-deps: $PREFIX/lib/$lib is missing" >&2; exit 1; }
done
if ls "$PREFIX"/lib/libSDL3.so* "$PREFIX"/lib/libogg.so* "$PREFIX"/lib/libvorbis*.so* >/dev/null 2>&1; then
  echo "build-deps: unexpected shared libraries in $PREFIX/lib" >&2; exit 1
fi

touch "$STAMP"
echo "build-deps: done -> $PREFIX"
ls -1 "$PREFIX/lib"/*.a "$PREFIX/lib"/libopenal.so*
