#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

command -v ffmpeg >/dev/null || { echo "FRAME PLANE ERROR: ffmpeg reference decoder not found" >&2; exit 2; }
command -v ffprobe >/dev/null || { echo "FRAME PLANE ERROR: ffprobe not found" >&2; exit 2; }

TOOL="$ROOT/tools/extract_h264_frame_planes.py"
OUT="$ROOT/build/p3_frame_planes"
mkdir -p "$OUT"

check_fixture() {
  local name="$1"
  local bitstream="$2"
  local sequence="$3"
  local planes="$4"
  local manifest="$5"
  local expect_w="${6:-0}"
  local expect_h="${7:-0}"
  local gen_planes="$OUT/${name}.i420"
  local gen_manifest="$OUT/${name}.json"

  args=(
    --input "$bitstream"
    --sequence "$sequence"
    --planes-out "$gen_planes"
    --manifest-out "$gen_manifest"
    --h264-loop-filter disabled
  )
  if [[ "$expect_w" != "0" ]]; then args+=(--expect-width "$expect_w"); fi
  if [[ "$expect_h" != "0" ]]; then args+=(--expect-height "$expect_h"); fi
  "$TOOL" "${args[@]}"
  cmp -s "$gen_planes" "$planes"
  "$TOOL" --verify --input "$bitstream" --sequence "$sequence" --planes "$planes" --manifest "$manifest" \
    --expected-h264-loop-filter disabled
  "$TOOL" --verify --input "$bitstream" --sequence "$sequence" --planes "$planes" --manifest "$manifest" \
    --expected-h264-loop-filter disabled \
    --candidate-planes "$gen_planes" --candidate-colorspace I420_NATIVE
}

check_fixture \
  wcap_residual14_idr_plus_p \
  tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264 \
  tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p_sequence_v1.json \
  tests/fixtures/p3_frame_planes/wcap_residual14_idr_plus_p_i420.yuv \
  tests/fixtures/p3_frame_planes/wcap_residual14_idr_plus_p_frame_planes_v1.json

check_fixture \
  plex_inter_p16_320x240_12f \
  tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json

check_fixture \
  plex_inter_p16_624x480_12f \
  tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_624x480_12f.264 \
  tests/fixtures/p3_multinal/plex_inter_p16_624x480_sequence_v1.json \
  tests/fixtures/p3_frame_planes/plex_inter_p16_624x480_12f_i420.yuv \
  tests/fixtures/p3_frame_planes/plex_inter_p16_624x480_12f_frame_planes_v1.json \
  624 480

ENABLED_PLANES="$OUT/plex_inter_p16_320x240_12f_deblocked.i420"
ENABLED_MANIFEST="$OUT/plex_inter_p16_320x240_12f_deblocked.json"
"$TOOL" \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes-out "$ENABLED_PLANES" \
  --manifest-out "$ENABLED_MANIFEST" \
  --h264-loop-filter enabled
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes "$ENABLED_PLANES" \
  --manifest "$ENABLED_MANIFEST" \
  --expect-h264-loop-filter enabled
set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes "$ENABLED_PLANES" \
  --manifest "$ENABLED_MANIFEST" \
  --expected-h264-loop-filter disabled \
  > "$OUT/enabled_ref_as_disabled.log" 2>&1
ENABLED_AS_DISABLED_RC=$?
set -e
if [[ "$ENABLED_AS_DISABLED_RC" -ne 9 ]]; then
  cat "$OUT/enabled_ref_as_disabled.log"
  echo "FAIL frame-plane red-check: deblocked reference rc=$ENABLED_AS_DISABLED_RC, want rc=9 refusal under disabled contract" >&2
  exit 1
fi
grep -q 'loop-filter mismatch\|disabled H.264 loop filter\|loop_filter is not skip_loop_filter=all' "$OUT/enabled_ref_as_disabled.log"

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json \
  > "$OUT/missing_expected_loop_filter.log" 2>&1
MISSING_LOOP_RC=$?
set -e
if [[ "$MISSING_LOOP_RC" -ne 9 ]]; then
  cat "$OUT/missing_expected_loop_filter.log"
  echo "FAIL frame-plane red-check: missing expected loop-filter rc=$MISSING_LOOP_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'expected H.264 loop-filter state is undeclared' "$OUT/missing_expected_loop_filter.log"

