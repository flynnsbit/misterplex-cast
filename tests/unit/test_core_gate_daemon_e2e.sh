#!/usr/bin/env bash
# Core gate v2, whole daemon. The host misterplexd build has its /dev/mem opens pointed at
# a sparse fake file and /tmp/CORENAME, /tmp/RBFNAME at build-dir files, and runs in a
# loopback-only network namespace (unshare -rn) so companion/GDM never reach the LAN.
# core_gate_responder is the FPGA (fake_plex_core.hpp: SPI handshake, AU-ring replies,
# CAPS with the build ID in a file). The script is Main: the FPGA-manager stat and GPO
# core-reset bit (core_gate_fakemem.py fabric, under the daemon's SPI flock, as a stopped
# Main would be), a fake Main process (a "MiSTer" symlink to sleep, a new PID per load)
# and CORENAME/RBFNAME.
#
# Main starts before it writes CORENAME/RBFNAME, as on hardware (core gate v2.1 checks
# the names against its start).
# static:    IDLE_SCREEN=logo, no OSD poll. The daemon starts under Plex 480p 0x48005D45 and
#            opens on its own (settle, one CAPS probe), then Plex 45D -> Plex 5D50 with
#            CORENAME 'Plex' both times and 2 s of the old names: quiet, then reopened and
#            repainted.
# busy:      IDLE_SCREEN=screensaver (repaint every 100 ms until the OSD poll's first read
#            sets the logo), OSD_CONTROL=1 (OSD poll with its SPI fallback): something always
#            wants to write. The 480p conf ladder pairs the daemon with 480p. MENU at start,
#            then Plex 480p, Plex 240p (CORENAME unchanged, 3 s of old names:
#            re-pairs and opens), 0x48005D51 (rejected unless this build
#            table has it), MENU, an empty CORENAME, Plex 480p, and a short-lived child of
#            Main named MiSTer while open (no reload). While closed no byte of fake memory
#            changes and no SPI strobe happens.
# busyv2:    busy's closed windows with MPX_BROWSE_V2=1 (the V2 OSD poll, startOsdPollV2): MENU at start, Plex
#            480p (open, painted), MENU again; quiet while closed (kills mutant E5b).
# paired240: a daemon configured for 240p (MPX_FPGA_MODE=240p) started under Plex 480p:
#            its first core re-pairs to 480p and opens, and a 480p -> 240p reload
#            with CORENAME 'Plex' both times re-pairs to 240p, then opens and paints.
# stale:     started under a new Main that has not written its names: the last core's
#            'Plex' CORENAME is not trusted (0 bytes, 0 strobes, 0 probes); then NES names,
#            and CORENAME Plex over RBFNAME MENU, stay closed; a Plex load opens.
# tools:     the lab tool set_status refuses under MENU and beside a running misterplexd,
#            and on Plex with no daemon verifies the core by one probe before it writes.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${CORE_GATE_E2E_OUT:-$ROOT/build/core_gate_e2e}
mkdir -p "$OUT"
# T-DAEMON-LOCK-SHARED: a private lock dir for the daemon AND set_status (both MPX_TEST_BUILD), so
# this job never takes or unlinks the host-global /tmp/misterplexd.lock or /tmp/misterplex_spi.lock.
export MPX_LOCK_DIR="$OUT/locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
FAKE=$OUT/fake.mem
CN=$OUT/CORENAME
RBF=$OUT/RBFNAME
BUILD=$OUT/build_id
STAT=$OUT/proc.stat # btime for the daemon and set_status (set_btime)
SPILOCK=$MPX_LOCK_DIR/misterplex_spi.lock # the daemon's SPI lock: fabric edits serialize with its windows
MEM="python3 $ROOT/tests/unit/core_gate_fakemem.py"
CXX=${CXX:-g++}

