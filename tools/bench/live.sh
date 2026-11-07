#!/bin/sh
# live.sh -s <serial> [--package <pkg>] [--game-dir <dir>] [--wait S] <command>...
#   commands: set KEY=VALUE | unset KEY | mark <text...> (mark takes the rest of the arguments), or whole quoted lines
#   e.g. live.sh -s X set TOTK_VK_DESC_STOCK=1 mark ab arm=B block=2
#        live.sh -s X 'unset TOTK_VK_DESC_STOCK' 'mark ab arm=A block=1'
#
# Publishes the lines as one atomic replacement of <game>/totk.ctl (runtime/live_ctl.h), so they apply at the same
# frame boundary (one generation). Root (the serial's persistent root shell, rootsh.py) prepares a temp file with
# the game folder's owner, mode 0600 and SELinux context before rename, so it is readable on first appearance.
# Then waits up to --wait seconds (default 10) for the game's [totk-ctl]
# lines in totk.log and prints them: the mark's line when there is a mark, refusals always. Exit 3 when a mark was not
# acknowledged in time, 4 when the game refused a line (the other lines of the write still apply).
set -u
usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
die() { echo "live: $*" >&2; exit 1; }

SER=""; PKG=com.aure.totk; GAME=""; WAIT=10; LINES=""; MARK=""
add() {   # one validated control line
  case $1 in
    "set "*) kv=${1#set }; k=${kv%%=*}; v=${kv#*=}
      case $kv in *=*) ;; *) die "set needs KEY=VALUE: $1" ;; esac
      case $k in ""|*[!A-Z0-9_]*) die "malformed key: $1" ;; esac
      case $v in ""|*[!0-9+-]*) die "value must be a decimal: $1" ;; esac ;;
    "unset "*) k=${1#unset }; case $k in ""|*[!A-Z0-9_]*) die "malformed key: $1" ;; esac ;;
    "mark "*) MARK=${1#mark }; case $MARK in *[!A-Za-z0-9_=.:/,+@\ -]*) die "mark text: letters, digits, space and _=.:/,+@- only" ;; esac ;;
    *) die "unknown command: $1 (set KEY=VALUE, unset KEY, mark <text>)" ;;
  esac
  LINES="$LINES$1
"
}
while [ $# -gt 0 ]; do
  case $1 in
    -s) SER=${2:?}; shift 2 ;;
    --package) PKG=${2:?}; shift 2 ;;
    --game-dir) GAME=${2:?}; shift 2 ;;
    --wait) WAIT=${2:?}; shift 2 ;;
    -h|--help) usage ;;
    set|unset) [ $# -ge 2 ] || usage; add "$1 $2"; shift 2 ;;
    mark) shift; [ $# -ge 1 ] || usage; add "mark $*"; shift $# ;;
    *" "*) add "$1"; shift ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$SER" ] && [ -n "$LINES" ] || usage
[ -n "$GAME" ] || GAME=/data/user/0/$PKG/files/game
CTL=$GAME/totk.ctl; LOG=$GAME/totk.log
case $GAME in *[!A-Za-z0-9_./-]*) die "--game-dir: unexpected characters" ;; esac
adb -s "$SER" shell pidof "$PKG" >/dev/null 2>&1 || die "$PKG is not running on $SER"

HERE=$(cd "$(dirname "$0")" && pwd)
root() { python3 "$HERE/rootsh.py" -s "$SER" run -c "$1"; }   # the serial's persistent root shell
# Lines already in the log, so only the answer to this write is shown.
N0=$(root "grep -ac '\\[totk-ctl\\]' $LOG" 2>/dev/null | tr -d '\r')
case $N0 in ""|*[!0-9]*) N0=0 ;; esac
# Publish only this batch, not a copy of old commands: replacement is read from the beginning. Prepare owner and
# context before rename so neither an empty nor an unreadable root-owned control file is ever exposed.
# Keep the final newline: the game stages complete lines only. Validated lines cannot contain the delimiter.
REMOTE="umask 077
tmp=\$(mktemp $GAME/.totk.ctl.XXXXXX) || exit 1
trap 'rm -f \"\$tmp\"' EXIT
cat > \"\$tmp\" <<'TOTK_CTL_END'
${LINES}TOTK_CTL_END
chown \$(stat -c %u:%g $GAME) \"\$tmp\" && chmod 0600 \"\$tmp\" || exit 1
CTX=\$(ls -Zd $GAME) || exit 1
CTX=\${CTX%% *}
case \$CTX in *:*:*:*) ;; *) echo 'cannot read game SELinux context' >&2; exit 1 ;; esac
chcon \"\$CTX\" \"\$tmp\" || exit 1
mv -f \"\$tmp\" $CTL"
root "$REMOTE" || die "could not write $CTL (root, owner or SELinux context)"

end=$(( $(date +%s) + WAIT )); shown=0; acked=""
while :; do
  new=$(root "grep -a '\\[totk-ctl\\]' $LOG" 2>/dev/null | tr -d '\r' | tail -n +$((N0 + 1)))
  if [ -n "$new" ]; then
    printf '%s\n' "$new" | tail -n +$((shown + 1))
    shown=$(printf '%s\n' "$new" | wc -l)
    if [ -n "$MARK" ]; then
      printf '%s\n' "$new" | grep -q "\] gen=[0-9]* frame=[0-9]* mark $MARK\$" && { acked=1; break; }
    fi
  fi
  [ "$(date +%s)" -ge "$end" ] && break
  sleep 0.5
done
[ -z "$MARK" ] || [ -n "$acked" ] || { echo "live: the game did not acknowledge 'mark $MARK' within ${WAIT}s (paused, in a menu without frames, or not reading $CTL?)" >&2; exit 3; }
printf '%s\n' "${new:-}" | grep -q '^\[totk-ctl\] refused ' && exit 4
exit 0
