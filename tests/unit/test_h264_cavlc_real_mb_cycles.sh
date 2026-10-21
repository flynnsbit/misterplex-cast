#!/usr/bin/env bash
# Real-bitstream CAVLC residual cy/MB (product h264_cavlc_residual_block).
# LABEL: residual parse only — not full decoder. Recon lower bound is +34.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_cavlc_residual.sv"
TOP="$ROOT/tests/rtl/h264_cavlc_residual_tb_top.sv"
TB="$ROOT/tests/rtl/h264_cavlc_real_mb_cycles_tb.cpp"
# Default: lab rk7 direct-play extract (1280x720 CB L3.0 24fps, has_b=0).
# Override with CAVLC_CY_STREAM=... for 320x240 regression.
STREAM="${CAVLC_CY_STREAM:-$ROOT/tests/fixtures/p720_cavlc/rk7_1280x720_cb_l30.264}"
OSS_CAD_SUITE="${OSS_CAD_SUITE:-$HOME/.local/oss-cad-suite-20260726}"
BUILD_DIR="$ROOT/build/verilator/h264_cavlc_real_mb_cy"

set +e
VERILATOR_VERSION="$(OSS_CAD_SUITE="$OSS_CAD_SUITE" "$RUN_VERILATOR" --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  echo "SKIP-NOT-PASS: Verilator missing; soft-skip≠PASS" >&2
  exit 77
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed:" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

if [[ ! -f "$STREAM" ]]; then
  echo "FAIL missing stream $STREAM" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"
echo "RTL SIM: CAVLC real-MB cy/MB using $VERILATOR_VERSION" >&2
OSS_CAD_SUITE="$OSS_CAD_SUITE" "$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_DIR" \
  --top-module h264_cavlc_residual_tb_top -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
  -CFLAGS "-std=c++17 -O2 -I$ROOT/host -DCAVLC_CYCLE_PROBE" \
  -DCAVLC_CYCLE_PROBE \
  "$RTL" "$TOP" "$TB"

EXE="$BUILD_DIR/Vh264_cavlc_residual_tb_top"
# shellcheck source=tests/unit/lib_rtl_sim_gate.sh
source "$ROOT/tests/unit/lib_rtl_sim_gate.sh"

set +e
OUT="$("$EXE" --stream "$STREAM" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"
echo "cavlc_real_mb_cy true rc=$RC"
if [[ "$RC" -ne 0 ]]; then
  exit "$RC"
fi
assert_sim_executed "h264_cavlc_real_mb_cycles" "$OUT" "CAVLC_REAL_MB_CYCLES PASS"
# Require architecture lines so a truncated run cannot green.
if ! grep -q 'HEADLINE_720P_AT_20MHz' <<<"$OUT"; then
  echo "FAIL missing HEADLINE_720P_AT_20MHz" >&2
  exit 1
fi
if ! grep -q 'cy_MB_luma+chroma_residual' <<<"$OUT"; then
  echo "FAIL missing cy_MB stats" >&2
  exit 1
fi
echo "CAVLC real-MB cycle gate PASS"
