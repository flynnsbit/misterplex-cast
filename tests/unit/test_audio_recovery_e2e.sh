#!/usr/bin/env bash
# Coregate v2.3: the MrAudio session lane in the real daemon, through core_gate_responder
# (fake_plex_core.hpp: AU ring, CAPS, MVPS, and the audio lane with the RTL's mailbox and
# consumer rules) and a link-wrapped fake /dev/MrAudio (fake_mraudio_wrap.cpp). The host
# misterplexd is built with libav, as the ARM build is; --play-count N plays the clip N times
# in one process, as a cast's next play runs on the same daemon.
# Consumer cap (review 01:50, wT 01:06): every fake core instance runs with the fake MrAudio
# consumer capped at the producer, as sys/alsa.sv does (test_n2_eof_e2e.sh header; 2ed78229).
# start_core injects "cap 1" into each new core_gate_responder and waits until COUNT.audio
# shows cap=1 (load_core keeps the same responder, so the cap holds across reloads). Without
# it the consumer's clock was wall time, so a host-starved daemon saw consumed > written and
# stopped the play (the [killed] "first daemon's audio never ran" flake).
# Scenarios (AUDIO_RECOVERY_SCENARIOS, default all):
#   reboot          a lab MiSTer hdmideploy E7: boot start under MENU (gate closed), stopd, core load,
#                   start; the new core's mailbox does not forward the first audio request.
#                   Both plays must deliver frames, no reload.
#   reboot_reject   the same, with the consumer in a reset window for the first command.
#   persistent      the first two audio requests are not forwarded: play 1 fails cleanly and
#                   releases its epoch; play 2 must play (no "prior epoch" lockout).
#   killed          g240v3 B 10-03: a daemon SIGKILLed mid-play leaves the consumer running
#                   its session; a new daemon, no core reload, must reclaim it and play.
#   midreset        MPX_AUDIO_RESET pulses mid-play (status[0], OSD reset): the consumer
#                   forgets play 1's identity; play 1's Reset finds nothing of ours, the epoch
#                   is released, play 2 plays.
#   wedged          the mailbox never publishes: play 1 fails before video begins, says
#                   "reload the core" and owns no epoch; once the lane recovers, play 2 in the
#                   same process plays. No launcher is configured: the gate-open MAST probe and
#                   the begin say so, and the wedged begin is tried once (no blind retries).
# MAST-WEDGE (review 21:14 RCA; release 21:16/21:17), with MPX_MAST_RELOAD_CMD set to a stand-in
# for `misterplexd-boot.sh mast-reload` (mast_launcher.sh). The stand-in waits for the daemon to
# be gone, records the LOADCORE-GUARD 0-misterplexd scan (daemon exe, fake-DDR maps and fds, lock
# holders) and marks the reload attempted. This script then plays the launcher's guarded core
# reload (load_core) and restart:
#   wedge_reload    the lane is wedged at gate open: the probe says WEDGED and misterplexd hands
#                   off ONE reload and exits 86 before any begin. The launcher sees 0 misterplexd.
#                   After the reload the probe is live, the marker is recovered and the play plays.
#                   R5f (review 01:19/01:25): a stale valid audio commit seeded in DDR is 0 after
#                   daemon 1's exit 86, and daemon 2's own live commit is 0 after its exit 0.
#   wedge_twice     the reload does not help (the new instance is wedged too): the second
#                   daemon logs ONE 'MAST-WEDGE CURE FAILED' line at gate open, marks the marker
#                   cure-failed (terminal, review 21:39) and fails its begin clean (one
#                   attempt) with no second hand-off. The launcher ran once in all: no loop.
#   wedge_begin     the lane is live at gate open and wedges at play 2's begin (wedgeatprobe):
#                   one begin attempt, one hand-off (exit 86), reload, then the play plays.
#   force_once      lab MPX_MAST_FORCE_WEDGE=once on a live lane (the glass forced-repro
#                   switch): the probe line says FORCED, one hand-off, and after the reload the
#                   probe is live (the marker stops the force) and the play plays.
#   handoff_lost    the glass G1 23:49 defect, daemon side (review 23:53 R2): the stand-in
#                   drops a forced hand-off, as the old launcher did on its busy lock. The next
#                   daemon's probe is live: it logs 'hand-off not taken: no reload ran', leaves
#                   the marker requested, never calls the lane recovered, no CURE FAILED.
#   boot_race       the glass G1 23:49 race end to end (review 23:53 R1/R3) with the REAL
#                   launcher (scripts/misterplexd-boot-mast.sh), all in one private network namespace
#                   so its prod-mode daemon never reaches the LAN. The start launcher starts the
#                   daemon; the lane is wedged at the first gate open, inside START_WAIT. The
#                   start launcher yields; the mast-reload instance waits for the lock and runs
#                   exactly ONE guarded load (a loader stand-in: the 0-misterplexd scan, then this
#                   script's load_core ... unwedge), marks attempted and starts daemon 2: probe
#                   live, marker recovered, one daemon running and verified (PROD_START_OK).
#   reload_no_answer  glass G2 (R5, review 01:09, review 01:10/01:28): as boot_race, but the
#                   reloaded core's CTRL/CAPS lane is silent. At the bound (60 s from daemon 2's
#                   start) daemon 2 marks cure-failed why=gate-silent:<reason>, one '!!!' line in
#                   its log, the boot log and FAILED; no second hand-off or load; daemon 2 runs
#                   on; a reload by hand logs 'manual recovery after cure-failed', marker kept.
#                   R6 attribution: the loader stand-in's r6= outcome (the real launcher hands it
#                   the marker) is quoted in the loud line and kept by the cure-failed rewrite
#                   (boot_race: kept by the recovered rewrite).
# AUDIO_RECOVERY_DEFS: extra -D flags for the daemon build (the MAST-WEDGE mutants).
# MAST_BOOT_SCRIPT: the launcher boot_race runs (default scripts/misterplexd-boot-mast.sh; mutants).
# AUDIO_RECOVERY_DAEMON_SRC builds the daemon from another tree (the negative control,
# test_audio_recovery_negative.sh, puts this tree's main.cpp, with the lab --play-count
# flag, in a 3e5ea234 tree); the test fixtures always come from this tree. CORE_GATE_MUT_DIR: mutated sources (mutants).
# AUDIO_RECOVERY_BUILD (default 0x48005D45): the loaded Plex core's build ID. AUDIO_RECOVERY_PLAY_SECONDS
# (default 6): the length of the reboot and midreset scenarios' plays; the clip is 2 s longer (at least 8 s).
# FAKE_CORE_PLX=1 reaches core_gate_responder: the fake core keeps its 480p PLX page (the D52-LOG rows).
# Needs pkg-config libav*, ffmpeg (libx264, aac) and unshare; skips without them.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${AUDIO_RECOVERY_OUT:-$ROOT/build/audio_recovery_e2e}
SRC_TREE=${AUDIO_RECOVERY_DAEMON_SRC:-$ROOT}
SCENARIOS=${AUDIO_RECOVERY_SCENARIOS:-reboot reboot_reject persistent killed midreset wedged wedge_reload wedge_twice wedge_begin force_once handoff_lost boot_race reload_no_answer}
PLAY_BUILD=${AUDIO_RECOVERY_BUILD:-0x48005D45}
PLAY_SECONDS=${AUDIO_RECOVERY_PLAY_SECONDS:-6}
CLIP_SECONDS=$((PLAY_SECONDS + 2 > 8 ? PLAY_SECONDS + 2 : 8))
mkdir -p "$OUT"
# T-DAEMON-LOCK-SHARED: a private lock dir for every daemon of this test (MPX_TEST_BUILD, with the
# dir also baked in, so boot_race's daemon finds it through misterplexd-boot.sh's env -i).
export MPX_LOCK_DIR="$OUT/locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
CLIP=$OUT/clip.mp4
[ "$CLIP_SECONDS" = 8 ] || CLIP=$OUT/clip-${CLIP_SECONDS}s.mp4
FAKE=$OUT/fake.mem
CN=$OUT/CORENAME
RBF=$OUT/RBFNAME
BUILD=$OUT/build_id
AUDIO=$OUT/audio_inject
COUNT=$OUT/count
SPILOCK=$MPX_LOCK_DIR/misterplex_spi.lock # the daemons' SPI lock: fabric edits serialize with their windows
MEM="python3 $ROOT/tests/unit/core_gate_fakemem.py"
CXX=${CXX:-g++}

