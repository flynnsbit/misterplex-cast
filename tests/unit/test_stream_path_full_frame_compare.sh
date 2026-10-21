#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
set +e
VERILATOR_VERSION="$($RUN_VERILATOR --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  cat >&2 <<SKIP
SKIP RTL SIM: Verilator not found; full-frame stream_path comparison was NOT run.
Install oss-cad-suite under ~/.local/oss-cad-suite or run with VERILATOR=/path/to/verilator.
SKIP
  if [[ "${ALLOW_MISSING_VERILATOR:-0}" != "1" ]]; then
    echo "RTL SIM ERROR: Verilator not found; refusing to report PASS without running the simulation." >&2
    echo "A skipped RTL gate is NOT a pass. Set ALLOW_MISSING_VERILATOR=1 only if you accept that RTL was never verified." >&2
    exit 3
  fi
  exit 0
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed:" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

command -v ffmpeg >/dev/null || { echo "RTL SIM ERROR: ffmpeg reference decoder not found" >&2; exit 2; }
command -v ffprobe >/dev/null || { echo "RTL SIM ERROR: ffprobe not found" >&2; exit 2; }

QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
BITSTREAM="${FULL_FRAME_BITSTREAM:-$ROOT/tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264}"
SEQUENCE="${FULL_FRAME_SEQUENCE:-}"
RATCHET="${FULL_FRAME_RATCHET:-}"
THROUGHPUT_RATCHET="${FULL_FRAME_THROUGHPUT_RATCHET:-}"
BUILD="$ROOT/build/verilator/stream_path_full_frame"
BUILD_FAULT="$ROOT/build/verilator/stream_path_full_frame_fault"
REF_DIR="$ROOT/build/p3_full_frame"
THROUGHPUT_DIR="$ROOT/build/realtime_throughput"
COMPARE_JSON="$REF_DIR/frame_planes_compare.json"
FAULT_JSON="$REF_DIR/frame_planes_compare_fault.json"
NATIVE_SCORE_JSON="$REF_DIR/native_frame_score.json"
BAD_LOOP_MANIFEST="$REF_DIR/bad_loop_filter_manifest.json"
NATIVE_CANDIDATE_I420="$REF_DIR/native_inter_candidate.i420"
NATIVE_CANDIDATE_JSON="$REF_DIR/native_inter_candidate_score.json"
INTER_METADATA_JSON="$REF_DIR/native_inter_metadata.json"
MB0_TRACE_JSON="$REF_DIR/mb0_pipeline_trace.json"
FAULT_TRACE_JSON="$REF_DIR/mb0_pipeline_trace_fault.json"
MB0_GOLDEN_JSON="$REF_DIR/current_mb0_trace.json"
CANDIDATE_I420="$REF_DIR/candidate.i420"
FAULT_I420="$REF_DIR/candidate_fault.i420"
TOP="$ROOT/tests/rtl/stream_path_full_frame_tb_top.sv"
TB="$ROOT/tests/rtl/stream_path_full_frame_tb.cpp"
RTL_DIR="$ROOT/fpga/Plex_MiSTer/rtl"
PRODUCT_RTL=(
  stream_path.sv
  stream_ingest.sv
  ddr_bitstream_reader.sv
  bitstream_fifo.sv
  nalu_scanner.sv
  sps_parser.sv
  pps_parser.sv
  slice_hdr_parser.sv
  h264_iq_idct_4x4.sv
  h264_inter_pred.sv
  h264_deblock.sv
  h264_dpb.sv
  decode_stub.sv
)

for f in "$QIP" "$BITSTREAM" "$TOP" "$TB"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done

RTL_ARGS=()
for rtl in "${PRODUCT_RTL[@]}"; do
  if [[ ! -f "$RTL_DIR/$rtl" ]]; then
    echo "RTL SIM ERROR: missing product RTL: $RTL_DIR/$rtl" >&2
    exit 2
  fi
  if ! grep -q "rtl/$rtl" "$QIP"; then
    echo "RTL SIM ERROR: files.qip does not list product RTL under simulation: rtl/$rtl" >&2
    exit 2
  fi
  RTL_ARGS+=("$RTL_DIR/$rtl")
done

