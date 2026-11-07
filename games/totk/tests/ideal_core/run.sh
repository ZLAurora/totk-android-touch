#!/bin/sh
# Production SVC dispatcher core-id semantics; pass an absolute output directory under .scratch.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
COMMON="-D_GNU_SOURCE -DDM_THREADS_HOST_TEST -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-format -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -ffunction-sections -fdata-sections -pthread"
$CC $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
    "$ROOT/tests/ideal_core/probe.c" "$ROOT/runtime/threads.c" "$ROOT/tests/guest_waits/host_support.c" \
    -Wl,--gc-sections -o "$OUT/ideal-core"
for fix in 0 1; do
    for free in 0 1; do
        TOTK_IDEAL_CORE_FIX=$fix TOTK_FREE_CORES=$free TOTK_ASYNC_PREEMPT=0 timeout 60 "$OUT/ideal-core"
    done
done
