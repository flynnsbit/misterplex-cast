#!/usr/bin/env bash
# Hardware visual decode regression: deploy/verify Plex, push the proven
# Baseline/CAVLC IDR, capture the real HDMI output, and compare against the
# checked-in known-good hardware golden with quantified errors + diff artifact.
#
# This is a scheduled-token test. It does not build Quartus and only deploys an
# RBF when VISUAL_RBF (or the first positional arg) is supplied.
set -euo pipefail

HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${VISUAL_OUT:-$ROOT/build/hw_visual}"
DEV="${HDMI_DEV:-/dev/video4}"
CAP_FMT="${VISUAL_CAPTURE_FORMAT:-mjpeg}"
CAP_SIZE="${VISUAL_CAPTURE_SIZE:-1280x720}"
CAP_FPS="${VISUAL_CAPTURE_FPS:-60}"
CAP_ATTEMPTS="${VISUAL_CAPTURE_ATTEMPTS:-5}"
COLOR_MATRIX="${VISUAL_COLOR_MATRIX:-bt601}"
COLOR_RANGE="${VISUAL_COLOR_RANGE:-full}"
PIXEL_FORMAT="${VISUAL_PIXEL_FORMAT:-yuv420p}"
CONTENT_SIZE="${VISUAL_EXPECTED_CONTENT_SIZE:-320x240}"
VIDEO_MODE="${VISUAL_VIDEO_MODE:-0}"  # MiSTer preset 0 = 1280x720@60
REQUIRE_FRESH_DELIVERY="${VISUAL_REQUIRE_FRESH_DELIVERY:-1}"
MIN_BYTES_IN="${VISUAL_MIN_BYTES_IN:-512}"
if [[ "${VISUAL_FULL_FRAME:-0}" == "1" ]]; then
  COMPARE_BOX="${VISUAL_COMPARE_BOX:-active}" # full 618x480 active display region
else
  COMPARE_BOX="${VISUAL_COMPARE_BOX:-11,0,160,120}" # stable top-left decoded ROI containing MB0
fi
RBF="${VISUAL_RBF:-${1:-}}"
EXPECTED_RBF_MD5="${VISUAL_EXPECTED_RBF_MD5:-${VISUAL_RBF_MD5:-}}"
EXPECT="${VISUAL_EXPECT:-pass}"   # pass | fail (fail means same-provenance capture must mismatch golden)
BITSTREAM="${VISUAL_BITSTREAM:-$ROOT/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264}"
GOLDEN="${VISUAL_GOLDEN:-}"
TOOL="$ROOT/scripts/hw_visual_compare.py"

if [[ -z "$GOLDEN" ]]; then
  echo "FAIL: VISUAL_GOLDEN must be declared; no hardware golden is safe as a silent default." >&2
  echo "Legacy rollback captures are quarantined and only valid when their provenance matches the loaded RBF." >&2
  exit 2
fi

if [[ "$(basename "$BITSTREAM")" == "plex_visual_624x480_1f.264" && "${VISUAL_ALLOW_UNPROVEN_624:-0}" != "1" ]]; then
  echo "FAIL: 624x480 visual fixture is not a proven hardware gate on rollback 57674f2e." >&2
  echo "Use the default 320x240 proven vector/golden, or set VISUAL_ALLOW_UNPROVEN_624=1 for investigation only." >&2
  exit 2
fi
if [[ "${VISUAL_FULL_FRAME:-0}" == "1" && "${VISUAL_ALLOW_UNPROVEN_FULL:-0}" != "1" ]]; then
  echo "FAIL: full-frame visual gate is not proven on rollback 57674f2e." >&2
  echo "Use the proven default ROI gate, or set VISUAL_ALLOW_UNPROVEN_FULL=1 for scheduled investigation only." >&2
  exit 2
fi
if [[ -n "$RBF" && -z "$EXPECTED_RBF_MD5" ]]; then
  EXPECTED_RBF_MD5="$(md5sum "$RBF" | awk '{print tolower($1)}')"
