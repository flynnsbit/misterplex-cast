#!/usr/bin/env bash
# Profile real 480p-class H.264 playback cost on the MiSTer ARM.
#
# This script is intentionally token-gated by the operator: it SSHes to MiSTer,
# copies measurement tools, and runs read-only decode/copy probes plus the DDR
# write microbenchmark. It does not touch SPI, does not load a core, and does
# not reboot. Provide a real captured Plex sample on the device via PROFILE_SAMPLE.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${MISTER_HOST:?set MISTER_HOST to the MiSTer host for the scheduled device window}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS for sshpass during the scheduled device window}"
SAMPLE="${PROFILE_SAMPLE:?set PROFILE_SAMPLE to the real captured sample path on MiSTer}"
REMOTE_DIR="${PROFILE_REMOTE_DIR:-/media/fat/misterplex/profile}"
REMOTE_BIN="${PROFILE_REMOTE_BIN:-/media/fat/misterplex/bin}"
FFMPEG="${PROFILE_FFMPEG:-/media/fat/misterplex/bin/ffmpeg}"
FRAMES="${PROFILE_FRAMES:-300}"
WIDTH="${PROFILE_WIDTH:-624}"
HEIGHT="${PROFILE_HEIGHT:-480}"
FPS="${PROFILE_FPS:-25}"
PIPE_SIZE="${PROFILE_PIPE_SIZE:-1048576}"
LOOPS="${PROFILE_DDR_LOOPS:-1000}"
EVIDENCE="${PROFILE_EVIDENCE:-$ROOT/build/misterplex-agent-W-C1B-arm-profile.txt}"

mkdir -p "$ROOT/build"
: >"$EVIDENCE"

log() {
  printf '%s\n' "$*" | tee -a "$EVIDENCE"
}

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

scp_m() {
  sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$@"
}

remote_run() {
  local label="$1"
  shift
  log ""
  log "=== $label ==="
  ssh_m "$@" 2>&1 | tee -a "$EVIDENCE"
}

make -C "$ROOT" arm-profile-tools >/dev/null

ssh_m "mkdir -p '$REMOTE_BIN' '$REMOTE_DIR' && test -f '$SAMPLE' && test -x '$FFMPEG'"
scp_m "$ROOT/build/arm/ffmpeg_cpu_probe" \
      "$ROOT/build/arm/present_loop_harness" \
      "$ROOT/build/arm/ddr_write_bench" \
      "$USER@$HOST:$REMOTE_BIN/" >/dev/null
ssh_m "chmod +x '$REMOTE_BIN/ffmpeg_cpu_probe' '$REMOTE_BIN/present_loop_harness' '$REMOTE_BIN/ddr_write_bench'"

PROBE="$REMOTE_BIN/ffmpeg_cpu_probe"
PRESENT="$REMOTE_BIN/present_loop_harness"
DDR="$REMOTE_BIN/ddr_write_bench"
FRAME_BYTES=$((WIDTH * HEIGHT * 3 / 2))
VF="fps=${FPS},scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=decrease,pad=${WIDTH}:${HEIGHT}:(ow-iw)/2:(oh-ih)/2"

log "host=$HOST sample=$SAMPLE width=$WIDTH height=$HEIGHT fps=$FPS frames=$FRAMES"
log "frame_bytes=$FRAME_BYTES pipe_size=$PIPE_SIZE ddr_loops=$LOOPS"
log "All result lines below are raw measurements; interpretation should be done after capture."

remote_run "sample metadata" \
  "'$FFMPEG' -hide_banner -i '$SAMPLE' </dev/null || true"

remote_run "decode-only: H.264 -> decoded frames -> null mux" \
  "'$PROBE' --label decode_null -- '$FFMPEG' -hide_banner -loglevel error -nostdin -i '$SAMPLE' -map 0:v:0 -frames:v '$FRAMES' -an -sn -f null -"

remote_run "decode + scale/pad + yuv420p conversion -> /dev/null" \
  "'$PROBE' --label decode_scale_yuv420p_null_${WIDTH}x${HEIGHT} -- '$FFMPEG' -hide_banner -loglevel error -nostdin -i '$SAMPLE' -map 0:v:0 -frames:v '$FRAMES' -an -sn -vf '$VF' -pix_fmt yuv420p -f rawvideo -y /dev/null"

remote_run "decode + scale/pad + yuv420p conversion -> pipe drain/copy" \
  "'$PROBE' --label decode_scale_yuv420p_pipe_${WIDTH}x${HEIGHT} --frame-bytes '$FRAME_BYTES' --copy --pipe-size '$PIPE_SIZE' -- '$FFMPEG' -hide_banner -loglevel error -nostdin -i '$SAMPLE' -map 0:v:0 -frames:v '$FRAMES' -an -sn -vf '$VF' -pix_fmt yuv420p -f rawvideo pipe:1"

remote_run "synthetic present-loop pipe/copy at source rate" \
  "'$PRESENT' --frames '$FRAMES' --width '$WIDTH' --height '$HEIGHT' --fps '$FPS' --pipe-size '$PIPE_SIZE'"

remote_run "synthetic present-loop pipe/copy at 60 fps" \
  "'$PRESENT' --frames '$FRAMES' --width '$WIDTH' --height '$HEIGHT' --fps '60' --pipe-size '$PIPE_SIZE'"

remote_run "DDR write path: O_SYNC /dev/mem" \
  "'$DDR' --sync --format yuv420p --width '$WIDTH' --height '$HEIGHT' --loops '$LOOPS'"

remote_run "DDR write path: no O_SYNC /dev/mem" \
  "'$DDR' --no-sync --format yuv420p --width '$WIDTH' --height '$HEIGHT' --loops '$LOOPS'"

remote_run "DDR write path: no O_SYNC + ARM cacheflush" \
  "'$DDR' --no-sync --flush --format yuv420p --width '$WIDTH' --height '$HEIGHT' --loops '$LOOPS'"

log ""
log "DONE evidence=$EVIDENCE"
