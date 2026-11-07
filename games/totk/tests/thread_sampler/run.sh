#!/bin/sh
# Thread sampler (runtime/thread_sampler.h) on the host, plain and under ThreadSanitizer (when the compiler has it):
# a synthetic /proc task tree, this process's own threads, and a frame thread toggling the switch (race).
# Pass an absolute output directory under .scratch.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
FLAGS="-D_GNU_SOURCE -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -I$ROOT/runtime"
$CC $FLAGS "$ROOT/tests/thread_sampler/probe.c" -o "$OUT/thread-sampler"
TSAN=""
if $CC $FLAGS -fsanitize=thread "$ROOT/tests/thread_sampler/probe.c" -o "$OUT/thread-sampler-tsan" 2>/dev/null; then TSAN=1
else echo "skip: $CC has no -fsanitize=thread"; fi
for mode in synthetic real race; do
    mkdir -p "$OUT/plain-$mode"; "$OUT/thread-sampler" $mode "$OUT/plain-$mode"
    if [ -n "$TSAN" ]; then
        mkdir -p "$OUT/tsan-$mode"; TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$OUT/thread-sampler-tsan" $mode "$OUT/tsan-$mode"
    fi
done