# Mutation runs (core_gate_mutants.py): mutated copies of daemon sources in
# CORE_GATE_MUT_DIR (arm/misterplexd/*.cpp, host/libmisterplex/*.hpp) replace the tree's.
MUT=${CORE_GATE_MUT_DIR:-}
SRC="main.cpp companion.cpp fb_present.cpp media_player.cpp pms_timeline.cpp plex_resolve.cpp fpga_spi.cpp"
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
# Mutated headers in arm/misterplexd (core_gate_tool.hpp) come first too.
inc_arm=(-I"$ROOT/arm/misterplexd")
if [ -n "$MUT" ]; then inc_arm=(-I"$MUT/arm/misterplexd" "${inc_arm[@]}"); fi
"$CXX" -std=c++17 -O2 "${inc[@]}" "${inc_arm[@]}" -pthread -DMPX_TEST_BUILD -DMPX_TEST_LOCK_DIR="\"$MPX_LOCK_DIR\"" \
    -DMPX_DEVMEM_PATH="\"$FAKE\"" -DMPX_CORENAME_PATH="\"$CN\"" -DMPX_RBFNAME_PATH="\"$RBF\"" \
    -DMPX_PROC_STAT_PATH="\"$STAT\"" \
    -o "$OUT/misterplexd" "${srcs[@]}"
"$CXX" -std=c++17 -O2 -I"$ROOT/host" -o "$OUT/core_gate_responder" \
    "$ROOT/tests/unit/core_gate_responder.cpp"
# The lab tool set_status, built managed like the ARM tools (core_gate_tool.hpp), on the
# same fake paths.
ss_src=$ROOT/tools/set_status.cpp
if [ -n "$MUT" ] && [ -f "$MUT/tools/set_status.cpp" ]; then ss_src=$MUT/tools/set_status.cpp; fi
spi_src=$ROOT/arm/misterplexd/fpga_spi.cpp
if [ -n "$MUT" ] && [ -f "$MUT/arm/misterplexd/fpga_spi.cpp" ]; then
    spi_src=$MUT/arm/misterplexd/fpga_spi.cpp
fi
"$CXX" -std=c++17 -O2 "${inc[@]}" "${inc_arm[@]}" -pthread -DMPX_TEST_BUILD -DMPX_TEST_LOCK_DIR="\"$MPX_LOCK_DIR\"" \
    -DMPX_DEVMEM_PATH="\"$FAKE\"" -DMPX_CORENAME_PATH="\"$CN\"" -DMPX_RBFNAME_PATH="\"$RBF\"" \
    -DMPX_PROC_STAT_PATH="\"$STAT\"" \
    -o "$OUT/set_status" "$ss_src" "$spi_src"
ln -sf /bin/sleep "$OUT/MiSTer" # findMisterPids matches the argv0 basename "MiSTer"

# btime as the daemon reads it: this host's by default. set_btime 0 stands in for an
# RTC-less MiSTer before ntpd's step, or one that never syncs (it boots at 1970).
set_btime() { # real | seconds
    if [ "$1" = real ]; then
        printf 'cpu  0 0 0 0\n%s\n' "$(grep '^btime ' /proc/stat)" > "$STAT"
    else
        printf 'cpu  0 0 0 0\nbtime %s\n' "$1" > "$STAT"
    fi
}
set_btime real

allow_5d51=0
if grep -q '0x48005D51u' "$ROOT/host/libmisterplex/fpga_build_ids.hpp"; then allow_5d51=1; fi

