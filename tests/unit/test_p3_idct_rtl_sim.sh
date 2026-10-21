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
SKIP RTL SIM: Verilator not found; h264_iq_idct_4x4/decode_stub real RTL simulation was NOT run.
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

RTL_IQ="$ROOT/fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv"
RTL_DECODE="$ROOT/fpga/Plex_MiSTer/rtl/decode_stub.sv"
RTL_INTER="$ROOT/fpga/Plex_MiSTer/rtl/h264_inter_pred.sv"
RTL_DEBLOCK="$ROOT/fpga/Plex_MiSTer/rtl/h264_deblock.sv"
RTL_DPB="$ROOT/fpga/Plex_MiSTer/rtl/h264_dpb.sv"
QIP="$ROOT/fpga/Plex_MiSTer/files.qip"
TB_IQ="$ROOT/tests/rtl/h264_iq_idct_4x4_tb.cpp"
TOP_IQ="$ROOT/tests/rtl/h264_iq_idct_4x4_tb_top.sv"
TB_DECODE="$ROOT/tests/rtl/decode_stub_recon_tb.cpp"
TOP_DECODE="$ROOT/tests/rtl/decode_stub_recon_tb_top.sv"
FIXTURE="$ROOT/tests/fixtures/p3_host_recon/mb0_luma_v1.json"
BUILD_ROOT="$ROOT/build/verilator"
BUILD_IQ="$BUILD_ROOT/h264_iq_idct_4x4"
BUILD_DECODE="$BUILD_ROOT/decode_stub_recon"
BUILD_DECODE_FAULT="$BUILD_ROOT/decode_stub_recon_pred_only"

for f in "$RTL_IQ" "$RTL_DECODE" "$RTL_INTER" "$RTL_DEBLOCK" "$RTL_DPB" "$QIP" "$TB_IQ" "$TOP_IQ" "$TB_DECODE" "$TOP_DECODE" "$FIXTURE"; do
  if [[ ! -f "$f" ]]; then
    echo "RTL SIM ERROR: missing required file: $f" >&2
    exit 2
  fi
done
if ! grep -q 'rtl/h264_iq_idct_4x4.sv' "$QIP" || ! grep -q 'rtl/decode_stub.sv' "$QIP" || ! grep -q 'rtl/h264_inter_pred.sv' "$QIP" || ! grep -q 'rtl/h264_dpb.sv' "$QIP"; then
  echo "RTL SIM ERROR: files.qip does not list the product RTL under simulation" >&2
  exit 2
fi

mkdir -p "$BUILD_IQ" "$BUILD_DECODE" "$BUILD_DECODE_FAULT"
echo "RTL SIM: using $VERILATOR_VERSION" >&2

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_IQ" \
  --top-module h264_iq_idct_4x4 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP_IQ" "$RTL_IQ" "$TB_IQ"
"$BUILD_IQ/Vh264_iq_idct_4x4" "$FIXTURE"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_DECODE" \
  --top-module decode_stub_recon_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP_DECODE" "$RTL_IQ" "$RTL_INTER" "$RTL_DEBLOCK" "$RTL_DPB" "$RTL_DECODE" "$TB_DECODE"
"$BUILD_DECODE/Vdecode_stub_recon_tb" "$FIXTURE"

"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$BUILD_DECODE_FAULT" \
  --top-module decode_stub_recon_tb -GFAULT_PRED_ONLY=1 -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP_DECODE" "$RTL_IQ" "$RTL_INTER" "$RTL_DEBLOCK" "$RTL_DPB" "$RTL_DECODE" "$TB_DECODE"
set +e
FAULT_OUT="$($BUILD_DECODE_FAULT/Vdecode_stub_recon_tb "$FIXTURE" 2>&1)"
FAULT_RC=$?
set -e
if ! RED_CHECK="$(python3 "$ROOT/tests/unit/expected_red.py" decode_stub_pred_only "$FAULT_RC" <<<"$FAULT_OUT" 2>&1)"; then
  printf '%s\n%s\n' "$RED_CHECK" "$FAULT_OUT" >&2
  exit 1
fi
printf '%s\n' "$RED_CHECK"
echo "OK decode_stub RTL red-check: pred-only residual drop produced recon_sig=0x00 and failed golden 0x3b"
