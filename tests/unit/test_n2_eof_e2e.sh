#!/usr/bin/env bash
# N2 end of stream in the whole daemon (host libav build, as the ARM build), on the coregate
# e2e fixtures: fake_plex_core.hpp through core_gate_responder (build 0x48005D45), a
# link-wrapped fake /dev/MrAudio, and n2_eof_server.py serving the clip in the ladder proxy's
# response shape. The server and the daemon share one unshare -rn namespace (loopback only).
# Lab --play-file plays the URL to its natural end; --play-duration-ms declares the PMS
# duration and --play-start-ms the resume offset (review F4/F5,
# review-rel-batch-1428-1004.md section 4).
#
# Video only (AUDIO=0: the AU reader meets the EIO, "compressed demux"):
#   proxy    proxy shape, PMS duration clip + 500 ms -> FPGA_TERMINAL eof=1 fully_drained=1,
#            one drain, exactly one "input ended by the server's close" INFO line, no error
#   resume   /ladder.ts?offset_ms=3000 (the clip from 3 s), start 3000 -> the same
#   length   Content-Length (unchanged)               -> eof=1 fully_drained=1, no INFO line
# With audio (AUDIO=1: the PCM pump meets it first, "compressed audio demux", as on a lab MiSTer). An audio
# natural end is checked as fully as a video one: eof=1 with one acked drain, fully_drained=1 and
# every AU presented, plus no consumed-ahead stop and the fake's consumed <= written (notahead);
# the capped fake MrAudio consumer (below) accounts the drain as the real core does.
#   proxy    -> eof=1 fully_drained=1, one drain, every AU presented, one INFO line, no demux error
#   length   Content-Length (unchanged)               -> eof=1 fully_drained=1, no INFO, no demux
#            error, every AU presented (the same checks as the control base-a-length)
#   cut50    closed at half the clip                  -> the EIO demux error, eof=0, no INFO
#   unknown  proxy shape, no PMS duration (0)         -> the EIO demux error, eof=0, no INFO
#   stall    Content-Length with a 3 s server stall at 40% (n2_eof_server /stall.ts): the PCM
#            producer starves; the consumer leaves a gap (this play's gap_pairs > 0) and the play
#            still ends naturally: eof=1 fully_drained=1, every AU presented
#   stall-nocap  the same with the fake's old wall-clock consumer (inject "cap 0"): the
#            consumer runs ahead (this play's consumed > written, asserted) and media_player
#            :2360 stops the play. This row shows the stall row's teeth; it is expected to stop.
#   The fake MrAudio consumer mirrors sys/alsa.sv (review/release 22:59-23:01; glass-safety
#   note dev-480i-mast-consumed-glass-safety-1004.md): this harness injects "cap 1" into
#   core_gate_responder. A pair is consumed only once fake_mraudio_wrap.cpp has published it
#   as written; short of that the consumer leaves a gap, as the real core does, instead of
#   running ahead on wall time. Every audio play still retries the old consumed-ahead stop (the
#   fake's line with eof=0), 5 tries 10 s apart; with the cap it should not occur. Each play's
#   consumer counters are snapshotted from COUNT.audio when it ends ($OUT/NAME.count_audio).
# The daemon's own seek (--play-start-ms on a URL with no baked offset, F2 / 90e335b5 N-b):
#   seekpin  a byte-Range Content-Length server (all-IDR clip): FPGA_TERMINAL input_session_ms
#            restarts at the seek target (3000..3100); 2164c4ad then stops the play on its own
#            direct-seek rule ("non-IDR direct seek requires FPGA preroll"), as the control does
#   seekproxy the proxy shape cannot seek: "compressed seek: Operation not permitted", as the
#            control does. A proxied resume therefore only runs through the URL offset (resume).
# Control "without the fix": by default this tree with the N2 product change taken out (the
# reverse of the commit "N2, an FFmpeg http EIO at the server's close", found by subject, applied
# to av_inproc_decode.cpp/.hpp and media_player.cpp only; main.cpp keeps the two lab flags). So the
# control is the same code base as the fix, on any line it is picked onto (2164c4ad, r3, the
# combined candidate). N2_EOF_BASE_REF=<ref> instead builds <ref> with this tree's main.cpp.
#   proxy    -> "compressed audio demux: Input/output error", eof=0 (N2 as on a lab MiSTer)
#   length   -> eof=1 fully_drained=1, every AU presented (Content-Length is untouched by N2)
#   seekpin, seekproxy -> the same direct-seek and seek errors
# N2_EOF_E2E_MUTANTS=1: the daemon rebuilt four times (the window -> 0, F1 removed, the start
# offset dropped in media_player, the seek-target restart dropped); each must fail a case.
# Needs pkg-config libav*, ffmpeg (libx264, aac), unshare and ip; skips without them. Waits
# (up to N2_EOF_LOCK_WAIT_S, default 900) while an earlier daemon of this test still holds the
# private $MPX_LOCK_DIR/misterplexd.lock, at the start and again for any play it kept from
# starting; a play that never started
# is NORUN (a failure, and a mutant with one is INCONCLUSIVE, never caught). Heavy: run it in
# heavy-sim-slot on lab-pc.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${N2_EOF_E2E_OUT:-$ROOT/build/n2_eof_e2e}
CLIPS=${N2_EOF_OUT:-$ROOT/build/n2_eof}
BASE_REF=${N2_EOF_BASE_REF:-}
N2_SUBJECT="N2, an FFmpeg http EIO at the server's close"
CXX=${CXX:-g++}
LIBAV=$(pkg-config --libs libavformat libavcodec libswresample libavutil 2>/dev/null || true)
if [ -z "$LIBAV" ] || ! command -v ffmpeg > /dev/null || ! command -v unshare > /dev/null ||
    ! command -v ip > /dev/null; then
    echo "test_n2_eof_e2e: SKIP (needs host libav, ffmpeg, unshare and ip)"
    exit 0
