#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build/verilator/fpga_video_publish"
mkdir -p "$BUILD/compiler-scratch"
export TMPDIR="$BUILD/compiler-scratch"
INPUTS=(
  "$ROOT/tests/rtl/fpga_video_publish_tb_top.sv"
  "$ROOT/tests/rtl/fpga_video_publish_tb.cpp"
  "$ROOT/fpga/Plex_MiSTer/rtl/fpga_video_publish.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_transport_mux.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_bus_arbiter.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/mplex_hold_lcell.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv"
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv"
  "$ROOT/host/libmisterplex/ddr_bitstream_ring.hpp"
  "$ROOT/host/libmisterplex/mailbox_abi_spec.hpp"
)
sha256sum "${INPUTS[@]}" > "$BUILD/inputs.sha256"
"$ROOT/scripts/run_verilator.sh" --cc --exe --build -j 2 \
  --Mdir "$BUILD" --top-module fpga_video_publish_tb -Wno-fatal \
  -CFLAGS "-std=c++17 -O2 -I$ROOT/host/libmisterplex" \
  "$ROOT/tests/rtl/fpga_video_publish_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/fpga_video_publish.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_transport_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_bus_arbiter.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/mplex_hold_lcell.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/source_aspect_ack.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_base_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/async_fifo.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/line_buf_ram.sv" \
  "$ROOT/tests/rtl/fpga_video_publish_tb.cpp"
"$BUILD/Vfpga_video_publish_tb"
sha256sum --check --status "$BUILD/inputs.sha256"
sha256sum "$BUILD/Vfpga_video_publish_tb" > "$BUILD/binary.sha256"