read -r WIDTH HEIGHT FRAMES < <(
  ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,nb_read_frames \
    -count_frames -of csv=p=0 "$BITSTREAM" | tr ',' ' '
)
if [[ -z "${WIDTH:-}" || -z "${HEIGHT:-}" || -z "${FRAMES:-}" || "$FRAMES" == "N/A" || "$FRAMES" -lt 2 ]]; then
  echo "RTL SIM ERROR: ffprobe did not report a multi-frame reference geometry/count" >&2
  exit 2
fi

SOURCE_SHA=$(sha256sum "$BITSTREAM" | awk '{print $1}')
mkdir -p "$BUILD" "$BUILD_FAULT" "$REF_DIR" "$THROUGHPUT_DIR"
if [[ -z "$SEQUENCE" ]]; then
  case "$SOURCE_SHA" in
    d6f30bcb8226f7e1c204d01f9914bffe1ec661503e373f7312d23884b3bfa86e)
      SEQUENCE="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_sequence_v1.json"
      ;;
    9b79749478f331d6e523a548a88fbad38d1719beb6a2623b289e4e0190bf17a9)
      SEQUENCE="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_624x480_sequence_v1.json"
      ;;
    9f58c3f92a6c9cacc86d2b58c275329445017f328b54206fcdcef5de4b1a5b62)
      SEQUENCE="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p_sequence_v1.json"
      ;;
    *)
      echo "RTL SIM ERROR: no sequence manifest supplied for bitstream $BITSTREAM" >&2
      echo "Set FULL_FRAME_SEQUENCE after generating/committing a nal_sequence.v1 manifest." >&2
      exit 2
      ;;
  esac
fi
if [[ -z "$RATCHET" ]]; then
  case "$SOURCE_SHA" in
    d6f30bcb8226f7e1c204d01f9914bffe1ec661503e373f7312d23884b3bfa86e)
      RATCHET="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_full_frame_ratchet_v1.json"
      ;;
    9b79749478f331d6e523a548a88fbad38d1719beb6a2623b289e4e0190bf17a9)
      RATCHET="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_624x480_full_frame_ratchet_v1.json"
      ;;
    9f58c3f92a6c9cacc86d2b58c275329445017f328b54206fcdcef5de4b1a5b62)
      RATCHET="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p_full_frame_ratchet_v1.json"
      ;;
  esac
fi
if [[ -z "$THROUGHPUT_RATCHET" ]]; then
  case "$SOURCE_SHA" in
    d6f30bcb8226f7e1c204d01f9914bffe1ec661503e373f7312d23884b3bfa86e)
      THROUGHPUT_RATCHET="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_320x240_decode_throughput_v1.json"
      ;;
    9b79749478f331d6e523a548a88fbad38d1719beb6a2623b289e4e0190bf17a9)
      THROUGHPUT_RATCHET="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_624x480_decode_throughput_v1.json"
      ;;
    9f58c3f92a6c9cacc86d2b58c275329445017f328b54206fcdcef5de4b1a5b62)
      THROUGHPUT_RATCHET="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p_decode_throughput_v1.json"
      ;;
  esac
fi
if [[ -z "$RATCHET" || ! -f "$RATCHET" ]]; then
  echo "RTL SIM ERROR: missing native full-frame ratchet for bitstream: ${RATCHET:-<unset>}" >&2
  exit 2
fi
if [[ -z "$THROUGHPUT_RATCHET" || ! -f "$THROUGHPUT_RATCHET" ]]; then
  echo "RTL SIM ERROR: missing decode throughput ratchet for bitstream: ${THROUGHPUT_RATCHET:-<unset>}" >&2
  exit 2
fi
if [[ ! -f "$SEQUENCE" ]]; then
  echo "RTL SIM ERROR: missing required file: $SEQUENCE" >&2
  exit 2
fi
if ! grep -q '"format": "misterplex.p3.nal_sequence.v1"' "$SEQUENCE"; then
  echo "RTL SIM ERROR: sequence manifest is not misterplex.p3.nal_sequence.v1: $SEQUENCE" >&2
  exit 2
fi
if ! grep -q "$SOURCE_SHA" "$SEQUENCE"; then
  echo "RTL SIM ERROR: sequence manifest sha256 does not match bitstream" >&2
  exit 2
fi
if ! grep -q "$SOURCE_SHA" "$RATCHET"; then
  echo "RTL SIM ERROR: ratchet baseline sha256 does not match bitstream" >&2
  exit 2
fi

