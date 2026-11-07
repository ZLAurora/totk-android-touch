#!/bin/sh
# pan.sh -s <serial> --device <label> --scene <label> --out <dir> [--target-fps 30|60] [--seconds N] [--repeat N]
#        [--value 16000] [--first right|left] [--no-shot] [--event /dev/input/eventN] [--display <id>]
#        [--package <pkg>] [--game-dir <dir>]
#
# Steady horizontal camera pan: the right stick held at --value (of 32767) to the right for the first half of the time,
# then to the left for the second half (--first left: left, then right), then released, so the view returns.
# Defaults: infer the preset target (settings.env plus totk.env override); 30 uses 48 seconds and 60 uses 144 seconds
# to retain twelve complete windows even near 30 fps. Unknown target uses 48 seconds without claiming a pacing target.
# --target-fps overrides detection; --seconds overrides duration; --repeat runs N pans without another settle.
# Appends one line per pan to <out>/pans.txt (TARGET_FPS lets the analyzer warn when fewer than twelve windows survive):
#   <device> <scene> F0=<before> F1=<after> T0=<epoch s> T1=<epoch s> V=<value> D=<1|-1> TARGET_FPS=<30|60>
# (D: 1 right first, -1 left first) and two lines to <out>/freqs.txt, the frequency limits (`bench_input.sh freqs`)
# just before and just after the pan, plus raw cumulative GPU counters at both boundaries in gpu-time-in-state.jsonl:
#   <device> <scene> T0=<epoch s> start|end <cpuN=... gpu_...>
# Each GPU record has the same device/scene/T0/boundary identity; missing counters are recorded explicitly, not inferred
# from sampled clocks. A screenshot is taken after release (outside the measured windows) into
# <out>/shots/<device>-<scene>-pan-<T0>.png unless --no-shot. Root commands use the persistent root shell (rootsh.py).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)

usage() { sed -n '2,/^set -u/{ /^set -u/d; s/^# \{0,1\}//; p; }' "$0" >&2; exit 2; }
die() { echo "pan: $*" >&2; exit 1; }

SER=""; DEV=""; SCENE=""; SECS=""; V=16000; FIRST=right; OUT=""; EV=""; DISP=""; PKG=com.aure.totk; GAME=""; SHOT=1
TARGET_FPS=""; REPEAT=1
while [ $# -gt 0 ]; do
  case $1 in
    -s) SER=${2:?}; shift 2 ;;
    --device) DEV=${2:?}; shift 2 ;;
    --scene) SCENE=${2:?}; shift 2 ;;
    --seconds) SECS=${2:?}; shift 2 ;;
    --target-fps) TARGET_FPS=${2:?}; shift 2 ;;
    --repeat) REPEAT=${2:?}; shift 2 ;;
    --value) V=${2:?}; shift 2 ;;
    --first) FIRST=${2:?}; shift 2 ;;
    --no-shot) SHOT=""; shift ;;
    --out) OUT=${2:?}; shift 2 ;;
    --event) EV=${2:?}; shift 2 ;;
    --display) DISP=${2:?}; shift 2 ;;
    --package) PKG=${2:?}; shift 2 ;;
    --game-dir) GAME=${2:?}; shift 2 ;;
    -h|--help) usage ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$SER" ] && [ -n "$DEV" ] && [ -n "$SCENE" ] && [ -n "$OUT" ] || usage
case $TARGET_FPS in ''|30|60) ;; *) die "--target-fps: 30 or 60" ;; esac
case $REPEAT in ''|*[!0-9]*) die "--repeat: a positive integer" ;; esac
[ "$REPEAT" -gt 0 ] || die "--repeat: a positive integer"
case $DEV$SCENE in *[!A-Za-z0-9_-]*) die "--device / --scene: letters, digits, _ and - only" ;; esac
case $FIRST in right) D=1 ;; left) D=-1 ;; *) die "--first: right or left" ;; esac
[ -n "$GAME" ] || GAME=/data/user/0/$PKG/files/game
LOG=$GAME/totk.log
if [ -z "$TARGET_FPS" ]; then
  PRESET=$(python3 "$HERE/rootsh.py" -s "$SER" run -c \
    "cat /data/user/0/$PKG/files/settings.env $GAME/totk.env 2>/dev/null | grep '^TOTK_FPS60=' | tail -n1 | cut -d= -f2" | tr -d '\r')
  case $PRESET in 1) TARGET_FPS=60 ;; 0) TARGET_FPS=30 ;; esac
