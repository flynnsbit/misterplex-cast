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
SKIP RTL SIM: Verilator not found; h264_baseline_syntax real RTL simulation was NOT run.
Install oss-cad-suite under ~/.local/oss-cad-suite or run with VERILATOR=/path/to/verilator.
SKIP
  exit 0
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed:" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_syntax_primitives.sv"
QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TOP="$ROOT/tests/rtl/h264_baseline_syntax_tb_top.sv"
TB="$ROOT/tests/rtl/h264_baseline_syntax_tb.cpp"
BUILD_ROOT="$ROOT/build/verilator"
BUILD_OK="$BUILD_ROOT/h264_baseline_syntax"
BUILD_FAULT="$BUILD_ROOT/h264_baseline_syntax_p8_fault"

for f in "$RTL" "$QIP" "$TOP" "$TB"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
if ! grep -q 'rtl/h264_syntax_primitives.sv' "$QIP"; then
  echo "RTL SIM ERROR: files.qip does not list h264_syntax_primitives.sv product RTL" >&2
  exit 2
fi

mkdir -p "$BUILD_OK" "$BUILD_FAULT"
echo "RTL SIM: using $VERILATOR_VERSION" >&2

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_OK" \
  --top-module h264_baseline_syntax_tb_top -Wno-fatal \
  -CFLAGS "-std=c++17 -O2 -I$ROOT/host" \
  "$TOP" "$RTL" "$TB"
"$BUILD_OK/Vh264_baseline_syntax_tb_top"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_FAULT" \
  --top-module h264_baseline_syntax_tb_top -GFAULT_RARE_P8X8=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2 -I$ROOT/host" \
  "$TOP" "$RTL" "$TB"
set +e
FAULT_OUT="$("$BUILD_FAULT/Vh264_baseline_syntax_tb_top" 2>&1)"
FAULT_RC=$?
set -e
printf '%s\n' "$FAULT_OUT"
if [[ "$FAULT_RC" -eq 0 ]]; then
  echo "FAIL h264_baseline_syntax red-check: rare P8x8 fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'rare P8x8 partition' <<<"$FAULT_OUT"; then
  echo "FAIL h264_baseline_syntax red-check: rare P8x8 fault did not trip partition check" >&2
  exit 1
fi
echo "OK h264_baseline_syntax red-check: rare P8x8 perturbation failed partition check"