GOLDEN_PLANES="${FULL_FRAME_GOLDEN_PLANES:-}"
GOLDEN_MANIFEST="${FULL_FRAME_GOLDEN_MANIFEST:-}"
if [[ -z "$GOLDEN_PLANES" && -z "$GOLDEN_MANIFEST" ]]; then
  case "$SOURCE_SHA" in
    d6f30bcb8226f7e1c204d01f9914bffe1ec661503e373f7312d23884b3bfa86e)
      GOLDEN_PLANES="$ROOT/tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_i420.yuv"
      GOLDEN_MANIFEST="$ROOT/tests/fixtures/p3_frame_planes/plex_inter_p16_320x240_12f_frame_planes_v1.json"
      ;;
    9b79749478f331d6e523a548a88fbad38d1719beb6a2623b289e4e0190bf17a9)
      GOLDEN_PLANES="$ROOT/tests/fixtures/p3_frame_planes/plex_inter_p16_624x480_12f_i420.yuv"
      GOLDEN_MANIFEST="$ROOT/tests/fixtures/p3_frame_planes/plex_inter_p16_624x480_12f_frame_planes_v1.json"
      ;;
    9f58c3f92a6c9cacc86d2b58c275329445017f328b54206fcdcef5de4b1a5b62)
      GOLDEN_PLANES="$ROOT/tests/fixtures/p3_frame_planes/wcap_residual14_idr_plus_p_i420.yuv"
      GOLDEN_MANIFEST="$ROOT/tests/fixtures/p3_frame_planes/wcap_residual14_idr_plus_p_frame_planes_v1.json"
      ;;
  esac
fi
if [[ -z "$GOLDEN_PLANES" || -z "$GOLDEN_MANIFEST" ]]; then
  echo "RTL SIM ERROR: no proven frame-plane golden supplied for bitstream $BITSTREAM" >&2
  echo "Set FULL_FRAME_GOLDEN_PLANES and FULL_FRAME_GOLDEN_MANIFEST after generating/committing provenance." >&2
  exit 2
fi
"$ROOT/tools/extract_h264_frame_planes.py" --verify \
  --input "$BITSTREAM" --sequence "$SEQUENCE" \
  --planes "$GOLDEN_PLANES" --manifest "$GOLDEN_MANIFEST" \
  --expected-h264-loop-filter disabled

echo "RTL SIM: using $VERILATOR_VERSION (stream_path_full_frame_compare ${WIDTH}x${HEIGHT} frames=${FRAMES})" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module stream_path_full_frame_tb -GFRAME_W="$WIDTH" -GFRAME_H="$HEIGHT" -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "${RTL_ARGS[@]}" "$TB"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_FAULT" \
  --top-module stream_path_full_frame_tb -GFRAME_W="$WIDTH" -GFRAME_H="$HEIGHT" -GFAULT_PIXEL_XOR=1 -GFAULT_TRACE_COEFF0_PLUS1=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "${RTL_ARGS[@]}" "$TB"

"$BUILD/Vstream_path_full_frame_tb" \
  --annexb "$BITSTREAM" --golden-planes "$GOLDEN_PLANES" --golden-manifest "$GOLDEN_MANIFEST" \
  --candidate-i420-out "$CANDIDATE_I420" \
  --native-candidate-i420-out "$NATIVE_CANDIDATE_I420" \
  --inter-metadata-out "$INTER_METADATA_JSON" --sequence "$SEQUENCE" \
  --source-sha256 "$SOURCE_SHA" --width "$WIDTH" --height "$HEIGHT" \
  --json-out "$COMPARE_JSON" --trace-json-out "$MB0_TRACE_JSON" --expect-red
set +e
COLORSPACE_OUT="$("$ROOT/tools/extract_h264_frame_planes.py" --verify \
  --input "$BITSTREAM" --sequence "$SEQUENCE" \
  --planes "$GOLDEN_PLANES" --manifest "$GOLDEN_MANIFEST" \
  --expected-h264-loop-filter disabled \
  --candidate-planes "$CANDIDATE_I420" --candidate-colorspace I420_FROM_RGB565 2>&1)"
COLORSPACE_RC=$?
set -e
if [[ "$COLORSPACE_RC" -eq 0 ]]; then
  printf '%s\n' "$COLORSPACE_OUT"
  echo "FAIL full-frame colorspace red-check: RGB565-derived candidate was accepted as native I420" >&2
  exit 1
