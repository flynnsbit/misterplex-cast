#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERILATOR_BIN="${VERILATOR:-}"
if [[ -z "$VERILATOR_BIN" ]]; then
  if [[ -x "$ROOT/scripts/run_verilator.sh" ]]; then
    VERILATOR_BIN="$ROOT/scripts/run_verilator.sh"
  elif command -v verilator >/dev/null 2>&1; then
    VERILATOR_BIN="$(command -v verilator)"
  fi
fi

if [[ -z "$VERILATOR_BIN" ]]; then
  echo "SKIP: Verilator runner not found; SDRAM DQ turnaround co-sim was not run." >&2
  exit 0
fi

OUT="$ROOT/build/verilator_sdram_dq"
mkdir -p "$OUT"

"$VERILATOR_BIN" --version

build_variant() {
  local name="$1"
  local drives="$2"
  local cl3="$3"
  local dir="$OUT/$name"
  mkdir -p "$dir"
  local -a defs=()
  if [[ "$cl3" == "1" ]]; then
    defs+=(-DSDRAM_CL3)
  fi
  "$VERILATOR_BIN" -Wno-fatal -Wno-IMPLICITSTATIC -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND "${defs[@]}" --cc --exe --build \
    --top-module sdram_dq_turnaround_top \
    -GSDRAM_CLK_HZ=100000000 \
    -GDEVICE_DRIVES="$drives" \
    -Mdir "$dir" \
    "$ROOT/tests/rtl/sdram_dq_turnaround_top.sv" \
    "$ROOT/tests/rtl/sdram_read_model.sv" \
    "$ROOT/tests/rtl/verilator_altddio_stub.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/sdram.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/sdram_memtest.sv" \
    "$ROOT/tests/rtl/test_sdram_dq_turnaround.cpp" \
    >/dev/null
}

build_variant cl2 1 0
"$OUT/cl2/Vsdram_dq_turnaround_top" >"$OUT/cl2.log" 2>&1
cat "$OUT/cl2.log"
grep -q 'PASS: read capture aligns with the model drive window without contention' "$OUT/cl2.log"
grep -q 'Mode CAS latency decoded by model: 2' "$OUT/cl2.log"

build_variant cl3 1 1
"$OUT/cl3/Vsdram_dq_turnaround_top" >"$OUT/cl3.log" 2>&1
cat "$OUT/cl3.log"
grep -q 'PASS: read capture aligns with the model drive window without contention' "$OUT/cl3.log"
grep -q 'Mode CAS latency decoded by model: 3' "$OUT/cl3.log"

build_variant floating 0 0
"$OUT/floating/Vsdram_dq_turnaround_top" --floating >"$OUT/floating.log" 2>&1
cat "$OUT/floating.log"
grep -q 'PASS: floating DQ reproduces read_sample=0xffff, first_fail_addr=0, size_code=0' "$OUT/floating.log"
