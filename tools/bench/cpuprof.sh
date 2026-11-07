#!/bin/sh
# Rooted Android CPU capture; all policy and recovery live in the Python helper.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$HERE/cpuprof_capture.py" "$@"