fi
if [[ -z "$EXPECTED_RBF_MD5" ]]; then
  echo "FAIL: expected RBF md5 is not declared; set VISUAL_EXPECTED_RBF_MD5 (or pass VISUAL_RBF)." >&2
  exit 2
fi

mkdir -p "$OUT"

SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$USER@$HOST")
SCP=(sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -o ConnectTimeout=12)

ssh_m() {
  "${SSH[@]}" "$@"
}

verify_loaded_rbf() {
  echo "=== verify loaded Plex.rbf md5 (expected $EXPECTED_RBF_MD5) ==="
  ssh_m "md5sum /media/fat/_Utility/Plex.rbf" | tee "$OUT/rbf_md5.txt"
  local actual
  actual="$(sed -n 's/^\([0-9a-fA-F]\{32\}\).*/\1/p' "$OUT/rbf_md5.txt" | tr 'A-F' 'a-f' | head -1)"
  if [[ "$actual" != "$EXPECTED_RBF_MD5" ]]; then
    echo "FAIL: loaded core md5 $actual != expected $EXPECTED_RBF_MD5; refusing to grade" >&2
    exit 8
  fi
}

restore_mode() {
  "${SSH[@]}" "printf '%s\n' 'video_mode 5' > /dev/MiSTer_cmd" >/dev/null 2>&1 || true
}
trap restore_mode EXIT

echo "=== hw visual geometry ==="
python3 "$TOOL" geometry | tee "$OUT/geometry.json"

if [[ -n "$RBF" ]]; then
  echo "=== deploy scheduled RBF once: $RBF ==="
  MISTER_HOST="$HOST" MISTER_PASS="$PASS" DEPLOY_LOAD=menu DEPLOY_RECOVER=reboot \
    "$ROOT/scripts/deploy_plex_core.sh" "$RBF"
else
  echo "=== no VISUAL_RBF supplied; verifying existing Plex core ==="
  ssh_m "ps | grep -q '[P]lex.rbf'"
fi
verify_loaded_rbf

capture() {
  python3 "$TOOL" capture --device "$DEV" --input-format "$CAP_FMT" \
    --video-size "$CAP_SIZE" --framerate "$CAP_FPS" --attempts "$CAP_ATTEMPTS" \
    --color-matrix "$COLOR_MATRIX" --color-range "$COLOR_RANGE" "$@"
}

COMPARE_ARGS=()
if [[ -n "$COMPARE_BOX" ]]; then
  COMPARE_ARGS=(--compare-box "$COMPARE_BOX")
fi
RBF_COMPARE_ARGS=(
  --expected-rbf-md5 "$EXPECTED_RBF_MD5"
  --rbf-md5-log "$OUT/rbf_md5.txt"
  --expected-content-size "$CONTENT_SIZE"
  --expected-pixel-format "$PIXEL_FORMAT"
)

echo "=== set HDMI mode preset $VIDEO_MODE for capture ($CAP_FMT $CAP_SIZE@$CAP_FPS) ==="
ssh_m "printf '%s\n' 'video_mode $VIDEO_MODE' > /dev/MiSTer_cmd"
sleep 3

if [[ -z "$RBF" && "${VISUAL_PREVIOUS_MENU:-1}" == "1" ]]; then
  echo "=== capture MiSTer menu as previous condition for freshness ==="
  ssh_m "printf '%s\n' 'load_core /media/fat/menu.rbf' > /dev/MiSTer_cmd"
  for _ in $(seq 1 20); do
    ssh_m "cat /tmp/CORENAME 2>/dev/null || true" | grep -qi menu && break
    sleep 1
  done
  ssh_m "cat /tmp/CORENAME 2>/dev/null || true" | grep -qi menu
  ssh_m "printf '%s\n' 'video_mode $VIDEO_MODE' > /dev/MiSTer_cmd"
  sleep 2
  capture --out "$OUT/previous.png"
  echo "=== reload existing Plex core after menu freshness capture ==="
  ssh_m "printf '%s\n' 'load_core /media/fat/_Utility/Plex.rbf' > /dev/MiSTer_cmd"
  for _ in $(seq 1 20); do
    ssh_m "cat /tmp/CORENAME 2>/dev/null || true" | grep -qi plex && break
    sleep 1
  done
  ssh_m "cat /tmp/CORENAME 2>/dev/null || true" | grep -qi plex
  verify_loaded_rbf
  ssh_m "printf '%s\n' 'video_mode $VIDEO_MODE' > /dev/MiSTer_cmd"
  sleep 2
