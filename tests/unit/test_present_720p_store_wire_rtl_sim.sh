#!/usr/bin/env bash
# Prove ddr_frame_store geometry selection: 480p window unchanged, 720p full
# 1280x720 visible, bank stride 0x180000, corners 1279/719 in and 1280/720 out.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
RTL="$ROOT/fpga/Plex_MiSTer/rtl"
INC="-I$RTL"

set +e
VERILATOR_VERSION="$($RUN_VERILATOR --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  echo "RTL SIM ERROR: Verilator not found; refusing SKIP-as-pass" >&2
  exit 3
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

QSF="$ROOT/fpga/Plex_MiSTer/Plex.qsf"
if grep -E '^[^#]*PLEX_PRESENT_720P_L4=1' "$QSF" >/dev/null; then
  echo "RTL SIM ERROR: PLEX_PRESENT_720P_L4 active in product QSF" >&2
  exit 2
fi

echo "RTL SIM: using $VERILATOR_VERSION" >&2
B="$ROOT/build/verilator/ddr_frame_present_geom"
mkdir -p "$B"
"$RUN_VERILATOR" --cc --exe --build --Mdir "$B" \
  --top-module ddr_frame_present_geom_tb_top -Wno-fatal \
  $INC \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddr_frame_present_geom_tb_top.sv" \
  "$RTL/ddr_frame_present_geom.sv" \
  "$ROOT/tests/rtl/ddr_frame_present_geom_tb.cpp"

set +e
OUT="$("$B/Vddr_frame_present_geom_tb_top" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"
echo "ddr_frame_present_geom true rc=$RC"
if [[ "$RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: ddr_frame_present_geom boundaries failed" >&2
  exit "$RC"
fi
if ! grep -q "PASS ddr_frame_present_geom_tb" <<<"$OUT"; then
  echo "RTL SIM ERROR: missing PASS marker" >&2
  exit 2
fi
echo "PASS test_present_720p_store_wire_rtl_sim"
exit 0
