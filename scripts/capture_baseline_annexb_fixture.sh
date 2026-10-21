#!/usr/bin/env bash
# Capture a real PMS Baseline/CAVLC transcode to an Annex-B H.264 fixture.
# Requires a caller-provided stream URL/token; this script never stores or prints it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: "${MISTERPLEX_STREAM_URL:?set MISTERPLEX_STREAM_URL to a real PMS streaming URL; do not commit it}"
OUT="${OUT:-build/p3_baseline_480p/plex_real_baseline_624x480_12s.264}"
SECONDS_TO_CAPTURE="${SECONDS_TO_CAPTURE:-12}"
mkdir -p "$(dirname "$OUT")"
ffmpeg -y -hide_banner -loglevel error \
  -i "$MISTERPLEX_STREAM_URL" \
  -map 0:v:0 -c copy -an -t "$SECONDS_TO_CAPTURE" \
  -bsf:v h264_mp4toannexb -f h264 "$OUT"
bash tests/unit/test_no_private_data.sh >/dev/null
printf 'capture_baseline_annexb_fixture: OK wrote %s\n' "$OUT"
printf 'Next: ./build/extract_h264_golden --input %s --mb 0 --output build/p3_baseline_480p/mb0.json --verify-mb0-reference tests/fixtures/p3_host_recon/mb0_luma_v1.json\n' "$OUT"
