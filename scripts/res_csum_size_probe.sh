#!/usr/bin/env bash
# One-window residual_csum status alias discriminator.
#
# This script intentionally does not deploy or rebuild. It pushes the same F3
# residual vector at two byte sizes and prints raw/status after each push. If
# raw[13]/res_csum advances by file_size % 256, the published byte is still the
# stream byte counter low byte, not residual_csum.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${MISTER_HOST:?set MISTER_HOST to the MiSTer host for the scheduled lab window}"
MISTER_USER="${MISTER_USER:-root}"
MISTER_PASS="${MISTER_PASS:?set MISTER_PASS}"
REMOTE_BIN="${REMOTE_BIN:-/media/fat/misterplex/bin/push_frame}"

BASE="$ROOT/build/plex_real_baseline.264"
PAD="$ROOT/build/plex_real_baseline_pad37.264"

mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/gen_test_annexb_real.py" "$BASE"
python3 - "$BASE" "$PAD" <<'PY'
from pathlib import Path
import sys
src = Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_bytes(src + bytes(37))
PY

mod256() {
  python3 - "$1" <<'PY'
from pathlib import Path
import sys
print(Path(sys.argv[1]).stat().st_size % 256)
PY
}

echo "baseline size=$(stat -c%s "$BASE") mod256=$(mod256 "$BASE")"
echo "padded   size=$(stat -c%s "$PAD") mod256=$(mod256 "$PAD")"
echo "expected: product RBF keeps res_csum=20/raw[13]=0x14 for both;"
echo "          alias defect makes raw[13] track the per-push size low byte."

ssh_m() {
  sshpass -p "$MISTER_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
    "$MISTER_USER@$MISTER_HOST" "$@"
}

scp_m() {
  sshpass -p "$MISTER_PASS" scp -o StrictHostKeyChecking=no "$@"
}

scp_m "$BASE" "$MISTER_USER@$MISTER_HOST:/media/fat/plex_real_baseline.264"
scp_m "$PAD" "$MISTER_USER@$MISTER_HOST:/media/fat/plex_real_baseline_pad37.264"

run_case() {
  local label="$1"
  local remote="$2"
  echo "=== $label push ==="
  ssh_m "$REMOTE_BIN --index 3 '$remote'"
  sleep 0.2
  ssh_m "$REMOTE_BIN --raw" | tee "$ROOT/build/res_csum_${label}_raw.log"
  ssh_m "$REMOTE_BIN --status" | tee "$ROOT/build/res_csum_${label}_status.log"
}

run_case baseline /media/fat/plex_real_baseline.264
run_case padded /media/fat/plex_real_baseline_pad37.264

echo "Decode with: python3 tests/parse_res_csum_status.py build/res_csum_baseline_raw.log"
