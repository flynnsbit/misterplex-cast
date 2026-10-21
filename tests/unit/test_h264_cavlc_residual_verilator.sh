#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_cavlc_residual.sv"
TOP="$ROOT/tests/rtl/h264_cavlc_residual_tb_top.sv"
TB="$ROOT/tests/rtl/h264_cavlc_residual_tb.cpp"
OSS_CAD_SUITE="${OSS_CAD_SUITE:-$HOME/.local/oss-cad-suite-20260726}"

set +e
VERILATOR_VERSION="$(OSS_CAD_SUITE="$OSS_CAD_SUITE" "$RUN_VERILATOR" --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  cat >&2 <<SKIP
SKIP RTL SIM: Verilator not found; h264_cavlc_residual real RTL simulation was NOT run.
Install oss-cad-suite under ~/.local/oss-cad-suite-20260726 or set VERILATOR=/path/to/verilator.
SKIP
  if [[ "${ALLOW_MISSING_VERILATOR:-0}" != "1" ]]; then
    echo "RTL SIM ERROR: Verilator not found; refusing to report PASS without running the simulation." >&2
    echo "A skipped RTL gate is NOT a pass. Set ALLOW_MISSING_VERILATOR=1 only if you accept that RTL was never verified." >&2
    exit 3
  fi
  echo "SKIP-NOT-PASS: Verilator missing; soft-skip≠PASS" >&2
  exit 77
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed:" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

build_one() {
  local name="$1"
  local extra_define="$2"
  local build_dir="$ROOT/build/verilator/$name"
  mkdir -p "$build_dir"
  echo "RTL SIM: using $VERILATOR_VERSION ($name)" >&2
  OSS_CAD_SUITE="$OSS_CAD_SUITE" "$RUN_VERILATOR" --cc --exe --build \
    --Mdir "$build_dir" \
    --top-module h264_cavlc_residual_tb_top -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    -CFLAGS "-std=c++17 -O2 -I$ROOT/host" \
    $extra_define \
    "$RTL" "$TOP" "$TB"
}

build_one h264_cavlc_residual_neg -DCAVLC_NEGATIVE_TEST
NEG_EXE="$ROOT/build/verilator/h264_cavlc_residual_neg/Vh264_cavlc_residual_tb_top"
set +e
"$NEG_EXE" > "$ROOT/build/h264_cavlc_residual_negative.log" 2>&1
NEG_RC=$?
set -e
if [[ "$NEG_RC" -eq 0 ]]; then
  echo "RTL SIM ERROR: negative CAVLC perturbation unexpectedly passed" >&2
  cat "$ROOT/build/h264_cavlc_residual_negative.log" >&2
  exit 1
fi
echo "RTL SIM RED proof: CAVLC_NEGATIVE_TEST failed as expected (rc=$NEG_RC)" >&2
sed -n '1,5p' "$ROOT/build/h264_cavlc_residual_negative.log" >&2

build_one h264_cavlc_residual_pos ''
POS_EXE="$ROOT/build/verilator/h264_cavlc_residual_pos/Vh264_cavlc_residual_tb_top"
# shellcheck source=tests/unit/lib_rtl_sim_gate.sh
source "$ROOT/tests/unit/lib_rtl_sim_gate.sh"
set +e
POS_OUT="$("$POS_EXE" 2>&1)"
POS_RC=$?
set -e
printf '%s\n' "$POS_OUT"
echo "cavlc_residual_pos true rc=$POS_RC"
if [[ "$POS_RC" -ne 0 ]]; then
  exit "$POS_RC"
fi
assert_sim_executed "h264_cavlc_residual_pos" "$POS_OUT" "Verilator PASS"
