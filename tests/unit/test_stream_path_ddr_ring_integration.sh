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
SKIP RTL SIM: Verilator not found; stream_path DDR ring integration was NOT run.
Install oss-cad-suite under ~/.local/oss-cad-suite or run with VERILATOR=/path/to/verilator.
SKIP
  exit 0
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed:" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TOP="$ROOT/tests/rtl/stream_path_ddr_ring_tb_top.sv"
TB="$ROOT/tests/rtl/stream_path_ddr_ring_tb.cpp"
SHARED_FIXTURE="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264"
BUILD_ROOT="$ROOT/build/verilator"
BUILD_OK="$BUILD_ROOT/stream_path_ddr_ring"
BUILD_WRAP="$BUILD_ROOT/stream_path_ddr_ring_wrap_fault"
BUILD_UNDER="$BUILD_ROOT/stream_path_ddr_ring_underrun_fault"
BUILD_OVER="$BUILD_ROOT/stream_path_ddr_ring_overrun_fault"

RTL=(
  "$ROOT/fpga/Plex_MiSTer/rtl/stream_path.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/stream_ingest.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/bitstream_fifo.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/nalu_scanner.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/sps_parser.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/pps_parser.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_cavlc_residual.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_inter_pred.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_deblock.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/h264_dpb.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/decode_stub.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv"
)

for f in "$QIP" "$TOP" "$TB" "$SHARED_FIXTURE" "${RTL[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
for src in rtl/ddr_bitstream_reader.sv rtl/ddr_bus_arbiter.sv rtl/stream_path.sv; do
  if ! grep -q "$src" "$QIP"; then
    echo "RTL SIM ERROR: files.qip does not list product RTL $src" >&2
    exit 2
  fi
done

build_and_run() {
  local out="$1"; shift
  mkdir -p "$out"
  "$RUN_VERILATOR" --cc --exe --build \
    --Mdir "$out" \
    --top-module stream_path_ddr_ring_tb_top "$@" -Wno-fatal \
    -CFLAGS "-std=c++17 -O2" \
    "$TOP" "${RTL[@]}" "$TB"
  "$out/Vstream_path_ddr_ring_tb_top" "$SHARED_FIXTURE"
}

echo "RTL SIM: using $VERILATOR_VERSION" >&2
build_and_run "$BUILD_OK"

set +e
WRAP_OUT="$(build_and_run "$BUILD_WRAP" -GFAULT_WRAP_DATA=1 2>&1)"
WRAP_RC=$?
UNDER_OUT="$(build_and_run "$BUILD_UNDER" -GFAULT_UNDERRUN_TELEM=1 2>&1)"
UNDER_RC=$?
OVER_OUT="$(build_and_run "$BUILD_OVER" -GFAULT_OVERRUN_TELEM=1 2>&1)"
OVER_RC=$?
set -e
printf '%s\n' "$WRAP_OUT"
printf '%s\n' "$UNDER_OUT"
printf '%s\n' "$OVER_OUT"
if [[ "$WRAP_RC" -eq 0 ]] || ! grep -q 'wrap' <<<"$WRAP_OUT"; then
  echo "FAIL stream_path DDR ring red-check: wrap fault did not fail wrap gate" >&2
  exit 1
fi
if [[ "$UNDER_RC" -eq 0 ]] || ! grep -q 'underrun' <<<"$UNDER_OUT"; then
  echo "FAIL stream_path DDR ring red-check: underrun fault did not fail telemetry gate" >&2
  exit 1
fi
if [[ "$OVER_RC" -eq 0 ]] || ! grep -q 'overrun' <<<"$OVER_OUT"; then
  echo "FAIL stream_path DDR ring red-check: overrun fault did not fail telemetry gate" >&2
  exit 1
fi
echo "OK stream_path DDR ring red-checks: wrap, underrun, overrun each failed when perturbed"
