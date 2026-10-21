#!/usr/bin/env bash
# N2 end of stream through the daemon's own demux (av_inproc_decode.cpp, host libav) against
# n2_eof_server.py: the ladder proxy's response shape ends in FFmpeg http EIO; the demux takes
# it as the end only near the declared (PMS) duration (review F1-F5,
# review-rel-batch-1428-1004.md section 4). Cases, each through both readers (the audio
# pump on its own thread, as fpgaAudioPump) unless noted:
#   proxy        proxy shape, declared = clip + 500 ms   -> EOF, both readers, 1 conversion
#   unknown      proxy shape, no declared duration       -> EIO error, 0 conversions (F3)
#   cut50        proxy shape closed at half the clip     -> EIO error (mid-stream)
#   length       Content-Length                          -> EOF, 0 conversions (unchanged)
#   lengthshort  Content-Length 64 KiB long, then close  -> EIO error (F1: known size)
#   chunked      chunked (libavformat 63 ends it in EIO) -> EOF, 1 conversion
#   edge         declared = measured end + 2000 ms       -> EOF (the window is inclusive)
#   pastedge     declared = measured end + 2001 ms       -> EIO error
#   resume       /ladder.ts?offset_ms=3000 (the clip from 3 s), start 3000 -> EOF (F2)
#   videoonly    proxy shape, no audio reader            -> EOF through the AU reader
#   seekpin      the demux's own seek (opts.startMs 3000) on a byte-Range Content-Length
#                server (all-IDR clip): session time restarts at the seek target
#                (session_ms 3000..3100); the play then stops on 2164c4ad's own direct-seek
#                rule ("non-IDR direct seek requires FPGA preroll"), unchanged
#   seekproxy    the demux's own seek on the proxy shape: open fails ("compressed seek"),
#                unchanged; a proxied resume only ever runs through the URL offset
# N2_EOF_MUTANTS=1 then rebuilds the demux four times, mutated (the window -> 0, F1 removed,
# the start offset dropped, the seek-target restart dropped), and each must fail a case.
# Needs host libav (pkg-config) and ffmpeg with libx264/aac; skips without them.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${N2_EOF_OUT:-$ROOT/build/n2_eof}
CXX=${CXX:-g++}
LIBAV=$(pkg-config --cflags --libs libavformat libavcodec libswresample libavutil 2>/dev/null || true)
if [ -z "$LIBAV" ] || ! command -v ffmpeg > /dev/null; then
    echo "test_input_close_eof_demux: SKIP (needs host libav and ffmpeg)"
    exit 0
