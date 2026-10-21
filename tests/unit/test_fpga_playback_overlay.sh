#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=verilator_bench_absent_skip.sh
source "$ROOT/tests/unit/verilator_bench_absent_skip.sh"
verilator_bench_skip_if_absent \
  "$ROOT/fpga/Plex_MiSTer/rtl/playback_overlay_plane.sv" \
  "$ROOT/tests/rtl/playback_overlay_plane_tb.cpp"
OUT="$ROOT/build/verilator/playback_overlay"
mkdir -p "$OUT"
"$ROOT/scripts/run_verilator.sh" --cc --exe --build -j 1 \
  --Mdir "$OUT" --top-module playback_overlay_plane -Wno-fatal \
  -CFLAGS "-std=c++17 -O2 -I$ROOT/host/libmisterplex" \
  "$ROOT/fpga/Plex_MiSTer/rtl/playback_overlay_plane.sv" \
  "$ROOT/tests/rtl/playback_overlay_plane_tb.cpp"
"$OUT/Vplayback_overlay_plane"
