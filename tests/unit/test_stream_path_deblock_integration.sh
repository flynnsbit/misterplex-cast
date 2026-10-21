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
SKIP RTL SIM: Verilator not found; stream_path/deblock integration simulation was NOT run.
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
for src in rtl/stream_path.sv rtl/h264_deblock.sv rtl/decode_stub.sv rtl/slice_hdr_parser.sv; do
  if ! grep -q "$src" "$QIP"; then
    echo "RTL SIM ERROR: files.qip does not list product $src" >&2
    exit 2
  fi
done

SRC_ANNEXB="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264"
SEQUENCE="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p_sequence_v1.json"
BUILD_FIX="$ROOT/build/p3_stream_path_deblock"
GOLDEN="$BUILD_FIX/mb0.json"
mkdir -p "$BUILD_FIX"
make -s -C "$ROOT" h264-golden-tools
"$ROOT/build/extract_h264_golden" --input "$SRC_ANNEXB" --mb 0 --output "$GOLDEN" --verify-mb0-reference "$ROOT/tests/fixtures/p3_host_recon/mb0_luma_v1.json" >/dev/null
RTL=(
  "$ROOT/fpga/Plex_MiSTer/rtl/stream_ingest.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/bitstream_fifo.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/nalu_scanner.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/sps_parser.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/pps_parser.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_deblock.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_inter_pred.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_dpb.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/decode_stub.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/stream_path.sv"
)
TOP="$ROOT/tests/rtl/stream_path_deblock_tb_top.sv"
TB="$ROOT/tests/rtl/stream_path_deblock_tb.cpp"
BUILD="$ROOT/build/verilator/stream_path_deblock"
mkdir -p "$BUILD"
echo "RTL SIM: using $VERILATOR_VERSION" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module stream_path_deblock_tb -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
  -CFLAGS "-std=c++17 -O2" \
  "${RTL[@]}" "$TOP" "$TB"
EXE="$BUILD/Vstream_path_deblock_tb"
"$EXE" --annexb "$SRC_ANNEXB" --mb-golden "$GOLDEN" --nal-sequence "$SEQUENCE"

for fault in bs threshold chroma boundary loop slice-controls; do
  set +e
  FAULT_OUT="$($EXE --annexb "$SRC_ANNEXB" --mb-golden "$GOLDEN" --nal-sequence "$SEQUENCE" "--fault-$fault" 2>&1)"
  FAULT_RC=$?
  set -e
  printf '%s\n' "$FAULT_OUT"
  if [[ "$FAULT_RC" -eq 0 ]]; then
    echo "FAIL stream_path/deblock red-check: $fault unexpectedly passed" >&2
    exit 1
  fi
  if ! grep -q 'expected .* red-check' <<<"$FAULT_OUT"; then
    echo "FAIL stream_path/deblock red-check: expected diagnostic for $fault" >&2
    exit 1
  fi
  echo "OK stream_path/deblock red-check: $fault property trips"
done
