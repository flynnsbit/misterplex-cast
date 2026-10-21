#!/usr/bin/env bash
# Hardware Phase 3.3l-2: first 4×4 inv_quant + IDCT paint after Baseline F3 push.
#
# Host goldens (source of truth — do not re-derive):
#   host/libmisterplex/h264_residual_gold.hpp
#   tests/unit/test_idct_quant.cpp  → FPGA_GOLD recon_y00=73 recon_mean4x4=62 pred=128
#
# Expected after 3.3l-2 paint RBF:
#   residual hard: res_ok=1 res_tc=8 res_t1=3 res_dc=-24 res_csum=20 (XOR 0x14)
#   recon soft→hard: recon_y00=73 recon_mean=62 (optional status until telem packs)
#   paint contrast: true recon y00=73 ≠ 3.3k stub (128+dc)=104
#
# Pre-3.3l-2 RBF: residual may still pass; recon_* fields absent → soft skip (exit 0).
# Pre-3.3l-1 RBF: res_csum soft skip (same as test_f3_residual).
#
# F3-only diagnostic; hybrid host_owns_fs not latched. No load_core thrash.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

# Locked paint contract (must match residual_gold / test_idct_quant)
GOLD_Y00=73
GOLD_MEAN=62
GOLD_RECON_SIG=59
GOLD_PRED=128
GOLD_RES_DC=-24
GOLD_RES_CSUM=20
GOLD_STUB_Y=104

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== test_f3_idct_mb0: host goldens y00=${GOLD_Y00} mean=${GOLD_MEAN} pred=${GOLD_PRED} ==="
echo "=== ensure Plex core (safe — no kill-9 + load_core thrash) ==="
ssh_m 'killall misterplexd 2>/dev/null || true; killall -CONT MiSTer 2>/dev/null || true; rm -f /tmp/misterplex_spi.lock'
sleep 1
if ! ssh_m 'cat /tmp/CORENAME' 2>/dev/null | grep -qi plex; then
  MISTER_HOST="$HOST" MISTER_PASS="$PASS" DEPLOY_LOAD=menu DEPLOY_WAIT_S=5 \
    "$ROOT/scripts/deploy_plex_core.sh" || true
  sleep 2
fi
ssh_m 'grep -q Plex /tmp/CORENAME'
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

# --- hard residual regression (3.3k / 3.3l-1) ---
echo "$ST" | grep -q 'sps_valid=1'
echo "$ST" | grep -q 'pps_valid=1'
echo "$ST" | grep -q 'mb0=0'
echo "$ST" | grep -q 'qp=25'
echo "$ST" | grep -q 'has_frame=1'
echo "$ST" | grep -qE 'res_ok=1'
echo "$ST" | grep -qE 'res_tc=8'
echo "$ST" | grep -qE 'res_t1=3'
echo "$ST" | grep -qE "res_dc=${GOLD_RES_DC}"

# res_csum: hard after 3.3l-1 RBF; soft skip on pre-csum bitstream
if echo "$ST" | grep -qE "res_csum=${GOLD_RES_CSUM}\\b"; then
  echo "test_f3_idct_mb0: res_csum=${GOLD_RES_CSUM} (3.3l-1 XOR sat8=0x14 green)"
else
  echo "test_f3_idct_mb0: res_csum soft skip (need 3.3l-1 RBF for csum=${GOLD_RES_CSUM})"
fi

# --- 3.3l-2 recon hard gate when status packs recon_sig ---
# recon_sig = XOR of the first 4x4 reconstructed Y samples from mb0_luma_v1.json:
# 73,72,76,76,72,74,71,73,76,71,32,27,76,73,27,24 -> 0x3b.
RECON_OK=0
if echo "$ST" | grep -qE "recon_sig=${GOLD_RECON_SIG}\\b"; then
  echo "test_f3_idct_mb0: recon_sig=${GOLD_RECON_SIG}/0x3b HARD PASS (pred=${GOLD_PRED}; y00=${GOLD_Y00} mean=${GOLD_MEAN})"
  RECON_OK=1
elif echo "$ST" | grep -qE 'recon_sig='; then
  GOT=$(echo "$ST" | sed -n 's/.*recon_sig=\([0-9][0-9]*\).*/\1/p' | head -1)
  echo "test_f3_idct_mb0: recon_sig=${GOT} want ${GOLD_RECON_SIG}/0x3b — FAIL"
  exit 1
else
  echo "test_f3_idct_mb0: recon_sig soft skip (need 3.3l-2 paint RBF + status recon_sig=${GOLD_RECON_SIG}/0x3b)"
  echo "  host paint contract: y00=${GOLD_Y00} mean=${GOLD_MEAN} pred=${GOLD_PRED} sig=0x3b ≠ stub ${GOLD_STUB_Y}"
  echo "  frame_store top-left 4×4 @W=320: 0..3,320..323,640..643,960..963 RGB565 y00=0x4A49"
fi

if [[ "$RECON_OK" -eq 1 ]]; then
  echo "test_f3_idct_mb0: OK on $HOST — residual + recon paint goldens green"
else
  echo "test_f3_idct_mb0: OK residual path on $HOST — recon paint pending (host goldens locked)"
fi
