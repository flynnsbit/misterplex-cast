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
SKIP RTL SIM: Verilator not found; stream_path integrated inter simulation was NOT run.
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

QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TOP="$ROOT/tests/rtl/stream_path_inter_tb_top.sv"
TB="$ROOT/tests/rtl/stream_path_inter_tb.cpp"
IDR_FIXTURE="$ROOT/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264"
INTER_FIXTURE="$ROOT/tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264"
BUILD="$ROOT/build/verilator/stream_path_inter"
BUILD_FAULT="$ROOT/build/verilator/stream_path_inter_bad_pixel"
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

for f in "$QIP" "$TOP" "$TB" "$IDR_FIXTURE" "$INTER_FIXTURE"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
RTL_ARGS=()
for f in "${PRODUCT_RTL[@]}"; do
  if [[ ! -f "$RTL_DIR/$f" ]]; then
    echo "RTL SIM ERROR: missing product RTL: $RTL_DIR/$f" >&2
    exit 2
  fi
  if ! grep -q "rtl/$f" "$QIP"; then
    echo "RTL SIM ERROR: files.qip does not list product RTL under simulation: rtl/$f" >&2
    exit 2
  fi
  RTL_ARGS+=("$RTL_DIR/$f")
done

mkdir -p "$BUILD" "$BUILD_FAULT"
echo "RTL SIM: using $VERILATOR_VERSION" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module stream_path_inter_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "${RTL_ARGS[@]}" "$TB"
"$BUILD/Vstream_path_inter_tb" "$IDR_FIXTURE"
"$BUILD/Vstream_path_inter_tb" "$INTER_FIXTURE"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_FAULT" \
  --top-module stream_path_inter_tb -GFAULT_INTER_DIAG_PIXEL=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "${RTL_ARGS[@]}" "$TB"
set +e
FAULT_OUT="$($BUILD_FAULT/Vstream_path_inter_tb "$INTER_FIXTURE" 2>&1)"
FAULT_RC=$?
set -e
if ! RED_CHECK="$(python3 "$ROOT/tests/unit/expected_red.py" stream_path_inter_bad_pixel "$FAULT_RC" <<<"$FAULT_OUT" 2>&1)"; then
  printf '%s\n%s\n' "$RED_CHECK" "$FAULT_OUT" >&2
  exit 1
fi
printf '%s\n' "$RED_CHECK"
echo "OK stream_path inter RTL red-check: bad diagnostic pixel fault failed golden"
