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
SKIP RTL SIM: Verilator not found; h264_syntax_primitives real RTL simulation was NOT run.
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

RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_syntax_primitives.sv"
QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TOP="$ROOT/tests/rtl/h264_syntax_primitives_tb_top.sv"
TB="$ROOT/tests/rtl/h264_syntax_primitives_tb.cpp"
FIXTURE="$ROOT/tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264"
BUILD_ROOT="$ROOT/build/verilator"
BUILD_OK="$BUILD_ROOT/h264_syntax_primitives"
BUILD_EPB_FAULT="$BUILD_ROOT/h264_syntax_primitives_epb_fault"
BUILD_SE_FAULT="$BUILD_ROOT/h264_syntax_primitives_se_fault"

for f in "$RTL" "$QIP" "$TOP" "$TB" "$FIXTURE"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
if ! grep -q 'rtl/h264_syntax_primitives.sv' "$QIP"; then
  echo "RTL SIM ERROR: files.qip does not list h264_syntax_primitives.sv product RTL" >&2
  exit 2
fi

mkdir -p "$BUILD_OK" "$BUILD_EPB_FAULT" "$BUILD_SE_FAULT"
echo "RTL SIM: using $VERILATOR_VERSION" >&2

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_OK" \
  --top-module h264_syntax_primitives_tb_top -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
"$BUILD_OK/Vh264_syntax_primitives_tb_top"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_EPB_FAULT" \
  --top-module h264_syntax_primitives_tb_top -GFAULT_LEAK_EPB=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
set +e
EPB_OUT="$($BUILD_EPB_FAULT/Vh264_syntax_primitives_tb_top 2>&1)"
EPB_RC=$?
set -e
printf '%s\n' "$EPB_OUT"
if [[ "$EPB_RC" -eq 0 ]]; then
  echo "FAIL h264_syntax_primitives red-check: EPB leak fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'epb_' <<<"$EPB_OUT"; then
  echo "FAIL h264_syntax_primitives red-check: EPB fault did not fail EPB cases" >&2
  exit 1
fi
echo "OK h264_syntax_primitives red-check: EPB leak fault failed synthetic/real RBSP checks"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_SE_FAULT" \
  --top-module h264_syntax_primitives_tb_top -GFAULT_SE_SIGN=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
set +e
SE_OUT="$($BUILD_SE_FAULT/Vh264_syntax_primitives_tb_top 2>&1)"
SE_RC=$?
set -e
printf '%s\n' "$SE_OUT"
if [[ "$SE_RC" -eq 0 ]]; then
  echo "FAIL h264_syntax_primitives red-check: SE sign fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q '^se ' <<<"$SE_OUT"; then
  echo "FAIL h264_syntax_primitives red-check: SE fault did not fail signed Exp-Golomb cases" >&2
  exit 1
fi
echo "OK h264_syntax_primitives red-check: SE sign fault failed signed Exp-Golomb checks"
