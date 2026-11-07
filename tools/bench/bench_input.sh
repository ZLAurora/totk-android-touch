#!/bin/sh
# bench_input.sh -s <serial> [--event /dev/input/eventN] [--display <id>] [--package <pkg>] [--game-dir <dir>] <action> ...
#
# Drive the game on a rooted Android device over adb (sendevent needs root). Root commands, the screenshot included,
# go through the serial's persistent root shell (rootsh.py: one su per session, pinned to the slowest CPU cluster).
#   press <button> [ms]   hold a built-in controller button (A B X Y L R PLUS MINUS UP DOWN LEFT RIGHT), default 150 ms
#   rstick <x> [y]        set the right stick (ABS_Z / ABS_RZ, -32767..32767); "rstick 0" releases it
#   shot <file>           screenshot of the main display, written to <file> on the host
#   log                   print the game's totk.log
#   detect                print the detected "event=<node> display=<id>" and exit
#   freqs                 print the frequency limits as one line of tokens: per cpufreq policy N
#                         cpuN=<related cpus>:<scaling_min>:<scaling_max>:<cpuinfo_max> (kHz; scaling_max = effective cap,
#                         cpuinfo_max = hardware max), GPU gpu_maxclk (kgsl max_gpuclk, Hz), gpu_freqs (available, Hz),
#                         gpu_devfreq_max, gpu_min_pwrlevel / gpu_max_pwrlevel / gpu_thermal_pwrlevel (absent ones omitted)
# The controller node is the input device whose `getevent -p` lists ABS_Z and ABS_RZ (0002 / 0005) and BTN_SOUTH (0130);
# the display is the first (internal) one of `dumpsys SurfaceFlinger --display-id`.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
die() { echo "bench_input: $*" >&2; exit 1; }

# Device-side script of `freqs` (fed to a root shell on stdin; prints one line).
FREQS_SH=$(cat <<'EOF'
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  r=$(echo $(cat $p/related_cpus 2>/dev/null) | tr ' ' ,)
  printf 'cpu%s=%s:%s:%s:%s ' "${p##*policy}" "$r" "$(cat $p/scaling_min_freq 2>/dev/null)" \
    "$(cat $p/scaling_max_freq 2>/dev/null)" "$(cat $p/cpuinfo_max_freq 2>/dev/null)"
done
K=/sys/class/kgsl/kgsl-3d0
D=""; for d in /sys/class/devfreq/*; do case ${d##*/} in *kgsl*|*gpu*|*mali*) D=$d; break;; esac; done
v=$(cat $K/max_gpuclk 2>/dev/null); [ -n "$v" ] && printf 'gpu_maxclk=%s ' "$v"
f=$(cat $K/gpu_available_frequencies 2>/dev/null); [ -n "$f" ] || [ -z "$D" ] || f=$(cat $D/available_frequencies 2>/dev/null)
[ -n "$f" ] && printf 'gpu_freqs=%s ' "$(echo $f | tr ' ' ,)"
[ -n "$D" ] && v=$(cat $D/max_freq 2>/dev/null) && [ -n "$v" ] && printf 'gpu_devfreq_max=%s ' "$v"
for k in min_pwrlevel max_pwrlevel thermal_pwrlevel; do
  v=$(cat $K/$k 2>/dev/null); [ -n "$v" ] && printf 'gpu_%s=%s ' $k "$v"
done
echo
EOF
)

SER=""; EV=""; DISP=""; PKG=com.aure.totk; GAME=""
while [ $# -gt 0 ]; do
  case $1 in
    -s) SER=${2:?}; shift 2 ;;
    --event) EV=${2:?}; shift 2 ;;
    --display) DISP=${2:?}; shift 2 ;;
    --package) PKG=${2:?}; shift 2 ;;
    --game-dir) GAME=${2:?}; shift 2 ;;
    -h|--help) usage ;;
    -*) die "unknown option $1" ;;
    *) break ;;
  esac
done
[ -n "$SER" ] && [ $# -ge 1 ] || usage
[ -n "$GAME" ] || GAME=/data/user/0/$PKG/files/game
ACT=$1; shift

adbs() { adb -s "$SER" "$@"; }
root() { python3 "$HERE/rootsh.py" -s "$SER" run -c "$1"; }

detect_event() {
  root "getevent -p" | tr -d '\r' | awk '
    function done_dev() { if (dev != "" && south && z && rz && found == "") found = dev }
    /^add device/ { done_dev(); dev = $NF; south = z = rz = 0; sect = ""; next }
    /^ *[A-Z]+ +\([0-9a-f]+\):/ { sect = "" }
    /^ *KEY \(0001\):/ { sect = "KEY" } /^ *ABS \(0003\):/ { sect = "ABS" }
    sect == "KEY" { for (i = 1; i <= NF; i++) if ($i == "0130") south = 1 }
    sect == "ABS" { sub(/^ *ABS \(0003\): */, ""); if ($1 == "0002") z = 1; if ($1 == "0005") rz = 1 }
    END { done_dev(); print found }'
}
detect_display() {
  adbs shell dumpsys SurfaceFlinger --display-id | tr -d '\r' | awk '/^Display [0-9]+/ { print $2; exit }'
}
need_event() {
  [ -n "$EV" ] || EV=$(detect_event)
  [ -n "$EV" ] || die "no controller input node found (getevent -p: ABS_Z/ABS_RZ + BTN_SOUTH); give --event"
}
need_display() {
  [ -n "$DISP" ] || DISP=$(detect_display)
  [ -n "$DISP" ] || die "no display id from dumpsys SurfaceFlinger --display-id; give --display"
}

case $ACT in
  detect)
    need_event; need_display; echo "event=$EV display=$DISP" ;;
  press)
    [ $# -ge 1 ] || usage
    case $1 in
      A) c=304 ;; B) c=305 ;; X) c=307 ;; Y) c=308 ;; L) c=310 ;; R) c=311 ;; MINUS) c=314 ;; PLUS) c=315 ;;
      UP) c=544 ;; DOWN) c=545 ;; LEFT) c=546 ;; RIGHT) c=547 ;;
      *) die "unknown button $1" ;;
    esac
    s=$(awk "BEGIN { print ${2:-150} / 1000 }")
    need_event
    root "sendevent $EV 1 $c 1; sendevent $EV 0 0 0; sleep $s; sendevent $EV 1 $c 0; sendevent $EV 0 0 0" ;;
  rstick)
    [ $# -ge 1 ] || usage
    need_event
    root "sendevent $EV 3 2 $1; sendevent $EV 3 5 ${2:-0}; sendevent $EV 0 0 0" ;;
  shot)
    [ $# -ge 1 ] || usage
    need_display
    mkdir -p "$(dirname "$1")"
    root "screencap -d $DISP -p" > "$1" || die "screencap failed"
    [ -s "$1" ] || die "empty screenshot $1"
    echo "$1" ;;
  log)
    out=$(root "cat $GAME/totk.log" 2>&1) || die "cannot read $GAME/totk.log: $out"
    printf '%s\n' "$out" ;;
  freqs)
    out=$(printf '%s\n' "$FREQS_SH" | python3 "$HERE/rootsh.py" -s "$SER" run | tr -d '\r')
    case $out in *cpu[0-9]*=*) printf '%s\n' "$out" ;; *) die "no cpufreq limits read (root / sysfs): '$out'" ;; esac ;;
  *) die "unknown action $ACT" ;;
esac
