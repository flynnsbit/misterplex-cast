#!/usr/bin/env bash
# Phase 3.1b: push 320×240 YUV420p via DDR bulk and check has_frame.
# Requires Plex.rbf with ddram_frame_rd on the MiSTer.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${ROOT}/build/arm/push_frame"
FRAME="${FRAME:-${ROOT}/build/plex_test_320x240.yuv420p}"
REMOTE_FRAME="/tmp/plex_test_320x240.yuv420p"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN — run: make arm-plexd"
  exit 1
fi

if [[ ! -f "$FRAME" ]]; then
  mkdir -p "$(dirname "$FRAME")"
  python3 "${ROOT}/scripts/gen_edge_markers.py" --format yuv420p "$FRAME"
fi
echo "=== ensure Plex core loaded ==="
ssh_m 'killall misterplexd 2>/dev/null || true; killall -CONT MiSTer 2>/dev/null || true; rm -f /tmp/misterplex_spi.lock'
sleep 1
if ! ssh_m 'cat /tmp/CORENAME' 2>/dev/null | grep -qi plex; then
  ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
  DEPLOY_LOAD=menu "$ROOT/scripts/deploy_plex_core.sh" 2>/dev/null || true
  sleep 3
fi
ssh_m 'grep -q Plex /tmp/CORENAME'

echo "== scp push_frame + YUV420p frame to $HOST =="
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -q "$BIN" "$FRAME" \
  "$USER@$HOST:/tmp/"

echo "== DDR push =="
# Reset so has_frame 0→1 is a strong proof of DMA (not stale SPI frame).
OUT=$(ssh_m '/tmp/push_frame --pulse 0 >/dev/null
sleep 0.05
/tmp/push_frame --ddr --yuv420p 320x240 '"$REMOTE_FRAME"'
/tmp/push_frame --status
' 2>&1)
echo "$OUT"
echo "$OUT" | grep -qE 'pushed .* DDR .* OK' || {
  echo "FAIL: DDR push did not report OK (see above)"
  exit 1
}
echo "$OUT" | grep -q 'has_frame=1' || {
  echo "FAIL: has_frame!=1 after DDR"
  exit 1
}
MS=$(echo "$OUT" | grep -oE '[0-9]+\.[0-9]+ ms' | head -1 | grep -oE '^[0-9]+' || echo 999)
echo "push_ms≈$MS (expect DDR ≪ legacy SPI F1)"
if [[ "$MS" -gt 80 ]]; then
  echo "WARN: DDR push not much faster than SPI (ms=$MS) — check RBF / MainPause batching"
fi
echo "test_ddr_frame: OK on $HOST (DDR ${MS} ms)"