PID=
RPID=
MPID=
CASE=
LOG=/dev/null
COUNT=
H=
HN=
S=
P=
stop_pid() {
    if [ -n "$1" ]; then
        kill -CONT "$1" 2>/dev/null || true
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}
stop_children() {
    stop_pid "$PID"
    stop_pid "$RPID"
    stop_pid "$MPID"
    PID=
    RPID=
    MPID=
}
cleanup() {
    stop_children
    rm -f "$FAKE" "$OUT/MiSTer"
}
trap cleanup EXIT
fail() {
    echo "FAIL [$CASE]: $*" >&2
    grep -n "core gate\|idle screen\|play refused" "$LOG" >&2 || true
    exit 1
}
count() { grep -c -- "$1" "$LOG" || true; }
hashm() { $MEM hash "$FAKE"; }
hashnr() { $MEM hash-noring "$FAKE"; } # frame side only: a paint moves it, a probe does not
strobes() { cut -d' ' -f1 "$COUNT"; }
probes() { cut -d' ' -f2 "$COUNT"; }
fabric() { flock "$SPILOCK" $MEM fabric "$FAKE" "$1" "$2"; }
refill() { flock "$SPILOCK" $MEM fill "$FAKE"; }
mark() { H=$(hashm); HN=$(hashnr); S=$(strobes); P=$(probes); }
quiet() { # what
    [ "$(hashm)" = "$H" ] || fail "fake memory written: $1"
    [ "$(strobes)" = "$S" ] || fail "SPI strobes: $1"
    [ "$(probes)" = "$P" ] || fail "CAPS probe: $1"
}
main_start() {
    "$OUT/MiSTer" 100000 &
    MPID=$!
}
names() {
    printf '%s' "$1" > "$CN"
    printf '%s' "$2" > "$RBF"
}

# Main loads a core: GPO bit 30 (fpga_io.cpp:474), the FPGA manager through configuration,
# a new Main whose fpga_io_init clears GPO (app_restart), the new core, and STALE seconds
# later SetUARTMode's CORENAME/RBFNAME (user_io.cpp:1237-1238). From 0.3 s after bit 30
# until the names are written, nothing may change.
load_core() { # build core rbf stale_seconds
    fabric 4 1
    sleep 0.3
    refill
    mark
    fabric 2 1
    echo 0 > "$BUILD"
    sleep 0.3
    fabric 4 1
    stop_pid "$MPID"
    main_start
    fabric 4 0
    echo "$1" > "$BUILD"
    sleep "$4"
    quiet "loading $2 ($3), before its CORENAME write"
    names "$2" "$3"
}

start_daemon() { # idle_screen osd_control [extra conf line]
    local conf=$OUT/e2e-$CASE.conf
    # The prod conf's non-secret keys (933a4104); no PMS, no token.
    printf '%s\n' CONTENT_RES=480p DISPLAY_RES=480p DECODE=640x480 \
        MPX_VIDEO_BACKEND=fpga-h264 PRESENT=fpga AUDIO_DELAY_MS=50 \
        "IDLE_SCREEN=$1" "OSD_CONTROL=$2" ${3:+"$3"} > "$conf"
    "$OUT/core_gate_responder" "$FAKE" "$COUNT" "$BUILD" &
    RPID=$!
    sleep 0.3
    mark
    unshare -rn "$OUT/misterplexd" --conf "$conf" --port 39005 --name core-gate-e2e \
        --id core-gate-e2e > "$LOG" 2>&1 &
    PID=$!
    lp_expect "$LOG" 1 || fail "the daemon's lock paths"
}

expect_open() { # build n
    [ "$(count "core gate open: 'Plex' build $1")" = "$2" ] ||
        fail "want $2 open line(s) for $1, got $(count "core gate open: 'Plex' build $1")"
}