LIBAV=$(pkg-config --libs libavformat libavcodec libswresample libavutil 2>/dev/null || true)
if [ -z "$LIBAV" ] || ! command -v ffmpeg > /dev/null; then
    echo "test_audio_recovery_e2e: SKIP (needs host libav and ffmpeg)"
    exit 0
fi

MUT=${CORE_GATE_MUT_DIR:-}
pick() { # file relative to the tree: the mutant's, else the daemon source tree's
    if [ -n "$MUT" ] && [ -f "$MUT/$1" ]; then echo "$MUT/$1"; else echo "$SRC_TREE/$1"; fi
}
srcs=()
for s in main.cpp companion.cpp fb_present.cpp media_player.cpp pms_timeline.cpp plex_resolve.cpp \
    fpga_spi.cpp av_inproc_decode.cpp; do
    srcs+=("$(pick "arm/misterplexd/$s")")
done
inc=(-I"$SRC_TREE/host" -I"$SRC_TREE/arm/misterplexd")
if [ -n "$MUT" ]; then inc=(-I"$MUT/host" -I"$MUT/arm/misterplexd" "${inc[@]}"); fi
# shellcheck disable=SC2086
if [ "${AUDIO_RECOVERY_REUSE_BUILD:-0}" != 1 ] || [ ! -x "$OUT/misterplexd" ]; then
    "$CXX" -std=c++17 -O2 "${inc[@]}" -pthread -DMPX_HAVE_LIBAV -DMPX_TEST_BUILD \
        -DMPX_TEST_LOCK_DIR="\"$MPX_LOCK_DIR\"" \
        -DMPX_DEVMEM_PATH="\"$FAKE\"" -DMPX_CORENAME_PATH="\"$CN\"" \
        -DMPX_RBFNAME_PATH="\"$RBF\"" -DMPX_MAST_RELOAD_MARKER_PATH="\"$OUT/mast.marker\"" \
        -DMPX_MAST_BOOT_LOG_PATH="\"$OUT/br/tmp/misterplexd-boot.log\"" \
        -DMPX_MAST_BOOT_FAILED_PATH="\"$OUT/br/tmp/misterplexd-boot.FAILED\"" \
        ${AUDIO_RECOVERY_DEFS:-} -o "$OUT/misterplexd" "${srcs[@]}" \
        "$ROOT/tests/unit/fake_mraudio_wrap.cpp" $LIBAV \
        -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close
    "$CXX" -std=c++17 -O2 -I"$ROOT/host" -o "$OUT/core_gate_responder" \
        "$ROOT/tests/unit/core_gate_responder.cpp"
fi
ln -sf /bin/sleep "$OUT/MiSTer"
# 8-bit baseline CAVLC video and 48 kHz stereo AAC, as the lab ladder serves.
if [ ! -s "$CLIP" ]; then
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=30000/1001 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -t "$CLIP_SECONDS" \
        -c:v libx264 -profile:v baseline -level 3.0 -x264-params ref=1:bframes=0:keyint=30 \
        -pix_fmt yuv420p -c:a aac -b:a 96k -ar 48000 -ac 2 -shortest "$CLIP"
fi
# The prod conf's non-secret keys (933a4104), audio on (the wrapped MrAudio), and the 480p
# decoder's profile (no deblock: the 45D core does not offer it).
printf '%s\n' CONTENT_RES=480p DISPLAY_RES=480p DECODE=640x480 MPX_VIDEO_BACKEND=fpga-h264 \
    PRESENT=fpga AUDIO=1 AUDIO_DELAY_MS=50 IDLE_SCREEN=logo MPX_H264_FILTER=off > "$OUT/e2e.conf"

