#!/usr/bin/env bash
# Geometry parameterization gates:
#  1) present_store_geom default == prerefactor (byte-identical store map)
#  2) ddram_frame_rd default == prerefactor (pixel+addr identical) + 720p stride
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

# Product default-off
QSF="$ROOT/fpga/Plex_MiSTer/Plex.qsf"
if grep -E '^[^#]*PLEX_PRESENT_720P_L4=1' "$QSF" >/dev/null; then
  echo "RTL SIM ERROR: PLEX_PRESENT_720P_L4 active in product QSF" >&2
  exit 2
fi

echo "RTL SIM: using $VERILATOR_VERSION" >&2

# ---- 1) present_store_geom identity ----
B1="$ROOT/build/verilator/present_store_geom"
mkdir -p "$B1"
"$RUN_VERILATOR" --cc --exe --build --Mdir "$B1" \
  --top-module present_store_geom_tb_top -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/present_store_geom_tb_top.sv" \
  "$RTL/present_store_geom.sv" \
  "$ROOT/tests/rtl/ref/present_store_geom_prerefactor.sv" \
  "$ROOT/tests/rtl/present_store_geom_tb.cpp"
set +e
"$B1/Vpresent_store_geom_tb_top"
RC1=$?
set -e
echo "present_store_geom true rc=$RC1"
if [[ "$RC1" -ne 0 ]]; then
  echo "RTL SIM ERROR: present_store_geom identity/720p failed" >&2
  exit "$RC1"
fi

# ---- 2) ddram_frame_rd dual + 720p ----
B2="$ROOT/build/verilator/ddram_frame_rd_geom"
mkdir -p "$B2"
"$RUN_VERILATOR" --cc --exe --build --Mdir "$B2" \
  --top-module ddram_frame_rd_geom_tb_top -Wno-fatal -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
  $INC \
  -CFLAGS "-std=c++17 -O2" \
  "$ROOT/tests/rtl/ddram_frame_rd_geom_tb_top.sv" \
  "$RTL/ddram_frame_rd.sv" \
  "$ROOT/tests/rtl/ref/ddram_frame_rd_prerefactor.sv" \
  "$ROOT/tests/rtl/ddram_frame_rd_geom_tb.cpp"
set +e
"$B2/Vddram_frame_rd_geom_tb_top"
RC2=$?
set -e
echo "ddram_frame_rd_geom true rc=$RC2"
if [[ "$RC2" -ne 0 ]]; then
  echo "RTL SIM ERROR: ddram_frame_rd geom failed" >&2
  exit "$RC2"
fi

echo "PASS test_present_geom_params_rtl_sim"
exit 0