run_static() {
    CASE=static
    LOG=$OUT/daemon-static.log
    COUNT=$OUT/count-static
    $MEM create "$FAKE"
    echo 0x48005D45 > "$BUILD"
    main_start
    names Plex Plex_480p
    start_daemon logo 0
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    if grep -q "no present path" "$LOG"; then fail "no present path (fake /dev/mem not opened)"; fi
    [ "$(count 'core gate closed at start')" = 1 ] || fail "no 'closed at start' line"
    expect_open 0x48005D45 1
    [ "$(probes)" = 1 ] || fail "want 1 CAPS probe at start, got $(probes)"
    [ "$(hashnr)" != "$HN" ] || fail "no idle paint after the open"

    load_core 0x48005D50 Plex Plex_480p_5D50 2
    [ "$(count 'core gate closed: ')" = 1 ] || fail "want 1 closed line, got $(count 'core gate closed: ')"
    sleep 2
    expect_open 0x48005D50 1
    [ "$(probes)" = $((P + 1)) ] || fail "want 1 probe for the reload"
    [ "$(hashnr)" != "$HN" ] || fail "no repaint on the way back"
    echo "  [static] start under Plex: opened by CAPS 0x48005D45 and painted; 45D -> 5D50" \
        "(CORENAME 'Plex' both, 2 s stale): 0 bytes, 0 strobes, 0 probes, then reopened" \
        "on 0x48005D50 and repainted; $(count 'misterplexd: core gate') gate lines"
    stop_children
}

run_busy() {
    CASE=busy
    LOG=$OUT/daemon-busy.log
    COUNT=$OUT/count-busy
    $MEM create "$FAKE"
    echo 0 > "$BUILD"
    main_start
    names MENU menu
    start_daemon screensaver 1
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    if grep -q "no present path" "$LOG"; then fail "no present path (fake /dev/mem not opened)"; fi
    [ "$(count 'core gate closed at start')" = 1 ] || fail "no 'closed at start' line"
    [ "$(count "core gate stays closed: loaded core 'MENU' is not Plex")" = 1 ] ||
        fail "want 1 MENU hold line"
    quiet "MENU loaded at start, 3 s"

    load_core 0x48005D45 Plex Plex_480p 0.3
    sleep 2
    expect_open 0x48005D45 1
    [ "$(count 'core gate: this daemon is paired with 480p cores (FPGA mode 480p: conf ladder')" = 1 ] ||
        fail "no 480p pairing line"
    [ "$(hashnr)" != "$HN" ] || fail "no DDR write after the open"
    [ "$(strobes)" -gt "$S" ] || fail "no SPI after the open"
    local s1=$(($(strobes) - S))
    mark

    # A later 240p core re-pairs and opens (CORENAME unchanged through the stale window).
    load_core 0x2400F40B Plex Plex_240p 3
    sleep 2
    expect_open 0x2400F40B 1
    [ "$(count 'core gate re-paired: 480p -> 240p on core load build 0x2400F40B')" = 1 ] ||
        fail "no 240p re-pair line"
    [ "$(hashnr)" != "$HN" ] || fail "no paint after the 240p re-pair"

    load_core 0x48005D51 Plex Plex_480p_5D51 0.3
    sleep 2
    if [ "$allow_5d51" = 1 ]; then
        expect_open 0x48005D51 1
        local five="opened (in this build table)"
    else
        [ "$(count "answered CAPS with build 0x48005D51 (not in the FPGA build table), not on the allowlist")" = 1 ] ||
            fail "no 5D51 allowlist reject line"
        sleep 0.5
        refill
        mark
        sleep 3
        quiet "5D51 rejected, 3 s"
        local five="rejected, probed once, then 0 bytes and 0 strobes for 3 s"
    fi

    load_core 0 MENU menu 0.3
    sleep 1.5
    [ "$(count "core gate stays closed: loaded core 'MENU' is not Plex")" = 2 ] ||
        fail "want 2 MENU hold lines"
    refill
    mark
    sleep 3
    quiet "MENU loaded again, 3 s"
    : > "$CN"
    sleep 1.5
    quiet "CORENAME empty"
    [ "$(count 'core gate stays closed: CORENAME empty or unreadable')" -ge 1 ] ||
        fail "no empty-CORENAME hold line"

    load_core 0x48005D45 Plex Plex_480p 0.3
    sleep 2
    expect_open 0x48005D45 2
    [ "$(hashnr)" != "$HN" ] || fail "no DDR write on the last return"

    # Main's own child, named MiSTer until its exec (system(), fork): newer than Main, so
    # no reload. It runs for 0.5 s (several ticks) and exits while the gate is open.
    local closed0
    closed0=$(count 'core gate closed: ')
    "$OUT/MiSTer" 0.5 &
    local kid=$!
    sleep 1.2
    wait "$kid" 2>/dev/null || true
    sleep 0.6
    [ "$(count 'core gate closed: ')" = "$closed0" ] || fail "Main's child closed the gate"
    # (The idle screen is static by now: the OSD poll's first read set the logo mode.)
    mark
    sleep 1
    [ "$(strobes)" -gt "$S" ] || fail "no SPI after Main's child"
    local lines
    lines=$(count 'misterplexd: core gate')
    [ "$lines" -le 45 ] || fail "$lines core-gate lines: per-tick logging?"
    echo "  [busy] MENU: 0 bytes, 0 strobes, 0 probes; Plex 480p: DDR + $s1 strobes;" \
        "480p -> 240p (CORENAME unchanged): re-paired and painted;" \
        "5D51 $five; MENU and empty CORENAME: quiet; Plex 480p again: open; Main's child:" \
        "gate stays open; $lines gate lines"
    stop_children
}

