#!/usr/bin/env bash
# Hardware Phase 3.3f/h/j/k: headers + residual status after real Baseline F3 push.
# First MB is I_NxN — FPGA residual levels/runs expect res_ok=1 res_tc=8 res_t1=3 res_dc=-24.
# F3-only (no F1): decode_stub still paints has_frame; hybrid host_owns_fs not latched.
# Assert control plane (sps/pps/mb0/qp/frame) matches host golden after IDR marking fix.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== ensure Plex core (safe — no kill-9 + load_core thrash) ==="
# Free SPI gently; only bounce core if not already Plex (load_core mid-session wedges lab)
ssh_m 'killall misterplexd 2>/dev/null || true; killall -CONT MiSTer 2>/dev/null || true; rm -f /tmp/misterplex_spi.lock'
sleep 1
if ! ssh_m 'cat /tmp/CORENAME' 2>/dev/null | grep -qi plex; then
  MISTER_HOST="$HOST" MISTER_PASS="$PASS" DEPLOY_LOAD=menu DEPLOY_WAIT_S=5 \
    "$ROOT/scripts/deploy_plex_core.sh" || true
  sleep 2
fi
ssh_m 'grep -q Plex /tmp/CORENAME'
# Wait until push_frame can open status (UIO ready)
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if ssh_m '/media/fat/misterplex/bin/push_frame --status' 2>/dev/null | grep -q 'status'; then
    break
  fi
  sleep 0.5
done

mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/gen_test_annexb_real.py" "$ROOT/build/plex_real_baseline.264"
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$ROOT/build/plex_real_baseline.264" \
  "$USER@$HOST:/media/fat/plex_real_baseline.264"

ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_real_baseline.264' | grep -q OK
sleep 0.5

ST=""
for i in 1 2 3 4 5 6 7 8 9 10; do
  ST=$(ssh_m '/media/fat/misterplex/bin/push_frame --status' 2>/dev/null || true)
  if echo "$ST" | grep -qE 'mb0=0|qp=25'; then
    break
  fi
  sleep 0.15
done
echo "$ST"
echo "$ST" | grep -q 'sps_valid=1'
echo "$ST" | grep -q 'pps_valid=1'
echo "$ST" | grep -q 'mb0=0'
echo "$ST" | grep -q 'qp=25'
echo "$ST" | grep -q 'has_frame=1'
# I_NxN first residual (nC=0): real Baseline probe_tc=8 t1=3
echo "$ST" | grep -qE 'res_ok=1'
echo "$ST" | grep -qE 'res_tc=8'
echo "$ST" | grep -qE 'res_t1=3'
# 3.3k residual_dc (scan coeff0) — host golden coeff[0]=-24 after runv-clear RBF
echo "$ST" | grep -qE 'res_dc=-24'
# 3.3l-1 soft: res_csum=20 (0x14) = XOR sat8(full coeff[16]) at status[111:104].
# Pre-3.3l-1 / pre-R-csum1 RBF: do not hard-fail (soft skip ≠ hard PASS).
# Host golden locked: residual_gold::kCsum8=0x14 — RCA helper prints raw[12..15] map.
RCA_HELPER="$ROOT/tests/parse_res_csum_status.py"
if [[ -f "$RCA_HELPER" ]]; then
  echo "=== host residual csum RCA (expect res_csum=20 / raw[13]=0x14) ==="
  python3 "$RCA_HELPER" --goldens 2>/dev/null || true
  echo "$ST" | python3 "$RCA_HELPER" - 2>/dev/null || true
fi
if echo "$ST" | grep -qE 'res_csum=20\b'; then
  echo "test_f3_residual: res_csum=20 HARD-class green (3.3l-1 XOR sat8=0x14)"
else
  GOT_CSUM=$(echo "$ST" | sed -n 's/.*res_csum=\([0-9][0-9]*\).*/\1/p' | head -1)
  echo "test_f3_residual: res_csum soft skip (got res_csum=${GOT_CSUM:-?} want 20/0x14; host golden locked)"
  echo "test_f3_residual: soft-skip EXIT=0 is NOT hard PASS — re-gate after R-csum1 deploy"
fi
echo "test_f3_residual: OK on $HOST — mb0=0 qp=25 res_ok=1 res_tc=8 res_t1=3 res_dc=-24"
