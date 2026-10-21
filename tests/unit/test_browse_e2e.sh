#!/usr/bin/env bash
# ARM Plex browse P1, the whole daemon (host build, no libav), browse driven by PLXI words
# written into the fake DDR as the core publishes them (browse_fakemem.py), against a fake
# mpx-menu helper and a fake PMS (fake_browse_server.py, which reuses fake_pms.py for
# metadata/decision), the fake core (core_gate_responder: CAPS 0x48005D45, so the gate opens)
# and a fake Main. Everything runs in a loopback-only network namespace with a private /tmp
# and its own PID namespace (unshare -rnmpf --mount-proc).
#
#   parity   flags OFF, base (BROWSE_E2E_BASE, default 2ffd5c14 = 2164c4ad + w9 de8d3d17) and this tree run the same
#            script: open (Stop), cursor, enter a section, back, the new commands 6-11 (must
#            do nothing), Browse (5), enter, play a row. The browse/input/PLAY log lines, the
#            fake PMS request log and the frame-bank hash after every step must be identical.
#   v2       MPX_BROWSE_V2=1 (this tree), the live mpx-menu row shapes: helper root, paging by
#            NEXT/PREV rows, Back keeps cursors, item detail RESUME/FROM START tickets (local:
#            the conf PMS, never the helper's 127.0.0.1 view) -> doPlay "PLAY .../ladder.ts ...
#            off=<row offset>" with no PMS viewOffset (the PMS says 90000), Stop reopens the
#            detail refreshed, an UNPLAYABLE ticket refused, OFFLINE server not enterable, a
#            remote ticket resolved on its own server (198.51.100.7, WAN): Q4 server-transcode
#            = the owning server's transcode at the offset, probed header-less as the ladder
#            fetches it, and the Part when that server needs headers on start; wT's relay
#            (mpxLadderSrc /menu/tsrc/.../o<ms>) as given, no offset_ms, explicit fps; P0's resolve
#            row (helper src); no token in any log line (A12), 20 fast presses = 20 rows (A1),
#            helper down -> HELPER OFFLINE + PMS sections (A9), a slow helper fetch cancelled
#            by the next press (A11).
# Needs unshare, ip, curl, python3; skips without them.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${BROWSE_E2E_OUT:-$ROOT/build/browse_e2e}
BASE_REF=${BROWSE_E2E_BASE:-2ffd5c14}
SCENARIOS=${BROWSE_E2E_SCENARIOS:-parity v2 mvpb}
CXX=${CXX:-g++}
FAKE=$OUT/fake.mem
CN=$OUT/CORENAME
RBF=$OUT/RBFNAME
STAT=$OUT/proc.stat
MEM="python3 $ROOT/tests/unit/core_gate_fakemem.py"
BMEM="python3 $ROOT/tests/unit/browse_fakemem.py"
SRV="python3 $ROOT/tests/unit/fake_browse_server.py"
CONFTOKEN=CONFTOKEN0123456789abc
REMOTETOKEN=REMOTETOKEN9876543210xyz