else
  echo "=== capture previous condition for stale-capture rejection ==="
  capture --out "$OUT/previous.png"
fi

echo "=== wait for Plex status path ==="
for _ in $(seq 1 20); do
  if ssh_m '/media/fat/misterplex/bin/push_frame --status' >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done
ssh_m '/media/fat/misterplex/bin/push_frame --status' > "$OUT/status_before.txt" 2>/dev/null || true

echo "=== push checked-in Baseline/CAVLC visual bitstream: $(basename "$BITSTREAM") ==="
"${SCP[@]}" "$BITSTREAM" "$USER@$HOST:/media/fat/plex_visual_gate.264"
ST=""
>"$OUT/push.txt"
status_ready() {
  local st="$1"
  echo "$st" | grep -q 'sps_valid=1' || return 1
  echo "$st" | grep -q 'pps_valid=1' || return 1
  echo "$st" | grep -q 'mb0=0' || return 1
  return 0
}
for attempt in 1 2 3; do
  echo "push attempt $attempt" | tee -a "$OUT/push.txt"
  ssh_m '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_visual_gate.264' \
    | tee -a "$OUT/push.txt"
  sleep 1

  echo "=== status telemetry (off-screen decoder debug state; attempt $attempt) ==="
  for _ in $(seq 1 20); do
    ST="$(ssh_m '/media/fat/misterplex/bin/push_frame --status' 2>/dev/null || true)"
    if status_ready "$ST"; then
      break
    fi
    sleep 0.2
  done
  if status_ready "$ST"; then
    break
  fi
done
printf '%s\n' "$ST" | tee "$OUT/status.txt"
if [[ "${VISUAL_REQUIRE_STATUS:-1}" == "1" ]]; then
  echo "$ST" | grep -q 'sps_valid=1' || { echo "FAIL: visual bitstream did not latch SPS status" >&2; exit 2; }
  echo "$ST" | grep -q 'pps_valid=1' || { echo "FAIL: visual bitstream did not latch PPS status" >&2; exit 2; }
  echo "$ST" | grep -q 'mb0=0' || { echo "FAIL: visual bitstream did not reach MB0 status" >&2; exit 2; }
fi

STATUS_COMPARE_ARGS=()
if [[ "$REQUIRE_FRESH_DELIVERY" == "1" ]]; then
  STATUS_COMPARE_ARGS=(
    --status-log "$OUT/status.txt"
    --previous-status-log "$OUT/status_before.txt"
    --require-status-field has_frame=1
    --require-status-field has_stream=1
    --require-status-field has_idr=1
    --require-status-field sps_valid=1
    --require-status-field pps_valid=1
  )
  if grep -q 'bytes_in=' "$OUT/status.txt"; then
    STATUS_COMPARE_ARGS+=(--min-bytes-in "$MIN_BYTES_IN")
  fi
  if [[ "${VISUAL_REQUIRE_TOKEN:-0}" == "1" ]]; then
    STATUS_COMPARE_ARGS+=(--require-token-change)
  fi
fi

echo "=== repeated static captures for measured noise floor ==="
for i in 0 1 2 3 4; do
  capture --out "$OUT/cap_${i}.png"
  sleep 0.2
done
python3 "$TOOL" noise \
  "${COMPARE_ARGS[@]}" \
  --frames "$OUT/cap_0.png" "$OUT/cap_1.png" "$OUT/cap_2.png" "$OUT/cap_3.png" "$OUT/cap_4.png" \
  --out "$OUT/noise.json" | tee "$OUT/noise.txt"