# busyv2 (T-CG-MUTANTS-E3B-E5B, review 00:43): busy's closed windows with MPX_BROWSE_V2=1, so the OSD poll is
# startOsdPollV2 (media_player.cpp :929-944 @8d52ace6; startOsdPoll dispatches to it, :870-871). MENU at start: quiet
# 3 s; Plex 480p: open and painted; MENU again: quiet 3 s. A closed-gate V2 poll must not touch the fake core: with
# its gate and the SPI frame gate removed (mutant E5b) it falls through to getCoreStatus's SPI frame and strobes.
run_busyv2() {
    CASE=busyv2
    LOG=$OUT/daemon-busyv2.log
    COUNT=$OUT/count-busyv2
    $MEM create "$FAKE"
    echo 0 > "$BUILD"
    main_start
    names MENU menu
    export MPX_BROWSE_V2=1   # read with getenv (main.cpp :672); start_daemon forks the daemon with it
    start_daemon screensaver 1
    unset MPX_BROWSE_V2
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    if grep -q "no present path" "$LOG"; then fail "no present path (fake /dev/mem not opened)"; fi
    grep -q 'LAB MPX_BROWSE_V2=1' "$LOG" || fail "no V2 notice: the V2 OSD poll is not under test"
    [ "$(count "core gate stays closed: loaded core 'MENU' is not Plex")" = 1 ] ||
        fail "want 1 MENU hold line"
    quiet "V2 OSD poll, MENU loaded at start, 3 s"

    load_core 0x48005D45 Plex Plex_480p 0.3
    sleep 2
    expect_open 0x48005D45 1
    [ "$(hashnr)" != "$HN" ] || fail "no DDR write after the open (V2)"

    load_core 0 MENU menu 0.3
    sleep 1.5
    [ "$(count "core gate stays closed: loaded core 'MENU' is not Plex")" = 2 ] ||
        fail "want 2 MENU hold lines"
    refill
    mark
    sleep 3
    quiet "V2 OSD poll, MENU loaded again, 3 s"
    echo "  [busyv2] MPX_BROWSE_V2=1 (startOsdPollV2): MENU at start and again: 0 bytes, 0 strobes, 0 probes;" \
        "Plex 480p between: open and painted"
    stop_children
}