if [ "${BROWSE_E2E_INNER:-}" != 1 ]; then
    for t in unshare ip curl python3; do
        command -v "$t" > /dev/null || { echo "test_browse_e2e: SKIP (no $t)"; exit 0; }
    done
    mkdir -p "$OUT"
    # T-DAEMON-LOCK-SHARED: the head daemon takes its locks in a private dir (MPX_TEST_BUILD, dir
    # baked in too); the env carries it through the re-exec below.
    export MPX_LOCK_DIR="$OUT/locks"
    mkdir -p "$MPX_LOCK_DIR"
    build() { # src_root out
        local src=$1 out=$2 s srcs=()
        local mut=${CORE_GATE_MUT_DIR:-}
        for s in main.cpp companion.cpp fb_present.cpp media_player.cpp pms_timeline.cpp \
            plex_resolve.cpp fpga_spi.cpp; do
            if [ "$src" = "$ROOT" ] && [ -n "$mut" ] && [ -f "$mut/arm/misterplexd/$s" ]; then
                srcs+=("$mut/arm/misterplexd/$s")
            else
                srcs+=("$src/arm/misterplexd/$s")
            fi
        done
        local inc=(-I"$src/host" -I"$src/arm/misterplexd")
        if [ "$src" = "$ROOT" ] && [ -n "$mut" ]; then
            inc=(-I"$mut/host" -I"$mut/arm/misterplexd" "${inc[@]}")
        fi
        "$CXX" -std=c++17 -O2 "${inc[@]}" -pthread -DMPX_TEST_BUILD -DMPX_TEST_LOCK_DIR="\"$MPX_LOCK_DIR\"" \
            -DMPX_DEVMEM_PATH="\"$FAKE\"" -DMPX_CORENAME_PATH="\"$CN\"" \
            -DMPX_RBFNAME_PATH="\"$RBF\"" -DMPX_PROC_STAT_PATH="\"$STAT\"" \
            -o "$out" "${srcs[@]}"
    }
    build "$ROOT" "$OUT/misterplexd-head"
    if [[ " $SCENARIOS " == *" parity "* ]]; then
        rm -rf "$OUT/base-src"
        mkdir -p "$OUT/base-src"
        git -C "$ROOT" archive "$BASE_REF" arm/misterplexd host assets/plex-profiles |
            tar -x -C "$OUT/base-src"
        build "$OUT/base-src" "$OUT/misterplexd-base"
    fi
    "$CXX" -std=c++17 -O2 -I"$ROOT/host" -o "$OUT/core_gate_responder" \
        "$ROOT/tests/unit/core_gate_responder.cpp"
    exec unshare -rnmpf --mount-proc env BROWSE_E2E_INNER=1 bash "$0" "$@"
fi

# ---- inside the network, mount and PID namespaces ----
# A private /tmp (on disk under $OUT). The head daemon uses the private MPX_LOCK_DIR; the parity
# BASE_REF daemon predates lock_paths.hpp, so this private /tmp is its only isolation from the
# host's /tmp/misterplexd.lock and /tmp/misterplex_spi.lock: fail closed without it. The
# PID namespace (own /proc): the daemon's Main lookup sees only this test's fake MiSTer, not
# another lane's (which reads as "Main restarted" and closes the gate).
mkdir -p "$OUT/tmp"
mount --bind "$OUT/tmp" /tmp 2>/dev/null || {
    echo "FAIL [setup]: no private /tmp (bind mount refused); the base daemon would share the host's /tmp locks" >&2
    exit 1
}
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
ip link set lo up
# The "remote" shared server: a public-looking address (TEST-NET-2) on this netns only, so
# the resolver takes its WAN branch as for a friend's server.
REMOTE_IP=198.51.100.7
ip addr add "$REMOTE_IP/32" dev lo
PIDS=()
DPID=
MENUPID=
CASE=
LOG=/dev/null
cleanup() {
    for p in $DPID $MENUPID "${PIDS[@]}"; do
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    rm -f "$FAKE" "$OUT/MiSTer"
}
trap cleanup EXIT
fail() {
    echo "FAIL [$CASE]: $*" >&2
    grep -n "browse\|library\|PLAY\|input cmd\|ERROR" "$LOG" 2>/dev/null | tail -n 40 | cut -c1-240 >&2 || true
    exit 1
}
count() { grep -c -- "$1" "$LOG" || true; }
waitlog() { # pattern timeout_s
    local i
    for i in $(seq 1 $(($2 * 20))); do
        grep -q -- "$1" "$LOG" && return 0
        sleep 0.05
    done
    return 1
}
press() { $BMEM plxi "$FAKE" "$OUT/plxi.state" "$1"; }
startsrv() { # pidvar portfile args...
    local var=$1 pf=$2
    shift 2
    rm -f "$pf"
    $SRV --port-file "$pf" "$@" &
    eval "$var=$!"
    local i
    for i in $(seq 1 100); do
        [ -s "$pf" ] && return 0
        sleep 0.05
    done
    fail "server did not start ($*)"
}