PID=
RPID=
MPID=
BPID=
BR_ACTIVE=
CASE=
LOG=/dev/null
ASEQ=0
stop_pid() {
    if [ -n "${1:-}" ]; then
        kill -CONT "$1" 2>/dev/null || true
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}
stop_children() {
    stop_pid "$PID"
    stop_pid "$RPID"
    stop_pid "$MPID"
    PID= RPID= MPID=
}
# boot_race's real-launcher tree (launchers, loader, daemons): stopped by pid, found by its own
# paths (BR, the marker). Inert unless boot_race is running.
br_stop() {
    [ -n "$BR_ACTIVE" ] || return 0
    local p
    for p in ${BPID:-} $(pgrep -f -- "$BR/|mast-reload .*$MARKER" 2>/dev/null || true); do
        kill "$p" 2>/dev/null || true
    done
    BPID= BR_ACTIVE=
}
cleanup() {
    br_stop
    stop_children
    rm -f "$FAKE" "$OUT/MiSTer"
}
trap cleanup EXIT
fail() {
    echo "FAIL [$CASE]: $*" >&2
    grep -n "core gate\|ERROR\|LAB play-file\|audio Begin\|reclaimed\|epoch\|wedged" "$LOG" \
        2>/dev/null | cut -c1-260 >&2 || true
    exit 1
}
count() { grep -c -- "$1" "$LOG" || true; }
field() { cut -d' ' -f"$1" "$COUNT"; }
names() { printf '%s' "$1" > "$CN"; printf '%s' "$2" > "$RBF"; }
fabric() { flock "$SPILOCK" $MEM fabric "$FAKE" "$1" "$2"; }
inject() { ASEQ=$((ASEQ + 1)); echo "$ASEQ $*" > "$AUDIO"; sleep 0.1; }
cap_on() { # the consumer cap (header), applied and confirmed before any other injection
    inject cap 1
    local n
    for n in $(seq 1 50); do
        grep -q ' cap=1 ' "$COUNT.audio" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "the responder did not apply 'cap 1'"
}
main_start() {
    "$OUT/MiSTer" 100000 &
    MPID=$!
    sleep 0.1
}
start_core() { # build: a running fake core, no Main load sequence
    $MEM create "$FAKE"
    echo "$1" > "$BUILD"
    : > "$AUDIO"
    "$OUT/core_gate_responder" "$FAKE" "$COUNT" "$BUILD" "$AUDIO" &
    RPID=$!
    sleep 0.3
    cap_on
}
# Main loads a core (as in test_core_gate_daemon_e2e.sh): GPO bit 30, configuration, a new
# Main, the new core's instance; injections land on that instance before its names appear.
load_core() { # build [injection words]
    local build=$1
    shift
    fabric 4 1
    sleep 0.3
    fabric 2 1
    echo 0 > "$BUILD"
    sleep 0.3
    fabric 4 1
    stop_pid "$MPID"
    main_start
    fabric 4 0
    echo "$build" > "$BUILD"
    sleep 0.2
    if [ $# -gt 0 ]; then inject "$@"; fi
    sleep 0.3
    names Plex Plex
}
play() { # log runs seconds: the lab play path, N runs in one process
    LOG=$1
    unshare -rn "$OUT/misterplexd" --conf "$OUT/e2e.conf" --play-file "$CLIP" \
        --play-seconds "$3" --play-count "$2" > "$LOG" 2>&1 &
    PID=$!
    lp_expect "$LOG" 1 || fail "the daemon's lock paths"
}
runs_done() { count 'LAB play-file done frames='; }

# a lab MiSTer hdmideploy E7, signature as logged: the first audio request of the first post-boot
# session is not adopted. variant: ignore (the mailbox does not forward it) or reject (the
# consumer ACKs it with an error from a reset window). count: requests affected.
scenario_reboot() { # variant count
    local variant=$1 n=$2
    # Boot: MENU loaded, the boot daemon starts with the gate closed and must stay quiet.
    start_core 0
    main_start
    names MENU MENU
    LOG=$OUT/$CASE-boot.log
    unshare -rn "$OUT/misterplexd" --conf "$OUT/e2e.conf" --port 39007 --name boot \
        --id boot > "$LOG" 2>&1 &
    PID=$!
    lp_expect "$LOG" 1 || fail "the boot daemon's lock paths"
    sleep 1.5
    local h0 s0 p0
    h0=$($MEM hash "$FAKE")
    s0=$(field 1)
    p0=$(field 2)
    sleep 2
    kill -0 "$PID" 2>/dev/null || fail "the boot daemon exited"
    [ "$(count 'core gate closed at start')" = 1 ] || fail "boot daemon: no 'closed at start'"
    [ "$($MEM hash "$FAKE")" = "$h0" ] || fail "boot daemon wrote fake memory under MENU"
    [ "$(field 1)" = "$s0" ] && [ "$(field 1)" = 0 ] || fail "boot daemon SPI strobes under MENU"
    [ "$(field 2)" = "$p0" ] && [ "$(field 2)" = 0 ] || fail "boot daemon probed under MENU"
    # stopd, then the harness's loadside (menu, then Plex) and the prod start.
    stop_pid "$PID"
    PID=
    load_core "$PLAY_BUILD" "$variant" "$n"
    play "$OUT/$CASE.log" 2 "$PLAY_SECONDS"
    local rc=0
    wait "$PID" || rc=$?
    PID=
}

check_two_plays() { # what
    [ "$(count 'LAB play-file run 2/2')" = 1 ] || fail "$1: the second play never ran"
    [ "$(count 'prior epoch still owns')" = 0 ] || fail "$1: a later play hit 'prior epoch still owns'"
    [ "$(count 'reset ownership retained')" = 0 ] || fail "$1: an epoch was retained"
}

run_reboot() {
    CASE=reboot
    scenario_reboot ignore 1
    check_two_plays "reboot"
    [ "$(runs_done)" = 2 ] || fail "both plays must deliver frames after a cold-boot load (done=$(runs_done))"
    [ "$(count 'audio Begin not adopted by the MrAudio consumer')" = 1 ] ||
        fail "no 'audio Begin not adopted' line for the refused first Begin"
    [ "$(count 'the mailbox never forwarded it')" -ge 1 ] || fail "the refusal was not classified"
    echo "  [reboot] boot daemon closed under MENU: 0 bytes, 0 strobes, 0 probes; stopd, core" \
        "load, start: the first audio request was not forwarded, the epoch was released and the" \
        "session began again; plays 1 and 2 delivered frames, no reload"
    stop_children
}

run_reboot_reject() {
    CASE=reboot_reject
    scenario_reboot reject 1
    check_two_plays "reboot_reject"
    [ "$(runs_done)" = 2 ] || fail "both plays must deliver frames (done=$(runs_done))"
    [ "$(count 'the consumer rejected it')" -ge 1 ] || fail "the rejection was not classified"
    echo "  [reboot_reject] the consumer rejected the first audio Begin (reset window): one" \
        "re-begin; plays 1 and 2 delivered frames"
    stop_children
}

run_persistent() {
    CASE=persistent
    scenario_reboot ignore 2
    check_two_plays "persistent"
    [ "$(runs_done)" = 1 ] || fail "play 1 must fail and play 2 must play (done=$(runs_done))"
    [ "$(count 'ERROR FPGA H.264: audio session reset ACK failed')" = 1 ] || fail "play 1 did not fail on audio"
    grep -A2000 'LAB play-file run 2/2' "$LOG" | grep -q 'LAB play-file done frames=' ||
        fail "play 2 did not play"
    echo "  [persistent] two refused Begins: play 1 failed and released its epoch; play 2" \
        "played on the same daemon (no 'prior epoch' lockout)"
    stop_children
}

run_killed() {
    CASE=killed
    start_core "$PLAY_BUILD"
    main_start
    names Plex Plex
    play "$OUT/$CASE-owner.log" 1 30
    local i
    for i in $(seq 1 150); do
        [ "$(field 4)" = 1 ] && [ "$(field 3)" -ge 40 ] && break
        sleep 0.1
    done
    [ "$(field 4)" = 1 ] || fail "the first daemon's audio never ran"
    kill -KILL "$PID"
    wait "$PID" 2>/dev/null || true
    PID=
    sleep 0.5
    [ "$(field 4)" = 1 ] || fail "the consumer should still run the killed daemon's session"
    local orphan
    orphan=$(field 8)
    play "$OUT/$CASE.log" 1 6
    wait "$PID" || true
    PID=
    [ "$(count 'FPGA reclaimed the MrAudio consumer a previous owner left active')" = 1 ] ||
        fail "the killed owner's session was not reclaimed"
    [ "$(count 'refused the recovery Reset')" = 0 ] ||
        fail "the reclaim needed begin retries (a new daemon's first token must exceed the killed owner's last)"
    [ "$(runs_done)" = 1 ] || fail "the new daemon did not play"
    echo "  [killed] daemon SIGKILLed mid-play left the consumer running epoch $orphan; a new" \
        "daemon, no core reload, reclaimed it (recovery Reset) and played"
    stop_children
}

run_midreset() {
    CASE=midreset
    start_core "$PLAY_BUILD"
    main_start
    names Plex Plex
    play "$OUT/$CASE.log" 2 "$PLAY_SECONDS"
    local i
    for i in $(seq 1 150); do
        [ "$(field 4)" = 1 ] && [ "$(field 3)" -ge 40 ] && break
        sleep 0.1
    done
    [ "$(field 4)" = 1 ] || fail "play 1's audio never ran"
    inject creset
    for i in $(seq 1 20); do
        [ "$(field 4)" = 0 ] && break
        sleep 0.05
    done
    [ "$(field 4)" = 0 ] || fail "the consumer reset did not land"
    wait "$PID" || true
    PID=
    check_two_plays "midreset"
    [ "$(count 'reset ACK failed')" = 0 ] ||
        fail "play 1's release reported a failed reset with nothing of ours to reset"
    grep -A2000 'LAB play-file run 2/2' "$LOG" | grep -q 'LAB play-file done frames=' ||
        fail "play 2 did not play after the consumer reset"
    echo "  [midreset] the consumer was reset mid-play (identity cleared): play 1's Reset had" \
        "nothing of ours to reset, its epoch was released; play 2 played"
    stop_children
}

run_wedged() {
    CASE=wedged
    start_core "$PLAY_BUILD"
    main_start
    names Plex Plex
    inject wedge
    play "$OUT/$CASE.log" 2 5
    local i
    for i in $(seq 1 600); do
        grep -q 'LAB play-file run 2/2' "$LOG" && break
        sleep 0.1
    done
    grep -q 'LAB play-file run 2/2' "$LOG" || fail "play 1 never finished"
    inject unwedge
    wait "$PID" || true
    PID=
    local play1
    play1=$(sed -n '1,/LAB play-file run 2\/2/p' "$LOG")
    grep -q 'MrAudio lane wedged' <<< "$play1" || fail "play 1: the wedged lane was not reported"
    grep -q 'reload the core' <<< "$play1" || fail "play 1: the wedge line does not say 'reload the core'"
    grep -q 'LAB play-file ERROR zero frames' <<< "$play1" || fail "play 1 played on a wedged lane"
    [ "$(count 'prior epoch still owns')" = 0 ] || fail "a wedged begin left an epoch owned"
    [ "$(count 'reset ownership retained')" = 0 ] || fail "a wedged begin retained an epoch"
    grep -A2000 'LAB play-file run 2/2' "$LOG" | grep -q 'LAB play-file done frames=' ||
        fail "play 2 did not play after the lane recovered"
    # MAST-WEDGE, no launcher configured (MPX_MAST_RELOAD_CMD unset): probe, clean fail, and
    # no blind retries.
    [ "$(count 'MAST probe (gate open): WEDGED')" = 1 ] ||
        fail "no WEDGED gate-open MAST probe line"
    grep -q 'no auto reload configured' <<< "$play1" ||
        fail "the wedge did not say that no auto reload is configured"
    [ "$(grep -c 'FPGA begin attempt' <<< "$play1")" = 1 ] ||
        fail "play 1 retried a wedged begin (blind retries)"
    echo "  [wedged] no MAST publication: play 1 failed before video began ('MrAudio lane" \
        "wedged ... reload the core'), owning no epoch, one begin attempt, no reload" \
        "configured; the lane recovered and play 2 played"
    stop_children
}

# ---- MAST-WEDGE rows: the reload hand-off ----
LAUNCHER=$OUT/mast_launcher.sh
LAUNCHLOG=$OUT/mast_launcher.log
MARKER=$OUT/mast.marker
cat > "$LAUNCHER" << 'EOF'
#!/bin/sh
# e2e stand-in for `misterplexd-boot.sh mast-reload PID REASON MARKER`: the real launcher waits
# for misterplexd to be gone, runs the guarded load (LOADCORE-GUARD: no daemon exe, no DDR maps
# or fds, no lock holders) and restarts it. This one records the hand-off and that scan; the
# e2e script then plays the reload and the restart.
L=$MAST_LAUNCH_LOG
echo "launched argv=$*" >> "$L"
[ "$1" = mast-reload ] || { echo "bad mode $1" >> "$L"; exit 2; }
# handoff_lost: drop the hand-off, as the old launcher did on its busy lock (glass G1 23:49).
if [ "${MAST_STANDIN_DROP:-0}" = 1 ]; then echo "dropped: another misterplexd-boot running - exit" >> "$L"; exit 0; fi
i=0
t0=$(date +%s%N)
while kill -0 "$2" 2>/dev/null && [ $i -lt 200 ]; do i=$((i + 1)); sleep 0.05; done
echo "daemon gone after $((($(date +%s%N) - t0) / 1000000)) ms" >> "$L"
cn=$(cat "$MAST_CORENAME" 2>/dev/null)
if [ "$cn" != Plex ]; then
    printf 'state=refused\ntime=%s\nreason=%s\nwhy=core-switched:%s\n' "$(date +%s)" "$3" "$cn" > "$4"
    echo "refused core-switched:$cn" >> "$L"
    exit 0
fi
n=0
hits=
for p in /proc/[0-9]*; do
    q=${p#/proc/}
    case " $MAST_EXCLUDE " in *" $q "*) continue ;; esac
    case "$(readlink "$p/exe" 2>/dev/null)" in
        "$MAST_DAEMON_EXE"|"$MAST_DAEMON_EXE (deleted)") n=$((n + 1)); hits="$hits $q:exe" ;;
    esac
    if grep -qsF "$MAST_FAKE" "$p/maps"; then n=$((n + 1)); hits="$hits $q:maps"; fi
    fds=$(ls -l "$p/fd" 2>/dev/null)
    case $fds in *"$MAST_FAKE"*) n=$((n + 1)); hits="$hits $q:fd" ;; esac
    case $fds in *"${MAST_LOCK:?MAST_LOCK unset}"*) n=$((n + 1)); hits="$hits $q:lock" ;; esac
