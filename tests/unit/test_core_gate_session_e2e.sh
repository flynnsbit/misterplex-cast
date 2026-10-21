#!/usr/bin/env bash
# Core gate v2, a running session in the real daemon. The host misterplexd is built with
# libav (as the ARM build is) and with its /dev/mem, CORENAME and RBFNAME pointed at
# build-dir files. It plays a short local H.264 clip (--play-file, the lab play path,
# which runs the same MediaPlayer session code as a cast) through core_gate_responder
# (fake_plex_core.hpp: CAPS, session ACKs, and presentation feedback for every AU).
# Mid-play, Main starts loading a core (GPO bit 30). The session must stop on the AU
# loop's gate check, the gate must close on its own, the dead epoch must be dropped at
# session end (nothing can ACK it), and from then on no byte of fake memory may change,
# no SPI strobe may happen and no frame may be presented.
# Needs pkg-config libav* and ffmpeg (libx264) on the host; skips without them.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${CORE_GATE_SESSION_OUT:-$ROOT/build/core_gate_session_e2e}
mkdir -p "$OUT"
# T-DAEMON-LOCK-SHARED: a private lock dir (the daemon is an MPX_TEST_BUILD), so this job never
# takes or unlinks the host-global /tmp/misterplexd.lock or /tmp/misterplex_spi.lock.
export MPX_LOCK_DIR="$OUT/locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
FAKE=$OUT/fake.mem
CN=$OUT/CORENAME
RBF=$OUT/RBFNAME
BUILD=$OUT/build_id
SPILOCK=$MPX_LOCK_DIR/misterplex_spi.lock # the daemon's SPI lock: fabric edits serialize with its windows
COUNT=$OUT/count
LOG=$OUT/daemon.log
MEM="python3 $ROOT/tests/unit/core_gate_fakemem.py"
CXX=${CXX:-g++}

LIBAV=$(pkg-config --libs libavformat libavcodec libswresample libavutil 2>/dev/null || true)
if [ -z "$LIBAV" ] || ! command -v ffmpeg > /dev/null; then
    echo "test_core_gate_session_e2e: SKIP (needs host libav and ffmpeg)"
    exit 0
fi

# Mutation runs (core_gate_mutants.py): mutated copies replace the tree's sources.
MUT=${CORE_GATE_MUT_DIR:-}
SRC="main.cpp companion.cpp fb_present.cpp media_player.cpp pms_timeline.cpp plex_resolve.cpp
     fpga_spi.cpp av_inproc_decode.cpp"
srcs=()
for s in $SRC; do
    if [ -n "$MUT" ] && [ -f "$MUT/arm/misterplexd/$s" ]; then
        srcs+=("$MUT/arm/misterplexd/$s")
    else
        srcs+=("$ROOT/arm/misterplexd/$s")
    fi
done
inc=(-I"$ROOT/host")
if [ -n "$MUT" ]; then inc=(-I"$MUT/host" "${inc[@]}"); fi
# shellcheck disable=SC2086
"$CXX" -std=c++17 -O2 "${inc[@]}" -I"$ROOT/arm/misterplexd" -pthread -DMPX_HAVE_LIBAV -DMPX_TEST_BUILD \
    -DMPX_TEST_LOCK_DIR="\"$MPX_LOCK_DIR\"" \
    -DMPX_DEVMEM_PATH="\"$FAKE\"" -DMPX_CORENAME_PATH="\"$CN\"" -DMPX_RBFNAME_PATH="\"$RBF\"" \
    -o "$OUT/misterplexd" "${srcs[@]}" $LIBAV
"$CXX" -std=c++17 -O2 -I"$ROOT/host" -o "$OUT/core_gate_responder" \
    "$ROOT/tests/unit/core_gate_responder.cpp"
ln -sf /bin/sleep "$OUT/MiSTer"
# 8-bit baseline CAVLC, one reference, 30000/1001: what the FPGA decoder takes.
ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=30000/1001 -t 10 \
    -c:v libx264 -profile:v baseline -level 3.0 -x264-params ref=1:bframes=0:keyint=30 \
    -pix_fmt yuv420p -an "$OUT/clip.mp4"

PID=
RPID=
MPID=
stop_pid() {
    if [ -n "$1" ]; then
        kill -CONT "$1" 2>/dev/null || true
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}
cleanup() {
    stop_pid "$PID"
    stop_pid "$RPID"
    stop_pid "$MPID"
    rm -f "$FAKE" "$OUT/MiSTer"
}
trap cleanup EXIT
fail() {
    echo "FAIL [session]: $*" >&2
    grep -n "core gate\|ERROR\|FPGA_TERMINAL\|play-file" "$LOG" >&2 || true
    exit 1
}
count() { grep -c -- "$1" "$LOG" || true; }
strobes() { cut -d' ' -f1 "$COUNT"; }
presented() { cut -d' ' -f3 "$COUNT"; }