world() { # fresh fake core, Main and servers
    rm -f "$OUT/plxi.state" "$OUT/count" "$OUT/tsrc-on"
    MENUPORT=
    $MEM create "$FAKE"
    echo 0x48005D45 > "$OUT/build_id"
    printf 'cpu  0 0 0 0\n%s\n' "$(grep '^btime ' /proc/stat)" > "$STAT"
    "$OUT/core_gate_responder" "$FAKE" "$OUT/count" "$OUT/build_id" &
    PIDS+=($!)
    ln -sf /bin/sleep "$OUT/MiSTer"
    "$OUT/MiSTer" 100000 &
    PIDS+=($!)
    sleep 0.2
    printf 'Plex' > "$CN"
    printf 'Plex' > "$RBF"
    : > "$OUT/pms-$CASE.log"
    rm -f "$OUT/start-needs-headers"
    startsrv PMSPID "$OUT/pms.port" --pms --log "$OUT/pms-$CASE.log" --token "$CONFTOKEN" \
        --view-offset 103=90000 --remote-host "$REMOTE_IP" \
        --start-needs-headers-file "$OUT/start-needs-headers"
    PIDS+=($PMSPID)
    read -r PMS4 PMS6 PMSR < "$OUT/pms.port"
    printf '%s\n' CONTENT_RES=480p DISPLAY_RES=480p DECODE=640x480 MPX_VIDEO_BACKEND=fpga-h264 \
        PRESENT=fpga AUDIO_DELAY_MS=50 IDLE_SCREEN=logo OSD_CONTROL=0 MPX_H264_FILTER=off \
        "PLEX_BASE=http://127.0.0.1:$PMS4" "PLEX_TOKEN=$CONFTOKEN" \
        "REMOTE_LADDER_PROXY=http://192.0.2.1:18767" > "$OUT/e2e-$CASE.conf"
}
menu_up() { # extra server args; a restart keeps the port the daemon was given
    : >> "$OUT/menu-$CASE.log"
    local portarg=()
    [ -n "${MENUPORT:-}" ] && portarg=(--port "$MENUPORT")
    startsrv MENUPID "$OUT/menu.port" --log "$OUT/menu-$CASE.log" "${portarg[@]}" \
        --remote-base "http://$REMOTE_IP:$PMSR" --remote-token "$REMOTETOKEN" \
        --tsrc-file "$OUT/tsrc-on" "$@"
    MENUPORT=$(cat "$OUT/menu.port")
}
menu_down() {
    kill "$MENUPID" 2>/dev/null || true
    wait "$MENUPID" 2>/dev/null || true
    MENUPID=
}
daemon_up() { # binary [env...]
    local bin=$1
    shift
    LOG=$OUT/daemon-$CASE.log
    env "$@" "$bin" --conf "$OUT/e2e-$CASE.conf" --port 39005 --name browse-e2e \
        --id browse-e2e > "$LOG" 2>&1 &
    DPID=$!
    # c3: the head build logs its lock paths once; the BASE_REF build predates lock_paths.hpp
    # (it logs none) and is isolated by the private /tmp above instead.
    if [ "$bin" = "$OUT/misterplexd-base" ]; then
        lp_expect "$LOG" 0 || fail "the base daemon logged lock paths"
    else
        lp_expect "$LOG" 1 || fail "the daemon's lock paths"
    fi
    waitlog "core gate open: 'Plex' build 0x48005D45" 15 || fail "the gate never opened"
    waitlog "idle screen painted" 10 || fail "no idle paint"
    sleep 1
    press 0 # the baseline word: the first real press is an edge
    sleep 0.4
}
daemon_down() {
    kill "$DPID" 2>/dev/null || true
    wait "$DPID" 2>/dev/null || true
    DPID=
}
world_down() {
    daemon_down
    menu_down
    for p in "${PIDS[@]}"; do
        kill "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    PIDS=()
}