python3 - <<'PY'
from pathlib import Path
src = Path("tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv")
dst = Path("build/p3_frame_planes/plex_inter_p16_320x240_12f_corrupt.i420")
data = bytearray(src.read_bytes())
data[76800] ^= 0x55  # first U byte of frame 0
dst.write_bytes(data)
PY

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json \
  --expected-h264-loop-filter disabled \
  --candidate-planes build/p3_frame_planes/plex_inter_p16_320x240_12f_corrupt.i420 \
  --candidate-colorspace I420_NATIVE \
  > "$OUT/corrupt_compare.log" 2>&1
RED_RC=$?
set -e
if [[ "$RED_RC" -eq 0 ]]; then
  cat "$OUT/corrupt_compare.log"
  echo "FAIL frame-plane red-check: corrupt U plane unexpectedly matched" >&2
  exit 1
fi
grep -q 'FRAME_PLANE_COMPARE raw frame=0 plane=U' "$OUT/corrupt_compare.log"
grep -q 'candidate plane comparison diverged from golden' "$OUT/corrupt_compare.log"
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json \
  --expected-h264-loop-filter disabled \
  --candidate-planes build/p3_frame_planes/plex_inter_p16_320x240_12f_corrupt.i420 \
  --candidate-colorspace I420_NATIVE \
  --expect-red

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json \
  --expected-h264-loop-filter disabled \
  --candidate-planes build/p3_frame_planes/plex_inter_p16_320x240_12f_corrupt.i420 \
  > "$OUT/unknown_colorspace_compare.log" 2>&1
UNKNOWN_RC=$?
set -e
if [[ "$UNKNOWN_RC" -eq 0 ]]; then
  cat "$OUT/unknown_colorspace_compare.log"
  echo "FAIL frame-plane red-check: unknown candidate colorspace unexpectedly compared" >&2
  exit 1
fi
if [[ "$UNKNOWN_RC" -ne 9 ]]; then
  cat "$OUT/unknown_colorspace_compare.log"
  echo "FAIL frame-plane red-check: unknown candidate colorspace rc=$UNKNOWN_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'candidate colorspace is unknown' "$OUT/unknown_colorspace_compare.log"

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json \
  --expected-h264-loop-filter disabled \
  --candidate-planes build/p3_frame_planes/plex_inter_p16_320x240_12f_corrupt.i420 \
  --candidate-colorspace I420_FROM_RGB565 \
  > "$OUT/mismatched_colorspace_compare.log" 2>&1
MISMATCH_RC=$?
set -e
if [[ "$MISMATCH_RC" -eq 0 ]]; then
  cat "$OUT/mismatched_colorspace_compare.log"
  echo "FAIL frame-plane red-check: mismatched candidate colorspace unexpectedly compared" >&2
  exit 1
fi
if [[ "$MISMATCH_RC" -ne 9 ]]; then
  cat "$OUT/mismatched_colorspace_compare.log"
  echo "FAIL frame-plane red-check: mismatched candidate colorspace rc=$MISMATCH_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'candidate colorspace mismatch' "$OUT/mismatched_colorspace_compare.log"

python3 - <<'PY'
import json
from pathlib import Path
src = Path("tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json")
bad = Path("build/p3_frame_planes/plex_inter_p16_320x240_12f_bad_provenance.json")
data = json.loads(src.read_text())
data["provenance"]["rgb565_roundtrip"] = True
bad.write_text(json.dumps(data, indent=2) + "\n")

masked = Path("build/p3_frame_planes/plex_inter_p16_320x240_12f_masked_provenance.json")
data = json.loads(src.read_text())
data["provenance"]["presentation_border_or_pillar_mask"] = True
masked.write_text(json.dumps(data, indent=2) + "\n")

filtered = Path("build/p3_frame_planes/plex_inter_p16_320x240_12f_filtered_provenance.json")
data = json.loads(src.read_text())
data["decoder"]["loop_filter"] = "default"
data["provenance"]["h264_loop_filter"] = "enabled"
filtered.write_text(json.dumps(data, indent=2) + "\n")
PY

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest build/p3_frame_planes/plex_inter_p16_320x240_12f_bad_provenance.json \
  --expected-h264-loop-filter disabled \
  > "$OUT/bad_provenance_compare.log" 2>&1
PROVENANCE_RC=$?
set -e
if [[ "$PROVENANCE_RC" -eq 0 ]]; then
  cat "$OUT/bad_provenance_compare.log"
  echo "FAIL frame-plane red-check: RGB565-tainted manifest provenance unexpectedly verified" >&2
  exit 1
