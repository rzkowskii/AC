#!/bin/bash
# Profile-guided optimisation build of the client (clang), in three steps:
#   source/dev_tools/pgo-build.sh generate    build an instrumented client (source/src/ac_client)
#   ... run it from the game directory and exercise the game, for example on a few maps:
#       LLVM_PROFILE_FILE=source/src/pgo/%p.profraw source/src/ac_client --loadmap=ac_desert -e"maxfps 0; benchmark 20 1"
#   source/dev_tools/pgo-build.sh use         merge the recorded profiles and build the optimised client (with LTO)
#
# Measured on an M4 Max (Apple's OpenGL, GPU-bound): the game's own CPU phases got 5-25% faster (CPU world geometry
# 0.021 -> 0.016 ms/frame), which is small next to the driver's share of the frame. Plain LTO showed no measurable gain.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../src" && pwd)"
PROFDIR="$SRC/pgo"
case "${1:-}" in
  generate)
    mkdir -p "$PROFDIR"
    make -C "$SRC" clean > /dev/null
    make -C "$SRC" -j"$(getconf _NPROCESSORS_ONLN)" PGO=generate client
    echo "pgo-build: instrumented client built; record profiles into $PROFDIR (LLVM_PROFILE_FILE=$PROFDIR/%p.profraw), then run: $0 use"
    ;;
  use)
    if command -v xcrun > /dev/null; then PROFDATA="xcrun llvm-profdata"; else PROFDATA="llvm-profdata"; fi
    $PROFDATA merge -o "$PROFDIR/ac.profdata" "$PROFDIR"/*.profraw
    make -C "$SRC" clean > /dev/null
    make -C "$SRC" -j"$(getconf _NPROCESSORS_ONLN)" LTO=1 PGO=use PGOPROFILE="$PROFDIR/ac.profdata" client
    echo "pgo-build: optimised client built from $(ls "$PROFDIR"/*.profraw | wc -l | tr -d ' ') recorded profiles"
    ;;
  *)
    echo "usage: $0 generate|use" >&2; exit 1 ;;
esac