# A daemon configured for 240p (MPX_FPGA_MODE=240p, as the 240p overlay sets it) started
# under Plex 480p: the first core re-pairs to 480p and opens, and a later 240p load
# re-pairs again and paints.
run_paired240() {
    CASE=paired240
    LOG=$OUT/daemon-paired240.log
    COUNT=$OUT/count-paired240
    $MEM create "$FAKE"
    echo 0x48005D45 > "$BUILD"
    main_start
    names Plex Plex_480p
    start_daemon logo 0 MPX_FPGA_MODE=240p
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    [ "$(count 'core gate: this daemon is paired with 240p cores (FPGA mode 240p: MPX_FPGA_MODE=240p)')" = 1 ] ||
        fail "no 240p pairing line at start"
    expect_open 0x48005D45 1
    [ "$(count 'core gate re-paired: 240p -> 480p on core load build 0x48005D45')" = 1 ] ||
        fail "no 480p re-pair line"
    [ "$(hashnr)" != "$HN" ] || fail "no paint after the 480p re-pair"
    [ "$(probes)" = 1 ] || fail "want 1 CAPS probe at start, got $(probes)"
    mark

    load_core 0x2400F40B Plex Plex_240p 2
    sleep 2
    expect_open 0x2400F40B 1
    [ "$(count 'core gate re-paired: 480p -> 240p on core load build 0x2400F40B')" = 1 ] ||
        fail "no 240p re-pair line"
    [ "$(hashnr)" != "$HN" ] || fail "no paint after the 240p open"
    echo "  [paired240] 240p daemon started under Plex 480p 45D: re-paired and painted;" \
        "480p -> 240p (CORENAME 'Plex' both, 2 s stale): re-paired onto 0x2400F40B and painted"
    stop_children
}

# A daemon started while Main loads a non-Plex core: the fabric is live and a new Main
# runs, but CORENAME still says Plex (the last core's). No probe and no byte until Main
# writes its names. Then a Plex CORENAME over a non-Plex RBFNAME stays closed too.
run_stale() {
    CASE=stale
    LOG=$OUT/daemon-stale.log
    COUNT=$OUT/count-stale
    $MEM create "$FAKE"
    echo 0 > "$BUILD" # the core being loaded never answers CAPS
    main_start
    names Plex Plex # the last Plex core's names, as Main writes them on a lab MiSTer
    # Main's start is known to a lower bound (btime is whole seconds): on hardware the
    # reprogram puts seconds between the last core's names and the new Main.
    sleep 1.2
    stop_pid "$MPID"
    main_start # the new Main, still in its init
    start_daemon logo 0
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    [ "$(count 'CORENAME/RBFNAME predate Main pid')" = 1 ] || fail "no stale-name hold line"
    quiet "stale CORENAME at daemon start, 3 s"
    names NES NES # the new Main writes its names
    sleep 1.5
    [ "$(count "loaded core 'NES' is not Plex")" = 1 ] || fail "no NES hold line"
    quiet "NES loaded, 1.5 s"
    names Plex MENU
    sleep 1.5
    [ "$(count "RBFNAME 'MENU' is not a Plex core")" = 1 ] || fail "no bad-RBFNAME hold line"
    quiet "CORENAME Plex over RBFNAME MENU, 1.5 s"
    load_core 0x48005D45 Plex Plex 0.3
    sleep 2
    expect_open 0x48005D45 1
    [ "$(hashnr)" != "$HN" ] || fail "no paint after the open"
    echo "  [stale] daemon started under the last core's 'Plex' names: 0 bytes, 0 strobes," \
        "0 probes over 3 s; NES names, then CORENAME Plex over RBFNAME MENU: quiet; a Plex" \
        "load: opened on 0x48005D45 and painted"
    stop_children
}

# The boot clock step: an RTC-less MiSTer boots at 1970 and ntpd -g steps it to today once
# the network is up (a lab MiSTer: messages.0.229:269, :430). The boot core's Main wrote its names
# before the step, so on today's clock they predate boot, which no /tmp (tmpfs) file can.
# A daemon started after the step must not hold on them: the CAPS probe decides.
run_bootclock() {
    CASE=bootclock
    LOG=$OUT/daemon-bootclock.log
    COUNT=$OUT/count-bootclock
    $MEM create "$FAKE"
    echo 0x48005D45 > "$BUILD"
    main_start
    names Plex Plex
    touch -m -d @100 "$CN" "$RBF" # written under the 1970 boot clock
    start_daemon logo 0
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    [ "$(count 'predate Main pid')" = 0 ] || fail "held on names written before the clock step"
    expect_open 0x48005D45 1
    [ "$(probes)" = 1 ] || fail "want 1 CAPS probe, got $(probes)"
    [ "$(hashnr)" != "$HN" ] || fail "no idle paint after the open"
    echo "  [bootclock] names stamped 1970 (before the boot clock step) under a live 45D: no" \
        "hold, one probe, opened on 0x48005D45 and painted"
    stop_children
}