fi
if [[ "$PROVENANCE_RC" -ne 9 ]]; then
  cat "$OUT/bad_provenance_compare.log"
  echo "FAIL frame-plane red-check: RGB565-tainted manifest rc=$PROVENANCE_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'RGB/RGB565 round-trip' "$OUT/bad_provenance_compare.log"

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest build/p3_frame_planes/plex_inter_p16_320x240_12f_masked_provenance.json \
  --expected-h264-loop-filter disabled \
  > "$OUT/masked_provenance_compare.log" 2>&1
MASKED_RC=$?
set -e
if [[ "$MASKED_RC" -eq 0 ]]; then
  cat "$OUT/masked_provenance_compare.log"
  echo "FAIL frame-plane red-check: border/pillar-masked manifest provenance unexpectedly verified" >&2
  exit 1
fi
if [[ "$MASKED_RC" -ne 9 ]]; then
  cat "$OUT/masked_provenance_compare.log"
  echo "FAIL frame-plane red-check: border/pillar-masked manifest rc=$MASKED_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'presentation masking' "$OUT/masked_provenance_compare.log"

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv \
  --manifest build/p3_frame_planes/plex_inter_p16_320x240_12f_filtered_provenance.json \
  --expected-h264-loop-filter disabled \
  > "$OUT/filtered_provenance_compare.log" 2>&1
FILTERED_RC=$?
set -e
if [[ "$FILTERED_RC" -eq 0 ]]; then
  cat "$OUT/filtered_provenance_compare.log"
  echo "FAIL frame-plane red-check: loop-filtered manifest provenance unexpectedly verified" >&2
  exit 1
fi
if [[ "$FILTERED_RC" -ne 9 ]]; then
  cat "$OUT/filtered_provenance_compare.log"
  echo "FAIL frame-plane red-check: loop-filtered manifest rc=$FILTERED_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'loop_filter is not skip_loop_filter=all\|disabled H.264 loop filter' "$OUT/filtered_provenance_compare.log"

ffmpeg -v error -y \
  -i tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  -an -f rawvideo -pix_fmt yuv420p "$OUT/plex_inter_p16_320x240_12f_deblocked.i420"
python3 - <<'PY'
import hashlib
import json
from pathlib import Path

manifest = Path("tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json")
planes = Path("build/p3_frame_planes/plex_inter_p16_320x240_12f_deblocked.i420")
bad = Path("build/p3_frame_planes/plex_inter_p16_320x240_12f_deblocked_declared_undeblocked.json")
data = json.loads(manifest.read_text())
blob = planes.read_bytes()
data["decoder"]["command"] = (
    "ffmpeg -v error -y -i tests/fixtures/p3_inter_pred/"
    "plex_inter_p16_baseline_320x240_12f.264 -an -f rawvideo -pix_fmt yuv420p "
    "build/p3_frame_planes/plex_inter_p16_320x240_12f_deblocked.i420"
)
data["decoder"]["command_argv"] = [
    "ffmpeg", "-v", "error", "-y", "-i",
    "tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264",
    "-an", "-f", "rawvideo", "-pix_fmt", "yuv420p",
    "build/p3_frame_planes/plex_inter_p16_320x240_12f_deblocked.i420",
]
data["decoder"]["h264_loop_filter"] = "disabled"
data["decoder"]["h264_loop_filter_ffmpeg"] = "-skip_loop_filter all"
data["decoder"]["loop_filter"] = "skip_loop_filter=all"
data["provenance"]["h264_loop_filter"] = "disabled"
data["plane_blob"]["path"] = str(planes)
data["plane_blob"]["bytes"] = len(blob)
data["plane_blob"]["sha256"] = hashlib.sha256(blob).hexdigest()
bad.write_text(json.dumps(data, indent=2) + "\n")
PY

set +e
"$TOOL" --verify \
  --input tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264 \
  --sequence tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json \
  --planes "$OUT/plex_inter_p16_320x240_12f_deblocked.i420" \
  --manifest "$OUT/plex_inter_p16_320x240_12f_deblocked_declared_undeblocked.json" \
  --expected-h264-loop-filter disabled \
  > "$OUT/deblocked_declared_undeblocked.log" 2>&1
DEBLOCK_RC=$?
set -e
if [[ "$DEBLOCK_RC" -ne 9 ]]; then
  cat "$OUT/deblocked_declared_undeblocked.log"
  echo "FAIL frame-plane red-check: deblocked reference declared as undeblocked rc=$DEBLOCK_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q "declares H.264 loop filter disabled but decoder command does not include '-skip_loop_filter all'" \
  "$OUT/deblocked_declared_undeblocked.log"

echo "test_h264_frame_plane_goldens: OK regenerated I420 goldens, alias/explicit loop-filter provenance verified, corrupt-plane/provenance RED checked"
