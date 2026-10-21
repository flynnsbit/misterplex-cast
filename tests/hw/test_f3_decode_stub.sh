#!/usr/bin/env bash
# Hardware Phase 3.3b: F3 annex-B → nalu_scanner → decode_stub → frame_store.
# Asserts has_idr, has_frame after VCL NALs (slice_type may be 0 for synthetic SPS).
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

echo "=== generate annex-B test blob (SPS/PPS/IDR/P) ==="
mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/gen_test_annexb.py" "$ROOT/build/plex_test_annexb.264"
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$ROOT/build/plex_test_annexb.264" \
  "$USER@$HOST:/media/fat/plex_test_annexb.264"

echo "=== F3 push ==="
ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_test_annexb.264' | tee "$ROOT/build/f3_stub_push.txt"
grep -q OK "$ROOT/build/f3_stub_push.txt"
sleep 0.25

read_status() {
  local st="" i
  for i in 1 2 3 4 5 6 7 8; do
    st=$(ssh_m '/media/fat/misterplex/bin/push_frame --status' 2>/dev/null || true)
    if echo "$st" | grep -qE 'has_frame=1|has_idr=1'; then
      echo "$st"
      return 0
    fi
    sleep 0.15
  done
  echo "$st"
  return 1
}

echo "=== status (expect has_stream has_idr has_frame) ==="
ST=$(read_status) || true
echo "$ST"
echo "$ST" | grep -q 'has_stream=1'
echo "$ST" | grep -q 'has_idr=1'
NALU=$(echo "$ST" | sed -n 's/.*nalu=\([0-9]*\).*/\1/p')
python3 -c "import sys; n=int('$NALU' or 0); sys.exit(0 if n >= 4 else 1)"
IDR=$(echo "$ST" | sed -n 's/.*idr=\([0-9]*\).*/\1/p')
python3 -c "import sys; n=int('$IDR' or 0); sys.exit(0 if n >= 1 else 1)"
echo "$ST" | grep -q 'has_frame=1'

echo "=== second push grows nalu and idr ==="
NALU1=$NALU
IDR1=$IDR
ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_test_annexb.264' | grep -q OK
sleep 0.3
ST2=$(read_status) || true
echo "$ST2"
NALU2=$(echo "$ST2" | sed -n 's/.*nalu=\([0-9]*\).*/\1/p')
IDR2=$(echo "$ST2" | sed -n 's/.*idr=\([0-9]*\).*/\1/p')
python3 -c "import sys; a=int('$NALU1' or 0); b=int('$NALU2' or 0); sys.exit(0 if b >= a + 4 else 1)"
python3 -c "import sys; a=int('$IDR1' or 0); b=int('$IDR2' or 0); sys.exit(0 if b >= a + 1 else 1)"

echo "test_f3_decode_stub: OK on $HOST (nalu $NALU1→$NALU2 idr $IDR1→$IDR2)"
echo "NOTE: CRT/HDMI should show green-border MB-grid diagnostic frame (decode_stub)."
