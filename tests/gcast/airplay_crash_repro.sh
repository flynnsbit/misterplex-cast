#!/usr/bin/env bash
# Host ASan replay of the iPhone16,1 AirPlay crash path against airplay_fe_harness.
# usage: tests/gcast/airplay_crash_repro.sh [HARNESS] [PORT]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
H=${1:-${AIRPLAY_FE_HARNESS:?set AIRPLAY_FE_HARNESS}}
PORT=${2:-18777}
SENDER=$ROOT/tools/gcast/ios18_rtsp_replay.py
if [ ! -x "$H" ]; then echo "no harness at $H"; exit 2; fi
ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=0 \
  "$H" "gcast-crash-repro" "$PORT" >/tmp/ap-crash-repro.log 2>&1 &
pid=$!; sleep 1
rc=0
python3 "$SENDER" 127.0.0.1 "$PORT" all || rc=$?
if kill -0 $pid 2>/dev/null; then echo "HARNESS_ALIVE"; kill $pid; wait $pid 2>/dev/null || true
else echo "HARNESS_DEAD"; wait $pid || true; exit 1; fi
exit $rc
