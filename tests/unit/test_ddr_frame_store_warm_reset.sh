#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
set +e
VERILATOR_VERSION="$($RUN_VERILATOR --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  if [[ "${ALLOW_MISSING_VERILATOR:-0}" == "1" ]]; then
    echo "SKIP RTL SIM: Verilator not found and ALLOW_MISSING_VERILATOR=1; DDR warm-reset simulation was NOT run." >&2
    exit 0
  fi
  cat >&2 <<'ERR'
RTL SIM ERROR: Verilator not found; refusing to report PASS without running the simulation.
A skipped RTL gate is NOT a pass. Set ALLOW_MISSING_VERILATOR=1 only if you accept that RTL was never verified.
ERR
  exit 3
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed:" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset"
FAULT_BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset_fault"
SCHED_FAULT_BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset_sched_fault"
FORMAT_FAULT_BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset_format_fault"
UV_FAULT_BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset_uv_fault"
CHROMA_VERTICAL_FAULT_BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset_chroma_vertical_fault"
CHROMA_STRIDE_FAULT_BUILD="$ROOT/build/verilator/ddr_frame_store_warm_reset_chroma_stride_fault"
mkdir -p "$BUILD" "$FAULT_BUILD" "$SCHED_FAULT_BUILD" "$FORMAT_FAULT_BUILD" "$UV_FAULT_BUILD" "$CHROMA_VERTICAL_FAULT_BUILD" "$CHROMA_STRIDE_FAULT_BUILD"
echo "RTL SIM: using $VERILATOR_VERSION" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GSTALE_DOORBELL_FALLBACK_POLLS=256 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
"$BUILD/Vddr_frame_store_warm_reset_tb"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$FAULT_BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GIGNORE_STALE_DOORBELL_AFTER_RESET=0 -GSTALE_DOORBELL_FALLBACK_POLLS=256 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
set +e
FAULT_OUT="$("$FAULT_BUILD/Vddr_frame_store_warm_reset_tb" 2>&1)"
FAULT_RC=$?
set -e
printf '%s\n' "$FAULT_OUT"
if [[ "$FAULT_RC" -eq 0 ]]; then
  echo "FAIL ddr_frame_store warm-reset red-check: stale-doorbell fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'accepted stale doorbell' <<<"$FAULT_OUT"; then
  echo "FAIL ddr_frame_store warm-reset red-check: expected stale-doorbell diagnostic" >&2
  exit 1
fi
echo "OK ddr_frame_store warm-reset red-check: stale-doorbell fault failed"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$FORMAT_FAULT_BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GSTRICT_YUV_DOORBELL=0 -GSTALE_DOORBELL_FALLBACK_POLLS=256 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
set +e
FORMAT_FAULT_OUT="$("$FORMAT_FAULT_BUILD/Vddr_frame_store_warm_reset_tb" 2>&1)"
FORMAT_FAULT_RC=$?
set -e
printf '%s\n' "$FORMAT_FAULT_OUT"
if [[ "$FORMAT_FAULT_RC" -eq 0 ]]; then
  echo "FAIL ddr_frame_store warm-reset red-check: non-YUV doorbell fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'accepted non-YUV doorbell' <<<"$FORMAT_FAULT_OUT"; then
  echo "FAIL ddr_frame_store warm-reset red-check: expected non-YUV diagnostic" >&2
  exit 1
fi
echo "OK ddr_frame_store warm-reset red-check: non-YUV doorbell fault failed"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$UV_FAULT_BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GSTALE_DOORBELL_FALLBACK_POLLS=256 +define+DDR_FRAME_STORE_FAULT_SWAP_UV_READ -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
set +e
UV_FAULT_OUT="$("$UV_FAULT_BUILD/Vddr_frame_store_warm_reset_tb" 2>&1)"
UV_FAULT_RC=$?
set -e
printf '%s\n' "$UV_FAULT_OUT"
if [[ "$UV_FAULT_RC" -eq 0 ]]; then
  echo "FAIL ddr_frame_store warm-reset red-check: U/V read-swap fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'U/V read mapping' <<<"$UV_FAULT_OUT"; then
  echo "FAIL ddr_frame_store warm-reset red-check: expected U/V read mapping diagnostic" >&2
  exit 1
fi
echo "OK ddr_frame_store warm-reset red-check: U/V read-swap fault failed"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$CHROMA_VERTICAL_FAULT_BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GSTALE_DOORBELL_FALLBACK_POLLS=256 +define+DDR_FRAME_STORE_FAULT_CHROMA_VERTICAL_FULLRES -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
set +e
CHROMA_VERTICAL_FAULT_OUT="$("$CHROMA_VERTICAL_FAULT_BUILD/Vddr_frame_store_warm_reset_tb" 2>&1)"
CHROMA_VERTICAL_FAULT_RC=$?
set -e
printf '%s\n' "$CHROMA_VERTICAL_FAULT_OUT"
if [[ "$CHROMA_VERTICAL_FAULT_RC" -eq 0 ]]; then
  echo "FAIL ddr_frame_store warm-reset red-check: chroma vertical fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'chroma vertical subsampling/stride' <<<"$CHROMA_VERTICAL_FAULT_OUT"; then
  echo "FAIL ddr_frame_store warm-reset red-check: expected chroma vertical diagnostic" >&2
  exit 1
fi
echo "OK ddr_frame_store warm-reset red-check: chroma vertical full-res fault failed"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$CHROMA_STRIDE_FAULT_BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GSTALE_DOORBELL_FALLBACK_POLLS=256 +define+DDR_FRAME_STORE_FAULT_CHROMA_LUMA_STRIDE -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
set +e
CHROMA_STRIDE_FAULT_OUT="$("$CHROMA_STRIDE_FAULT_BUILD/Vddr_frame_store_warm_reset_tb" 2>&1)"
CHROMA_STRIDE_FAULT_RC=$?
set -e
printf '%s\n' "$CHROMA_STRIDE_FAULT_OUT"
if [[ "$CHROMA_STRIDE_FAULT_RC" -eq 0 ]]; then
  echo "FAIL ddr_frame_store warm-reset red-check: chroma stride fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'chroma vertical subsampling/stride' <<<"$CHROMA_STRIDE_FAULT_OUT"; then
  echo "FAIL ddr_frame_store warm-reset red-check: expected chroma stride diagnostic" >&2
  exit 1
fi
echo "OK ddr_frame_store warm-reset red-check: chroma luma-stride fault failed"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$SCHED_FAULT_BUILD" \
  --top-module ddr_frame_store_warm_reset_tb -GPIPELINE_REFILL_SCHEDULER=0 -GSTALE_DOORBELL_FALLBACK_POLLS=256 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-SELRANGE -Wno-UNSIGNED \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/tests/rtl/ddr_frame_store_warm_reset_tb.cpp"
set +e
SCHED_FAULT_OUT="$("$SCHED_FAULT_BUILD/Vddr_frame_store_warm_reset_tb" 2>&1)"
SCHED_FAULT_RC=$?
set -e
printf '%s\n' "$SCHED_FAULT_OUT"
if [[ "$SCHED_FAULT_RC" -eq 0 ]]; then
  echo "FAIL ddr_frame_store warm-reset red-check: scheduler fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'refill scheduler pipeline not observed' <<<"$SCHED_FAULT_OUT"; then
  echo "FAIL ddr_frame_store warm-reset red-check: expected scheduler diagnostic" >&2
  exit 1
fi
echo "OK ddr_frame_store warm-reset red-check: disabled refill scheduler failed pipeline proof"