fi
grep -q 'candidate colorspace mismatch' <<<"$COLORSPACE_OUT"
echo "OK full-frame colorspace red-check: RGB565-derived diagnostic candidate refused by native I420 golden"
grep -q '"format": "misterplex.p3.frame_planes_compare.v1"' "$COMPARE_JSON"
grep -q '"sequence_manifest":' "$COMPARE_JSON"
"$ROOT/tools/check_decode_throughput.py" \
  --compare-json "$COMPARE_JSON" \
  --ratchet "$THROUGHPUT_RATCHET" \
  --label "$(basename "$BITSTREAM"):${WIDTH}x${HEIGHT}" \
  --report "$THROUGHPUT_DIR/decode_throughput_${WIDTH}x${HEIGHT}.json"
NATIVE_CANDIDATE_OUT="$("$ROOT/tools/score_i420_candidate.py" \
  --sequence "$SEQUENCE" \
  --golden-manifest "$GOLDEN_MANIFEST" \
  --golden-planes "$GOLDEN_PLANES" \
  --candidate-planes "$NATIVE_CANDIDATE_I420" \
  --candidate-colorspace I420_NATIVE \
  --reference-h264-loop-filter disabled \
  --candidate-h264-loop-filter disabled \
  --mb-metadata "$INTER_METADATA_JSON" \
  --output "$NATIVE_CANDIDATE_JSON" \
  --expect-red)"
printf '%s\n' "$NATIVE_CANDIDATE_OUT" | grep -E 'I420_CANDIDATE_SCORE summary|score_i420_candidate: OK'
python3 - "$NATIVE_CANDIDATE_JSON" <<'PY'
import json
import sys

score = json.load(open(sys.argv[1]))
if score.get("format") != "misterplex.p3.i420_candidate_score.v1":
    raise SystemExit("FAIL native inter candidate score: unknown score format")
if score.get("colorspace") != "I420_NATIVE":
    raise SystemExit("FAIL native inter candidate score: not native I420")
summary = score["summary"]
inter = summary["inter"]
if inter["mb_total"] == 0:
    raise SystemExit("FAIL native inter candidate score: no inter MB population")
if summary["first_bad_inter"] is None:
    raise SystemExit("FAIL native inter candidate score: expected-red had no first_bad_inter")
print(
    "OK native inter candidate score: "
    f"intra={summary['intra']['mb_exact']}/{summary['intra']['mb_total']} "
    f"inter={inter['mb_exact']}/{inter['mb_total']} "
    f"first_bad_inter_mb={summary['first_bad_inter']['mb_index']} "
    f"plane={summary['first_bad_inter']['plane']}"
)
PY
python3 - "$INTER_METADATA_JSON" "$GOLDEN_MANIFEST" <<'PY'
import json
import sys

meta = json.load(open(sys.argv[1]))
gold = json.load(open(sys.argv[2]))
if meta.get("format") != "misterplex.p3.inter_mb_metadata.v1":
    raise SystemExit("FAIL native inter metadata: unknown format")
candidate = meta.get("candidate", {})
expected = {
    "colorspace": "I420_NATIVE",
    "h264_loop_filter": "disabled",
    "reconstruction_stage": "mc_prediction_only_pre_deblock_no_residual_add",
    "reference_picture_state": "diagnostic_filtered_reference_via_deblock_writeback_ctrl",
    "reference_picture_source": "generated_i420_pattern_not_decoded_prior_frame",
}
for key, want in expected.items():
    got = candidate.get(key)
    if got != want:
        raise SystemExit(f"FAIL native inter metadata: candidate.{key}={got!r} want {want!r}")
if gold.get("decoder", {}).get("loop_filter") != "skip_loop_filter=all":
    raise SystemExit("FAIL native inter metadata: golden decoder.loop_filter is not skip_loop_filter=all")
if gold.get("provenance", {}).get("h264_loop_filter") != "disabled":
    raise SystemExit("FAIL native inter metadata: golden h264_loop_filter is not disabled")
