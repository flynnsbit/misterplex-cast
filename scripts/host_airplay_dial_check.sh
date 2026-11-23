#!/bin/bash
# Start an ARM misterplexd under qemu-arm-static on the host and run tools/airplay/airplay_dial_probe.py.
# usage: scripts/host_airplay_dial_check.sh <arm misterplexd> [--expect-fail]
# --expect-fail starts it with GAIRPLAY=0 (mutant) and requires the probe to FAIL.
set -u
B=$(readlink -f "${1:?arm misterplexd}"); MODE=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
command -v qemu-arm-static >/dev/null || { echo "SKIP: qemu-arm-static not installed"; exit 77; }
busy(){ ss -ltn 2>/dev/null | grep -qE ":(7000|8008|3015) "; }
busy && { echo "FAIL: :7000/:8008/:3015 already in use"; exit 3; }
D=$(mktemp -d); : > "$D/empty.conf"
AP=1; [ "$MODE" = "--expect-fail" ] && AP=0
( cd "$D" && HOME=$D GAIRPLAY=$AP GYT=1 MPX_PREWARM=0 exec qemu-arm-static "$B" --name host-check --id host-check --port 3015 --conf "$D/empty.conf" ) > "$D/daemon.log" 2>&1 &
P=$!
for i in $(seq 1 40); do ss -ltn | grep -q ":8008 " && { [ $AP = 0 ] || ss -ltn | grep -q ":7000 "; } && break; sleep 1; done
sleep 2
python3 "$ROOT/tools/airplay/airplay_dial_probe.py" 127.0.0.1; rc=$?
kill $P 2>/dev/null; sleep 2; kill -9 $P 2>/dev/null; wait $P 2>/dev/null
for i in $(seq 1 10); do busy || break; sleep 1; done
[ $rc -ne 0 ] && [ "$MODE" != "--expect-fail" ] && { echo "--- daemon.log tail"; grep -v "=[01]$" "$D/daemon.log" | tail -15; }
rm -rf "$D"
if [ "$MODE" = "--expect-fail" ]; then
  [ $rc -ne 0 ] && { echo "host-airplay-dial-check mutant: GAIRPLAY=0 FAILed as required"; exit 0; }
  echo "host-airplay-dial-check mutant: probe PASSED with AirPlay off (check is blind)"; exit 1
fi
[ $rc -eq 0 ] && echo "host-airplay-dial-check: PASS" || echo "host-airplay-dial-check: FAIL"
exit $rc
