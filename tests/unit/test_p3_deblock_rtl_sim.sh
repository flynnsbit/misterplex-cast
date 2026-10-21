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
SKIP RTL SIM: Verilator not found; h264_deblock real RTL simulation was NOT run.
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

RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_deblock.sv"
QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TOP="$ROOT/tests/rtl/h264_deblock_tb_top.sv"
TB="$ROOT/tests/rtl/h264_deblock_tb.cpp"
BUILD="$ROOT/build/verilator/h264_deblock"
MB_COMMIT_FAULT_BUILD="$ROOT/build/verilator/h264_deblock_mb_commit_fault"
WRITEBACK_FAULT_BUILD="$ROOT/build/verilator/h264_deblock_writeback_fault"
GOLDEN="$ROOT/build/p3_golden/deblock_mb0.json"
ANNEXB="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264"
SEQUENCE="$ROOT/tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p_sequence_v1.json"
MB0_REF="$ROOT/tests/fixtures/p3_host_recon/mb0_luma_v1.json"

for f in "$RTL" "$QIP" "$TOP" "$TB"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
if ! grep -q 'rtl/h264_deblock.sv' "$QIP"; then
  echo "RTL SIM ERROR: files.qip does not list product h264_deblock.sv" >&2
  exit 2
fi

mkdir -p "$BUILD" "$(dirname "$GOLDEN")"
make -s -C "$ROOT" h264-golden-tools
"$ROOT/build/extract_h264_golden" --input "$ANNEXB" --mb 0 --output "$GOLDEN" --verify-mb0-reference "$MB0_REF" >/dev/null
echo "RTL SIM: using $VERILATOR_VERSION" >&2
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD" \
  --top-module h264_deblock_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
"$BUILD/Vh264_deblock_tb" --mb-golden "$GOLDEN" --nal-sequence "$SEQUENCE"

set +e
FAULT_OUT="$($BUILD/Vh264_deblock_tb --mb-golden "$GOLDEN" --nal-sequence "$SEQUENCE" --fault-horizontal-first 2>&1)"
FAULT_RC=$?
set -e
printf '%s\n' "$FAULT_OUT"
if [[ "$FAULT_RC" -eq 0 ]]; then
  echo "FAIL h264_deblock RTL red-check: swapped edge order unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'multi-frame drift' <<<"$FAULT_OUT"; then
  echo "FAIL h264_deblock RTL red-check: expected multi-frame drift mismatch" >&2
  exit 1
fi
echo "OK h264_deblock RTL red-check: swapped edge order produced drift mismatch"

mkdir -p "$MB_COMMIT_FAULT_BUILD"
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$MB_COMMIT_FAULT_BUILD" \
  --top-module h264_deblock_tb -Wno-fatal +define+H264_DEBLOCK_FAULT_MB_COMMIT_EARLY \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
set +e
MB_COMMIT_FAULT_OUT="$("$MB_COMMIT_FAULT_BUILD/Vh264_deblock_tb" --mb-golden "$GOLDEN" --nal-sequence "$SEQUENCE" 2>&1)"
MB_COMMIT_FAULT_RC=$?
set -e
printf '%s\n' "$MB_COMMIT_FAULT_OUT"
if [[ "$MB_COMMIT_FAULT_RC" -eq 0 ]]; then
  echo "FAIL h264_deblock RTL red-check: early MB commit fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'MB commit before all filtered samples' <<<"$MB_COMMIT_FAULT_OUT"; then
  echo "FAIL h264_deblock RTL red-check: expected filtered-sample/MB-commit diagnostic" >&2
  exit 1
fi
echo "OK h264_deblock RTL red-check: early MB commit before filtered samples failed"

mkdir -p "$WRITEBACK_FAULT_BUILD"
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$WRITEBACK_FAULT_BUILD" \
  --top-module h264_deblock_tb -Wno-fatal +define+H264_DEBLOCK_FAULT_REF_READY_EARLY \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
set +e
WRITEBACK_FAULT_OUT="$("$WRITEBACK_FAULT_BUILD/Vh264_deblock_tb" --mb-golden "$GOLDEN" --nal-sequence "$SEQUENCE" 2>&1)"
WRITEBACK_FAULT_RC=$?
set -e
printf '%s\n' "$WRITEBACK_FAULT_OUT"
if [[ "$WRITEBACK_FAULT_RC" -eq 0 ]]; then
  echo "FAIL h264_deblock RTL red-check: early DPB ref_ready fault unexpectedly passed" >&2
  exit 1
fi
if ! grep -q 'DPB ref ready before frame boundary' <<<"$WRITEBACK_FAULT_OUT"; then
  echo "FAIL h264_deblock RTL red-check: expected writeback/ref_ready diagnostic" >&2
  exit 1
fi
echo "OK h264_deblock RTL red-check: early DPB ref_ready fault failed"
