#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=verilator_bench_absent_skip.sh
source "$ROOT/tests/unit/verilator_bench_absent_skip.sh"
python3 "$ROOT/scripts/generate_audio_session_abi.py" --check
verilator_bench_skip_if_absent \
  "$ROOT/tests/rtl/ddr_bitstream_reader_transport_tb_top.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/audio_session_ddr_mux.sv" \
  "$ROOT/fpga/Plex_MiSTer/rtl/audio_session_mailbox.sv" \
  "$ROOT/tests/rtl/ddr_bitstream_reader_transport_tb.cpp"
configs=(legacy au-conservative au-ring-limit au-real-audio)
if (($#)); then configs=("$@"); fi
for config in "${configs[@]}"; do
  mode=1
  limit=8192
  real_audio=0
  case "$config" in
    legacy) mode=0 ;;
    au-conservative) ;;
    au-ring-limit) limit=262048 ;;
    au-real-audio) real_audio=1 ;;
    *) printf 'Unknown reader transport configuration: %s\n' "$config" >&2; exit 2 ;;
  esac
  if ((real_audio)); then
    python3 - "$ROOT" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
def compact(path):
    return re.sub(r"\s+", "", (root / path).read_text())

core = compact("fpga/Plex_MiSTer/Plex.sv")
framework = compact("fpga/Plex_MiSTer/sys/sys_top.v")
bench = compact("tests/rtl/ddr_bitstream_reader_transport_tb_top.sv")
for binding in (
    "wirereset=RESET|status[0]|buttons[1];",
    "`ifdefFPGA_VIDEO_320assignMPX_AUDIO_RESET=reset;`endif",
    ".session_epoch(stream_video_session),.probe_nonce(stream_video_nonce)",
    ".session_active(stream_ddr_active&&!stream_reset_pending)",
    "publish_idle&&(!stream_reset_pending||audio_consumer_quiescent)",
):
    if binding not in core:
        raise SystemExit("Reader/audio composition no longer matches Plex binding: " + binding)
reset_cdc = (
    "always@(posedgeclk_audioorposedgempx_audio_reset)begin"
    "if(mpx_audio_reset)mpx_audio_reset_sync<=2'b11;"
    "elsempx_audio_reset_sync<={mpx_audio_reset_sync[0],1'b0};end"
)
if reset_cdc not in framework or reset_cdc not in bench:
    raise SystemExit("Reader/audio bench and framework reset synchronizer differ")
for binding in (
    ".MPX_AUDIO_RESET(mpx_audio_reset)",
    "`ifdefFPGA_VIDEO_320.reset(mpx_audio_reset_sync[1]),`else.reset(reset),`endif",
):
    if binding not in framework:
        raise SystemExit("Actual framework ALSA reset route differs: " + binding)
print("Reader/audio reset composition matches current production caller bindings")
PY
  fi
  OUT="$ROOT/build/verilator/ddr_reader_transport_$config"
  mkdir -p "$OUT"
  "$ROOT/scripts/run_verilator.sh" --cc --exe --build -j 1 \
    --Mdir "$OUT" --top-module ddr_bitstream_reader_transport_tb_top \
    -GENABLE_AU_PROTOCOL="$mode" -GMAX_AU_BYTES="$limit" -GREAL_ALSA="$real_audio" -Wno-fatal \
    -I"$ROOT/fpga/Plex_MiSTer/rtl" \
    -CFLAGS "-std=c++17 -O2 -DAU_MODE=$mode -DTEST_MAX_AU_BYTES=$limit -DREAL_ALSA_TEST=$real_audio -I$ROOT/host/libmisterplex" \
    "$ROOT/tests/rtl/ddr_bitstream_reader_transport_tb_top.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/audio_session_ddr_mux.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/audio_session_mailbox.sv" \
    "$ROOT/fpga/Plex_MiSTer/sys/alsa.sv" \
    "$ROOT/fpga/Plex_MiSTer/rtl/bitstream_fifo.sv" \
    "$ROOT/tests/rtl/ddr_bitstream_reader_transport_tb.cpp"
  if ((real_audio)); then
    if "$OUT/Vddr_bitstream_reader_transport_tb_top" --disconnect-core-audio-reset \
        >"$OUT/core-local-reset-disconnected.log" 2>&1; then
      printf 'FAIL: disconnected core/audio reset unexpectedly recovered\n' >&2
      exit 1
    fi
    printf 'EXPECTED RED (disconnected core-local reset): '
    grep 'FAIL reader transport: core-local reset stranded actual ALSA' \
      "$OUT/core-local-reset-disconnected.log"
    if [[ "${READER_RESET_NEGATIVE_ONLY:-0}" == 1 ]]; then continue; fi
  fi
  "$OUT/Vddr_bitstream_reader_transport_tb_top"
done
