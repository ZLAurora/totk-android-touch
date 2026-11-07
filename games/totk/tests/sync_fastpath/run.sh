#!/bin/sh
# All outputs live under the supplied .scratch directory; CC may include flags.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
COMMON="-D_GNU_SOURCE -DDM_THREADS_HOST_TEST -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-format -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -ffunction-sections -fdata-sections -pthread"
# Intentional command/flag splitting, as in make's CC convention.
$CC $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
    "$ROOT/tests/sync_fastpath/probe.c" "$ROOT/tests/guest_waits/host_support.c" -Wl,--gc-sections -o "$OUT/sync-fastpath"
$CC $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
    "$ROOT/tests/sync_fastpath/preempt.c" -Wl,--gc-sections -o "$OUT/sync-preempt"
$CC $COMMON -I"$ROOT/android/compat/include" -I"$ROOT/runtime" -I"$ROOT/third_party/d2r_gfx/gfx/nvn/include" \
    "$ROOT/tests/sync_fastpath/event.c" -Wl,--gc-sections -o "$OUT/core-wait-event"
for event in 0 1; do
    for kick in 0 1; do
        TOTK_CORE_WAIT_EVENT=$event TOTK_SYNC_KICK=$kick TOTK_SYNC_STATS=1 "$OUT/sync-fastpath" "${SYNC_ROUNDS:-3000}"
        TOTK_CORE_WAIT_EVENT=$event TOTK_SYNC_KICK=$kick TOTK_SYNC_STATS=1 "$OUT/sync-preempt" "${SYNC_PREEMPT_ROUNDS:-1000}"
    done
done
for kick in 0 1; do
    TOTK_SYNC_KICK=$kick TOTK_SYNC_STATS=1 "$OUT/core-wait-event"
done