print(
    "OK native inter provenance: "
    f"candidate_stage={candidate['reconstruction_stage']} "
    f"reference_state={candidate['reference_picture_state']} "
    "reference_h264_loop_filter=disabled"
)
PY
make -s -C "$ROOT" h264-golden-tools
python3 - "$GOLDEN_MANIFEST" "$BAD_LOOP_MANIFEST" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
data["decoder"]["loop_filter"] = "default"
data["provenance"]["h264_loop_filter"] = "enabled"
with open(sys.argv[2], "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY
set +e
LOOP_FILTER_OUT="$("$ROOT/build/score_h264_native_frames" \
  --input "$BITSTREAM" --planes "$GOLDEN_PLANES" --manifest "$BAD_LOOP_MANIFEST" 2>&1)"
LOOP_FILTER_RC=$?
set -e
if [[ "$LOOP_FILTER_RC" -ne 9 ]]; then
  printf '%s\n' "$LOOP_FILTER_OUT"
  echo "FAIL native score red-check: enabled loop filter rc=$LOOP_FILTER_RC, want rc=9 refusal" >&2
  exit 1
fi
grep -q 'provenance.h264_loop_filter=disabled' <<<"$LOOP_FILTER_OUT"
echo "OK native score loop-filter red-check: deblocked references are refused"
"$ROOT/build/extract_h264_golden" --input "$BITSTREAM" --output "$MB0_GOLDEN_JSON" >/dev/null
"$ROOT/build/score_h264_native_frames" \
  --input "$BITSTREAM" --planes "$GOLDEN_PLANES" --manifest "$GOLDEN_MANIFEST" --output "$NATIVE_SCORE_JSON"
python3 - "$MB0_TRACE_JSON" "$MB0_GOLDEN_JSON" "$COMPARE_JSON" <<'PY'
import json
import sys

trace = json.load(open(sys.argv[1]))
gold = json.load(open(sys.argv[2]))
compare = json.load(open(sys.argv[3]))
if trace.get("format") != "misterplex.p3.mb0_pipeline_trace.v1" or not trace.get("valid"):
    raise SystemExit("FAIL full-frame MB0 trace: missing valid RTL pipeline trace")
if trace.get("colorspace") != "I420_NATIVE":
    raise SystemExit("FAIL full-frame MB0 trace: trace colorspace is not I420_NATIVE")
if gold.get("format") != "misterplex.p3.mb_golden.v1":
    raise SystemExit("FAIL full-frame MB0 trace: unknown golden format")
if gold["slice"]["nal_type"] != 5 or gold["macroblock"]["index"] != 0:
    raise SystemExit("FAIL full-frame MB0 trace: golden is not frame0 IDR MB(0,0)")

block0 = gold["residual"]["luma4x4"][0]
recon_first4 = [gold["samples"]["recon_y"][y * 16 + x] for y in range(4) for x in range(4)]
checks = [
    ("qp", trace["qp"], gold["macroblock"]["qp"]),
    ("total_coeff", trace["total_coeff"], block0["total_coeff"]),
    ("residual_csum", trace["residual_csum"], gold["checks"]["first_residual_checksum8"]),
    ("coefficients_zigzag", trace["coefficients_zigzag"], block0["coefficients_zigzag"]),
    ("dequant", trace["dequant"], block0["dequant"]),
    ("idct", trace["idct"], block0["idct"]),
    ("recon", trace["recon"], recon_first4),
]
failures = [name for name, got, want in checks if got != want]
if failures:
    print("FAIL full-frame MB0 trace: RTL/reference mismatch in " + ", ".join(failures))
    for name, got, want in checks:
        if got != want:
            print(f"  {name}: got={got} want={want}")
    raise SystemExit(1)

ref_y00 = gold["samples"]["recon_y"][0]
got_y00 = trace["recon"][0]
print(
    "MB0_NATIVE_COMPARE frame=0 colorspace=I420_NATIVE plane=Y x=0 y=0 "
    f"mb_x=0 mb_y=0 pixel_in_mb_x=0 pixel_in_mb_y=0 got={got_y00} ref={ref_y00} abs={abs(got_y00 - ref_y00)}"
)
fb = compare["summary"]["first_bad"]
if fb["frame_index"] == 0 and fb["plane"] == "Y" and fb["x"] == 0 and fb["y"] == 0 and fb["got"] == 142:
    raise SystemExit("FAIL full-frame MB0 trace: diagnostic border still masks native MB0 luma at Y(0,0)")
print(
    "OK full-frame MB0 trace: CAVLC/scan/dequant/IDCT/clip match host; "
    f"RGB565 diagnostic first_bad now plane={fb['plane']} x={fb['x']} y={fb['y']} "
    f"got={fb['got']} ref={fb['ref']} abs={fb['abs']}"
)
# Gate audit coverage declaration (parent directive #16):
# RTL reconstruction scored for ALL MBs luma (via injection scorer).
# I_NxN: full pipeline (dequant+IDCT+recon). I_16x16: IDCT+recon bypass.
# Chroma reconstruction has never been tested in RTL.
frame_w = gold.get("frame", {}).get("width", 320)
frame_h = gold.get("frame", {}).get("height", 240)
total_y = frame_w * frame_h
total_c = total_y // 2
total_mbs = ((frame_w + 15) // 16) * ((frame_h + 15) // 16)
rtl_y_pixels_verified = total_y  # Full frame via RTL scorer (76800 for 320x240)
rtl_chroma_pixels_verified = 0
print(
    f"COVERAGE rtl_recon_y={rtl_y_pixels_verified}/{total_y} "
    f"rtl_recon_chroma={rtl_chroma_pixels_verified}/{total_c} "
    f"rtl_mbs_verified={total_mbs}/{total_mbs} "
    "NOTE: RTL scorer tests arithmetic modules by injection, not connected pipeline"
)
PY
python3 - "$NATIVE_SCORE_JSON" "$RATCHET" <<'PY'
import json
import sys

actual = json.load(open(sys.argv[1]))
ratchet = json.load(open(sys.argv[2]))
if actual.get("format") != "misterplex.p3.native_frame_score.v1":
    raise SystemExit("FAIL native full-frame ratchet: unknown score format")
if ratchet.get("format") != "misterplex.p3.native_frame_score_ratchet.v1":
    raise SystemExit("FAIL native full-frame ratchet: unknown ratchet format")
if actual["source"]["sha256"] != ratchet.get("source_sha256"):
    raise SystemExit("FAIL native full-frame ratchet: source sha256 mismatch")
if actual.get("colorspace") != ratchet.get("colorspace"):
    raise SystemExit("FAIL native full-frame ratchet: colorspace mismatch")
if actual.get("loop_filter") != ratchet.get("loop_filter"):
    raise SystemExit("FAIL native full-frame ratchet: loop-filter mismatch")
if actual.get("h264_loop_filter") != ratchet.get("h264_loop_filter"):
    raise SystemExit("FAIL native full-frame ratchet: H.264 loop-filter mismatch")

planes = {}
for frame in actual["frames"]:
    for plane in frame["planes"]:
        planes[(frame["frame_index"], plane["plane"])] = plane

failures = []
eps = 1e-6
summary = ratchet["summary"]
actual_summary = actual["summary"]
for key in ("frames", "i_frames", "p_frames", "inter_expected_red_frames"):
    if actual_summary[key] != summary[key]:
        failures.append(f"summary {key} {actual_summary[key]} != {summary[key]}")
if actual_summary["intra_mb_exact"] < summary["min_intra_mb_exact"]:
    failures.append(
        f"intra_mb_exact {actual_summary['intra_mb_exact']} < {summary['min_intra_mb_exact']}"
    )
if actual_summary["intra_mb_total"] != summary["intra_mb_total"]:
    failures.append(
        f"intra_mb_total {actual_summary['intra_mb_total']} != {summary['intra_mb_total']}"
    )
for metric in ratchet["metrics"]:
    key = (metric["frame_index"], metric["plane"])
    got = planes.get(key)
    if got is None:
        failures.append(f"missing frame={key[0]} plane={key[1]}")
        continue
    if got["exact_pixels"] < metric["min_exact_pixels"]:
        failures.append(f"frame={key[0]} plane={key[1]} exact {got['exact_pixels']} < {metric['min_exact_pixels']}")
    if got["mae"] > metric["max_mae"] + eps:
        failures.append(f"frame={key[0]} plane={key[1]} mae {got['mae']} > {metric['max_mae']}")
    if got["max_abs"] > metric["max_abs"]:
        failures.append(f"frame={key[0]} plane={key[1]} max_abs {got['max_abs']} > {metric['max_abs']}")
    frame = actual["frames"][metric["frame_index"]]
    if frame["mb_exact"] < metric["mb_exact"] or frame["mb_total"] != metric["mb_total"]:
        failures.append(
            f"frame={metric['frame_index']} mb_exact {frame['mb_exact']}/{frame['mb_total']} "
            f"outside ratchet {metric['mb_exact']}/{metric['mb_total']}"
        )

for expected in ratchet["inter_frames"]:
    frame = actual["frames"][expected["frame_index"]]
    if frame.get("implemented") or not frame.get("expected_red"):
        failures.append(f"frame={expected['frame_index']} inter expected-red classification changed")

if failures:
    print("FAIL native full-frame ratchet: divergence metrics regressed")
    for item in failures[:20]:
        print("  " + item)
    raise SystemExit(1)

print(
    "OK native full-frame ratchet (HOST-ONLY scorer, NOT RTL): "
    f"intra_mb_exact={actual_summary['intra_mb_exact']}/{actual_summary['intra_mb_total']} "
    f"inter_expected_red_frames={actual_summary['inter_expected_red_frames']} "
    f"plane_metrics={len(ratchet['metrics'])}"
)
PY

set +e
FAULT_OUT="$("$BUILD_FAULT/Vstream_path_full_frame_tb" \
  --annexb "$BITSTREAM" --golden-planes "$GOLDEN_PLANES" --golden-manifest "$GOLDEN_MANIFEST" \
  --candidate-i420-out "$FAULT_I420" --sequence "$SEQUENCE" \
  --source-sha256 "$SOURCE_SHA" --width "$WIDTH" --height "$HEIGHT" \
  --json-out "$FAULT_JSON" --trace-json-out "$FAULT_TRACE_JSON" 2>&1)"
FAULT_RC=$?
set -e
printf '%s\n' "$FAULT_OUT" | grep -E 'FULL_FRAME_COMPARE summary|FAIL full-frame strict|FULL_FRAME_COMPARE raw frame=0 .*plane='
if [[ "$FAULT_RC" -eq 0 ]]; then
  echo "FAIL full-frame red-check: faulted pixel stream unexpectedly matched reference" >&2
  exit 1
fi
if ! grep -q 'FAIL full-frame strict:' <<<"$FAULT_OUT"; then
  echo "FAIL full-frame red-check: strict comparator did not report the fault" >&2
  exit 1
fi
python3 - "$FAULT_TRACE_JSON" "$MB0_GOLDEN_JSON" <<'PY'
import json
import sys

trace = json.load(open(sys.argv[1]))
gold = json.load(open(sys.argv[2]))
block0 = gold["residual"]["luma4x4"][0]
if trace["coefficients_zigzag"] == block0["coefficients_zigzag"]:
    raise SystemExit("FAIL full-frame MB0 trace red-check: faulted trace unexpectedly matched coefficients")
if trace["coefficients_zigzag"][0] != block0["coefficients_zigzag"][0] + 1:
    raise SystemExit("FAIL full-frame MB0 trace red-check: injected coefficient fault was not observed")
print("OK full-frame MB0 trace red-check: injected coeff0 fault rejected by trace comparator")
PY
echo "OK full-frame red-check: behavioral pixel XOR fault fails strict reference comparison"

# --- RTL-in-the-loop reconstruction scorer ---
# Runs the actual RTL dequant/IDCT/recon pipeline on ALL luma blocks of ALL MBs.
# verification_target=RTL (not host). This is what tells us the hardware works.
RTL_SCORER_BUILD="$ROOT/build/verilator/rtl_recon_scorer"
RTL_GOLDEN_DIR="$REF_DIR/goldens_all_mbs"
mkdir -p "$RTL_SCORER_BUILD" "$RTL_GOLDEN_DIR"
"$RUN_VERILATOR" --cc --exe --build \
  -Mdir "$RTL_SCORER_BUILD" \
  --top-module h264_rtl_recon_scorer_tb \
  -Wno-fatal \
  "$ROOT/tests/rtl/h264_rtl_recon_scorer_tb.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv" \
  "$ROOT/tests/rtl/h264_rtl_recon_scorer_tb.cpp"
# Extract golden for all MBs if not already present (or stale)
if [ ! -f "$RTL_GOLDEN_DIR/mb_000.json" ] || \
   [ "$BITSTREAM" -nt "$RTL_GOLDEN_DIR/mb_000.json" ]; then
  "$ROOT/build/extract_h264_golden" --input "$BITSTREAM" --all-mbs --output-dir "$RTL_GOLDEN_DIR"
fi
# Score all MBs with RED-check
"$RTL_SCORER_BUILD/Vh264_rtl_recon_scorer_tb" --dir "$RTL_GOLDEN_DIR" --red-check