# ---- parity: flags off, base vs head ----
FILTER='^(media: library|media: input cmd|misterplexd: library overlay play|misterplexd: PLAY |misterplexd: applying PMS viewOffsetMs|media: browse|misterplexd: browse|misterplexd: LAB MPX_BROWSE)'
norm() {
    sed -E 's/127\.0\.0\.1(:|%3A)[0-9]+/127.0.0.1\1PORT/g; s/%5B%3A%3A1%5D%3A[0-9]+/[v6]PORT/g'
}
transcript() { # bin out
    local bin=$1 tr=$2 lines=0 plines=0 step
    world
    daemon_up "$bin"
    : > "$tr"
    snap() { # name
        local n p
        n=$(wc -l < "$LOG")
        p=$(wc -l < "$OUT/pms-$CASE.log")
        {
            echo "== $1 bank=$($BMEM bankhash "$FAKE")"
            sed -n "$((lines + 1)),${n}p" "$LOG" | grep -E "$FILTER" | norm || true
            sed -n "$((plines + 1)),${p}p" "$OUT/pms-$CASE.log" | sed 's/^/pms: /' | norm || true
        } >> "$tr"
        lines=$n
        plines=$p
    }
    snap start
    for step in "2 open" "3 down" "4 up" "1 enter" "3 down" "2 back" "6 new-up" "7 new-down" \
        "8 new-pgup" "9 new-pgdn" "10 new-back" "11 new-info" "5 browse" "1 enter" "1 play"; do
        press "${step%% *}"
        sleep 1.3
        [ "${step#* }" = play ] && sleep 2.5
        snap "${step#* }"
    done
    world_down
}

run_parity() {
    CASE=parity-base
    transcript "$OUT/misterplexd-base" "$OUT/parity-base.txt"
    CASE=parity-head
    transcript "$OUT/misterplexd-head" "$OUT/parity-head.txt"
    CASE=parity
    if ! diff -u "$OUT/parity-base.txt" "$OUT/parity-head.txt" > "$OUT/parity.diff"; then
        head -n 60 "$OUT/parity.diff" >&2
        fail "flags-off transcript differs from $BASE_REF (see $OUT/parity.diff)"
    fi
    LOG=$OUT/daemon-parity-head.log
    local opens plays banks
    opens=$(grep -c '^media: library browse open' "$OUT/parity-head.txt" || true)
    plays=$(grep -c '^misterplexd: PLAY ' "$OUT/parity-head.txt" || true)
    banks=$(grep '^== ' "$OUT/parity-head.txt" | awk '{print $3}' | sort -u | wc -l)
    [ "$opens" -ge 2 ] && [ "$plays" = 1 ] && [ "$banks" -ge 4 ] ||
        fail "parity script did not exercise browse (opens=$opens plays=$plays distinct banks=$banks)"
    grep -q 'applying PMS viewOffsetMs=90000' "$OUT/parity-head.txt" ||
        fail "flags-off play did not keep the PMS auto-resume"
    grep -q '^media: input cmd=6' "$OUT/parity-head.txt" &&
        fail "flags-off: command 6 was accepted"
    echo "  [parity] flags off: $BASE_REF and this tree give identical transcripts" \
        "($(grep -c '^== ' "$OUT/parity-head.txt") steps, $banks distinct frame banks," \
        "$(grep -c '^pms: ' "$OUT/parity-head.txt") PMS requests, commands 6-11 ignored," \
        "PLAY with the PMS viewOffset)"
}