fi
mkdir -p "$OUT"
SPID=
cleanup() { [ -n "$SPID" ] && kill "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null; }
trap cleanup EXIT

build() { # SRCDIR OUTBIN: the driver against SRCDIR's demux sources
    "$CXX" -std=c++17 -O2 -Wall -Wextra -I"$1/host" -I"$1/arm/misterplexd" -I"$ROOT/host" \
        -I"$ROOT/arm/misterplexd" -pthread -DMPX_HAVE_LIBAV -o "$2" \
        "$ROOT/tests/unit/input_close_eof_demux.cpp" "$1/arm/misterplexd/av_inproc_decode.cpp" $LIBAV
}
build "$ROOT" "$OUT/input_close_eof_demux" || { echo "test_input_close_eof_demux: FAIL (build)"; exit 1; }

# 6 s of 23.976 baseline CAVLC (as the lab ladder serves) + 48 kHz stereo AAC, and the same
# content from 3 s, re-encoded so its timestamps restart (the ladder's ffmpeg -ss).
X264=(-c:v libx264 -profile:v baseline -level 3.0 -x264-params ref=1:bframes=0:keyint=24 -pix_fmt yuv420p)
if [ ! -s "$OUT/full.ts" ] || [ ! -s "$OUT/tail.ts" ] || [ ! -s "$OUT/idr.ts" ]; then
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=24000/1001 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 "${X264[@]}" \
        -c:a aac -b:a 96k -ar 48000 -ac 2 -shortest -f mpegts "$OUT/full.ts" &&
    ffmpeg -nostdin -loglevel error -y -ss 3 -i "$OUT/full.ts" "${X264[@]}" \
        -c:a aac -b:a 96k -ar 48000 -ac 2 -f mpegts "$OUT/tail.ts" &&
    ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=size=320x240:rate=24000/1001 \
        -f lavfi -i sine=frequency=440:sample_rate=48000 -t 6 \
        -c:v libx264 -profile:v baseline -level 3.0 -x264-params ref=1:bframes=0:keyint=1 \
        -pix_fmt yuv420p -c:a aac -b:a 96k -ar 48000 -ac 2 -shortest -f mpegts "$OUT/idr.ts" ||
        { echo "test_input_close_eof_demux: FAIL (clip)"; exit 1; }
fi
rm -f "$OUT/port"
python3 "$ROOT/tests/unit/n2_eof_server.py" "$OUT/port" "$OUT/full.ts" "$OUT/tail.ts" "$OUT/idr.ts" &
SPID=$!
for _ in $(seq 1 50); do [ -s "$OUT/port" ] && break; sleep 0.1; done
[ -s "$OUT/port" ] || { echo "test_input_close_eof_demux: FAIL (server)"; exit 1; }
URL=http://127.0.0.1:$(cat "$OUT/port")

run_cases() { # DRIVER: prints PASS/FAIL per case, returns the failure count
    local drv=$1 fails=0 out reached
    case_() { # NAME PATH EXPECTED START AUDIO WANT_RESULT WANT_AUDIO WANT_CONVERSIONS [ERRTEXT [DEMUX_START]]
        local name=$1 got miss=""
        got=$("$drv" "$URL$2" "$3" "$4" "$5" ${10:+"${10}"})
        [[ "$got" == *"result=$6 "* ]] || miss="$miss result!=$6"
        [ "$7" = - ] || [[ "$got" == *"audio=$7 "* ]] || miss="$miss audio!=$7"
        [[ "$got" == *"conversions=$8 "* ]] || miss="$miss conversions!=$8"
        if [ -n "${9:-}" ] && [[ "$got" != *"$9"* ]]; then miss="$miss missing[$9]"; fi
        if [ -z "$miss" ]; then echo "  PASS $name: $got"; else echo "  FAIL $name:$miss: $got"; fails=$((fails + 1)); fi
        LAST=$got
    }
    case_ proxy /proxy.ts 6500 0 1 eof eof 1
    # session_ms is printed with or without a conversion, so a mutant cannot move the edge cases.
    reached=$(sed -n 's/.* session_ms=\([0-9][0-9]*\) .*/\1/p' <<< "$LAST")
    if [ -z "$reached" ]; then echo "  FAIL proxy: no session_ms"; fails=$((fails + 1)); reached=0; fi
    case_ unknown /proxy.ts 0 0 1 error - 0 "Input/output error"
    case_ cut50 /cut50.ts 6500 0 1 error - 0 "Input/output error"
    case_ length /length.ts 6500 0 1 eof eof 0
    case_ lengthshort /lengthshort.ts 6500 0 1 error - 0 "Input/output error"
    case_ chunked /chunked.ts 6500 0 1 eof eof 1
    case_ edge /proxy.ts $((reached + 2000)) 0 1 eof eof 1
    case_ pastedge /proxy.ts $((reached + 2001)) 0 1 error - 0 "Input/output error"
    case_ resume "/ladder.ts?src=x&offset_ms=3000" 6500 3000 1 eof eof 1
    case_ videoonly /proxy.ts 6500 0 0 eof none 1
    case_ seekpin /rangeable.ts 6500 3000 1 error - 0 "non-IDR direct seek requires FPGA preroll" 3000
    local sm
    sm=$(sed -n 's/.* session_ms=\([0-9][0-9]*\) .*/\1/p' <<< "$LAST")
    if [ -n "$sm" ] && [ "$sm" -ge 3000 ] && [ "$sm" -le 3100 ]; then
        echo "  PASS seekpin session: $sm ms (the seek target is 3000)"
    else
        echo "  FAIL seekpin session: ${sm:-none} ms, want 3000..3100"
        fails=$((fails + 1))
    fi
    case_ seekproxy /proxy.ts 6500 3000 1 open-failed - 0 "compressed seek" 3000
    echo "  (measured end of the proxy clip: $reached ms of session time)"
    return "$fails"
}
# Error cases pass "-" for the audio reader: either reader may hit the EIO first.
f=0
run_cases "$OUT/input_close_eof_demux" || f=$?
if [ "$f" != 0 ]; then echo "test_input_close_eof_demux: FAIL ($f case(s))"; exit 1; fi
echo "test_input_close_eof_demux: OK"

[ "${N2_EOF_MUTANTS:-0}" = 1 ] || exit 0
mutant() { # NAME FILE OLD NEW: a copy of the demux sources with one replacement
    local d=$OUT/mut-$1
    rm -rf "$d" && mkdir -p "$d/host" "$d/arm/misterplexd"
    cp -r "$ROOT/host/libmisterplex" "$d/host/"
    cp "$ROOT/arm/misterplexd/av_inproc_decode.cpp" "$d/arm/misterplexd/"
    python3 - "$d/$2" "$3" "$4" <<'EOF' || return 1
import sys
p, old, new = sys.argv[1:4]
t = open(p).read()
assert t.count(old) == 1, "mutant site not found once: " + old
open(p, "w").write(t.replace(old, new))
EOF
    build "$d" "$d/input_close_eof_demux" > "$d/build.log" 2>&1 || { echo "  mutant $1: BUILD FAILED"; return 1; }
    local mf=0
    run_cases "$d/input_close_eof_demux" > "$d/cases.log" 2>&1 || mf=$?
    if [ "$mf" -gt 0 ]; then
        echo "  mutant $1: caught ($mf case(s) fail: $(grep -o 'FAIL [a-z0-9]*' "$d/cases.log" | cut -d' ' -f2 | tr '\n' ' '))"
        return 0
    fi
    echo "  mutant $1: NOT CAUGHT"
    return 1
}
m=0
mutant window0 host/libmisterplex/av_inproc_decode.hpp \
    "constexpr int64_t kInputCloseEndWindowMs = 2000;" "constexpr int64_t kInputCloseEndWindowMs = 0;" || m=$((m + 1))
mutant no-f1 arm/misterplexd/av_inproc_decode.cpp \
    "const bool lengthUnknown = fmt && fmt->pb && avio_size(fmt->pb) < 0;" "const bool lengthUnknown = true;" || m=$((m + 1))
mutant no-startms arm/misterplexd/av_inproc_decode.cpp \
    "options.expectedDurationMs, options.sessionStartMs, spanMs))" "options.expectedDurationMs, 0, spanMs))" || m=$((m + 1))
mutant no-seekpin arm/misterplexd/av_inproc_decode.cpp \
    "        for (int slot = 0; slot < 2; ++slot) {
            impl_->spanFirstUs[slot] = seekTargetUs;
            impl_->spanEndUs[slot] = AV_NOPTS_VALUE;
        }
        impl_->spanPinned = true;
" "" || m=$((m + 1))
if [ "$m" != 0 ]; then echo "test_input_close_eof_demux: MUTANTS FAIL ($m not caught)"; exit 1; fi
echo "test_input_close_eof_demux: MUTANTS OK (4/4 caught)"