fi
mkdir -p "$OUT" "$CLIPS"
# T-DAEMON-LOCK-SHARED: a private lock dir (MPX_TEST_BUILD daemons, dir baked in too).
export MPX_LOCK_DIR="$OUT/locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
FAKE=$OUT/fake.mem
CN=$OUT/CORENAME
RBF=$OUT/RBFNAME
BUILD=$OUT/build_id
AUDIO=$OUT/audio_inject
COUNT=$OUT/count
MEM="python3 $ROOT/tests/unit/core_gate_fakemem.py"

build_daemon() { # TREE OUTBIN
    local srcs=() s
    for s in main.cpp companion.cpp fb_present.cpp media_player.cpp pms_timeline.cpp \
        plex_resolve.cpp fpga_spi.cpp av_inproc_decode.cpp; do
        srcs+=("$1/arm/misterplexd/$s")
    done
    # shellcheck disable=SC2086
    "$CXX" -std=c++17 -O2 -I"$1/host" -I"$1/arm/misterplexd" -pthread -DMPX_HAVE_LIBAV \
        -DMPX_TEST_BUILD -DMPX_TEST_LOCK_DIR="\"$MPX_LOCK_DIR\"" \
        -DMPX_DEVMEM_PATH="\"$FAKE\"" -DMPX_CORENAME_PATH="\"$CN\"" \
        -DMPX_RBFNAME_PATH="\"$RBF\"" -o "$2" "${srcs[@]}" \
        "$ROOT/tests/unit/fake_mraudio_wrap.cpp" $LIBAV \
        -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close
}
if [ "${N2_EOF_REUSE_BUILD:-0}" != 1 ] || [ ! -x "$OUT/misterplexd" ] || [ ! -x "$OUT/misterplexd-base" ]; then
    build_daemon "$ROOT" "$OUT/misterplexd"
    rm -rf "$OUT/base-src" && mkdir -p "$OUT/base-src"
    if [ -n "$BASE_REF" ]; then
        git -C "$ROOT" archive "$BASE_REF" arm host assets | tar -x -C "$OUT/base-src"
        cp "$ROOT/arm/misterplexd/main.cpp" "$ROOT/arm/misterplexd/lock_paths.hpp" "$OUT/base-src/arm/misterplexd/"
        # The copied main.cpp takes its daemon lock through lock_paths.hpp; a base fpga_spi.cpp
        # from before T-DAEMON-LOCK-SHARED would still take the host's /tmp SPI lock (split brain).
        grep -q 'spiLockPath()' "$OUT/base-src/arm/misterplexd/fpga_spi.cpp" ||
            { echo "test_n2_eof_e2e: FAIL (N2_EOF_BASE_REF $BASE_REF predates lock_paths.hpp: its SPI lock would be host-global)"; exit 1; }
    else
        # The SUBJECT line only: a later commit may quote this subject in its body (the harness
        # commit itself did, and --grep then picked it: an empty diff, so the control was this
        # very tree and the proxy controls failed on 87720e67).
        # awk reads all of git log (no early exit): with pipefail, an early exit would SIGPIPE
        # git and set -e would end this script silently.
        n2=$(git -C "$ROOT" log --format='%H%x09%s' HEAD |
            awk -F'\t' -v s="$N2_SUBJECT" '!found && index($2, s) { print $1; found = 1 }')
        [ -n "$n2" ] || { echo "test_n2_eof_e2e: FAIL (no commit '$N2_SUBJECT' in HEAD's history; set N2_EOF_BASE_REF)"; exit 1; }
        cp -r "$ROOT/arm" "$ROOT/host" "$ROOT/assets" "$OUT/base-src/"
        git -C "$ROOT" diff "$n2^" "$n2" -- arm/misterplexd/av_inproc_decode.cpp \
            host/libmisterplex/av_inproc_decode.hpp arm/misterplexd/media_player.cpp > "$OUT/base-n2.diff"
        [ -s "$OUT/base-n2.diff" ] ||
            { echo "test_n2_eof_e2e: FAIL (commit ${n2:0:8} has no N2 product diff; set N2_EOF_BASE_REF)"; exit 1; }
        # GNU patch, strict (no fuzz): base-src sits inside this repo, where git apply would
        # silently skip paths outside the current directory.
        patch -s -R -p1 -F0 --no-backup-if-mismatch -d "$OUT/base-src" < "$OUT/base-n2.diff" ||
            { echo "test_n2_eof_e2e: FAIL (the N2 change ${n2:0:8} does not reverse-apply here; set N2_EOF_BASE_REF)"; exit 1; }
        changed=0
        for f in arm/misterplexd/av_inproc_decode.cpp host/libmisterplex/av_inproc_decode.hpp \
            arm/misterplexd/media_player.cpp; do
            cmp -s "$ROOT/$f" "$OUT/base-src/$f" || changed=$((changed + 1))
        done
        [ "$changed" -gt 0 ] ||
            { echo "test_n2_eof_e2e: FAIL (the control equals this tree; set N2_EOF_BASE_REF)"; exit 1; }
        BASE_REF="this tree without ${n2:0:8} ($changed file(s) reverted)"
    fi
    build_daemon "$OUT/base-src" "$OUT/misterplexd-base"
    "$CXX" -std=c++17 -O2 -I"$ROOT/host" -o "$OUT/core_gate_responder" \
        "$ROOT/tests/unit/core_gate_responder.cpp"
