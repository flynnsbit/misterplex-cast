#!/usr/bin/env bash
# FIELD-1018 F2b: a lab MiSTer 10:44 sequence (mpxd-ytap-1045.log). Three back-to-back iOS AirPlay URL sessions to ONE
# receiver with the Apple TV+ HLS URL the phone sent: each does /reverse (PTTH "event"), the iPhone's 200 answers on the
# reverse socket, bplist /play, polling, /stop. Sessions 2 and 3 hit "deleting playlist ... duration 0" (the previous
# playlist never got a duration: live/unknown/refused). The receiver must stay up, never parse the reverse socket as
# requests (no HPE_INVALID_METHOD), and keep every session's reverse socket alive.
# The F2b guard (apply_gcast_uxplay_zerodur.py) keeps the current zero-duration playlist out of the /play sweep.
# usage: tests/gcast/airplay_ytap_repro.sh FIXED_HARNESS [MUTANT_HARNESS] [PORT]
#   mutants: GCAST_MUT_PTTH=1 tree (HPE) or GCAST_MUT_ZERODUR=1 tree (current playlist swept); both must be caught
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FIX=${1:?fixed harness}; MUT=${2:-}; PORT=${3:-18794}
URL="https://play-edge.itunes.apple.com/WebObjects/MZPlayLocal.woa/hls/subscription/playlist.m3u8?cc=US&svcId=tvs.vds.4105&isExternal=true"
run() { # harness tag -> 0 if all 3 sessions OK, harness alive, zero-duration delete seen, no HPE
  ASAN_OPTIONS=detect_leaks=0 "$1" gcast-ytap "$PORT" > /tmp/ap-ytap-$2.log 2>&1 & local p=$! rc=0
  sleep 1.5; : > /tmp/ap-ytap-$2.replay
  for s in 1 2 3; do
    HOLD_S=1 python3 "$ROOT/tools/gcast/ios_airplay_url_replay.py" 127.0.0.1 "$PORT" "$URL" >> /tmp/ap-ytap-$2.replay 2>&1 || rc=1
  done
  sleep 0.5
  kill -0 $p 2>/dev/null || { echo "  $2: receiver DIED"; rc=1; }
  grep -q "keeping current playlist .*(duration 0" /tmp/ap-ytap-$2.log || { echo "  $2: zero-duration sweep guard not hit (current playlist swept)"; rc=1; }
  grep -q HPE_INVALID_METHOD /tmp/ap-ytap-$2.log && { echo "  $2: reverse socket parsed as requests (HPE_INVALID_METHOD)"; rc=1; }
  kill $p 2>/dev/null; wait $p 2>/dev/null
  return $rc
}
fail=0
if run "$FIX" fix; then echo "ok   ytap fixed: 3 sessions, zero-duration guard, receiver up"
else echo "FAIL ytap fixed"; grep FAIL /tmp/ap-ytap-fix.replay; fail=1; fi
if [ -n "$MUT" ]; then
  if ! run "$MUT" mut; then echo "ok   ytap mutant caught"; else echo "FAIL ytap mutant survived"; fail=1; fi
fi
[ $fail = 0 ] && echo "airplay_ytap_repro: ALL OK" || echo "airplay_ytap_repro: FAIL"
exit $fail