done
echo "guard misterplexd=$n$hits" >> "$L"
printf 'state=attempted\ntime=%s\nreason=%s\n' "$(date +%s)" "$3" > "$4"
echo "reload requested" >> "$L"
EOF
chmod +x "$LAUNCHER"
mast_reset() { rm -f "$MARKER" "$MARKER.tmp" "$LAUNCHLOG"; }
mast_play() { # log runs seconds: play with the launcher configured
    LOG=$1
    local force=()
    if [ -n "${MAST_FORCE:-}" ]; then force=(MPX_MAST_FORCE_WEDGE="$MAST_FORCE"); fi
    env MPX_MAST_RELOAD_CMD="$LAUNCHER" MAST_LAUNCH_LOG="$LAUNCHLOG" \
        MAST_DAEMON_EXE="$OUT/misterplexd" MAST_FAKE="$FAKE" MAST_EXCLUDE="$RPID $MPID" \
        MAST_LOCK="$LP_DIR_REAL/misterplexd.lock" \
        MAST_CORENAME="$CN" MAST_STANDIN_DROP="${MAST_DROP:-0}" ${force[@]+"${force[@]}"} \
        unshare -rn "$OUT/misterplexd" --conf "$OUT/e2e.conf" --play-file "$OUT/clip.mp4" \
        --play-seconds "$3" --play-count "$2" > "$LOG" 2>&1 &
    PID=$!
    lp_expect "$LOG" 1 || fail "the mast_play daemon's lock paths"
}
mast_launches() { grep -c '^launched' "$LAUNCHLOG" 2>/dev/null || true; }
mast_launched_ok() { # the launcher ran once, found 0 misterplexd and marked the reload attempted
    local i
    for i in $(seq 1 100); do
        grep -q '^reload requested' "$LAUNCHLOG" 2>/dev/null && break
        sleep 0.1
    done
    grep -q '^reload requested' "$LAUNCHLOG" 2>/dev/null || fail "the reload launcher never finished"
    [ "$(mast_launches)" = 1 ] || fail "the launcher ran $(mast_launches) times, want 1"
    grep -q '^guard misterplexd=0$' "$LAUNCHLOG" ||
        fail "the launcher's 0-misterplexd scan failed: $(grep '^guard' "$LAUNCHLOG")"
    grep -q '^state=attempted' "$MARKER" || fail "the launcher did not mark the reload attempted"
    # review 21:52 (2): the hand-off's teardown must end well inside MR_EXIT_WAIT (10 s).
    MAST_GONE_MS=$(sed -n 's/^daemon gone after \([0-9]*\) ms$/\1/p' "$LAUNCHLOG" | tail -n 1)
    [ -n "$MAST_GONE_MS" ] && [ "$MAST_GONE_MS" -lt 5000 ] ||
        fail "the handing-off daemon took ${MAST_GONE_MS:-?} ms to exit (want < 5000; MR_EXIT_WAIT 10 s)"
    MAST_TEARDOWN=$(grep -o 'teardown [0-9]* ms since the hand-off' "$LOG" | tail -n 1)
    [ -n "$MAST_TEARDOWN" ] || fail "the daemon did not log its hand-off teardown"
}
mast_wait() { # MAST_RC: the daemon's exit code (not in $(...): wait needs this shell)
    MAST_RC=0
    wait "$PID" || MAST_RC=$?
    PID=
}
# R5f (review 01:19/01:25): the audio control commit word, +0x128 of the mailbox page
# (0x30140128; the fake DDR file's offset is the physical address), as 16 hex digits.
COMMIT_PHYS=$((0x30140128))
commit_word() { od -An -tx8 -j "$COMMIT_PHYS" -N8 "$FAKE" | tr -d ' \n'; }
commit_seed() { # a stale valid commit as on a lab MiSTer after G2 (MACC, publication 3)
    printf '\x43\x43\x41\x4d\x03\x00\x00\x00' |
        dd of="$FAKE" bs=1 seek="$COMMIT_PHYS" conv=notrunc status=none
}
commit_withdrawn() { # what: the commit is 0 and this daemon logged the exit withdraw
    [ "$(commit_word)" = 0000000000000000 ] ||
        fail "$1: the audio command commit is $(commit_word) after the exit, want 0 (R5f)"
    [ "$(count 'media: exit: audio command withdrawn (control commit +0x128 := 0)')" = 1 ] ||
        fail "$1: no exit withdraw line: $(grep 'media: exit: audio command' "$LOG" | head -n 2)"
}