fi
[ -n "$SECS" ] || { if [ "$TARGET_FPS" = 60 ]; then SECS=144; else SECS=48; fi; }
HALF=$(awk -v seconds="$SECS" 'BEGIN {
  if (seconds !~ /^([0-9]+([.][0-9]*)?|[.][0-9]+)$/ || seconds <= 0) exit 1
  print seconds / 2
}') || die "--seconds: a positive number"

adb -s "$SER" shell pidof "$PKG" >/dev/null 2>&1 || die "$PKG is not running on $SER: start the game and load the scene first"
if [ -z "$EV" ] || [ -z "$DISP" ]; then
  det=$("$HERE/bench_input.sh" -s "$SER" ${EV:+--event "$EV"} ${DISP:+--display "$DISP"} detect) || exit 1
  EV=${det#event=}; EV=${EV%% *}; DISP=${det##*display=}
fi
input() { "$HERE/bench_input.sh" -s "$SER" --event "$EV" --display "$DISP" "$@"; }
lastf() {
  python3 "$HERE/rootsh.py" -s "$SER" run -c "grep -ao '至帧 [0-9]*' $LOG | tail -n1" | tr -d '\r' | grep -o '[0-9][0-9]*'
}
gpu_snapshot() {
  python3 "$HERE/sampler.py" -s "$SER" --device "$DEV" --out "$OUT" \
    --gpu-snapshot --scene "$SCENE" --t0 "$T0" --boundary "$1" ||
    echo "pan: GPU $1 snapshot unavailable (see gpu-time-in-state.jsonl)" >&2
}

mkdir -p "$OUT/shots"
trap 'input rstick 0' EXIT
trap 'exit 130' INT TERM
I=1; PREV_T0=0
while [ "$I" -le "$REPEAT" ]; do
  F0=$(lastf) || die "no '至帧 N' timing line in $LOG yet (has the game rendered 300 frames?)"
  FS=$(input freqs) || die "cannot read the frequency limits"
  T0=$(date +%s)
  # Keep epoch-second identities distinct even for explicit subsecond repeats.
  while [ "$T0" -le "$PREV_T0" ]; do sleep 1; T0=$(date +%s); done
  PREV_T0=$T0
  gpu_snapshot start
  input rstick "$((D * V))" || die "cannot start the camera pan"
  sleep "$HALF" || die "camera pan sleep interrupted"
  input rstick "$((-D * V))" || die "cannot reverse the camera pan"
  sleep "$HALF" || die "camera pan sleep interrupted"
  F1=$(lastf) || die "cannot read the final frame boundary"
  T1=$(date +%s)
  gpu_snapshot end
  input rstick 0 || die "cannot release the camera pan"
  FE=$(input freqs) || echo "pan: cannot read the frequency limits after the pan" >&2
  printf '%s\n' "$DEV $SCENE T0=$T0 start $FS" "$DEV $SCENE T0=$T0 end $FE" >> "$OUT/freqs.txt"
  echo "$DEV $SCENE F0=$F0 F1=$F1 T0=$T0 T1=$T1 V=$V D=$D${TARGET_FPS:+ TARGET_FPS=$TARGET_FPS}" | tee -a "$OUT/pans.txt"
  [ -z "$SHOT" ] || input shot "$OUT/shots/$DEV-$SCENE-pan-$T0.png" >/dev/null || echo "pan: screenshot failed (continuing)" >&2
  I=$((I + 1))
done
trap - EXIT