$MEM create "$FAKE"
# The build ID the fake core answers to CAPS (default 0x48005D45). CORE_GATE_SESSION_EXPECT=refused: an ID that is not
# in the build table must keep the gate closed (refusal logged, no 'core gate open', no frame presented).
CAPS_ID=${CORE_GATE_SESSION_BUILD:-0x48005D45}
echo "$CAPS_ID" > "$BUILD"
"$OUT/MiSTer" 100000 &
MPID=$!
sleep 0.1
# Main writes its names after it starts (user_io_init); RBFNAME is the CONF_STR name.
printf 'Plex' > "$CN"
printf 'Plex' > "$RBF"
"$OUT/core_gate_responder" "$FAKE" "$COUNT" "$BUILD" &
RPID=$!
sleep 0.3
# The prod conf's non-secret keys (933a4104), no audio device on a host, and the 480p
# decoder's profile (no deblock: the 45D core does not offer it).
printf '%s\n' CONTENT_RES=480p DISPLAY_RES=480p DECODE=640x480 MPX_VIDEO_BACKEND=fpga-h264 \
    PRESENT=fpga AUDIO=0 AUDIO_DELAY_MS=50 IDLE_SCREEN=logo MPX_H264_FILTER=off > "$OUT/e2e.conf"
unshare -rn "$OUT/misterplexd" --conf "$OUT/e2e.conf" --play-file "$OUT/clip.mp4" \
    --play-seconds 9 > "$LOG" 2>&1 &
PID=$!
lp_expect "$LOG" 1 || fail "the daemon's lock paths"

for _ in $(seq 1 150); do
    [ "$(presented)" -ge 45 ] && break
    sleep 0.1
done
p0=$(presented)
if [ "${CORE_GATE_SESSION_EXPECT:-play}" = refused ]; then
    [ "$p0" = 0 ] || fail "build $CAPS_ID: $p0 frames presented with an unknown build"
    [ "$(count 'core gate open')" = 0 ] || fail "build $CAPS_ID: the gate opened"
    [ "$(count "answered CAPS with build $CAPS_ID (not in the FPGA build table)")" -ge 1 ] ||
        fail "build $CAPS_ID: no 'not in the FPGA build table' refusal"
    [ "$(count 'not on the allowlist')" -ge 1 ] || fail "build $CAPS_ID: no 'not on the allowlist' refusal"
    echo "  [session] build $CAPS_ID refused: gate closed, 0 frames, refusal logged"
    echo "test_core_gate_session_e2e: OK (refused $CAPS_ID)"
    exit 0
fi
[ "$p0" -ge 45 ] || fail "the session did not play (presented $p0 frames)"
[ "$(count "core gate open: 'Plex' build $CAPS_ID")" -ge 1 ] || fail "no 'core gate open' for build $CAPS_ID"

# Main begins loading a core while the cast plays.
flock "$SPILOCK" $MEM fabric "$FAKE" 4 1
sleep 0.5 # any call already past its check finishes
h1=$($MEM hash "$FAKE")
s1=$(strobes)
p1=$(presented)
sleep 2.5
[ "$($MEM hash "$FAKE")" = "$h1" ] || fail "fake memory written after GPO bit 30"
[ "$(strobes)" = "$s1" ] || fail "SPI strobes after GPO bit 30"
[ "$(presented)" = "$p1" ] || fail "frames presented after GPO bit 30"

rc=0
wait "$PID" || rc=$?
PID=
[ "$(count 'core gate closed: core held in reset (GPO bit 30)')" = 1 ] ||
    fail "no gate close line for GPO bit 30"
# Whichever gate layer the loop meets first: its own check at the top of the AU loop, or
# the AU push refused at the FpgaSpi choke point (the ring map re-reads the fabric).
if [ "$(count "FPGA session: core gate closed (loaded core 'Plex' not verified)")" -ge 1 ]; then
    stopped_by="the AU loop's gate check"
elif [ "$(count 'ERROR FPGA H.264: AU transport rejected sequence')" -ge 1 ]; then
    stopped_by="the gated AU push"
else
    fail "the session did not stop on the gate"
fi
[ "$(count 'died with the swapped core; ownership dropped')" = 1 ] ||
    fail "the dead epoch was not dropped at session end"
grep -q "FPGA_TERMINAL classification=" "$LOG" || fail "no FPGA_TERMINAL line"
echo "  [session] build $CAPS_ID: played $p0 frames, GPO bit 30 mid-play: stopped by $stopped_by, gate" \
    "closed, epoch dropped at session end; $((p1 - p0)) frames in flight, then 0 bytes," \
    "0 SPI strobes, 0 frames over 2.5 s; daemon rc $rc"
echo "test_core_gate_session_e2e: OK"