run_wedge_reload() {
    CASE=wedge_reload
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    inject wedge
    commit_seed # R5f: a stale command the wedged instance never reads
    [ "$(commit_word)" = 000000034d414343 ] || fail "commit seed: $(commit_word)"
    mast_play "$OUT/$CASE-1.log" 1 5
    local rc live=
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 86 ] || fail "no MAST reload hand-off at gate open (daemon 1 exit $rc, want 86)"
    grep -q 'MAST probe (gate open): WEDGED' "$LOG" || fail "daemon 1: no WEDGED gate-open probe"
    [ "$(count 'handing ONE guarded core reload')" = 1 ] || fail "daemon 1: not exactly one hand-off"
    [ "$(count 'FPGA begin attempt')" = 0 ] || fail "daemon 1 began a play on a wedged lane"
    commit_withdrawn "daemon 1 (exit 86)"
    mast_launched_ok
    load_core 0x48005D45 unwedge # the launcher's guarded reload: a new, publishing instance
    mast_play "$OUT/$CASE-2.log" 1 5
    local i
    for i in $(seq 1 100); do # R5f: daemon 2's own command is outstanding while it plays
        live=$(commit_word)
        [ "$live" != 0000000000000000 ] && break
        sleep 0.1
    done
    [ "$live" != 0000000000000000 ] || fail "daemon 2: no audio command committed during the play"
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 0 ] || fail "daemon 2, after the reload, exit $rc"
    commit_withdrawn "daemon 2 (exit 0)"
    grep -q 'MAST probe (gate open): live' "$LOG" || fail "daemon 2: no live gate-open probe"
    grep -q 'MAST lane live after the auto core reload' "$LOG" ||
        fail "daemon 2 did not record the recovery"
    grep -q '^state=recovered' "$MARKER" || fail "the marker is not recovered after a live probe"
    grep -q 'LAB play-file done frames=' "$LOG" || fail "daemon 2 did not play after the reload"
    [ "$(mast_launches)" = 1 ] || fail "the launcher ran $(mast_launches) times, want 1"
    echo "  [wedge_reload] wedged at gate open: probe WEDGED, one hand-off, exit 86, launcher saw" \
        "0 misterplexd, daemon gone in ${MAST_GONE_MS} ms (${MAST_TEARDOWN}); after the reload the" \
        "probe was live, marker recovered, play 2 played; R5f: the seeded stale commit" \
        "000000034d414343 was 0 after the exit 86, daemon 2's live commit $live was 0 after its exit 0"
    stop_children
}

run_wedge_twice() {
    CASE=wedge_twice
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    inject wedge
    mast_play "$OUT/$CASE-1.log" 1 5
    local rc
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 86 ] || fail "no MAST reload hand-off at gate open (daemon 1 exit $rc, want 86)"
    mast_launched_ok
    load_core 0x48005D45 wedge # the reload did not help: the new instance is wedged too
    mast_play "$OUT/$CASE-2.log" 1 4
    mast_wait
    rc=$MAST_RC
    [ "$rc" != 86 ] || fail "a second MAST wedge handed off another reload (a loop)"
    [ "$(count 'handing ONE guarded core reload')" = 0 ] || fail "daemon 2 asked for a second reload"
    [ "$(count 'MAST-WEDGE CURE FAILED (gate-open probe)')" = 1 ] ||
        fail "daemon 2: want exactly one CURE FAILED line, at gate open"
    [ "$(count 'again after the auto core reload')" = 1 ] ||
        fail "daemon 2: want one clean-fail line at begin"
    [ "$(count 'FPGA begin attempt 1/5 failed')" = 1 ] || fail "daemon 2: the begin did not fail once"
    [ "$(count 'FPGA begin attempt 2/5')" = 0 ] || fail "daemon 2 retried a wedged begin (blind retries)"
    grep -q 'LAB play-file ERROR zero frames' "$LOG" || fail "daemon 2 played on a wedged lane"
    sleep 1
    [ "$(mast_launches)" = 1 ] || fail "the launcher ran $(mast_launches) times, want 1 (a loop)"
    grep -q '^state=cure-failed' "$MARKER" || fail "the failed cure did not mark cure-failed"
    echo "  [wedge_twice] the reload did not help: one CURE FAILED line, marker cure-failed," \
        "the begin failed clean (one attempt), no second hand-off, the launcher ran once"
    stop_children
}

run_wedge_begin() {
    CASE=wedge_begin
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    mast_play "$OUT/$CASE-1.log" 2 4
    local i
    for i in $(seq 1 300); do
        grep -q 'FPGA stream geometry' "$LOG" && break
        sleep 0.1
    done
    grep -q 'FPGA stream geometry' "$LOG" || fail "play 1 never began"
    inject wedgeatprobe # play 1's begin probe is answered: play 2's begin wedges the lane
    local rc
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 86 ] || fail "no MAST reload hand-off at begin (daemon 1 exit $rc, want 86)"
    grep -q 'MAST probe (gate open): live' "$LOG" || fail "the gate-open probe was not live"
    local play1
    play1=$(sed -n '1,/LAB play-file run 2\/2/p' "$LOG")
    grep -q 'LAB play-file done frames=' <<< "$play1" || fail "play 1 did not play"
    [ "$(count 'FPGA begin attempt 1/5 failed')" = 1 ] || fail "play 2: the begin did not fail once"
    [ "$(count 'FPGA begin attempt 2/5')" = 0 ] || fail "play 2 retried a wedged begin (blind retries)"
    [ "$(count 'MAST wedge (begin): handing ONE guarded core reload')" = 1 ] ||
        fail "play 2: no hand-off at begin"
    mast_launched_ok
    load_core 0x48005D45 unwedge
    mast_play "$OUT/$CASE-2.log" 1 4
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 0 ] || fail "daemon 2, after the reload, exit $rc"
    grep -q 'LAB play-file done frames=' "$LOG" || fail "daemon 2 did not play after the reload"
    [ "$(mast_launches)" = 1 ] || fail "the launcher ran $(mast_launches) times, want 1"
    echo "  [wedge_begin] live at gate open, wedged at play 2's begin: one attempt, one hand-off," \
        "exit 86, launcher saw 0 misterplexd, daemon gone in ${MAST_GONE_MS} ms (worst case:" \
        "exit during the session's teardown; ${MAST_TEARDOWN}); after the reload the play played"
    stop_children
}

