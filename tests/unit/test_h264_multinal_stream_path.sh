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
SKIP multi-NAL stream_path RTL sim: Verilator not found.
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
cd "$ROOT"
WCAP_FIXTURE="tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264"
INTER_FIXTURE="tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_320x240_12f.264"
BUILD="build/verilator/h264_multinal_stream_path"
BUILD_FAULT="build/verilator/h264_multinal_stream_path_recon_zero"
mkdir -p "$BUILD"
echo "RTL SIM: using $VERILATOR_VERSION" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module h264_multinal_stream_path_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  tests/rtl/h264_multinal_stream_path_tb_top.sv \
  fpga/Plex_MiSTer/rtl/stream_path.sv \
  fpga/Plex_MiSTer/rtl/stream_ingest.sv \
  fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv \
  fpga/Plex_MiSTer/rtl/bitstream_fifo.sv \
  fpga/Plex_MiSTer/rtl/nalu_scanner.sv \
  fpga/Plex_MiSTer/rtl/sps_parser.sv \
  fpga/Plex_MiSTer/rtl/pps_parser.sv \
  fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv \
  fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv \
  fpga/Plex_MiSTer/rtl/h264_inter_pred.sv \
  fpga/Plex_MiSTer/rtl/h264_deblock.sv \
  fpga/Plex_MiSTer/rtl/h264_dpb.sv \
  fpga/Plex_MiSTer/rtl/decode_stub.sv \
  tests/rtl/h264_multinal_stream_path_tb.cpp

set +e
"$BUILD/Vh264_multinal_stream_path_tb" "$WCAP_FIXTURE" > "$BUILD/wcap_implicit_defaults.log" 2>&1
IMPLICIT_RC=$?
set -e
if [[ "$IMPLICIT_RC" -eq 0 ]]; then
  cat "$BUILD/wcap_implicit_defaults.log"
  echo "FAIL multi-NAL stream_path: implicit unproven defaults unexpectedly passed" >&2
  exit 1
fi
grep -q "Refuse implicit defaults" "$BUILD/wcap_implicit_defaults.log"
echo "test_h264_multinal_stream_path: OK refuses implicit unproven defaults rc=$IMPLICIT_RC"

"$BUILD/Vh264_multinal_stream_path_tb" "$WCAP_FIXTURE" 5 1 0x14
"$BUILD/Vh264_multinal_stream_path_tb" "$INTER_FIXTURE" 15 11 0x10

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_FAULT" \
  --top-module h264_multinal_stream_path_tb -GFAULT_RECON_SIG_ZERO=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  tests/rtl/h264_multinal_stream_path_tb_top.sv \
  fpga/Plex_MiSTer/rtl/stream_path.sv \
  fpga/Plex_MiSTer/rtl/stream_ingest.sv \
  fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv \
  fpga/Plex_MiSTer/rtl/bitstream_fifo.sv \
  fpga/Plex_MiSTer/rtl/nalu_scanner.sv \
  fpga/Plex_MiSTer/rtl/sps_parser.sv \
  fpga/Plex_MiSTer/rtl/pps_parser.sv \
  fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv \
  fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv \
  fpga/Plex_MiSTer/rtl/h264_inter_pred.sv \
  fpga/Plex_MiSTer/rtl/h264_deblock.sv \
  fpga/Plex_MiSTer/rtl/h264_dpb.sv \
  fpga/Plex_MiSTer/rtl/decode_stub.sv \
  tests/rtl/h264_multinal_stream_path_tb.cpp
set +e
"$BUILD_FAULT/Vh264_multinal_stream_path_tb" "$INTER_FIXTURE" 15 11 0x10 > "$BUILD/recon_zero_fault.log" 2>&1
RECON_ZERO_RC=$?
set -e
if [[ "$RECON_ZERO_RC" -eq 0 ]]; then
  cat "$BUILD/recon_zero_fault.log"
  echo "FAIL multi-NAL stream_path: forced recon_sig=0 unexpectedly passed" >&2
  exit 1
fi
grep -q "parsed P DPB/MC recon signature missing" "$BUILD/recon_zero_fault.log"
echo "test_h264_multinal_stream_path: OK red-check forced recon_sig=0 rejected parsed P DPB/MC liveness"

set +e
"$BUILD/Vh264_multinal_stream_path_tb" "$WCAP_FIXTURE" 5 1 0xff > "$BUILD/wcap_bad_csum.log" 2>&1
NEG_RC=$?
set -e
if [[ "$NEG_RC" -eq 0 ]]; then
  cat "$BUILD/wcap_bad_csum.log"
  echo "FAIL multi-NAL stream_path: deliberate bad checksum unexpectedly passed" >&2
  exit 1
fi
grep -q "expected residual_csum was never observed" "$BUILD/wcap_bad_csum.log"
echo "test_h264_multinal_stream_path: OK deliberate RED wrong expected checksum rc=$NEG_RC"
