#!/bin/sh
# Live-switch channel (runtime/live_ctl.h) on the host, plain and under ThreadSanitizer (when the compiler has it):
# the channel itself (core) and every live switch toggled many times (switches).
# Pass an absolute output directory under .scratch.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
FLAGS="-D_GNU_SOURCE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-misleading-indentation -pthread -I$ROOT/runtime"
$CC $FLAGS "$ROOT/tests/live_ctl/probe.c" -o "$OUT/live-ctl"
TSAN=""
if $CC $FLAGS -fsanitize=thread "$ROOT/tests/live_ctl/probe.c" -o "$OUT/live-ctl-tsan" 2>/dev/null; then TSAN=1
else echo "skip: $CC has no -fsanitize=thread"; fi
for mode in core switches; do
    mkdir -p "$OUT/plain-$mode"; "$OUT/live-ctl" $mode "$OUT/plain-$mode"
    if [ -n "$TSAN" ]; then
        mkdir -p "$OUT/tsan-$mode"; TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$OUT/live-ctl-tsan" $mode "$OUT/tsan-$mode"
    fi
done
