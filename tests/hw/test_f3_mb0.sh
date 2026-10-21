#!/usr/bin/env bash
# Hardware Phase 3.3e/h: real Baseline → first_mb_type + slice_qp (IDR marking fixed).
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== reload Plex core ==="
ssh_m 'killall misterplexd 2>/dev/null || true; killall -CONT MiSTer 2>/dev/null || true'
# skip load_core if already Plex — avoids DE10 lockups
if ! ssh_m 'cat /tmp/CORENAME' 2>/dev/null | grep -qi plex; then
  DEPLOY_LOAD=menu "$(cd "$(dirname "$0")/../.." && pwd)/scripts/deploy_plex_core.sh" || true
fi
sleep 3
ssh_m 'grep -q Plex /tmp/CORENAME'

mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/gen_test_annexb_real.py" "$ROOT/build/plex_real_baseline.264"
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$ROOT/build/plex_real_baseline.264" \
  "$USER@$HOST:/media/fat/plex_real_baseline.264"

ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_real_baseline.264' | grep -q OK
sleep 0.5

ST=""
for i in 1 2 3 4 5 6 7 8 9 10; do
  ST=$(ssh_m '/media/fat/misterplex/bin/push_frame --status' 2>/dev/null || true)
  if echo "$ST" | grep -qE 'mb0='; then
    break
  fi
  sleep 0.15
done
echo "$ST"
echo "$ST" | grep -q 'sps_valid=1'
echo "$ST" | grep -q 'pps_valid=1'
echo "$ST" | grep -q 'slice_type=7'
# Correct IDR parse: mb0=0 (I_NxN), qp=25
echo "$ST" | grep -qE 'mb0=0'
echo "$ST" | grep -qE 'qp=25'
echo "$ST" | grep -q 'has_frame=1'
echo "test_f3_mb0: OK on $HOST"
