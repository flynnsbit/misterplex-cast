#!/usr/bin/env bash
# Verilator sim for io_ack_follow — product ACK ≤2 clk_sys; FAULT twin never ACKs.
# Soft-skip≠PASS. true rc captured directly (never through a pipe).
set -euo pipefail

assert_sim_executed() {
  local label="$1"; shift
  local log="$1"; shift
  local missing=0
  local m
  for m in "$@"; do
    if ! grep -q -- "$m" <<<"$log"; then
      echo "FAIL $label: sim did not EXECUTE expected marker: $m" >&2
      missing=1
    fi
  done
  if [[ "$missing" -ne 0 ]]; then
    echo "FAIL $label: compile-only or empty run is not a pass" >&2
    exit 2
  fi
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
MDIR="$ROOT/build/verilator/io_ack_follow"
RTL="$ROOT/fpga/Plex_MiSTer/rtl/io_ack_follow.sv"
TOP="$ROOT/tests/rtl/io_ack_follow_tb_top.sv"
TB="$ROOT/tests/rtl/io_ack_follow_tb.cpp"
mkdir -p "$MDIR"

echo "=== test_io_ack_follow_rtl_sim EXECUTED ==="

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

test -f "$RTL" || { echo "FAIL missing $RTL" >&2; echo "true rc=2"; exit 2; }

set +e
"$RUN_VERILATOR" --cc --exe --build \
  --Mdir "$MDIR" \
  --top-module io_ack_follow_tb_top \
  -Wno-fatal \
  -CFLAGS "-std=c++17 -O2" \
  "$TOP" "$RTL" "$TB"
v_rc=$?
set -e
echo "verilator_build true rc=$v_rc"
if [[ "$v_rc" -ne 0 ]]; then
  echo "FAIL verilator build" >&2
  echo "true rc=$v_rc"
  exit "$v_rc"
fi

BIN="$MDIR/Vio_ack_follow_tb_top"
test -x "$BIN" || BIN="$MDIR/Vio_ack_follow_tb_top.exe"
set +e
out=$("$BIN" 2>&1)
rc=$?
set -e
printf '%s\n' "$out"
echo "sim true rc=$rc"
echo "SIM_ARTIFACT=$BIN"

assert_sim_executed "io_ack_follow" "$out" \
  "CASE EXECUTED" \
  "OK product_ack_clk_scale" \
  "OK 2ms_wall_is_not_product_ack_scale" \
  "OK fault_twin_never_acks" \
  "PASS io_ack_follow_tb"

if [[ "$rc" -ne 0 ]]; then
  echo "FAIL io_ack_follow sim true rc=$rc" >&2
  echo "true rc=$rc"
  exit "$rc"
fi

echo "OK io_ack_follow red-check: FAULT_STUCK_WAIT never ACKs"
echo "PASS test_io_ack_follow_rtl_sim"
echo "true rc=0"
exit 0
