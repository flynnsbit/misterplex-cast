#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERILATOR_BIN="${VERILATOR:-}"
if [[ -z "$VERILATOR_BIN" ]]; then
  if [[ -x "$ROOT/scripts/run_verilator.sh" ]]; then
    VERILATOR_BIN="$ROOT/scripts/run_verilator.sh"
  elif command -v verilator >/dev/null 2>&1; then
    VERILATOR_BIN="$(command -v verilator)"
  elif [[ -x "$HOME/.local/oss-cad-suite-20260726/bin/verilator" ]]; then
    VERILATOR_BIN="$HOME/.local/oss-cad-suite-20260726/bin/verilator"
  fi
fi

if [[ -z "$VERILATOR_BIN" || ! -x "$VERILATOR_BIN" ]]; then
  echo "SKIP: Verilator not found; SDRAM startup command-bus co-sim was not run." >&2
  echo "SKIP: set VERILATOR=/path/to/verilator or install oss-cad-suite under ~/.local." >&2
  exit 0
fi

OUT="$ROOT/build/verilator_sdram_startup"
mkdir -p "$OUT"

"$VERILATOR_BIN" --version

build_variant() {
  local freq="$1"
  local dir="$2"
  local cl3="${3:-0}"
  local -a defs=()
  if [[ "$cl3" == "1" ]]; then
    defs+=(-DSDRAM_CL3)
  fi
  "$VERILATOR_BIN" -Wno-fatal -Wno-IMPLICITSTATIC -Wno-WIDTHTRUNC "${defs[@]}" --cc --exe --build \
    --top-module sdram_startup_top \
    -GSDRAM_CLK_HZ="$freq" \
    -Mdir "$dir" \
    "$ROOT/tests/rtl/sdram_startup_top.sv" \
    "$ROOT/tests/rtl/verilator_altddio_stub.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/sdram.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/sdram_memtest.sv" \
    "$ROOT/tests/rtl/test_sdram_startup.cpp" \
    >/dev/null
}

build_variant 100000000 "$OUT"

set +e
"$OUT/Vsdram_startup_top" --inject-early-read >"$OUT/red.log" 2>&1
red_rc=$?
set -e
if [[ "$red_rc" -eq 0 ]]; then
  cat "$OUT/red.log"
  echo "FAIL: injected early READ did not trip the SDRAM startup monitor" >&2
  exit 1
fi
grep -q 'FAIL: READ before first MODE_REGISTER_SET' "$OUT/red.log" || {
  cat "$OUT/red.log"
  echo "FAIL: red run failed for an unexpected reason" >&2
  exit 1
}
echo "RED OK: injected early READ tripped the startup monitor"
grep 'FAIL: READ before first MODE_REGISTER_SET' "$OUT/red.log"

"$OUT/Vsdram_startup_top" --freq-hz=100000000 >"$OUT/cold-green.log" 2>&1
cat "$OUT/cold-green.log"
grep -q 'PASS: no ACTIVE/READ/WRITE before first MODE_REGISTER_SET' "$OUT/cold-green.log"

"$OUT/Vsdram_startup_top" --freq-hz=100000000 --force-ready-init-high >"$OUT/ready-high.log" 2>&1
cat "$OUT/ready-high.log"
grep -q 'PASS: no ACTIVE/READ/WRITE before first MODE_REGISTER_SET' "$OUT/ready-high.log"
! grep -q 'First memtest sel+(rd|wr) observed at cycle 10' "$OUT/ready-high.log"

build_variant 133333333 "$OUT/freq133" 1
"$OUT/freq133/Vsdram_startup_top" --freq-hz=133333333 --constants-only >"$OUT/freq133.log" 2>&1
cat "$OUT/freq133.log"
grep -q 'PASS: computed startup/refresh constants satisfy SDRAM timing' "$OUT/freq133.log"
grep -q 'cas_latency=3' "$OUT/freq133.log"

build_variant 142000000 "$OUT/freq142" 1
"$OUT/freq142/Vsdram_startup_top" --freq-hz=142000000 --constants-only >"$OUT/freq142.log" 2>&1
cat "$OUT/freq142.log"
grep -q 'PASS: computed startup/refresh constants satisfy SDRAM timing' "$OUT/freq142.log"
grep -q 'startup_cycles=17182' "$OUT/freq142.log"
grep -q 'refresh_cycles=1108' "$OUT/freq142.log"
grep -q 'cas_latency=3' "$OUT/freq142.log"

build_variant 75000000 "$OUT/freq75"
"$OUT/freq75/Vsdram_startup_top" --freq-hz=75000000 --constants-only >"$OUT/freq75.log" 2>&1
cat "$OUT/freq75.log"
grep -q 'PASS: computed startup/refresh constants satisfy SDRAM timing' "$OUT/freq75.log"

set +e
"$OUT/Vsdram_startup_top" --freq-hz=133333333 --check-legacy-100mhz-constants >"$OUT/legacy-133-red.log" 2>&1
legacy_133_rc=$?
"$OUT/Vsdram_startup_top" --freq-hz=75000000 --check-legacy-100mhz-constants >"$OUT/legacy-75-red.log" 2>&1
legacy_75_rc=$?
build_variant 133333333 "$OUT/freq133-cl2-red" 0
"$OUT/freq133-cl2-red/Vsdram_startup_top" --freq-hz=133333333 --constants-only >"$OUT/cas-133-cl2-red.log" 2>&1
cas_133_rc=$?
set -e
if [[ "$legacy_133_rc" -eq 0 || "$legacy_75_rc" -eq 0 || "$cas_133_rc" -eq 0 ]]; then
 cat "$OUT/legacy-133-red.log" "$OUT/legacy-75-red.log" "$OUT/cas-133-cl2-red.log"
 echo "FAIL: legacy 100MHz constants did not trip the frequency timing assertions" >&2
 exit 1
fi
echo "RED OK: legacy 100MHz constants trip frequency timing assertions"
grep 'FAIL:' "$OUT/legacy-133-red.log"
grep 'FAIL:' "$OUT/legacy-75-red.log"
echo "RED OK: invalid high-frequency CAS setting trips timing assertion"
grep 'FAIL: configured CAS latency 2 is not valid for 133333333 Hz' "$OUT/cas-133-cl2-red.log"