# ---- v2 ----
since() { sed -n "$(($1 + 1)),\$p" "$LOG" || true; } # never SIGPIPE-fails a pipeline (pipefail)
mark() { MARK=$(wc -l < "$LOG"); }
expect_since() { # pattern what
    since "$MARK" | grep -F -- "$1" >/dev/null || fail "$2 (no '$1')"
}
act() { # cmd [settle_s]
    press "$1"
    sleep "${2:-0.6}"
}
bank_moved() { # old deadline_s: poll the frame bank until it differs from old; print it (T-CG-MUTANTS E3b, H264 e2)
    local h now end
    end=$((${EPOCHREALTIME//[!0-9]/} + $2 * 1000000))
    while :; do
        h=$($BMEM bankhash "$FAKE")
        if [ "$h" != "$1" ]; then
            echo "$h"
            return 0
        fi
        now=${EPOCHREALTIME//[!0-9]/}
        [ "$now" -lt "$end" ] || return 1
        sleep 0.1
    done
}
# A fetching press must finish its fetch before the next press: a newer input cancels an older
# fetch by design (kit A11), and the worker paints LOADING... before it starts the fetch, which
# is slow on the fake core under load (run 6: a Down 0.6 s after Select cancelled the item
# fetch at 0 ms). These poll the daemon log instead of trusting a fixed sleep.
wait_count() { # pattern n what [max_s]: >= n lines with pattern since MARK
    local i=0 max=${4:-10} c
    while :; do
        c=$(since "$MARK" | grep -c -F -- "$1" || true)
        [ "${c:-0}" -ge "$2" ] && return 0
        i=$((i + 1))
        [ "$i" -gt $((max * 20)) ] && fail "$3 (want $2 x '$1' within ${max}s, got ${c:-0})"
        sleep 0.05
    done
}
wait_since() { wait_count "$1" 1 "$2" "${3:-10}"; } # pattern what [max_s]
actf() { # cmd pattern what: press, then wait for that fetch to finish
    mark
    press "$1"
    wait_since "$2" "$3"
}

run_v2() {
    CASE=v2
    world
    menu_up
    daemon_up "$OUT/misterplexd-head" MPX_BROWSE_V2=1 "MPX_MENU_URL=http://127.0.0.1:$MENUPORT/menu"
    grep -q 'LAB MPX_BROWSE_V2=1' "$LOG" || fail "no V2 notice"
    grep -q 'input commands 1-5 (MVP-a)' "$LOG" || fail "MVP-a must read commands 1-5 only"
    # MVP-a keys only (prod 4c210d99 emits 1-4): Space=1 select, Esc=2 back/open,
    # Right=3 down, Left=4 up.
    mark
    # T-CG-MUTANTS E3b: the browse controller's repaint hook (media_player.cpp :1240) paints each browse step at once.
    # The IDLE_SCREEN=logo safety net repaints only every 30 s (host/libmisterplex/r1_idle_paint.hpp:247), so the frame
    # bank moving on BOTH of two steps (each polled <= 5 s, so both within 10 s) can only be the hook (mutant E3b: it
    # never moves). Polls, not fixed sleeps (H264 e2: slot load).
    local bank0 bank1
    bank0=$($BMEM bankhash "$FAKE")
    act 2 # Stop while idle: open
    wait_since "media: browse fetch http://127.0.0.1:$MENUPORT/menu?... http=200" "helper root"
    bank1=$(bank_moved "$bank0" 5) || fail "browse open did not repaint within 5 s (frame bank still the idle one)"
    grep -q 'no SPI, no lock; commands 1-5)' "$LOG" || fail "input not read lock-free (1-5)"
    act 3 # Right: MOVIES
    bank_moved "$bank1" 5 >/dev/null || fail "browse cursor move did not repaint within 5 s (frame bank unchanged)"
    actf 1 "/menu/section/local/1/all?... http=200" "section page" # Select
    # Paging with the rows (no paging keys in MVP-a): Left from row 0 wraps to NEXT.
    mark
    act 4
    act 1 # NEXT -> page 2 (cursor on PREV)
    wait_count '/menu/section/local/1/all?... http=200' 1 "paging: page 2"
    act 4
    act 1 # NEXT -> page 3
    wait_count '/menu/section/local/1/all?... http=200' 2 "paging: page 3"
    act 1 # PREV -> page 2
    wait_count '/menu/section/local/1/all?... http=200' 3 "paging: back to page 2"
    [ "$(since "$MARK" | grep -c '/menu/section/local/1/all?... http=200')" = 3 ] ||
        fail "paging: want 3 page fetches"
    grep -q 'GET /menu/section/local/1/all?page=2' "$OUT/menu-$CASE.log" || fail "never reached page 3"
    # PageDown (9) is MVP-b: ignored here, as on today's RBF.
    mark
    act 9
    since "$MARK" | grep 'input cmd=9' >/dev/null && fail "MVP-a accepted command 9"
    since "$MARK" | grep 'browse fetch' >/dev/null && fail "command 9 fetched"
    act 2 # Esc: back to the root (MOVIES cursor kept)
    actf 1 "/menu/section/local/1/all?... http=200" "page 1 again" # Select MOVIES again -> page 1
    actf 1 "/menu/item/local/103 http=200" "item detail (path-only key, no cols added)" # BBB THIRTY (rk 103)
    act 1 # Select on the info row: nothing
    expect_since "browse row not activatable (info)" "info row"
    act 3
    act 3 # RESUME FROM 01:01
    mark
    act 1 2.5
    expect_since "/menu/ticket/local/103/o61000 http=200" "local ticket fetched"
    expect_since "media: browse play ratingKey=103 server=local source=local start=61000ms via conf PMS" \
        "resume play"
    expect_since "misterplexd: browse play gen=" "dispatch"
    expect_since "misterplexd: PLAY http://192.0.2.1:18767/ladder.ts?src=" "local play through the ladder (A6)"
    expect_since "&offset_ms=61000 off=61000" "the row's resume offset"
    since "$MARK" | grep 'applying PMS viewOffsetMs' >/dev/null && fail "doPlay applied the PMS viewOffset"
    since "$MARK" | grep 'X-Plex-Token%3DREDACTED' >/dev/null || fail "PLAY line token not redacted"
    since "$MARK" | grep 'browse ladder input' >/dev/null && fail "a local ticket changed the ladder input"
    since "$MARK" | grep '127.0.0.1%3A32400' >/dev/null && fail "the helper's own 127.0.0.1 PMS view was used"
    mark
    act 2 1.2 # Esc (the play ended; idle): open the kept detail, refreshed
    wait_since "/menu/item/local/103 http=200" "detail refreshed on reopen (A8)"
    act 3 # PLAY FROM START
    mark
    act 1 2.5
    expect_since "/menu/ticket/local/103/o0 http=200" "from-start ticket fetched"
    expect_since "start=0ms via conf PMS" "from-start ticket"
    expect_since "misterplexd: PLAY http://192.0.2.1:18767/ladder.ts" "from-start PLAY"
    since "$MARK" | grep 'misterplexd: PLAY ' | grep 'offset_ms=' >/dev/null && fail "from start carried an offset"
    since "$MARK" | grep 'applying PMS viewOffsetMs' >/dev/null && fail "from start applied the PMS viewOffset"
    since "$MARK" | grep 'misterplexd: PLAY ' | grep ' off=0 ' >/dev/null || fail "from start is not off=0"
    # An UNPLAYABLE ticket (mpxPlayable=no): the reason in the status line, no play.
    mark
    act 2 1.2 # reopen the detail
    wait_since "/menu/item/local/103 http=200" "detail reopened"
    act 2     # list (cursor on BBB THIRTY)
    act 3
    act 3
    act 3     # MOVIE 4 (rk 2001)
    actf 1 "/menu/item/local/2001 http=200" "MOVIE 4 detail" # its detail: info, info, PLAY
    act 3
    act 3     # PLAY
    mark
    act 1
    wait_since "media: browse play ticket refused: NOT PLAYABLE HERE" "mpxPlayable=no refused"
    sleep 1   # nothing may start after the refusal
    since "$MARK" | grep 'misterplexd: PLAY \|media: browse play ratingKey' >/dev/null &&
        fail "an unplayable ticket was played"
    # Remote: servers -> OFFLINE not enterable -> SHARED A -> its rows.
    act 2     # list
    act 2     # root (its cursor is still on MOVIES)
    act 3     # SERVERS
    actf 1 "/menu/servers http=200" "servers page" # servers
    act 3
    act 3     # SHARED B OFFLINE
    mark
    act 1
    expect_since "browse row not activatable (offline)" "offline server"
    act 4     # SHARED A
    actf 1 "/menu/s/sremote1 http=200" "remote list" # a live ticket row, then P0's resolve row
    # Q4 server-transcode (a 25 Mbps Part on a friend's server): the owning server's own
    # transcode at the offset, as the ladder will fetch it (no X-Plex headers).
    mark
    local pmark play probe
    pmark=$(wc -l < "$OUT/pms-$CASE.log")
    act 1 3
    expect_since "/menu/ticket/sremote1/101/o95000 http=200" "remote ticket fetched"
    expect_since "source=ladder start=95000ms via http://$REMOTE_IP:$PMSR" "remote ticket server"
    expect_since "browse ladder input for /library/metadata/101: the owning server's transcode" \
        "Q4 server-transcode asked"
    expect_since "browse ladder input: the owning server's own transcode" "owner transcode used"
    play=$(since "$MARK" | grep 'misterplexd: PLAY ' | tail -n 1)
    [[ "$play" == *"ladder.ts?src=http%3A%2F%2F$REMOTE_IP%3A$PMSR%2Fvideo%2F%3A%2Ftranscode%2Funiversal%2Fstart.mp4"* ]] ||
        fail "server-transcode src: $play"
    local want
    for want in "X-Plex-Client-Identifier%3Dmisterplex" \
        "X-Plex-Session-Identifier%3D" "X-Plex-Product%3DPlex" "offset%3D95" " off=95000 "; do
        [[ "$play" == *"$want"* ]] || fail "server-transcode PLAY lacks $want: $play"
    done
    [[ "$play" == *"offset_ms="* ]] && fail "an owner transcode at the offset also got offset_ms"
    sed -n "$((pmark + 1)),\$p" "$OUT/pms-$CASE.log" | grep 'GET /library/metadata/101' >/dev/null ||
        fail "remote metadata not resolved on the owning server"
    sed -n "$((pmark + 1)),\$p" "$OUT/pms-$CASE.log" | grep 'transcode/universal/decision' |
        grep -q 'X-Plex-Client-Profile-Extra: ' && fail "decision sent Profile-Extra without opt-in"
    probe=$(sed -n "$((pmark + 1)),\$p" "$OUT/pms-$CASE.log" |
        grep 'GET /video/:/transcode/universal/start.mp4' | head -n 1)
    [ -n "$probe" ] || fail "no start probe on the owning server"
    [[ "${probe#* | }" == *X-Plex-* ]] && fail "the start probe sent X-Plex headers: ${probe#* | }"
    # Negative control: the same row while that server needs headers on start (wT's 13:06
    # finding): the original Part at the offset (the contract's interim).
    : > "$OUT/start-needs-headers"
    actf 2 "/menu/s/sremote1 http=200" "remote list reopened"
    mark
    act 1 3
    expect_since "browse ladder input: owning server: its transcode does not stream without headers (http=400" \
        "header-less refusal seen"
    play=$(since "$MARK" | grep 'misterplexd: PLAY ' | tail -n 1)
    [[ "$play" == *"%2Flibrary%2Fparts%2F1101%2F"* && "$play" == *"&offset_ms=95000 off=95000 "* ]] ||
        fail "the fallback is not the Part at the offset: $play"
    rm -f "$OUT/start-needs-headers"
    # wT's relay (mpx-menu 59044aa, contract §9.3 rev): the ticket's mpxLadderSrc is the
    # helper's /menu/tsrc/<slug>/<rk>/o<ms>. The ladder gets it as given, with no offset_ms
    # (the start is in its path) and an explicit fps (never fps=src).
    : > "$OUT/tsrc-on"
    actf 2 "/menu/s/sremote1 http=200" "remote list reopened"
    mark
    act 1 3
    expect_since "browse ladder input for /library/metadata/101: the helper's source" "relay as src"
    expect_since "browse ladder input: the helper's server-transcode relay (offset in its path, fps 24000/1001)" \
        "relay with an explicit fps"
    play=$(since "$MARK" | grep 'misterplexd: PLAY ' | tail -n 1)
    [[ "$play" == *"ladder.ts?src=http%3A%2F%2F127.0.0.1%3A$MENUPORT%2Fmenu%2Ftsrc%2Fsremote1%2F101%2Fo95000&"* ]] ||
        fail "relay src: $play"
    [[ "$play" == *"&fps=24000/1001"* && "$play" != *"fps=src"* ]] || fail "relay fps not explicit: $play"
    [[ "$play" == *"offset_ms="* ]] && fail "a relay play got offset_ms: $play"
    [[ "$play" == *" off=95000 "* ]] || fail "relay play start: $play"
    rm -f "$OUT/tsrc-on"
    # P0's resolve row: the helper's own ladder input (mpxLadderSrc), a Part-like src.
    actf 2 "/menu/s/sremote1 http=200" "remote list reopened"
    act 3     # the resolve row
    mark
    act 1 3
    expect_since "/menu/resolve/sremote1/101?... http=200" "P0 resolve row"
    expect_since "browse ladder input for /library/metadata/101: the helper's source" "helper src"
    play=$(since "$MARK" | grep 'misterplexd: PLAY ' | tail -n 1)
    [[ "$play" == *"mpxq4%3Downer"* && "$play" == *"%26offset%3D95&w="* && "$play" != *"offset_ms="* &&
       "$play" == *" off=95000 "* ]] ||
        fail "the helper's ladder input did not carry the start in its own offset=: $play"
    # A1: 20 fast presses, 20 rows, none lost (browser visible again first).
    act 2 1.2
    mark
    local i
    for i in $(seq 1 20); do
        press 3
        sleep 0.12
    done
    sleep 0.5
    [ "$(since "$MARK" | grep -c 'media: browse input down')" = 20 ] ||
        fail "20 presses gave $(since "$MARK" | grep -c 'media: browse input down') moves"
    # A9: helper down: HELPER OFFLINE, today's PMS sections.
    menu_down
    mark
    act 5 2.5 # Browse (no RBF sends it today; the decode accepts 5 as before)
    expect_since "HELPER OFFLINE, local PMS sections" "fallback"
    tail -n 5 "$OUT/pms-$CASE.log" | grep 'GET /library/sections?' >/dev/null || fail "no PMS sections fetch"
    # A11: a slow helper fetch, cancelled by the next press.
    menu_up --delay-ms 2500
    mark
    press 5
    sleep 0.4
    local t0 t1
    t0=$(date +%s%N)
    press 3
    for i in $(seq 1 100); do
        since "$MARK" | grep 'cancelled after' >/dev/null && break
        sleep 0.01
    done
    t1=$(date +%s%N)
    since "$MARK" | grep 'cancelled after' >/dev/null || fail "the slow fetch was not cancelled"
    [ $(((t1 - t0) / 1000000)) -lt 400 ] || fail "cancel took $(((t1 - t0) / 1000000)) ms after the press"
    expect_since "media: browse input down" "the newer press ran"
    # A12: no token anywhere in the daemon log.
    sleep 0.5
    grep -q "$CONFTOKEN" "$LOG" && fail "conf token in the daemon log"
    grep -q "$REMOTETOKEN" "$LOG" && fail "remote token in the daemon log"
    echo "  [v2] MVP-a keys: helper root, paging by NEXT/PREV rows (3 pages), cmd 9 ignored," \
        "local tickets RESUME (off=61000, PMS viewOffset 90000 not applied) and FROM START" \
        "(off=0) on the conf PMS through /ladder.ts, Esc reopens the detail refreshed," \
        "UNPLAYABLE ticket refused, OFFLINE row inert, remote ticket on $REMOTE_IP: Q4 owner" \
        "transcode at the offset (header-less probe), Part when start needs headers, P0 helper" \
        "src (off=95000), 20/20 presses, HELPER OFFLINE fallback, slow fetch cancelled" \
        "$(((t1 - t0) / 1000000)) ms after the press, no token in the log"
    world_down
}

run_mvpb() {
    CASE=mvpb
    world
    menu_up
    daemon_up "$OUT/misterplexd-head" MPX_BROWSE_V2=1 MPX_BROWSE_MVPB=1 \
        "MPX_MENU_URL=http://127.0.0.1:$MENUPORT/menu"
    grep -q 'input commands 1-11 (MPX_BROWSE_MVPB=1)' "$LOG" || fail "no MVP-b notice"
    act 2 # open
    act 7 # Down (7): MOVIES
    mark
    act 1
    act 9 # PageDown: NEXT >
    act 8 # PageUp: < PREV
    [ "$(since "$MARK" | grep -c '/menu/section/local/1/all?... http=200')" = 3 ] ||
        fail "MVP-b paging: want 3 page fetches"
    grep -q 'no SPI, no lock; commands 1-11)' "$LOG" || fail "MVP-b reads 1-11"
    mark
    act 10 # Back
    act 6  # Up
    act 11 # Info
    expect_since "media: browse input back" "Back (10)"
    expect_since "media: browse input up" "Up (6)"
    expect_since "media: browse input info" "Info (11)"
    echo "  [mvpb] MPX_BROWSE_MVPB=1: Down/PageDown/PageUp/Back/Up/Info (7, 9, 8, 10, 6, 11) drive" \
        "the browser"
    world_down
}

for s in $SCENARIOS; do
    "run_$s"
done
echo "test_browse_e2e: OK ($SCENARIOS)"