# An RTC-less MiSTer before ntpd's step, or one that never syncs: btime is 0, and Main's
# start is its start ticks, both on the 1970 clock. Names this Main wrote open; names
# older than this Main (the last core's) hold.
run_prentp() {
    CASE=prentp
    LOG=$OUT/daemon-prentp.log
    COUNT=$OUT/count-prentp
    $MEM create "$FAKE"
    echo 0x48005D45 > "$BUILD"
    set_btime 0
    local hz t0
    hz=$(getconf CLK_TCK)
    main_start
    sleep 0.2
    t0=$(($(cut -d' ' -f22 "/proc/$MPID/stat") / hz)) # Main's start, s on the 1970 clock
    names Plex Plex
    touch -m -d "@$((t0 + 2))" "$CN" "$RBF" # this Main's names, after it started
    start_daemon logo 0
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    [ "$(count 'predate Main pid')" = 0 ] || fail "held on this Main's names with btime 0"
    expect_open 0x48005D45 1
    [ "$(probes)" = 1 ] || fail "want 1 CAPS probe, got $(probes)"
    [ "$(hashnr)" != "$HN" ] || fail "no idle paint after the open"
    stop_children
    LOG=$OUT/daemon-prentp-stale.log
    COUNT=$OUT/count-prentp-stale
    $MEM create "$FAKE"
    main_start
    sleep 0.2
    t0=$(($(cut -d' ' -f22 "/proc/$MPID/stat") / hz))
    names Plex Plex
    touch -m -d "@$((t0 - 5))" "$CN" "$RBF" # the last core's names, before this Main
    start_daemon logo 0
    sleep 3
    kill -0 "$PID" 2>/dev/null || fail "daemon exited"
    [ "$(count 'predate Main pid')" = 1 ] || fail "no stale-name hold with btime 0"
    quiet "names older than Main with btime 0, 3 s"
    set_btime real
    echo "  [prentp] btime 0 (RTC-less, before ntpd's step): this Main's names opened on" \
        "0x48005D45 and painted; names older than Main held: 0 bytes, 0 strobes, 0 probes"
    stop_children
}

