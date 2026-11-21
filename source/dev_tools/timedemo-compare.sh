#!/bin/bash
# timedemo-compare.sh: plays a demo with "timedemo" in two runs (two renderers, or two builds) and compares the
# screenshots both runs take at the same demo times. timedemo makes every run draw the same frames of the game, so what
# differs is what the renderers or builds draw differently. (The console, the fps counter and the HUD messages run on
# real time, so they're hidden.)
#
#   source/dev_tools/timedemo-compare.sh [options] <demo.dmo> "<client A> [options]" "<client B> [options]"
#
#   e.g. (renderers)  source/dev_tools/timedemo-compare.sh mydemo.dmo "build/ac_client --gl" "build/ac_client --gpu"
#        (builds)     source/dev_tools/timedemo-compare.sh mydemo.dmo "old/ac_client --gl" "build/ac_client --gl"
#   Add --hidden to the client options on a computer someone is using: the window then never shows or takes the
#   mouse or the focus.
#
# Options: -s demo seconds to play (default 30, 0 for the full demo)   -i seconds between screenshots (10)
#          -w width (1280)   -h height (800)
#          -t tolerance per pixel, largest channel difference (8)   -m minimum % of pixels within it (95)
#          -o output directory (timedemo-compare)   -x run each client with xvfb-run (Linux without a display)
#          -T seconds after which a client that is still running is stopped, and killed 10 s later if it doesn't end
#             (0, no limit; needs the "timeout" command)
# The demo ends before a screenshot at the last second is taken: the defaults give two pairs, at 10 and 20 s.
# Each client must finish and write all expected screenshots. Explicit --gl or --gpu is checked against the
# active renderer. Set SDL_GPU_DRIVER to require a specific driver in a --gpu run.
# Run it from the repository root (the game's data). It needs imgcompare (built from imgcompare.c next to this script,
# or IMGCOMPARE=path). Exits with 1 if a screenshot pair is below the minimum, 2 on errors.
set -u
SECONDS_=30; INTERVAL=10; W=1280; H=800; TOL=8; MIN=95; OUT=timedemo-compare; USEXVFB=0; LIMIT=0
while getopts "s:i:w:h:t:m:o:xT:" opt; do
  case $opt in
    s) SECONDS_=$OPTARG ;; i) INTERVAL=$OPTARG ;; w) W=$OPTARG ;; h) H=$OPTARG ;; t) TOL=$OPTARG ;; m) MIN=$OPTARG ;;
    o) OUT=$OPTARG ;; x) USEXVFB=1 ;; T) LIMIT=$OPTARG ;; *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ $# -eq 3 ] || { sed -n 2,24p "$0"; exit 2; }