run_force_once() {
    CASE=force_once
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    MAST_FORCE=once mast_play "$OUT/$CASE-1.log" 1 5
    local rc
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 86 ] || fail "no MAST reload hand-off at gate open (forced; daemon 1 exit $rc, want 86)"
    grep -q 'MAST probe (gate open): live.*FORCED WEDGE: lab MPX_MAST_FORCE_WEDGE=once' "$LOG" ||
        fail "daemon 1: the probe line does not say FORCED on a live lane"
    [ "$(count 'handing ONE guarded core reload')" = 1 ] || fail "daemon 1: not exactly one hand-off"
    mast_launched_ok
    load_core 0x48005D45
    MAST_FORCE=once mast_play "$OUT/$CASE-2.log" 1 5
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 0 ] || fail "daemon 2, after the forced reload, exit $rc"
    if grep -q 'FORCED WEDGE' "$LOG"; then fail "daemon 2 forced a wedge although the marker exists"; fi
    grep -q 'MAST lane live after the auto core reload' "$LOG" ||
        fail "daemon 2 did not record the recovery"
    grep -q 'LAB play-file done frames=' "$LOG" || fail "daemon 2 did not play"
    [ "$(mast_launches)" = 1 ] || fail "the launcher ran $(mast_launches) times, want 1"
    echo "  [force_once] forced wedge on a live lane: FORCED probe line, one hand-off, launcher" \
        "saw 0 misterplexd; after the reload the marker stopped the force and the play played"
    stop_children
}

run_handoff_lost() {
    CASE=handoff_lost
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    MAST_DROP=1 MAST_FORCE=once mast_play "$OUT/$CASE-1.log" 1 5
    local rc i
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 86 ] || fail "no MAST reload hand-off at gate open (forced; daemon 1 exit $rc, want 86)"
    for i in $(seq 1 50); do grep -q '^dropped' "$LAUNCHLOG" 2>/dev/null && break; sleep 0.1; done
    grep -q '^dropped' "$LAUNCHLOG" 2>/dev/null || fail "the stand-in did not drop the hand-off"
    grep -q '^state=requested' "$MARKER" || fail "the dropped hand-off did not leave the marker requested"
    # No reload ran; the start launcher's retry starts the daemon again (glass G1 23:49).
    MAST_FORCE=once mast_play "$OUT/$CASE-2.log" 1 5
    mast_wait
    rc=$MAST_RC
    [ "$rc" = 0 ] || fail "daemon 2 exit $rc"
    grep -q 'MAST probe (gate open): live' "$LOG" || fail "daemon 2: no live gate-open probe"
    if grep -q 'FORCED WEDGE' "$LOG"; then fail "daemon 2 forced a wedge although the marker exists"; fi
    grep -q 'MAST lane live; hand-off not taken: no reload ran (marker requested' "$LOG" ||
        fail "daemon 2 did not say the hand-off was not taken"
    if grep -q 'after the auto core reload' "$LOG"; then
        fail "daemon 2 called the lane recovered after a reload that never ran"
    fi
    if grep -q 'CURE FAILED' "$LOG"; then fail "a hand-off not taken was reported as a failed cure"; fi
    grep -q '^state=requested' "$MARKER" ||
        fail "daemon 2 changed the marker: $(tr '\n' ' ' < "$MARKER")"
    grep -q 'LAB play-file done frames=' "$LOG" || fail "daemon 2 did not play"
    [ "$(mast_launches)" = 1 ] || fail "the launcher ran $(mast_launches) times, want 1"
    echo "  [handoff_lost] a forced hand-off the launcher dropped (glass G1): daemon 2's live probe" \
        "says 'hand-off not taken: no reload ran', the marker stays requested, nothing recovered," \
        "no CURE FAILED; the play played"
    stop_children
}

# boot_race: the REAL launcher in a sandbox root (BR). Its start mode runs the daemon in prod
# server mode, exactly as at boot; the daemon's hand-off runs the same script again (via
# boot-self.sh, which carries the sandbox settings past the daemon's env -i).
BR=$OUT/br
BR_PORT=39017
BOOT_SCRIPT=${MAST_BOOT_SCRIPT:-$ROOT/scripts/misterplexd-boot-mast.sh}
br_setup() {
    rm -rf "$BR"
    mkdir -p "$BR/media/fat/linux" "$BR/tmp" "$BR/root"
    local d=$BR/media/fat/linux/misterplexd-coregate-2164c4ad
    cp "$OUT/misterplexd" "$d"
    cp "$OUT/e2e.conf" "$BR/media/fat/linux/misterplex.conf"
    BR_MD5=$(md5sum < "$d" | cut -c1-32)
    cat > "$BR/media/fat/linux/misterplexd-mast-load.sh" << 'EOF'
#!/bin/sh
# e2e stand-in for the guarded loader: the LOADCORE-GUARD 0-misterplexd scan, then the e2e
# script performs the reload (load_core ... unwedge) and lets this return.
echo "load $*" >> "$BR_DIR/loader.calls"
echo "marker ${MAST_LOAD_MARKER:-unset}" >> "$BR_DIR/loader.calls"
# R6: the real loader appends its quiesce outcome to the launcher's marker before the load.
if [ -s "$BR_DIR/loader.r6" ] && [ -n "${MAST_LOAD_MARKER:-}" ]; then
    printf 'r6=%s\n' "$(cat "$BR_DIR/loader.r6")" >> "$MAST_LOAD_MARKER"
fi
n=0
hits=
for p in /proc/[0-9]*; do
    q=${p#/proc/}
    case " $BR_EXCL " in *" $q "*) continue ;; esac
    case "$(readlink "$p/exe" 2>/dev/null)" in
        "$BR_DAEMON"|"$BR_DAEMON (deleted)") n=$((n + 1)); hits="$hits $q:exe" ;;
    esac
    if grep -qsF "$BR_FAKE" "$p/maps"; then n=$((n + 1)); hits="$hits $q:maps"; fi
    fds=$(ls -l "$p/fd" 2>/dev/null)
    case $fds in *"$BR_FAKE"*) n=$((n + 1)); hits="$hits $q:fd" ;; esac
    case $fds in *"${BR_LOCK:?BR_LOCK unset}"*) n=$((n + 1)); hits="$hits $q:lock" ;; esac
done
echo "guard misterplexd=$n$hits" >> "$BR_DIR/loader.calls"
i=0
while [ ! -e "$BR_DIR/loader.go" ] && [ $i -lt 400 ]; do i=$((i + 1)); sleep 0.1; done
[ -e "$BR_DIR/loader.go" ]
EOF
    {
        echo '#!/bin/sh'
        echo "export ROOT='$BR' BOOT_PROC=/proc BOOT_STARTUP_DELAY=0 BOOT_NET_WAIT=0 BOOT_CORE_WAIT=5"
        echo "export BOOT_EXPECT_MD5=$BR_MD5 BOOT_PORT=$BR_PORT BOOT_LOCK='$LP_DIR_REAL/misterplexd.lock'"
        echo "export BOOT_CORENAME='$CN' BOOT_MAST_MARKER='$MARKER' BOOT_MAST_SELF='$BR/boot-self.sh'"
        echo "export BOOT_MR_SETTLE=0 BOOT_MR_EXIT_WAIT=10 BOOT_MR_LOCK_WAIT=60 BOOT_START_WAIT=20"
        # kill_all/verify only ever see this sandbox's daemon, even outside iso2
        echo "export BOOT_MATCH='$BR/media/fat/linux/misterplexd*'"
        echo "export BR_DIR='$BR' BR_EXCL='$RPID $MPID' BR_FAKE='$FAKE' BR_DAEMON='$d' BR_LOCK='$LP_DIR_REAL/misterplexd.lock'"
        echo "exec sh '$BOOT_SCRIPT' \"\$@\""
    } > "$BR/boot-self.sh"
    chmod +x "$BR/boot-self.sh"
}
br_count() { grep -c -- "$1" "$2" 2>/dev/null || true; }

