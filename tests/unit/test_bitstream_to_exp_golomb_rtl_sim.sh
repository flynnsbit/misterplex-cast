#!/usr/bin/env bash
# Verilator: bitstream_bit_feeder → h264_exp_golomb_reader integration (w-path).
# Soft-skip≠PASS.
set -euo pipefail

assert_sim_executed() {
  local label="$1"; shift
  local log="$1"; shift
  local missing=0
  local m
  for m in "$@"; do
    if ! grep -q -- "$m" <<<"$log"; then
      echo "FAIL $label: missing marker: $m" >&2
      missing=1
    fi
  done
  if [[ "$missing" -ne 0 ]]; then
    echo "FAIL $label: compile-only/empty is not a pass" >&2
    exit 2
  fi
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
MDIR="$ROOT/build/verilator/bitstream_to_exp_golomb"
FEED_RTL="$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv"
EG_RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_syntax_primitives.sv"
TOP="$ROOT/tests/rtl/bitstream_to_exp_golomb_tb_top.sv"
TB="$ROOT/tests/rtl/bitstream_to_exp_golomb_tb.cpp"
mkdir -p "$MDIR"

echo "=== test_bitstream_to_exp_golomb_rtl_sim EXECUTED ==="

set +e
VERILATOR_VERSION="$($RUN_VERILATOR --version 2>&1)"
VERILATOR_RC=$?
set -e
if [[ "$VERILATOR_RC" -eq 127 ]]; then
  if [[ "${ALLOW_MISSING_VERILATOR:-0}" == "1" ]]; then
    echo "SKIP-NOT-PASS RTL SIM: Verilator not found" >&2
    exit 77
  fi
  echo "RTL SIM ERROR: Verilator not found" >&2
  exit 3
elif [[ "$VERILATOR_RC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed" >&2
  printf '%s\n' "$VERILATOR_VERSION" >&2
  exit "$VERILATOR_RC"
fi

for f in "$FEED_RTL" "$EG_RTL" "$TOP" "$TB"; do
  test -f "$f" || { echo "FAIL missing $f" >&2; exit 2; }
done

set +e
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$MDIR" \
  --top-module bitstream_to_exp_golomb_tb_top \
  -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$FEED_RTL" "$EG_RTL" "$TB"
v_rc=$?
set -e
echo "verilator_build true rc=$v_rc"
if [[ "$v_rc" -ne 0 ]]; then
  echo "FAIL verilator build" >&2
  exit "$v_rc"
fi

BIN="$MDIR/Vbitstream_to_exp_golomb_tb_top"
test -x "$BIN" || BIN="$MDIR/Vbitstream_to_exp_golomb_tb_top.exe"
set +e
out=$("$BIN" 2>&1)
rc=$?
set -e
printf '%s\n' "$out"
echo "sim true rc=$rc"

assert_sim_executed "bitstream_to_exp_golomb" "$out" \
  "CASE EXECUTED" \
  "OK A_plain_ue" \
  "OK B_gap_plain_ue" \
  "OK B2 epb_straddle_bits" \
  "OK C NEGATIVE naive_keep_0x03_rejected" \
  "OK D mid_symbol_stall_resume" \
  "OK E skid_backpressure" \
  "PASS bitstream_to_exp_golomb"

if [[ "$rc" -ne 0 ]]; then
  echo "FAIL sim rc=$rc" >&2
  exit "$rc"
fi
echo "PASS test_bitstream_to_exp_golomb_rtl_sim"
exit 0