DEMO=$1; RUNA=$2; RUNB=$3
[ -f "$DEMO" ] || { echo "timedemo-compare: no demo file $DEMO" >&2; exit 2; }
[ -d packages ] && [ -d config ] || { echo "timedemo-compare: run this from the repository root" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
IMGCOMPARE=${IMGCOMPARE:-$HERE/imgcompare}
[ -x "$IMGCOMPARE" ] || { echo "timedemo-compare: build $HERE/imgcompare.c first (see its header), or set IMGCOMPARE" >&2; exit 2; }
NAME=$(basename "$DEMO" .dmo)
if ! [[ $SECONDS_ =~ ^[0-9]+$ && $INTERVAL =~ ^[0-9]+$ ]]; then
  echo "timedemo-compare: -s and -i need whole seconds" >&2
  exit 2
fi
SECONDS_=$((10#$SECONDS_))
INTERVAL=$((10#$INTERVAL))
if (( INTERVAL <= 0 || (SECONDS_ > 0 && INTERVAL >= SECONDS_) )); then
  echo "timedemo-compare: the screenshot interval must allow at least one capture" >&2
  exit 2
fi
XVFB=()
[ $USEXVFB -eq 1 ] && XVFB=(xvfb-run -a -s "-screen 0 ${W}x${H}x24")
STOP=()
case $LIMIT in ''|*[!0-9]*) echo "timedemo-compare: -T needs a number of seconds" >&2; exit 2 ;; esac
if [ "$LIMIT" -gt 0 ]; then
  command -v timeout > /dev/null || { echo "timedemo-compare: -T needs the timeout command" >&2; exit 2; }
  STOP=(timeout -k 10 "$LIMIT") # (a client that hangs outside its event loop doesn't end on the first signal)
fi
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd) || exit 2

run() # <label> <client and options>
{
  local home="$OUT/$1" renderer="" arg backend completion=finished second shot
  local -a client shots
  read -r -a client <<< "$2"
  [ -n "${client[0]:-}" ] || { echo "timedemo-compare: missing client for run $1" >&2; exit 2; }
  for arg in "${client[@]}"; do
    case $arg in --gl|--gpu) renderer=$arg ;; esac
  done
  rm -rf "$home" && mkdir -p "$home/config" "$home/demos" && cp "$DEMO" "$home/demos/$NAME.dmo" || exit 2
  # no first-run dialogs, no sound, a window of a fixed size, lossless screenshots
  printf "autodownload 0\naudio 0\nfullscreen 0\nscr_w %s\nscr_h %s\nvsync 0\nscreenshottype 2\n" "$W" "$H" > "$home/config/init.cfg" || exit 2
  # quit when it ends; no first-start welcome sequence
  printf "onTimedemoEnd = [ quit ]\nmapstartonce = []\n" > "$home/config/autoexec.cfg" || exit 2
  # (the console, the fps counter and the HUD messages differ between runs, so they're hidden)
  ${XVFB[@]+"${XVFB[@]}"} ${STOP[@]+"${STOP[@]}"} "${client[@]}" --home="$home" --init -t0 -w"$W" -h"$H" -v0 \
    "-emaxfps 0; showstats 0; consize 0; hidehudmsgs 1; timedemo $NAME $SECONDS_ $INTERVAL" > "$home/output.txt" 2>&1
  local status=$?
  [ $status -ne 124 ] && [ $status -ne 137 ] || { echo "timedemo-compare: run $1 was stopped after $LIMIT s (see $home/output.txt)" >&2; exit 2; }
  [ $status -eq 0 ] || { echo "timedemo-compare: run $1 exited with status $status (see $home/output.txt)" >&2; exit 2; }
  grep -E '^(timedemo|Renderer|renderer):' "$home/output.txt" | sed "s/^/  [$1] /"

  backend=$(sed -n 's/^renderer: SDL GPU (\([^)]*\)),.*/\1/p' "$home/output.txt")
  case $renderer in
    --gpu) [ -n "$backend" ] && [ "$backend" = "${SDL_GPU_DRIVER:-$backend}" ] ;;
    --gl) [ -z "$backend" ] && grep -q '^Renderer: ' "$home/output.txt" ;;
    *) grep -q '^Renderer: ' "$home/output.txt" ;;
  esac || { echo "timedemo-compare: run $1 did not start the requested renderer (see $home/output.txt)" >&2; exit 2; }

  if (( SECONDS_ == 0 )); then completion=ended; fi
  grep -Fq "timedemo: $NAME $completion after " "$home/output.txt" ||
    { echo "timedemo-compare: run $1 did not complete the demo (see $home/output.txt)" >&2; exit 2; }

  for ((second = INTERVAL; second < SECONDS_; second += INTERVAL)); do
    printf -v shot 'timedemo_%s_%06d.png' "$NAME" "$((second * 1000))"
    [ -s "$home/screenshots/$shot" ] ||
      { echo "timedemo-compare: run $1 is missing $shot (see $home/output.txt)" >&2; exit 2; }
  done
  shots=("$home"/screenshots/timedemo_*.png)
  [ -s "${shots[0]}" ] || { echo "timedemo-compare: run $1 saved no screenshots (see $home/output.txt)" >&2; exit 2; }
  if (( SECONDS_ > 0 && ${#shots[@]} != (SECONDS_ - 1) / INTERVAL )); then
    echo "timedemo-compare: run $1 saved an unexpected set of screenshots (see $home/output.txt)" >&2
    exit 2
  fi
}

echo "timedemo-compare: $NAME, $SECONDS_ s, a screenshot every $INTERVAL s, ${W}x${H}"
run a "$RUNA"
run b "$RUNB"
a_shots=("$OUT"/a/screenshots/timedemo_*.png)
b_shots=("$OUT"/b/screenshots/timedemo_*.png)
[ "${#a_shots[@]}" -eq "${#b_shots[@]}" ] || { echo "timedemo-compare: runs saved different numbers of screenshots" >&2; exit 2; }
fail=0; n=0
for shot in "${a_shots[@]}"; do
  f=$(basename "$shot")
  [ -s "$OUT/b/screenshots/$f" ] || { echo "timedemo-compare: run b is missing $f" >&2; exit 2; }
  printf "  %s: " "$f"
  "$IMGCOMPARE" "$shot" "$OUT/b/screenshots/$f" "$TOL" "$MIN" "$OUT/diff_$f"
  case $? in 0) ;; 1) fail=1 ;; *) exit 2 ;; esac
  n=$((n + 1))
done
if [ $fail -ne 0 ]; then echo "timedemo-compare: some screenshots differ by more than allowed (difference images in $OUT)"; exit 1; fi
echo "timedemo-compare: all $n screenshot pairs have at least $MIN% of pixels within $TOL"