# The lab tool set_status (core_gate_tool.hpp): refuses under MENU and beside a running
# misterplexd; on a verified Plex core with no daemon it probes once and writes.
run_tools() {
    CASE=tools
    LOG=$OUT/tools.log
    COUNT=$OUT/count-tools
    $MEM create "$FAKE"
    echo 0 > "$BUILD"
    main_start
    names MENU menu
    "$OUT/core_gate_responder" "$FAKE" "$COUNT" "$BUILD" &
    RPID=$!
    sleep 0.3
    : > "$LOG"
    mark
    local rc=0
    "$OUT/set_status" --bit 16 1 >> "$LOG" 2>&1 || rc=$?
    [ "$rc" = 3 ] || fail "set_status under MENU: rc $rc, want 3"
    lp_expect "$LOG" 1 || fail "set_status lock paths (MENU)"
    grep -q "set_status: refusing: the loaded core is not a verified Plex build" "$LOG" ||
        fail "no refusal line under MENU"
    quiet "set_status under MENU"
    load_core 0x48005D45 Plex Plex 0.3
    mark
    rc=0
    "$OUT/set_status" --bit 16 1 >> "$LOG" 2>&1 || rc=$?
    [ "$rc" = 0 ] || fail "set_status on Plex 45D: rc $rc"
    lp_expect "$LOG" 2 || fail "set_status lock paths (Plex 45D)"
    grep -q "core gate open: 'Plex' build 0x48005D45" "$LOG" || fail "no open line from set_status"
    [ "$(probes)" = $((P + 1)) ] || fail "set_status did not verify the core by one probe"
    [ "$(strobes)" -gt "$S" ] || fail "set_status wrote no SPI on Plex 45D"
    # The boot core's names, written under the 1970 clock before ntpd's step: not held.
    touch -m -d @100 "$CN" "$RBF"
    local p0
    p0=$(probes)
    rc=0
    "$OUT/set_status" --bit 16 1 >> "$LOG" 2>&1 || rc=$?
    [ "$rc" = 0 ] || fail "set_status on names from before the clock step: rc $rc"
    lp_expect "$LOG" 3 || fail "set_status lock paths (pre-step names)"
    [ "$(probes)" = $((p0 + 1)) ] || fail "set_status did not probe names from before the step"
    local conf=$OUT/e2e-tools.conf
    printf '%s\n' CONTENT_RES=480p DISPLAY_RES=480p DECODE=640x480 \
        MPX_VIDEO_BACKEND=fpga-h264 PRESENT=fpga AUDIO_DELAY_MS=50 IDLE_SCREEN=logo \
        OSD_CONTROL=0 > "$conf"
    unshare -rn "$OUT/misterplexd" --conf "$conf" --port 39005 --name core-gate-e2e \
        --id core-gate-e2e > "$OUT/daemon-tools.log" 2>&1 &
    PID=$!
    lp_expect "$OUT/daemon-tools.log" 1 || fail "the tools-case daemon's lock paths"
    sleep 2.5
    local p1
    p1=$(probes)
    rc=0
    "$OUT/set_status" --bit 16 1 >> "$LOG" 2>&1 || rc=$?
    [ "$rc" = 3 ] || fail "set_status beside a running misterplexd: rc $rc, want 3"
    lp_expect "$LOG" 4 || fail "set_status lock paths (beside the daemon)"
    grep -q "set_status: refusing: misterplexd is running" "$LOG" ||
        fail "no running-daemon refusal"
    [ "$(probes)" = "$p1" ] || fail "set_status probed beside a running misterplexd"
    echo "  [tools] set_status: MENU refused (rc 3, 0 bytes, 0 strobes, 0 probes); Plex 45D" \
        "with no daemon: one probe, then the write; names from before the clock step: one" \
        "probe, rc 0; beside a running misterplexd: refused (rc 3, no probe)"
    stop_children
    # T3b (T-DAEMON-LOCK-SHARED A1 c3): set_status and this script's own fabric flock resolve the
    # SAME SPI lock file. While the script holds it, set_status's first SPI window must block.
    flock "$SPILOCK" sleep 2 &
    local hold=$! t0 t1
    sleep 0.2
    t0=$(date +%s%N)
    rc=0
    "$OUT/set_status" --bit 16 1 >> "$LOG" 2>&1 || rc=$?
    t1=$(date +%s%N)
    wait "$hold" 2>/dev/null || true
    lp_expect "$LOG" 5 || fail "set_status lock paths (T3b)"
    [ $(((t1 - t0) / 1000000)) -ge 1500 ] ||
        fail "T3b: set_status did not block on the held SPI lock ($(((t1 - t0) / 1000000)) ms, rc $rc)"
    echo "  [tools] T3b: set_status waited $(((t1 - t0) / 1000000)) ms on the script's SPI flock (rc $rc);" \
        "lock paths checked $LP_CHECKS times, one pair: $LP_WANT"
}

run_static
run_busy
run_busyv2
run_paired240
run_stale
run_bootclock
run_prentp
run_tools
echo "test_core_gate_daemon_e2e: OK"
