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
SKIP RTL SIM: Verilator not found; h264_inter_pred real RTL simulation was NOT run.
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

RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_inter_pred.sv"
QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TB="$ROOT/tests/rtl/h264_inter_pred_tb.cpp"
TOP="$ROOT/tests/rtl/h264_inter_pred_tb_top.sv"
FIXTURE="$ROOT/tests/fixtures/p3_inter_pred/inter_mc_v1.json"
BUILD="$ROOT/build/verilator/h264_inter_pred"
BUILD_FAULT="$ROOT/build/verilator/h264_inter_pred_bad_round"
BUILD_PART_FAULT="$ROOT/build/verilator/h264_inter_pred_bad_part_mv"
REGEN="$ROOT/build/p3_inter_mc_v1.regen.json"

for f in "$RTL" "$QIP" "$TB" "$TOP" "$FIXTURE"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
if ! grep -q 'rtl/h264_inter_pred.sv' "$QIP"; then
  echo "RTL SIM ERROR: files.qip does not list the product RTL under simulation" >&2
  exit 2
fi

mkdir -p "$BUILD" "$BUILD_FAULT" "$BUILD_PART_FAULT"
python3 "$ROOT/scripts/gen_p3_inter_mc_fixture.py" "$REGEN" >/dev/null
if ! cmp -s "$FIXTURE" "$REGEN"; then
  echo "RTL SIM ERROR: inter MC fixture is not byte-identical to generator" >&2
  exit 2
fi
echo "RTL SIM: using $VERILATOR_VERSION" >&2

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module h264_inter_pred_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
"$BUILD/Vh264_inter_pred_tb" "$FIXTURE"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_FAULT" \
  --top-module h264_inter_pred_tb -GFAULT_BAD_ROUND=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
set +e
FAULT_OUT="$("$BUILD_FAULT/Vh264_inter_pred_tb" "$FIXTURE" 2>&1)"
FAULT_RC=$?
set -e
if ! RED_CHECK="$(python3 "$ROOT/tests/unit/expected_red.py" h264_inter_pred_bad_round "$FAULT_RC" <<<"$FAULT_OUT" 2>&1)"; then
  printf '%s\n%s\n' "$RED_CHECK" "$FAULT_OUT" >&2
  exit 1
fi
printf '%s\n' "$RED_CHECK"
echo "OK h264_inter_pred RTL red-check: bad rounding fault failed golden"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_PART_FAULT" \
  --top-module h264_inter_pred_tb -GFAULT_BAD_PART_MV=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
set +e
PART_FAULT_OUT="$($BUILD_PART_FAULT/Vh264_inter_pred_tb "$FIXTURE" 2>&1)"
PART_FAULT_RC=$?
set -e
if ! RED_CHECK="$(python3 "$ROOT/tests/unit/expected_red.py" h264_inter_pred_bad_part_mv "$PART_FAULT_RC" <<<"$PART_FAULT_OUT" 2>&1)"; then
  printf '%s\n%s\n' "$RED_CHECK" "$PART_FAULT_OUT" >&2
  exit 1
fi
printf '%s\n' "$RED_CHECK"
echo "OK h264_inter_pred RTL red-check: bad partition MV fault failed golden"