fi
ln -sf /bin/sleep "$OUT/MiSTer"
X264=(-c:v libx264 -profile:v baseline -level 3.0 -x264-params ref=1:bframes=0:keyint=24 -pix_fmt yuv420p)
if [ ! -s "$CLIPS/full.ts" ] || [ ! -s "$CLIPS/tail.ts" ] || [ ! -s "$CLIPS/idr.ts" ]; then
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=24000/1001 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 "${X264[@]}" \
        -c:a aac -b:a 96k -ar 48000 -ac 2 -shortest -f mpegts "$CLIPS/full.ts"
    ffmpeg -nostdin -loglevel error -y -ss 3 -i "$CLIPS/full.ts" "${X264[@]}" \
        -c:a aac -b:a 96k -ar 48000 -ac 2 -f mpegts "$CLIPS/tail.ts"
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=24000/1001 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 \
        -c:v libx264 -profile:v baseline -level 3.0 -x264-params ref=1:bframes=0:keyint=1 \
        -pix_fmt yuv420p -c:a aac -b:a 96k -ar 48000 -ac 2 -shortest -f mpegts "$CLIPS/idr.ts"
fi
for a in 0 1; do
    printf '%s\n' CONTENT_RES=480p DISPLAY_RES=480p DECODE=640x480 MPX_VIDEO_BACKEND=fpga-h264 \
        PRESENT=fpga AUDIO=$a AUDIO_DELAY_MS=50 IDLE_SCREEN=logo MPX_H264_FILTER=off > "$OUT/audio$a.conf"
