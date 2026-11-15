# Building AssaultCube for macOS

This produces a self-contained, universal `AssaultCube.app` that runs natively on Apple Silicon and Intel Macs with macOS 11 (Big Sur) or later.

```bash
source/macos/make-app.sh          # -> source/macos/build/AssaultCube.app
source/macos/make-app.sh --dmg    # ... plus source/macos/build/AssaultCube-<version>-macOS.dmg
```

Requirements: the Xcode command line tools (`xcode-select --install`), plus `cmake` (for example `brew install cmake`). The first run downloads the dependency sources and takes about a minute. Later runs are incremental.

## What the scripts do

- **`build-deps.sh`** downloads SDL2, libogg, libvorbis and OpenAL Soft from their official sources and checks each download against a pinned SHA-256. It builds them **universal** (arm64 + x86_64) for macOS 11+ in `source/macos/deps/universal`. Homebrew libraries can't be shipped: they're built for the build machine's macOS version only and pull in many optional libraries. Textures are decoded by the game itself with stb_image (`source/src/stbimage.cpp`), so there are no image libraries. Everything is static except **OpenAL Soft**. OpenAL Soft is LGPL, so it's built as a shared library (CoreAudio output, built-in HRTF dataset) that the app ships in `Contents/Frameworks`, where it can be replaced.
- **`make-app.sh`**:
  1. Compiles the client and standalone server once per architecture. Each build happens in its own copy of the source tree under `source/macos/build/obj-<arch>`, so it never interferes with a development build in `source/src`.
  2. Combines the two architectures with `lipo`.
  3. Assembles the bundle: game data, icon and `Info.plist`.
  4. Checks the result: both architectures are present, the minimum macOS version is 11.0, and nothing links outside the OS.
  5. Code-signs the bundle.

  Game data is taken from the files under version control and cleaned up the same way as `source/dev_tools/package_assaultcube.sh` does.

The bundle layout:

```
AssaultCube.app/Contents/
  MacOS/assaultcube     the game (universal)
  MacOS/ac_server       standalone server (universal), see below
  Frameworks/libopenal.1.dylib   OpenAL Soft 1.25.2 (LGPL), the only bundled shared library
  Resources/            game data: config, packages, bot, mods, docs, ...
  Resources/licenses/   license texts of the libraries in the app (and where OpenAL Soft's source is)
  Info.plist            macOS 11+, games category (Game Mode on macOS 14+), Retina capable
```

## 3D audio (HRTF)

The app uses OpenAL Soft instead of Apple's deprecated OpenAL.framework. That's the same audio engine AssaultCube uses on Linux and Windows. It adds **HRTF**, binaural rendering for headphones: sounds can be heard in front, behind, above and below you, not just left and right. Choose it in *Settings → Sound → 3D audio for headphones (HRTF)*, or use the `hrtf` variable:

- `hrtf -1` (Auto, default): on when headphones are detected. On macOS that means wired headphones on the Mac's own headphone jack.
- `hrtf 1` (On): for headphones that aren't detected, such as AirPods and other Bluetooth or USB headsets.
- `hrtf 0` (Off). Use this with speakers.

The console shows the result, for example `HRTF: enabled, Built-In HRTF (on)`. The read-only `hrtfactive` variable is 1 while HRTF is in use. The development build (Apple's OpenAL.framework) reports HRTF as not supported.

To test audio without hearing it, point OpenAL Soft at its file writer:

```bash
printf '[general]\ndrivers = wave\n[wave]\nfile = /tmp/ac-audio.wav\n' > /tmp/alsoft.conf
ALSOFT_CONF=/tmp/alsoft.conf AssaultCube.app/Contents/MacOS/assaultcube
```

## Running

The app starts straight into the game; there's no separate launcher any more. When it's started from Finder, the Dock or `open` without arguments, it runs from the bundled data. Settings, maps, demos, screenshots and logs (`clientlog.txt`) go to `~/Library/Application Support/assaultcube/v1.4`: the place the old macOS launcher used, with the version of this release, like `assaultcube.sh` and `assaultcube.bat` (`v1.4`). A 1.3 installation keeps its own `v1.3` folder next to it; its settings aren't taken over. Passing `--home=...` or `--init` turns this default off, and then the usual command-line rules apply:

```bash
open AssaultCube.app --args -t0 -w1280 -h800          # windowed, using the default user folder
AssaultCube.app/Contents/MacOS/assaultcube --home=/tmp/actest --init
```

**Server:** `ac_server` reads and writes files relative to its working directory: `config/`, `packages/maps/`, `logs/` and `private/`. Don't run it from inside the app bundle. That would modify the bundle and break its code signature. Copy the game data to a folder of its own instead:

```bash
cp -R /Applications/AssaultCube.app/Contents/Resources ~/assaultcube-server
cd ~/assaultcube-server && /Applications/AssaultCube.app/Contents/MacOS/ac_server
```

## Signing, notarization and Gatekeeper

By default the app is signed **ad-hoc**, which is enough to run it on the machine that built it. A copy downloaded from the internet is quarantined, and Gatekeeper will block it. On macOS 15 and later, allow it under *System Settings → Privacy & Security → Open Anyway*; on earlier versions, right-click the app and choose *Open*.

To distribute it, sign with a Developer ID certificate. The script then enables the hardened runtime and a secure timestamp. Then notarize and staple:

```bash
CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" source/macos/make-app.sh --dmg
xcrun notarytool submit source/macos/build/AssaultCube-1.4.0.0-macOS.dmg --keychain-profile "<profile>" --wait
xcrun stapler staple source/macos/build/AssaultCube-1.4.0.0-macOS.dmg
```

The only non-system library is the bundled OpenAL Soft, which `make-app.sh` signs with the same identity before the app. With matching signatures, library validation under the hardened runtime should accept it, and no extra entitlements should be needed. That hasn't been verified with a real Developer ID yet.

## Development builds

For day-to-day work you don't need any of this. `cd source/src && make -j client server` builds a native, single-architecture client and server against Homebrew's `sdl2-compat` and `libvorbis`. Run it from the repository root:

```bash
source/src/ac_client --home="$HOME/Library/Application Support/assaultcube/v1.4" --init
```

## The Xcode project

`source/xcode/` is the old Xcode project. Its bundled Ogg, Vorbis and SDL2_image frameworks are Intel-only, and its launcher is 32-bit, so it can't produce an Apple Silicon build. `make-app.sh` replaces it.
