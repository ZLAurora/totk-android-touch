#!/bin/sh
# Production thread-start helper on the host: TOTK_TIMER_SLACK_NS semantics plus nanosleep lateness numbers.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${1:?pass an absolute .scratch output directory}
case "$OUT" in /*/.scratch/*) ;; *) echo "output must be an absolute path under .scratch" >&2; exit 2;; esac
mkdir -p "$OUT"
CC=${CC:-cc}
$CC -D_GNU_SOURCE -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-misleading-indentation -pthread -I"$ROOT/runtime" \
    "$ROOT/tests/timer_slack/probe.c" -o "$OUT/timer-slack"
"$OUT/timer-slack"