run_boot_race() {
    CASE=boot_race
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    inject wedge
    BR_ACTIVE=1
    br_setup
    echo 'quiesced (e2e stand-in)' > "$BR/loader.r6"
    LOG=$BR/tmp/mpxd.log
    local blog=$BR/tmp/misterplexd-boot.log i rc P gone
    # Boot, as user-startup.sh runs it: the start launcher, in its own network namespace.
    unshare -rn sh "$BR/boot-self.sh" start > "$BR/start.out" 2>&1 &
    BPID=$!
    for i in $(seq 1 400); do [ -s "$BR/loader.calls" ] && break; sleep 0.1; done
    [ -s "$BR/loader.calls" ] ||
        fail "no guarded load ran: $(grep -E 'another|MAST_|PROD_' "$blog" 2>/dev/null | tail -n 4 | cut -c21-200 | tr '\n' '|')"
    load_core 0x48005D45 unwedge # what the launcher's guarded load_core does to the core
    touch "$BR/loader.go"
    for i in $(seq 1 400); do grep -q 'PROD_START_OK' "$blog" 2>/dev/null && break; sleep 0.1; done
    grep -q 'PROD_START_OK' "$blog" 2>/dev/null ||
        fail "no verified daemon after the reload: $(tail -n 4 "$blog" 2>/dev/null | cut -c21-200 | tr '\n' '|')"
    rc=0
    wait "$BPID" || rc=$?
    BPID=
    P=$(sed -n 's/.*PROD_START_OK attempt [0-9]* pid=\([0-9]*\).*/\1/p' "$blog" | tail -n 1)
    for i in $(seq 1 100); do grep -q '^state=recovered' "$MARKER" 2>/dev/null && break; sleep 0.1; done
    [ "$rc" = 0 ] || fail "the start launcher exit $rc, want 0 (it yields)"
    grep -q 'MAST_HANDOFF pending at .*start launcher yields' "$blog" ||
        fail "the start launcher did not yield to the hand-off (no race exercised?)"
    if grep -q 'another misterplexd-boot' "$blog"; then fail "the hand-off was dropped on the launcher lock"; fi
    if grep -q 'PROD_START_BAD' "$blog"; then fail "a start attempt went bad with a hand-off pending"; fi
    [ "$(br_count '^load ' "$BR/loader.calls")" = 1 ] ||
        fail "$(br_count '^load ' "$BR/loader.calls") guarded loads, want exactly 1"
    grep -q '^guard misterplexd=0$' "$BR/loader.calls" ||
        fail "the guarded load saw a misterplexd: $(grep '^guard' "$BR/loader.calls")"
    [ "$(br_count 'MAST_RELOAD request' "$blog")" = 1 ] || fail "not exactly one MAST_RELOAD request"
    [ "$(br_count 'marker attempted; ONE guarded load_core' "$blog")" = 1 ] ||
        fail "the reload was not marked attempted exactly once"
    [ "$(br_count 'PROD_START_OK' "$blog")" = 1 ] || fail "not exactly one verified start"
    # review 00:18 P4: one yield, one lock take, nothing dropped or loud, in the order glass G1
    # must show (yield, lock taken, attempted).
    [ "$(br_count 'MAST_HANDOFF pending at' "$blog")" = 1 ] || fail "not exactly one start-launcher yield line"
    [ "$(br_count 'MAST_RELOAD: launcher lock taken after' "$blog")" = 1 ] || fail "not exactly one 'lock taken' line"
    if grep -q 'PROD START FAILED' "$blog"; then fail "PROD START FAILED with a hand-off pending"; fi
    if grep -q '!!!' "$blog"; then fail "loud boot-log line: $(grep '!!!' "$blog" | head -n 2 | cut -c21-160 | tr '\n' '|')"; fi
    [ ! -s "$BR/tmp/misterplexd-boot.FAILED" ] || fail "FAILED file: $(head -n 2 "$BR/tmp/misterplexd-boot.FAILED")"
    local ly lt la
    ly=$(grep -n 'MAST_HANDOFF pending at' "$blog" | head -n 1 | cut -d: -f1)
    lt=$(grep -n 'MAST_RELOAD: launcher lock taken after' "$blog" | head -n 1 | cut -d: -f1)
    la=$(grep -n 'marker attempted; ONE guarded load_core' "$blog" | head -n 1 | cut -d: -f1)
    [ "$ly" -lt "$lt" ] && [ "$lt" -lt "$la" ] || fail "boot log order: yield@$ly lock-taken@$lt attempted@$la"
    grep -q 'MAST probe (gate open): WEDGED' "$LOG" || fail "daemon 1: no WEDGED gate-open probe"
    [ "$(count 'handing ONE guarded core reload')" = 1 ] || fail "not exactly one hand-off"
    grep -q 'exiting 86 (teardown [0-9]* ms since the hand-off' "$LOG" || fail "daemon 1 did not exit 86"
    grep -q 'MAST lane live after the auto core reload' "$LOG" || fail "daemon 2 did not record the recovery"
    grep -q '^state=recovered' "$MARKER" || fail "the marker is not recovered: $(tr '\n' ' ' < "$MARKER")"
    grep -q "^marker $MARKER\$" "$BR/loader.calls" ||
        fail "the launcher did not hand the loader its marker: $(grep '^marker' "$BR/loader.calls")"
    grep -q '^r6=quiesced (e2e stand-in)$' "$MARKER" ||
        fail "the recovered rewrite dropped the loader's r6 line: $(tr '\n' ' ' < "$MARKER")"
    [ -n "$P" ] && kill -0 "$P" 2>/dev/null || fail "no daemon running after the verified start"
    [ "$(tr -dc 0-9 < "$MPX_LOCK_DIR/misterplexd.lock")" = "$P" ] || fail "the running daemon does not own the lock"
    # c3/m1: every daemon that boot.sh started logged its lock paths into its DLOG ($BR/tmp/mpxd.log,
    # boot.sh :77/:240), one per 'start: cd' line in boot.sh's own log (:237).
    lp_expect "$BR/tmp/mpxd.log" "$(grep -c ' up=[0-9.]* start: cd ' "$blog")" ||
        fail "the boot.sh-started daemons' lock paths"
    gone=$(sed -n 's/.*MAST_RELOAD: daemon pid=[0-9]* gone after \([0-9.]*s\).*/\1/p' "$blog" | tail -n 1)
    echo "  [boot_race] the real launcher: daemon 1 wedged at its first gate open inside START_WAIT" \
        "and handed off; the start launcher yielded, the hand-off waited for the lock (daemon gone" \
        "after ${gone:-?}), ONE guarded load (0 misterplexd), attempted -> recovered, daemon $P running" \
        "and verified"
    kill "$P" 2>/dev/null || true
    for i in $(seq 1 50); do kill -0 "$P" 2>/dev/null || break; sleep 0.1; done
    br_stop
    stop_children
}