done

# One daemon at a time per lock dir: wait while an earlier daemon of THIS test still holds the
# private lock. The lock files are never removed (the inode rule).
waited=0
until flock -n "$MPX_LOCK_DIR/misterplexd.lock" true 2>/dev/null; do
    [ "$waited" -lt "${N2_EOF_LOCK_WAIT_S:-900}" ] || {
        echo "test_n2_eof_e2e: SKIP ($MPX_LOCK_DIR/misterplexd.lock held for ${waited}s)"
        exit 0
    }
    sleep 5
    waited=$((waited + 5))
done
RPID=
MPID=
stop_pid() { if [ -n "${1:-}" ]; then kill "$1" 2>/dev/null || true; wait "$1" 2>/dev/null || true; fi; }
cleanup() {
    stop_pid "$RPID"
    stop_pid "$MPID"
    rm -f "$FAKE" "$OUT/MiSTer"
}
trap cleanup EXIT

$MEM create "$FAKE"
echo 0x48005D45 > "$BUILD"
: > "$AUDIO"
"$OUT/core_gate_responder" "$FAKE" "$COUNT" "$BUILD" "$AUDIO" &
RPID=$!
sleep 0.3
# The consumer cap (see the header): one injection per sequence number.
INJ=0
inject() { # CMD [ARGS]: apply one responder injection and wait until COUNT.audio shows it
    INJ=$((INJ + 1))
    printf '%s %s\n' "$INJ" "$*" > "$AUDIO"
    local want="" n
    [ "$1" = cap ] && want="cap=$2"
    for n in $(seq 1 50); do
        [ -z "$want" ] && { sleep 0.1; return 0; }
        grep -q " $want " "$COUNT.audio" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "test_n2_eof_e2e: FAIL (the responder did not apply '$*')"
    exit 1
}
inject cap 1
"$OUT/MiSTer" 100000 &
MPID=$!
sleep 0.2
printf Plex > "$CN"
printf Plex > "$RBF"

