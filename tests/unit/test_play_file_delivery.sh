#!/usr/bin/env bash
# Lab --play-file under core gate v2: with no verified core, play-file must refuse loudly
# and deliver nothing (the delivery checks moved to a fake-core e2e ticket).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/misterplexd"
FFMPEG="${FFMPEG:-$(command -v ffmpeg || true)}"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[ -n "$FFMPEG" ] && [ -x "$FFMPEG" ] || fail "ffmpeg not found"
make -C "$ROOT" plexd >/dev/null

WORK="$ROOT/build/unit_play_file_delivery"
mkdir -p "$WORK"
# T-DAEMON-LOCK-SHARED: a private lock dir (build/misterplexd is an MPX_TEST_BUILD).
export MPX_LOCK_DIR="$WORK/locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
CONF="$WORK/misterplex_none.conf"
AUDIO_DEV="$WORK/fake_mraudio"
VIDEO="$WORK/video_only.mp4"
EMPTY="$WORK/empty.mp4"
GOOD_LOG="$WORK/good.log"

: >"$AUDIO_DEV"
cat >"$CONF" <<EOF_CONF
PRESENT=none
STREAM=0
IDLE_SCREEN=off
AUDIO_DEVICE=$AUDIO_DEV
EOF_CONF

"$FFMPEG" -v error -y \
  -f lavfi -i testsrc2=size=160x120:rate=4 \
  -t 1 -an -c:v libx264 -profile:v baseline -pix_fmt yuv420p "$VIDEO"

# Core gate v2 (467cf308): --play-file must not touch the FPGA until the loaded core is
# verified (main.cpp waitCoreGateOpen). A host with no FPGA manager registers never opens
# the gate, so play-file must refuse loudly and deliver nothing. The frame-delivery checks
# (audio-disable guard, frames=, zero-frame short_read) need a verified fake core: ticket
# review/daemon/play-file-delivery-e2e-ticket-1005.md.
for SRC in "$VIDEO" "$EMPTY"; do
  : >"$EMPTY"
  set +e
  "$BIN" --ffmpeg "$FFMPEG" --conf "$CONF" --decode 160x120 \
    --play-file "$SRC" --play-seconds 1 >"$GOOD_LOG" 2>&1
  rc=$?
  set -e
  lp_expect "$GOOD_LOG" 1 || fail "the daemon's lock paths ($SRC)"
  [ "$rc" -eq 1 ] || {
    cat "$GOOD_LOG" >&2
    fail "play-file with no verified core: rc=$rc, want 1 ($SRC)"
  }
  grep -q "core gate stays closed: FPGA manager registers not mapped" "$GOOD_LOG" || {
    cat "$GOOD_LOG" >&2
    fail "gate did not report the unmapped FPGA manager ($SRC)"
  }
  grep -qx "misterplexd: play-file failed: core gate closed (loaded core not verified within 5 s)" "$GOOD_LOG" || {
    cat "$GOOD_LOG" >&2
    fail "play-file did not refuse with the core-gate line ($SRC)"
  }
  if grep -qE "LAB play-file done|frames delivered" "$GOOD_LOG"; then
    cat "$GOOD_LOG" >&2
    fail "play-file delivered or counted frames with the core gate closed ($SRC)"
  fi
done

echo "test_play_file_delivery: OK"
