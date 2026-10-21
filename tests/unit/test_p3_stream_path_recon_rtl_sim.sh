#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
set +e
VERILATOR_VERSION="$($RUN_VERILATOR --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  echo "SKIP stream_path RTL SIM: Verilator not found." >&2
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

BUILD="$ROOT/build/verilator/stream_path_recon"
FIXTURE="$ROOT/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264"
mkdir -p "$BUILD"

echo "RTL SIM: using $VERILATOR_VERSION" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module stream_path_recon_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/stream_path_recon_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/stream_ingest.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/bitstream_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/nalu_scanner.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/sps_parser.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/pps_parser.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_inter_pred.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_deblock.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_dpb.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/decode_stub.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/stream_path.sv" \
  "$ROOT/tests/rtl/stream_path_recon_tb.cpp"

"$BUILD/Vstream_path_recon_tb" "$FIXTURE"
