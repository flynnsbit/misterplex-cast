#!/usr/bin/env bash
# Product front-end chain: annex-B → rbsp_filter → bit window → exp_golomb (w-path).
set -euo pipefail
assert_sim_executed() {
  local label="$1"; shift; local log="$1"; shift; local missing=0 m
  for m in "$@"; do
    if ! grep -q -- "$m" <<<"$log"; then echo "FAIL $label: missing $m" >&2; missing=1; fi
  done
  if [[ "$missing" -ne 0 ]]; then exit 2; fi
}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
MDIR="$ROOT/build/verilator/annexb_rbsp_exp_golomb"
FEED="$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv"
SYN="$ROOT/fpga/Plex_MiSTer/rtl/h264_syntax_primitives.sv"
TOP="$ROOT/tests/rtl/annexb_rbsp_exp_golomb_tb_top.sv"
TB="$ROOT/tests/rtl/annexb_rbsp_exp_golomb_tb.cpp"
mkdir -p "$MDIR"
echo "=== test_annexb_rbsp_exp_golomb_rtl_sim EXECUTED ==="
set +e; VER="$($RUN_VERILATOR --version 2>&1)"; VRC=$?; set -e
if [[ "$VRC" -eq 127 ]]; then
  [[ "${ALLOW_MISSING_VERILATOR:-0}" == "1" ]] && { echo "SKIP-NOT-PASS" >&2; exit 77; }
  echo "RTL SIM ERROR: Verilator not found" >&2; exit 3
elif [[ "$VRC" -ne 0 ]]; then echo "RTL SIM ERROR" >&2; exit "$VRC"; fi
set +e
"$RUN_VERILATOR" --cc --exe --build --Mdir "$MDIR" \
  --top-module annexb_rbsp_exp_golomb_tb_top \
  -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$FEED" "$SYN" "$TB"
v_rc=$?; set -e
echo "verilator_build true rc=$v_rc"
[[ "$v_rc" -eq 0 ]] || exit "$v_rc"
BIN="$MDIR/Vannexb_rbsp_exp_golomb_tb_top"
test -x "$BIN" || BIN="$BIN.exe"
set +e; out=$("$BIN" 2>&1); rc=$?; set -e
printf '%s\n' "$out"
echo "sim true rc=$rc"
assert_sim_executed "annexb_rbsp_exp_golomb" "$out" \
  "CASE EXECUTED" \
  "OK A_plain_via_rbsp_filter" \
  "OK B epb_by_rbsp_filter_only" \
  "OK C NEGATIVE filter_strip_not_naive" \
  "OK D nal_last_filter_done" \
  "PASS annexb_rbsp_exp_golomb"
[[ "$rc" -eq 0 ]] || exit "$rc"
echo "PASS test_annexb_rbsp_exp_golomb_rtl_sim"
exit 0