echo "=== compare capture against checked-in golden ==="
set +e
python3 "$TOOL" compare \
  "${COMPARE_ARGS[@]}" \
  "${RBF_COMPARE_ARGS[@]}" \
  "${STATUS_COMPARE_ARGS[@]}" \
  --golden "$GOLDEN" \
  --golden-color-matrix "$COLOR_MATRIX" \
  --golden-color-range "$COLOR_RANGE" \
  --previous "$OUT/previous.png" \
  --capture "$OUT/cap_4.png" \
  --capture-color-matrix "$COLOR_MATRIX" \
  --capture-color-range "$COLOR_RANGE" \
  --noise-report "$OUT/noise.json" \
  --report "$OUT/compare.json" \
  --diff "$OUT/diff.png" | tee "$OUT/compare.txt"
compare_rc=${PIPESTATUS[0]}
set -e
case "$EXPECT:$compare_rc" in
  pass:0)
    echo "VISUAL_EXPECT=pass: green compare accepted"
    ;;
  pass:1)
    echo "FAIL: visual mismatch against golden (diff=$OUT/diff.png)" >&2
    exit 1
    ;;
  fail:1)
    echo "VISUAL_EXPECT=fail: same-provenance capture went red as expected (diff=$OUT/diff.png)"
    ;;
  fail:0)
    echo "FAIL: VISUAL_EXPECT=fail but capture matched golden; red specimen did not go red" >&2
    exit 1
    ;;
  *:3|*:4|*:5|*:6|*:7)
    echo "FAIL: capture/freshness integrity error rc=$compare_rc (stale/corrupt/absent/busy/no-fresh-frame), not a core result" >&2
    exit "$compare_rc"
    ;;
  *:8)
    echo "FAIL: loaded RBF identity error rc=$compare_rc; not a core result" >&2
    exit "$compare_rc"
    ;;
  *:9)
    echo "FAIL: golden provenance error rc=$compare_rc; not a core result" >&2
    exit "$compare_rc"
    ;;
  *)
    echo "FAIL: comparator error rc=$compare_rc" >&2
    exit "$compare_rc"
    ;;
esac

if [[ "${VISUAL_FAULT_DEMO:-0}" == "1" ]]; then
  echo "=== red-path demonstration: deliberately corrupt one active captured pixel ==="
  python3 - "$OUT/cap_4.png" "$OUT/cap_bad.png" <<'PY'
from pathlib import Path
import sys
import numpy as np
from PIL import Image
src, dst = map(Path, sys.argv[1:])
im = np.array(Image.open(src).convert("RGB"), dtype=np.uint8)
im[20, 20, 1] = (int(im[20, 20, 1]) + 64) & 0xFF
Image.fromarray(im, "RGB").save(dst)
PY
  set +e
  python3 "$TOOL" compare \
    "${COMPARE_ARGS[@]}" \
    "${RBF_COMPARE_ARGS[@]}" \
    "${STATUS_COMPARE_ARGS[@]}" \
    --golden "$GOLDEN" \
    --golden-color-matrix "$COLOR_MATRIX" \
    --golden-color-range "$COLOR_RANGE" \
    --capture "$OUT/cap_bad.png" \
    --capture-color-matrix "$COLOR_MATRIX" \
    --capture-color-range "$COLOR_RANGE" \
    --noise-report "$OUT/noise.json" \
    --report "$OUT/compare_bad_expected_fail.json" \
    --diff "$OUT/diff_bad_expected_fail.png" | tee "$OUT/compare_bad_expected_fail.txt"
  rc=$?
  set -e
  if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: VISUAL_FAULT_DEMO did not go red" >&2
    exit 1
  fi
  echo "VISUAL_FAULT_DEMO: expected red path returned rc=$rc"
fi

echo "test_f3_visual_golden: OK artifacts=$OUT"
