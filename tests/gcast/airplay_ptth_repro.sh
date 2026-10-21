#!/usr/bin/env bash
# FIELD-1018 F2: replay the iOS 18 AirPlay URL session (POST /reverse PTTH, the iPhone's "HTTP/1.1 200 OK" answers on
# the reverse socket, bplist /play, /rate /playback-info /scrub polling, /stop) against airplay_fe_harness.
# usage: tests/gcast/airplay_ptth_repro.sh FIXED_HARNESS [MUTANT_HARNESS] [PORT]
#   Build both with UXPLAY_SRC=<tree patched by apply_gcast_uxplay_ptth.py> (mutant: GCAST_MUT_PTTH=1) and
#   a host build of airplay_fe_harness. The fixed harness must pass; the mutant (unpatched UxPlay) must fail with
#   HPE_INVALID_METHOD, a lab MiSTer field signature.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FIX=${1:?fixed harness}; MUT=${2:-}; PORT=${3:-18791}
run() { # harness -> rc of the replay; harness log in /tmp/ap-ptth-$2.log
  ASAN_OPTIONS=detect_leaks=0 "$1" gcast-ptth "$PORT" > /tmp/ap-ptth-$2.log 2>&1 & local p=$!
  sleep 1.5
  python3 "$ROOT/tools/gcast/ios_airplay_url_replay.py" 127.0.0.1 "$PORT" > /tmp/ap-ptth-$2.replay 2>&1; local rc=$?
  kill $p 2>/dev/null; wait $p 2>/dev/null
  return $rc
}
fail=0
if run "$FIX" fix && ! grep -q HPE_INVALID_METHOD /tmp/ap-ptth-fix.log; then echo "ok   ptth fixed: replay OK"
else echo "FAIL ptth fixed"; cat /tmp/ap-ptth-fix.replay; fail=1; fi
if [ -n "$MUT" ]; then
  if ! run "$MUT" mut && grep -q HPE_INVALID_METHOD /tmp/ap-ptth-mut.log; then echo "ok   ptth mutant caught (HPE_INVALID_METHOD)"
  else echo "FAIL ptth mutant survived"; fail=1; fi
fi
[ $fail = 0 ] && echo "airplay_ptth_repro: ALL OK" || echo "airplay_ptth_repro: FAIL"
exit $fail
