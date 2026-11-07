#!/bin/sh
# ab.sh -s <serial> --device <label> --scene <label> --arms 'A:<lines>;B:<lines>' [--order ABBAABBA]
#       [--pan-seconds 30] [--settle 15] [--value 16000] [--restore '<lines>'] --out <dir>
#       [--event /dev/input/eventN] [--display <id>] [--package <pkg>] [--game-dir <dir>]
#
# In-session A/B in ONE loaded scene with live switches (tools/bench/live.sh, runtime/live_ctl.h): for each letter of
# --order, applies that arm's lines and the mark "ab arm=<X> block=<n> scene=<scene> direction=<right|left>" in one write,
# waits for the game to acknowledge it, settles --settle seconds, then pans with pan.sh (right first on odd blocks, left
# first on even ones, so the view returns and the direction alternates). Arm lines are live.sh lines separated by
# commas, e.g. --arms 'A:unset TOTK_VK_DESC_STOCK;B:set TOTK_VK_DESC_STOCK=1'. At the end it applies --restore (if
# given) with the mark "ab done scene=<scene>".
# Appends per block to <out>/arms.txt: <device> <scene> block=<n> arm=<X> direction=<right|left> gen=<N> frame=<F> T=<s> lines=<...>
# and the pan lines to <out>/pans.txt / freqs.txt (pan.sh). Load and settle the scene and start sampler.py first;
# collect.sh afterwards, then analyze.py <out> --arms and abtest.py.
# Use ABBAABBA or longer on 60 fps devices: same-direction pairs separate the switch effect from view content.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
die() { echo "ab: $*" >&2; exit 1; }

SER=""; DEV=""; SCENE=""; ARMS=""; ORDER=ABBAABBA; PAN=30; SETTLE=15; V=16000; RESTORE=""; OUT=""; EV=""; DISP=""
PKG=com.aure.totk; GAME=""
while [ $# -gt 0 ]; do
  case $1 in
    -s) SER=${2:?}; shift 2 ;;
    --device) DEV=${2:?}; shift 2 ;;
    --scene) SCENE=${2:?}; shift 2 ;;
    --arms) ARMS=${2:?}; shift 2 ;;
    --order) ORDER=${2:?}; shift 2 ;;
    --pan-seconds) PAN=${2:?}; shift 2 ;;
    --settle) SETTLE=${2:?}; shift 2 ;;
    --value) V=${2:?}; shift 2 ;;
    --restore) RESTORE=${2:?}; shift 2 ;;
    --out) OUT=${2:?}; shift 2 ;;
    --event) EV=${2:?}; shift 2 ;;
    --display) DISP=${2:?}; shift 2 ;;
    --package) PKG=${2:?}; shift 2 ;;
    --game-dir) GAME=${2:?}; shift 2 ;;
    -h|--help) usage ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$SER" ] && [ -n "$DEV" ] && [ -n "$SCENE" ] && [ -n "$ARMS" ] && [ -n "$OUT" ] || usage
case $DEV$SCENE in *[!A-Za-z0-9_-]*) die "--device / --scene: letters, digits, _ and - only" ;; esac
case $ORDER in ""|*[!A-Z]*) die "--order: arm letters A-Z, e.g. ABBAABBA" ;; esac
case $PAN$SETTLE in *[!0-9]*) die "--pan-seconds / --settle: whole seconds" ;; esac
[ -z "$GAME" ] && GAME=/data/user/0/$PKG/files/game
COMMON="-s $SER --package $PKG --game-dir $GAME"

# arm_lines X: the arm's lines, one per line ('' when the arm is not defined).
arm_lines() {
  printf '%s\n' "$ARMS" | tr ';' '\n' | while IFS= read -r spec; do
    case $spec in "$1":*) printf '%s\n' "${spec#"$1":}" | tr ',' '\n' | sed 's/^ *//; s/ *$//' | grep -v '^$' ;; esac
  done
}
for X in $(printf '%s\n' "$ORDER" | fold -w1 | sort -u); do
  [ -n "$(arm_lines "$X")" ] || die "arm $X of --order has no lines in --arms"
done
# live LINE...: one live.sh write; prints the game's acknowledgement.
live() { "$HERE/live.sh" $COMMON --wait 30 "$@"; }

# The controller node and display once, not before every pan.
if [ -z "$EV" ] || [ -z "$DISP" ]; then
  det=$("$HERE/bench_input.sh" -s "$SER" ${EV:+--event "$EV"} ${DISP:+--display "$DISP"} detect) || exit 1
  EV=${det#event=}; EV=${EV%% *}; DISP=${det##*display=}
fi
mkdir -p "$OUT"
n=$(printf '%s' "$ORDER" | wc -c)
echo "ab: $n blocks ($ORDER) in $SCENE on $DEV: about $(( n * (SETTLE + PAN + 12) / 60 + 1 )) min (settle ${SETTLE}s, pan ${PAN}s each)"
block=0
for X in $(printf '%s\n' "$ORDER" | fold -w1); do
  block=$((block + 1))
  [ $((block % 2)) -eq 1 ] && first=right || first=left
  set --; while IFS= read -r line; do set -- "$@" "$line"; done <<EOF
$(arm_lines "$X")
EOF
  ack=$(live "$@" "mark ab arm=$X block=$block scene=$SCENE direction=$first"); rc=$?
  [ "$rc" -eq 4 ] && die "block $block (arm $X): a line was refused: $ack"
  [ "$rc" -eq 0 ] || die "block $block (arm $X) was not applied: $ack"
  gf=$(printf '%s\n' "$ack" | grep "mark ab arm=$X block=$block " | sed -n 's/.*\] gen=\([0-9]*\) frame=\([0-9]*\) mark.*/gen=\1 frame=\2/p' | tail -n1)
  echo "$DEV $SCENE block=$block arm=$X direction=$first $gf T=$(date +%s) lines=$(arm_lines "$X" | paste -sd, -)" | tee -a "$OUT/arms.txt"
  sleep "$SETTLE"
  "$HERE/pan.sh" -s "$SER" --device "$DEV" --scene "$SCENE" --seconds "$PAN" --value "$V" --first "$first" --out "$OUT" \
    --package "$PKG" --game-dir "$GAME" ${EV:+--event "$EV"} ${DISP:+--display "$DISP"} || die "pan of block $block failed"
done
set --
if [ -n "$RESTORE" ]; then
  while IFS= read -r line; do [ -n "$line" ] && set -- "$@" "$line"; done <<EOF
$(printf '%s\n' "$RESTORE" | tr ',' '\n' | sed 's/^ *//; s/ *$//')
EOF
fi
live "$@" "mark ab done scene=$SCENE" >/dev/null || echo "ab: the final mark was not acknowledged" >&2
echo "ab: done; next: collect.sh -s $SER --device $DEV --out $OUT, then analyze.py $OUT --arms --json $OUT/ab.json and abtest.py $OUT/ab.json"
