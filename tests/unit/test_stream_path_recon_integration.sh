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
SKIP RTL SIM: Verilator not found; stream_path integrated reconstruction simulation was NOT run.
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
RTL_STREAM="$ROOT/fpga/Plex_MiSTer/rtl/stream_path.sv"
RTL_INGEST="$ROOT/fpga/Plex_MiSTer/rtl/stream_ingest.sv"
RTL_DDR="$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv"
RTL_FIFO="$ROOT/fpga/Plex_MiSTer/rtl/bitstream_fifo.sv"
RTL_SCAN="$ROOT/fpga/Plex_MiSTer/rtl/nalu_scanner.sv"
RTL_SPS="$ROOT/fpga/Plex_MiSTer/rtl/sps_parser.sv"
RTL_PPS="$ROOT/fpga/Plex_MiSTer/rtl/pps_parser.sv"
RTL_SLICE="$ROOT/fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv"
RTL_DECODE="$ROOT/fpga/Plex_MiSTer/rtl/decode_stub.sv"
RTL_IQ="$ROOT/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv"
RTL_INTER="$ROOT/fpga/Plex_MiSTer/rtl/h264_inter_pred.sv"
RTL_DEBLOCK="$ROOT/fpga/Plex_MiSTer/rtl/h264_deblock.sv"
RTL_DPB="$ROOT/fpga/Plex_MiSTer/rtl/h264_dpb.sv"
TOP="$ROOT/tests/rtl/stream_path_recon_integration_tb_top.sv"
TB="$ROOT/tests/rtl/stream_path_recon_integration_tb.cpp"
BITSTREAM="$ROOT/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264"
REF="$ROOT/tests/fixtures/p3_host_recon/mb0_luma_v1.json"
GOLD="$ROOT/build/p3_golden/mb0_stream_path_integration.json"
BUILD="$ROOT/build/verilator/stream_path_recon_integration"
BUILD_FAULT="$ROOT/build/verilator/stream_path_recon_integration_fault"

for f in "$QIP" "$RTL_STREAM" "$RTL_INGEST" "$RTL_DDR" "$RTL_FIFO" "$RTL_SCAN" "$RTL_SPS" "$RTL_PPS" \
         "$RTL_SLICE" "$RTL_DECODE" "$RTL_IQ" "$RTL_INTER" "$RTL_DEBLOCK" "$RTL_DPB" "$TOP" "$TB" "$BITSTREAM" "$REF"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
for rtl in rtl/stream_path.sv rtl/stream_ingest.sv rtl/ddr_bitstream_reader.sv rtl/bitstream_fifo.sv rtl/nalu_scanner.sv \
           rtl/sps_parser.sv rtl/pps_parser.sv rtl/slice_hdr_parser.sv rtl/decode_stub.sv \
           rtl/h264_iq_idct_4x4.sv rtl/h264_inter_pred.sv rtl/h264_deblock.sv rtl/h264_dpb.sv; do
  if ! grep -q "$rtl" "$QIP"; then
    echo "RTL SIM ERROR: files.qip does not list product RTL under simulation: $rtl" >&2
    exit 2
  fi
done

mkdir -p "$BUILD" "$BUILD_FAULT" "$ROOT/build/p3_golden"
if [[ ! -x "$ROOT/build/extract_h264_golden" ]]; then
  make -C "$ROOT" h264-golden-tools >/dev/null
fi
"$ROOT/build/extract_h264_golden" --input "$BITSTREAM" --mb 0 --output "$GOLD" --verify-mb0-reference "$REF" >/dev/null

echo "RTL SIM: using $VERILATOR_VERSION (stream_path_recon_integration)" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module stream_path_recon_integration_tb_top -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL_STREAM" "$RTL_INGEST" "$RTL_DDR" "$RTL_FIFO" "$RTL_SCAN" "$RTL_SPS" "$RTL_PPS" \
  "$RTL_SLICE" "$RTL_DECODE" "$RTL_IQ" "$RTL_INTER" "$RTL_DEBLOCK" "$RTL_DPB" "$TB"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_FAULT" \
  --top-module stream_path_recon_integration_tb_top -GFAULT_RECON_SIG_ZERO=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL_STREAM" "$RTL_INGEST" "$RTL_DDR" "$RTL_FIFO" "$RTL_SCAN" "$RTL_SPS" "$RTL_PPS" \
  "$RTL_SLICE" "$RTL_DECODE" "$RTL_IQ" "$RTL_INTER" "$RTL_DEBLOCK" "$RTL_DPB" "$TB"

"$BUILD/Vstream_path_recon_integration_tb_top" normal "$BITSTREAM" "$GOLD"
"$BUILD/Vstream_path_recon_integration_tb_top" escape-red "$BITSTREAM" "$GOLD"

set +e
FAULT_OUT="$("$BUILD_FAULT/Vstream_path_recon_integration_tb_top" normal "$BITSTREAM" "$GOLD" 2>&1)"
FAULT_RC=$?
set -e
printf '%s\n' "$FAULT_OUT"
if [[ "$FAULT_RC" -eq 0 ]]; then
  echo "FAIL stream_path integration red-check: forced recon_sig=0x00 unexpectedly passed" >&2
  exit 1
fi
if ! grep -qi 'recon_sig got 0x0 want 0x3b' <<<"$FAULT_OUT"; then
  echo "FAIL stream_path integration red-check: did not reject forced 0x00 vs true 0x3b" >&2
  exit 1
fi
echo "OK stream_path integration red-check: forced recon_sig=0x00 rejected against true golden 0x3b"
