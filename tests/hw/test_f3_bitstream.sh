#!/usr/bin/env bash
# Hardware: push annex-B to F3; verify nalu_count via UIO_GET_STATUS readback.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== core is Plex ==="
ssh_m 'grep -q Plex /tmp/CORENAME'

echo "=== generate annex-B test blob ==="
mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/gen_test_annexb.py" "$ROOT/build/plex_test_annexb.264"
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$ROOT/build/plex_test_annexb.264" \
  "$USER@$HOST:/media/fat/plex_test_annexb.264"

echo "=== F3 SPI push (index 3) ==="
# push_frame pauses Main_MiSTer during SPI for clean FIO + status readback
OUT=$(ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_test_annexb.264')
echo "$OUT"
echo "$OUT" | grep -q 'OK'
echo "$OUT" | grep -q 'index=3'
sleep 0.15

read_status() {
  # Retry until has_stream or nalu non-zero (SPI can still glitch)
  local st="" i
  for i in 1 2 3 4 5 6; do
    st=$(ssh_m '/media/fat/misterplex/bin/push_frame --status' 2>/dev/null || true)
    if echo "$st" | grep -qE 'has_stream=1|nalu=[1-9]'; then
      echo "$st"
      return 0
    fi
    sleep 0.15
  done
  echo "$st"
  return 1
}

echo "=== status readback (expect nalu>=4 has_stream=1; byte counter is not in status ABI) ==="
ST=$(read_status) || true
echo "$ST"
echo "$ST" | grep -q 'has_stream=1'
NALU=$(echo "$ST" | sed -n 's/.*nalu=\([0-9]*\).*/\1/p')
echo "nalu_count=$NALU"
python3 -c "import sys; n=int('$NALU' or 0); sys.exit(0 if n >= 4 else 1)"
STREAM_NALUS=$(echo "$ST" | sed -n 's/.*stream_nalus=\([0-9]*\).*/\1/p')
echo "stream_nalus=$STREAM_NALUS"
python3 -c "import sys; n=int('$STREAM_NALUS' or 0); sys.exit(0 if n >= 4 else 1)"
# Fake SPS in synthetic blob is not Baseline-parseable — sps_valid may stay 0

echo "=== second append push (nalu should grow by >=4) ==="
ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_test_annexb.264' | grep -q OK
sleep 0.2
ST2=$(read_status) || true
echo "$ST2"
NALU2=$(echo "$ST2" | sed -n 's/.*nalu=\([0-9]*\).*/\1/p')
python3 -c "import sys; a=int('$NALU' or 0); b=int('$NALU2' or 0); sys.exit(0 if b >= a + 4 else 1)"

echo "test_f3_bitstream: OK on $HOST (nalu $NALU → $NALU2)"
