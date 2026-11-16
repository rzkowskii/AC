# Building the AssaultCube AppImage for Linux

This produces a self-contained AppImage, a single executable file that runs AssaultCube on most Linux distributions without installing anything:

```bash
source/linux/make-appimage.sh     # -> source/linux/build/AssaultCube-<version>-<arch>.AppImage
```

It builds for the machine it runs on, x86_64 or aarch64 (64-bit ARM). The AppImage runs on distributions with the same or a newer C and C++ runtime (glibc, libstdc++) than the build machine's; the script prints the versions. Built on Ubuntu 24.04, it needs glibc 2.38 and the libstdc++ of GCC 13: Ubuntu 24.04, Debian 13, Fedora 39 and later, and current rolling distributions. On older ones, such as Debian 12, it stops with an error that `GLIBC_2.38` was not found.

Requirements on Debian/Ubuntu (the headers are for the window systems and sound servers that the game uses at run time):

```bash
sudo apt-get install build-essential clang cmake ninja-build pkg-config curl ca-certificates rsync zip file \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libdrm-dev libgbm-dev \
  libdbus-1-dev libudev-dev libibus-1.0-dev libpipewire-0.3-dev libpulse-dev libasound2-dev zlib1g-dev
```

The first run downloads the library sources and takes a few minutes. Later runs only rebuild the game.

## What the scripts do

- **`build-deps.sh`** downloads SDL3, libogg, libvorbis and OpenAL Soft from their official sources, checks each download against a pinned SHA-256 (the same releases as the macOS app), and builds them in `source/linux/deps/<arch>`. SDL3, libogg and libvorbis are static libraries. **OpenAL Soft** is LGPL, so it's a shared library that the AppImage ships in `usr/lib`, where it can be replaced. SDL3 and OpenAL Soft load the X11, Wayland, D-Bus, PipeWire, PulseAudio and ALSA libraries when the game starts, so the build only needs their headers. The script fails if SDL3 is built without X11, XInput2, Wayland with libdecor, GLX, EGL, Vulkan or D-Bus support.
- **`make-appimage.sh`**:
  1. Compiles the client and the standalone server with CMake against those libraries.
  2. Assembles the AppDir: the game, the server, OpenAL Soft, the game data, the desktop entry, icon and AppStream metadata from `source/freedesktop/`, and the entry point `AppRun`.
  3. Checks the result: the programs link only to libraries every desktop Linux has (libc, libstdc++, libGL, libX11, zlib), and the client loads the bundled OpenAL Soft.
  4. Packs it with `appimagetool` and the AppImage runtime, official releases pinned by version and SHA-256.

Game data is taken from the files under version control and cleaned up the same way as `source/dev_tools/package_assaultcube.sh` does.

The AppImage layout:

```
AppRun                         entry point (source/linux/AppRun): the game, or the server with "server"
usr/bin/assaultcube            the game
usr/bin/ac_server              standalone server
usr/lib/libopenal.so.1         OpenAL Soft 1.25.2 (LGPL), the only bundled shared library
usr/share/assaultcube/         game data: config, packages, bot, mods, docs, ...
usr/share/assaultcube/licenses/  third-party licenses
usr/share/applications/, usr/share/icons/, usr/share/metainfo/   desktop entry, icon, AppStream metadata
```

What the AppImage leaves to the system, as AppImages should: the C and C++ runtime, the graphics driver (libGL, libvulkan), X11 and Wayland, zlib, and the sound servers.

## Running

```bash
chmod +x AssaultCube-1.4.0.0-x86_64.AppImage
./AssaultCube-1.4.0.0-x86_64.AppImage                 # the game
./AssaultCube-1.4.0.0-x86_64.AppImage -t0 -w1280 -h720 --gpu   # windowed, 1280x720, Vulkan renderer
```

Settings, maps, demos, screenshots and logs (`clientlog.txt`) go to `~/.assaultcube/v1.4`, the same folder `assaultcube.sh` uses, so the AppImage and an installation started with `assaultcube.sh` share their settings. Passing `--home=...` or `--init` turns this default off; a relative `--home` folder is relative to the current directory. All other options are passed on to the game.

AppImages need FUSE to run (on most distributions it's installed; otherwise install the `fuse3` package). Without FUSE, for example in a container, use `--appimage-extract-and-run` or set `APPIMAGE_EXTRACT_AND_RUN=1`.

**Server:** `./AssaultCube-1.4.0.0-x86_64.AppImage server [options]` starts the standalone server. It runs in its own folder, `~/.assaultcube/v1.4/server`, or the folder in `AC_SERVER_DIR`. On the first start that folder gets the default server configuration in `config/`; it's never overwritten later. Like `server.sh`, the server reads `config/servercmdline.txt`, and options on the command line take precedence. Logs and demos are written there too. The official maps always come from the AppImage (`packages/maps/official` is a link into it), so they match its game version. Custom maps go into `packages/maps/servermaps`.

```bash
./AssaultCube-1.4.0.0-x86_64.AppImage server -c12 -n"My server"
AC_SERVER_DIR=/srv/assaultcube ./AssaultCube-1.4.0.0-x86_64.AppImage server
```

To try a different OpenAL Soft (or to look inside), extract the AppImage with `--appimage-extract` and run `squashfs-root/AppRun`.

## Testing without a display or sound card

The game runs under Xvfb with Mesa's software renderers (llvmpipe for OpenGL, lavapipe for `--gpu`/Vulkan). Point OpenAL Soft at its file writer to check the audio:

```bash
printf '[general]\ndrivers = wave\n[wave]\nfile = /tmp/ac-audio.wav\n' > /tmp/alsoft.conf
ALSOFT_CONF=/tmp/alsoft.conf xvfb-run -a -s "-screen 0 1280x800x24" ./AssaultCube-*.AppImage --appimage-extract-and-run \
  --home=/tmp/actest --init -t0 -w1280 -h800 --loadmap=ac_desert \
  -e"hrtf 1; sleep 5000 [screenshottype 0; screenshot] 1; sleep 7000 [quit] 1"
grep -E "Renderer|Sound|HRTF" /tmp/actest/clientlog.txt; ls /tmp/actest/screenshots
```

## Releases

`.github/workflows/release.yml` builds the AppImages for x86_64 and aarch64 together with the macOS disk image, and attaches them to a draft GitHub release when a `v*` tag is pushed.