# R5 (review 01:09; review 01:10/01:28): glass G2 (10/5 01:03) end to end with the REAL
# launcher. As boot_race, but the guarded reload brings up an instance whose CTRL/CAPS lane stays
# silent (fake core 'silent'), so daemon 2's gate never opens. At the bound (60 s from daemon 2's
# start; the gate's 5-probe give-up comes first) daemon 2 marks cure-failed
# why=gate-silent:<reason> and logs one '!!!' line, and the launcher's loud channel (boot log,
# FAILED file) carries it. There is no second hand-off or load and daemon 2 keeps running. A
# later reload by hand that answers logs 'manual recovery after cure-failed', never
# 'recovered', and the marker stays cure-failed.
run_reload_no_answer() {
    CASE=reload_no_answer
    mast_reset
    start_core 0x48005D45
    main_start
    names Plex Plex
    inject wedge
    BR_ACTIVE=1
    br_setup
    echo 'not-quiesced (e2e stand-in: consumed != written)' > "$BR/loader.r6"
    LOG=$BR/tmp/mpxd.log
    local blog=$BR/tmp/misterplexd-boot.log failf=$BR/tmp/misterplexd-boot.FAILED i rc P since
    local loud='MAST cure failed: the core did not answer CAPS after the reload'
    unshare -rn sh "$BR/boot-self.sh" start > "$BR/start.out" 2>&1 &
    BPID=$!
    for i in $(seq 1 400); do [ -s "$BR/loader.calls" ] && break; sleep 0.1; done
    [ -s "$BR/loader.calls" ] ||
        fail "no guarded load ran: $(grep -E 'another|MAST_|PROD_' "$blog" 2>/dev/null | tail -n 4 | cut -c21-200 | tr '\n' '|')"
    load_core 0x48005D45 silent # glass G2: the reloaded core never answers CTRL/CAPS
    touch "$BR/loader.go"
    for i in $(seq 1 400); do grep -q 'PROD_START_OK' "$blog" 2>/dev/null && break; sleep 0.1; done
    grep -q 'PROD_START_OK' "$blog" 2>/dev/null ||
        fail "no verified daemon after the reload: $(tail -n 4 "$blog" 2>/dev/null | cut -c21-200 | tr '\n' '|')"
    rc=0
    wait "$BPID" || rc=$?
    BPID=
    [ "$rc" = 0 ] || fail "the start launcher exit $rc, want 0 (it yields)"
    P=$(sed -n 's/.*PROD_START_OK attempt [0-9]* pid=\([0-9]*\).*/\1/p' "$blog" | tail -n 1)
    [ -n "$P" ] || fail "no daemon pid in the PROD_START_OK line"
    for i in $(seq 1 900); do grep -q "media: !!! $loud" "$LOG" 2>/dev/null && break; sleep 0.1; done
    grep -q "media: !!! $loud" "$LOG" ||
        fail "daemon 2: no loud cure-failed line within 90 s of its verified start: $(grep -E 'core gate|MAST' "$LOG" | tail -n 3 | cut -c1-200 | tr '\n' '|')"
    sleep 1
    [ "$(count "media: !!! $loud")" = 1 ] || fail "daemon 2: not exactly one loud cure-failed line"
    grep -q "media: !!! $loud (no CAPS reply after 5 probes; " "$LOG" ||
        fail "the loud line does not carry the gate's give-up: $(grep -m 1 "!!! $loud" "$LOG" | cut -c1-220)"
    since=$(sed -n 's/.*media: !!! MAST cure failed.* \([0-9][0-9]*\) s since this daemon started.*/\1/p' "$LOG" | head -n 1)
    [ -n "$since" ] && [ "$since" -ge 60 ] && [ "$since" -le 62 ] ||
        fail "the verdict came ${since:-?} s after daemon 2 started, want 60..62 (the bound)"
    grep -q '^state=cure-failed$' "$MARKER" && grep -q '^why=gate-silent:no CAPS reply after 5 probes$' "$MARKER" ||
        fail "marker: $(tr '\n' ' ' < "$MARKER")"
    # R6 attribution (review 02:08 (2)): the loader's outcome is quoted and kept.
    grep -q "media: !!! $loud (.*; R6 before the reload: not-quiesced (e2e stand-in: consumed != written)): no further reload" "$LOG" ||
        fail "the loud line does not attribute R6: $(grep -m 1 "!!! $loud" "$LOG" | cut -c1-260)"
    grep -q '^r6=not-quiesced (e2e stand-in: consumed != written)$' "$MARKER" ||
        fail "the cure-failed rewrite dropped the loader's r6 line: $(tr '\n' ' ' < "$MARKER")"
    [ "$(br_count '!!!' "$blog")" = 1 ] && [ "$(br_count " up=[0-9.]* !!! $loud (" "$blog")" = 1 ] ||
        fail "boot log loud lines: $(grep '!!!' "$blog" | cut -c1-160 | tr '\n' '|')"
    [ "$(br_count "$loud (" "$failf")" = 1 ] || fail "FAILED file: $(head -n 2 "$failf" 2>/dev/null)"
    [ "$(br_count '^load ' "$BR/loader.calls")" = 1 ] ||
        fail "$(br_count '^load ' "$BR/loader.calls") guarded loads, want exactly 1"
    grep -q '^guard misterplexd=0$' "$BR/loader.calls" ||
        fail "the guarded load saw a misterplexd: $(grep '^guard' "$BR/loader.calls")"
    [ "$(br_count 'MAST_RELOAD request' "$blog")" = 1 ] || fail "not exactly one MAST_RELOAD request"
    [ "$(count 'handing ONE guarded core reload')" = 1 ] || fail "not exactly one hand-off (daemon 1's)"
    if grep -q 'MAST lane live after the auto core reload' "$LOG"; then fail "the silent reload was called recovered"; fi
    if grep -q 'MAST-WEDGE CURE FAILED' "$LOG"; then fail "a gate-open CURE FAILED line, but the gate never opened"; fi
    kill -0 "$P" 2>/dev/null || fail "daemon 2 is not running after the verdict"
    [ "$(tr -dc 0-9 < "$MPX_LOCK_DIR/misterplexd.lock")" = "$P" ] || fail "the running daemon does not own the lock"
    # c3/m1: every daemon that boot.sh started logged its lock paths into its DLOG ($BR/tmp/mpxd.log,
    # boot.sh :77/:240), one per 'start: cd' line in boot.sh's own log (:237).
    lp_expect "$BR/tmp/mpxd.log" "$(grep -c ' up=[0-9.]* start: cd ' "$blog")" ||
        fail "the boot.sh-started daemons' lock paths"
    # R5c: a reload by hand that answers is a manual recovery: never 'recovered', no new reload.
    load_core 0x48005D45 unwedge
    for i in $(seq 1 300); do
        grep -q 'MAST lane live: manual recovery after cure-failed' "$LOG" && break
        sleep 0.1
    done
    grep -q 'MAST lane live: manual recovery after cure-failed (marker cure-failed [0-9]* s ago, gate-silent:no CAPS reply after 5 probes): the marker stays cure-failed' "$LOG" ||
        fail "no manual-recovery line after a reload by hand: $(grep -E 'core gate|MAST' "$LOG" | tail -n 3 | cut -c1-200 | tr '\n' '|')"
    grep -q '^state=cure-failed$' "$MARKER" || fail "a manual recovery changed the marker: $(tr '\n' ' ' < "$MARKER")"
    if grep -q 'marker now recovered' "$LOG"; then fail "a manual recovery marked the reload recovered"; fi
    [ "$(count "media: !!! $loud")" = 1 ] || fail "the loud line repeated"
    [ "$(br_count '^load ' "$BR/loader.calls")" = 1 ] || fail "a second guarded load ran"
    [ "$(br_count 'MAST_RELOAD request' "$blog")" = 1 ] || fail "a second MAST_RELOAD request"
    [ "$(count 'handing ONE guarded core reload')" = 1 ] || fail "a second hand-off"
    kill -0 "$P" 2>/dev/null || fail "daemon 2 died after the manual recovery"
    echo "  [reload_no_answer] the real launcher: daemon 1 wedged at gate open, ONE guarded load" \
        "brought up a silent core; daemon 2 (pid $P) gave up after 5 probes and at ${since} s since" \
        "its start marked cure-failed why=gate-silent, one '!!!' line in its log, the boot log and" \
        "FAILED; no second hand-off or load, daemon 2 kept running; a reload by hand: 'manual" \
        "recovery after cure-failed', marker still cure-failed"
    kill "$P" 2>/dev/null || true
    for i in $(seq 1 50); do kill -0 "$P" 2>/dev/null || break; sleep 0.1; done
    br_stop
    stop_children
}

for s in $SCENARIOS; do
    "run_$s"
done
echo "test_audio_recovery_e2e: OK ($SCENARIOS)"
