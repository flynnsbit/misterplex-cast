#!/usr/bin/env bash
# Red/green gate for h264_intra_nb_ctx MB_WIDTH_MAX lift + sticky geom_reject.
#
# RED  : MAX=40, mb_x=79 — naive test expects geom_reject==0 → must FAIL (rc!=0)
#        proves silent pass is impossible once reject is required.
# GREEN: MAX=80, mb_x=79 — geom_reject must stay 0 → rc=0
# EXTRA: MAX=40, mb_x=79 — require geom_reject==1 → rc=0 (loud reject works)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_VERILATOR="$ROOT/scripts/run_verilator.sh"
RTL="$ROOT/fpga/Plex_MiSTer/rtl/h264_intra_nb_ctx.sv"
TB="$ROOT/tests/unit/rtl/h264_intra_nb_ctx_geom_tb.sv"
BUILD_ROOT="$ROOT/build/verilator/nb_ctx_geom"

set +e
VER="$($RUN_VERILATOR --version 2>&1)"
VRC=$?
set -e
if [[ "$VRC" -ne 0 ]]; then
  echo "RTL SIM ERROR: Verilator probe failed rc=$VRC" >&2
  printf '%s\n' "$VER" >&2
  exit 3
fi
echo "RTL SIM: $VER" >&2

write_main() {
  local expect_reject="$1"   # 0 or 1
  local max_label="$2"
  local outfile="$3"
  cat >"$outfile" <<EOF
#include "Vh264_intra_nb_ctx_geom_tb.h"
#include "verilated.h"
#include <cstdio>
#include <cstdint>

static void tick(Vh264_intra_nb_ctx_geom_tb& d) {
  d.clk = 0; d.eval();
  d.clk = 1; d.eval();
}

int main() {
  Verilated::randReset(2);
  Vh264_intra_nb_ctx_geom_tb dut;
  dut.reset = 1;
  dut.mb_start = 0;
  dut.block_valid = 0;
  dut.mb_x = 0;
  dut.mb_y = 0;
  dut.mb_width = 80;
  dut.block_idx = 0;
  for (int i = 0; i < 4; i++) tick(dut);
  dut.reset = 0;
  tick(dut);

  // Drive mb_x=79 (last column of 80-wide row) with mb_width=80
  dut.mb_x = 79;
  dut.mb_y = 1;
  dut.mb_width = 80;
  dut.mb_start = 1;
  tick(dut);
  dut.mb_start = 0;
  dut.block_idx = 15;
  dut.block_valid = 1;
  tick(dut);
  dut.block_valid = 0;
  for (int i = 0; i < 8; i++) tick(dut);

  const int got = dut.geom_reject ? 1 : 0;
  const int want = ${expect_reject};
  std::printf("geom_reject got=%d want=%d mb_x=79 mb_width=80 MB_WIDTH_MAX=${max_label}\\n",
              got, want);
  if (got != want) {
    std::fprintf(stderr, "FAIL nb_ctx geom: geom_reject=%d expected %d\\n", got, want);
    return 1;
  }
  std::printf("OK nb_ctx geom_reject check\\n");
  return 0;
}
EOF
}

run_case() {
  local name="$1"
  local max="$2"
  local expect_reject="$3"
  local define_extra="${4:-}"
  local bdir="$BUILD_ROOT/$name"
  mkdir -p "$bdir"
  write_main "$expect_reject" "$max" "$bdir/main.cpp"
  local args=(--cc --exe --build --Mdir "$bdir" --top-module h264_intra_nb_ctx_geom_tb -Wno-fatal
    -CFLAGS "-std=c++17 -O2"
    -GMB_WIDTH_MAX="$max")
  if [[ -n "$define_extra" ]]; then
    args+=("$define_extra")
  fi
  args+=("$TB" "$RTL" "$bdir/main.cpp")
  echo "BUILD $name MAX=$max expect_reject=$expect_reject $define_extra" >&2
  "$RUN_VERILATOR" "${args[@]}"
  set +e
  "$bdir/Vh264_intra_nb_ctx_geom_tb"
  local rc=$?
  set +e
  echo "RUN $name true rc=$rc" >&2
  return "$rc"
}

echo "=== RED twin: MAX=40 mb_x=79 expect reject=0 (must FAIL) ===" >&2
set +e
run_case red_naive 40 0
RED_RC=$?
set -e
echo "red_naive true rc=$RED_RC" >&2
if [[ "$RED_RC" -eq 0 ]]; then
  echo "FAIL: red twin did not fail — silent pass still possible" >&2
  exit 1
fi
echo "OK red twin failed as required rc=$RED_RC" >&2

echo "=== LOUD reject: MAX=40 mb_x=79 expect reject=1 (must PASS) ===" >&2
set +e
run_case loud40 40 1
LOUD_RC=$?
set -e
echo "loud40 true rc=$LOUD_RC" >&2
if [[ "$LOUD_RC" -ne 0 ]]; then
  echo "FAIL: geom_reject did not fire at MAX=40 mb_x=79" >&2
  exit 1
fi

echo "=== GREEN lift: MAX=80 mb_x=79 expect reject=0 (must PASS) ===" >&2
set +e
run_case green80 80 0
GREEN_RC=$?
set -e
echo "green80 true rc=$GREEN_RC" >&2
if [[ "$GREEN_RC" -ne 0 ]]; then
  echo "FAIL: lifted MAX=80 still rejects mb_x=79" >&2
  exit 1
fi

echo "=== FAULT twin: NO_GEOM_REJECT define, MAX=40 expect reject=1 → must FAIL ===" >&2
set +e
run_case fault_silent 40 1 -DFAULT_NB_CTX_NO_GEOM_REJECT
FAULT_RC=$?
set -e
echo "fault_silent true rc=$FAULT_RC" >&2
if [[ "$FAULT_RC" -eq 0 ]]; then
  echo "FAIL: FAULT define did not break reject check" >&2
  exit 1
fi

echo "ALL nb_ctx geom_reject red/green gates OK"
exit 0