fails=0
# Without the consumer cap (cap 0, the a-stall-nocap row) the fake MrAudio consumer's clock is
# wall time, so a daemon starved by host load "underruns" it and the play stops before its input
# ends; 2164c4ad does it too. With the cap (every other play) it should not occur; the retry stays
# as a guard. Audio plays retry only that signature (the fake's line, eof=0, and neither a demux
# error nor the INFO line), up to N2_EOF_AUDIO_TRIES attempts (default 5) N2_EOF_AUDIO_RETRY_S
# apart (default 10 s, out of a load burst), and print every retry.
underran() {
    grep -q 'audio DMA epoch/error/consumed counter does not match submitted PCM' "$1" &&
        ! grep -q -E "compressed (audio )?demux: Input/output error|input ended by the server's close" "$1" &&
        [[ "$(grep -m1 '^FPGA_TERMINAL ' "$1" || true)" == *" eof=0 "* ]]
}
# A stall's counters (review 22:53 (3)): the AU index (FPGA_TERMINAL), the PCM pairs the
# fake /dev/MrAudio accepted, the fake core's consumed clock at its last publication while
# active ($COUNT.audio) and the load.
stall_info() {
    local term
    term=$(grep -m1 '^FPGA_TERMINAL ' "$1" || true)
    printf 'submitted_au=%s presented_au=%s pcm_written_pairs=%s %s load=%s' \
        "$(tf "$term" submitted_au)" "$(tf "$term" presented_au)" \
        "$(grep -oE 'FAKE_MRAUDIO [a-z]+ written_bytes=[0-9]+ pairs=[0-9]+' "$1" | tail -n 1 |
            grep -oE 'pairs=[0-9]+' | cut -d= -f2 || true)" \
        "$(tr -d '\n' 2>/dev/null < "$COUNT.audio" || true)" \
        "$(cut -d' ' -f1-3 /proc/loadavg)"
}
# A daemon refuses to start ("already running") while another holds its lock. With the private
# MPX_LOCK_DIR that is only an earlier daemon of this test still exiting. Such a play did not run:
# it is played again once the lock is free (N2_EOF_LOCK_WAIT_S), and if it still did not run,
# check() reports NORUN, a failure that never counts as a mutant catch.
ran() { grep -q 'misterplexd: LAB play-file=' "$1"; }
lock_free_wait() {
    local waited=0
    until flock -n "$MPX_LOCK_DIR/misterplexd.lock" true 2>/dev/null; do
        [ "$waited" -lt "${N2_EOF_LOCK_WAIT_S:-900}" ] || return 1
        sleep 5
        waited=$((waited + 5))
    done
}
play_ran() { # play_once, played again while another daemon held the lock
    local n=0
    play_once "$@"
    while ! ran "$OUT/$1.log" && grep -q 'misterplexd: already running' "$OUT/$1.log" && [ "$n" -lt 10 ]; do
        n=$((n + 1))
        echo "  WAIT $1: an earlier daemon held $MPX_LOCK_DIR/misterplexd.lock; playing it again when free ($n)"
        lock_free_wait || return 0
        play_once "$@"
    done
}
play() { # NAME DAEMON AUDIO PATH DURATION_MS START_MS
    local try=1 info
    play_ran "$@"
    while [ "$3" = 1 ] && [ "$try" -lt "${N2_EOF_AUDIO_TRIES:-5}" ] && underran "$OUT/$1.log"; do
        info=$(stall_info "$OUT/$1.log")
        mv "$OUT/$1.log" "$OUT/$1.try$try.log"
        echo "  RETRY $1: the fake MrAudio underran before the input ended (host load); try $try: $info"
        try=$((try + 1))
        sleep "${N2_EOF_AUDIO_RETRY_S:-10}"
        play_ran "$@"
    done
    if [ "$3" = 1 ] && underran "$OUT/$1.log"; then
        echo "  STALL $1: the fake MrAudio underran on the last try ($try): $(stall_info "$OUT/$1.log")"
    fi
    # This play's consumer counters (consumed_last_active, cap, gap_pairs) for check(); COUNT.audio
    # is replaced atomically (rename) and holds until the next Start or injection.
    cp -f "$COUNT.audio" "$OUT/$1.count_audio" 2> /dev/null || : > "$OUT/$1.count_audio"
}
play_once() { # NAME DAEMON AUDIO(0|1) PATH DURATION_MS START_MS: plays URL PATH to its end (12 s cap)
    local log=$OUT/$1.log
    rm -f "$OUT/port"
    N2_IDR=$CLIPS/idr.ts unshare -rn bash -c '
        ip link set lo up || { echo "n2_eof_e2e: ip link set lo up failed"; exit 9; }
        python3 "$1" "$2" "$3" "$4" "$N2_IDR" & sp=$!
        for _ in $(seq 1 300); do [ -s "$2" ] && break; sleep 0.1; done
        [ -s "$2" ] || { echo "n2_eof_e2e: the test server did not start in 30 s"; kill $sp; exit 8; }
        "$5" --conf "$6" --play-file "http://127.0.0.1:$(cat "$2")$7" --play-seconds 12 \
            --play-duration-ms "$8" --play-start-ms "$9"
        rc=$?
        kill $sp 2>/dev/null
        exit $rc' _ "$ROOT/tests/unit/n2_eof_server.py" "$OUT/port" "$CLIPS/full.ts" \
        "$CLIPS/tail.ts" "$2" "$OUT/audio$3.conf" "$4" "$5" "$6" > "$log" 2>&1 || true
    # c3: a daemon that started (it played, or refused as already running) logged its lock paths.
    if ran "$log" || grep -q 'misterplexd: already running' "$log"; then
        lp_expect "$log" 1 || { echo "test_n2_eof_e2e: FAIL (lock paths: $1, $log)"; exit 1; }
    fi
}
# check NAME [eof1 | eof0] [drained] [info=N] [eio | noeio] [presented] [fakeaudio | nofakeaudio]
#       [gap] [ahead | notahead] [anchors=V,A]
# FPGA_TERMINAL repeats its demux fields (before_quiesce, before_reset): tf takes the first.
tf() { { grep -o " $2=[^ }]*" <<< "$1" || true; } | head -n 1 | cut -d= -f2; }
check() {
    local name=$1 log=$OUT/$1.log term miss="" want info err sub pre av aa counters=""
    shift
    if ! ran "$log"; then
        echo "  NORUN $name: the daemon never started the play ($(grep -m1 -E 'already running|n2_eof_e2e:|unshare|Traceback' "$log" | cut -c1-120 || true))"
        fails=$((fails + 1))
        return 0
    fi
    term=$(grep -m1 '^FPGA_TERMINAL ' "$log" || true)
    info=$(grep -c "media: input ended by the server's close" "$log" || true)
    err=$(grep -c -E 'ERROR FPGA H.264: compressed (audio )?demux: Input/output error' "$log" || true)
    av=$(grep -c 'media: av anchor video: first ACK count=' "$log" || true)
    aa=$(grep -c 'media: av anchor audio: first write due mono_us=' "$log" || true)
    for want in "$@"; do
        case $want in
            eof1) [ "$(tf "$term" eof)" = 1 ] && [ "$(tf "$term" drain_attempted)" = 1 ] &&
                  [ "$(tf "$term" drain_ack)" = 1 ] || miss="$miss [not eof=1 with one acked drain]" ;;
            eof0) [ "$(tf "$term" eof)" = 0 ] || miss="$miss [not eof=0]" ;;
            drained) [ "$(tf "$term" fully_drained)" = 1 ] || miss="$miss [not fully_drained=1]" ;;
            info=*) [ "$info" = "${want#info=}" ] || miss="$miss [INFO lines $info, want ${want#info=}]" ;;
            eio) [ "$err" -ge 1 ] || miss="$miss [no EIO demux error]" ;;
            noeio) [ "$err" = 0 ] || miss="$miss [EIO demux error x$err]" ;;
            presented)
                sub=$(tf "$term" submitted_au)
                pre=$(tf "$term" presented_au)
                [ -n "$sub" ] && [ "$sub" = "$pre" ] && [ "$sub" -gt 50 ] ||
                    miss="$miss [presented $pre of $sub AUs]" ;;
            fakeaudio) grep -q 'audio DMA epoch/error/consumed counter does not match submitted PCM' "$log" ||
                           miss="$miss [no fake-MrAudio end line]" ;;
            nofakeaudio) ! grep -q 'audio DMA epoch/error/consumed counter does not match submitted PCM' "$log" ||
                           miss="$miss [the consumed-ahead stop]" ;;
            gap) # the consumer starved and left a gap (this play's gap_pairs)
                local gp
                gp=$(grep -oE 'gap_pairs=[0-9]+' "$OUT/$name.count_audio" 2>/dev/null | head -n 1 | cut -d= -f2)
                [[ "$gp" =~ ^[0-9]+$ ]] && [ "$gp" -gt 0 ] || miss="$miss [no consumer gap (gap_pairs=${gp:-none})]" ;;
            ahead | notahead) # this play's fake consumed (pairs) vs the PCM pairs fake /dev/MrAudio accepted
                local cs wr
                cs=$(grep -oE 'consumed_last_active=[0-9]+' "$OUT/$name.count_audio" 2>/dev/null | head -n 1 | cut -d= -f2)
                wr=$(grep -oE 'FAKE_MRAUDIO [a-z]+ written_bytes=[0-9]+ pairs=[0-9]+' "$log" | tail -n 1 |
                    grep -oE 'pairs=[0-9]+' | cut -d= -f2 || true)
                if ! [[ "$cs" =~ ^[0-9]+$ && "$wr" =~ ^[0-9]+$ ]]; then
                    miss="$miss [no consumed/written counters (consumed=${cs:-none} written=${wr:-none})]"
                elif [ "$want" = ahead ] && [ "$cs" -le "$wr" ]; then
                    miss="$miss [consumed $cs <= written $wr: not the consumed-ahead stop]"
                elif [ "$want" = notahead ] && [ "$cs" -gt "$wr" ]; then
                    miss="$miss [consumed $cs > written $wr]"
                fi
                counters=" consumed=$cs written=$wr" ;;
            anchors=*) # N1: video,audio anchor lines, once per play
                [ "$av,$aa" = "${want#anchors=}" ] || miss="$miss [anchor lines $av,$aa, want ${want#anchors=}]" ;;
            errtext=*) grep -qF -- "${want#errtext=}" "$log" || miss="$miss [missing: ${want#errtext=}]" ;;
            session=*) # input_session_ms in LO-HI
                local s lo hi
                s=$(tf "$term" input_session_ms)
                lo=${want#session=}
                hi=${lo#*-}
                lo=${lo%-*}
                [[ "$s" =~ ^[0-9]+$ ]] && [ "$s" -ge "$lo" ] && [ "$s" -le "$hi" ] ||
                    miss="$miss [input_session_ms ${s:-none}, want $lo-$hi]" ;;
        esac
    done
    local summary="" k
    for k in classification eof fully_drained drain_attempted drain_ack submitted_au presented_au \
        input_closed_at_end input_close_short_ms input_session_ms; do
        summary="$summary $k=$(tf "$term" "$k")"
    done
    if [ -z "$miss" ]; then
        echo "  PASS $name:$summary INFO=$info EIO=$err ANCHORS=$av,$aa$counters"
    else
        echo "  FAIL $name:$miss ($summary$counters )"
        grep -n -E 'ERROR|input ended|LAB play-file|n2_eof_e2e:|unshare|Traceback' "$log" | cut -c1-220 |
            sed 's/^/      | /' | head -6 || true
        fails=$((fails + 1))
    fi
}
fix_cases() { # DAEMON PREFIX [video-only]: the fixed daemon's cases; returns the failures
    local d=$1 p=$2 before=$fails
    play "$p"v-proxy "$d" 0 /proxy.ts 6500 0
    check "$p"v-proxy eof1 drained info=1 noeio presented anchors=1,0
    play "$p"v-resume "$d" 0 "/ladder.ts?src=x&offset_ms=3000" 6500 3000
    check "$p"v-resume eof1 drained info=1 noeio presented
    play "$p"v-length "$d" 0 /length.ts 6500 0
    check "$p"v-length eof1 drained info=0 noeio presented
    play "$p"v-lengthshort "$d" 0 /lengthshort.ts 6500 0
    check "$p"v-lengthshort eof0 info=0 eio
    # The daemon's own seek (--play-start-ms on a URL without a baked offset): session time
    # restarts at the seek target; 2164c4ad then stops the play on its direct-seek rule.
    play "$p"v-seekpin "$d" 0 /rangeable.ts 6500 3000
    check "$p"v-seekpin info=0 noeio "errtext=non-IDR direct seek requires FPGA preroll" session=3000-3100
    play "$p"v-seekproxy "$d" 0 /proxy.ts 6500 3000
    check "$p"v-seekproxy info=0 noeio "errtext=compressed seek: Operation not permitted"
    if [ "${3:-}" != video-only ]; then
        echo "== with audio (AUDIO=1)"
        play "$p"a-proxy "$d" 1 /proxy.ts 6500 0
        check "$p"a-proxy eof1 drained info=1 noeio presented notahead anchors=1,1
        # Content-Length with audio on the fixed build, the same checks as the control
        # base-a-length (review 22:41 (4): N-e needs this row on the fixed build).
        play "$p"a-length "$d" 1 /length.ts 6500 0
        check "$p"a-length eof1 drained info=0 noeio nofakeaudio presented notahead
        # The producer starves (3 s server stall): a gap, not a stop (review/release 22:59-23:01).
        play "$p"a-stall "$d" 1 /stall.ts 6500 0
        check "$p"a-stall eof1 drained info=0 noeio nofakeaudio presented gap notahead
        # Teeth: the old wall-clock consumer runs ahead in the same stall and stops the play.
        inject cap 0
        N2_EOF_AUDIO_TRIES=1 play "$p"a-stall-nocap "$d" 1 /stall.ts 6500 0
        inject cap 1
        check "$p"a-stall-nocap eof0 fakeaudio ahead
        play "$p"a-cut50 "$d" 1 /cut50.ts 6500 0
        check "$p"a-cut50 eof0 info=0 eio
        play "$p"a-unknown "$d" 1 /proxy.ts 0 0
        check "$p"a-unknown eof0 info=0 eio
    fi
    return $((fails - before))
}
echo "== video only (AUDIO=0)"
fix_cases "$OUT/misterplexd" "" || true
echo "== control: $BASE_REF (without the fix)"
B=$OUT/misterplexd-base
play base-a-proxy "$B" 1 /proxy.ts 6500 0
check base-a-proxy eof0 info=0 eio
play base-a-length "$B" 1 /length.ts 6500 0
check base-a-length eof1 drained info=0 noeio nofakeaudio presented notahead
play base-v-proxy "$B" 0 /proxy.ts 6500 0
check base-v-proxy eof0 info=0 eio
play base-v-seekpin "$B" 0 /rangeable.ts 6500 3000
check base-v-seekpin info=0 noeio "errtext=non-IDR direct seek requires FPGA preroll"
play base-v-seekproxy "$B" 0 /proxy.ts 6500 3000
check base-v-seekproxy info=0 noeio "errtext=compressed seek: Operation not permitted"
if [ "$fails" != 0 ]; then echo "test_n2_eof_e2e: FAIL ($fails case(s)); logs in $OUT"; exit 1; fi
echo "test_n2_eof_e2e: OK"

# N2_EOF_E2E_MUTANTS=1: the daemon rebuilt with each mutation must fail a video case.
[ "${N2_EOF_E2E_MUTANTS:-0}" = 1 ] || exit 0
mutant() { # NAME FILE OLD NEW
    local m=$OUT/mut-$1 got
    rm -rf "$m" && mkdir -p "$m"
    cp -r "$ROOT/arm" "$ROOT/host" "$ROOT/assets" "$m/"
    python3 - "$m/$2" "$3" "$4" <<'PY' || return 1
import sys
p, old, new = sys.argv[1:4]
t = open(p).read()
assert t.count(old) == 1, "mutant site not found once: " + old
open(p, "w").write(t.replace(old, new))
PY
    build_daemon "$m" "$m/misterplexd" > "$m/build.log" 2>&1 || { echo "  mutant $1: BUILD FAILED"; return 1; }
    got=0
    fix_cases "$m/misterplexd" "mut-$1-" video-only > "$m/cases.log" 2>&1 || got=$?
    if grep -q '  NORUN ' "$m/cases.log"; then
        echo "  mutant $1: INCONCLUSIVE (a play never ran: $(grep -o 'NORUN mut-[a-z0-9-]*' "$m/cases.log" | sed "s/NORUN mut-$1-//" | tr '\n' ' '))"
        return 1
    fi
    if [ "$got" -gt 0 ]; then
        echo "  mutant $1: caught ($(grep -o 'FAIL mut-[a-z0-9-]*' "$m/cases.log" | sed "s/FAIL mut-$1-//" | tr '\n' ' '))"
        return 0
    fi
    echo "  mutant $1: NOT CAUGHT"
    return 1
}
fails=0
mm=0
mutant window0 host/libmisterplex/av_inproc_decode.hpp \
    "constexpr int64_t kInputCloseEndWindowMs = 2000;" "constexpr int64_t kInputCloseEndWindowMs = 0;" || mm=$((mm + 1))
mutant no-f1 arm/misterplexd/av_inproc_decode.cpp \
    "const bool lengthUnknown = fmt && fmt->pb && avio_size(fmt->pb) < 0;" "const bool lengthUnknown = true;" || mm=$((mm + 1))
mutant no-startms arm/misterplexd/media_player.cpp \
    "opts.sessionStartMs = std::max<int64_t>(0, startMs);" "opts.sessionStartMs = 0;" || mm=$((mm + 1))
mutant no-seekpin arm/misterplexd/av_inproc_decode.cpp \
    "        for (int slot = 0; slot < 2; ++slot) {
            impl_->spanFirstUs[slot] = seekTargetUs;
            impl_->spanEndUs[slot] = AV_NOPTS_VALUE;
        }
        impl_->spanPinned = true;
" "" || mm=$((mm + 1))
if [ "$mm" != 0 ]; then echo "test_n2_eof_e2e: MUTANTS FAIL ($mm not caught)"; exit 1; fi
echo "test_n2_eof_e2e: MUTANTS OK (4/4 caught)"
