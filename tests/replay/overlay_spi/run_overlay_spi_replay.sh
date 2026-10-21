#!/usr/bin/env bash
# Flags-off SPI-sequence replay of the native-overlay publisher: base ref vs candidate.
#
#   tests/replay/overlay_spi/run_overlay_spi_replay.sh [base-ref] [candidate-ref] [outdir]
#   (defaults: dbcc42fa HEAD build/overlay_spi_replay)
#
# For each ref: extract its media_player.cpp publisher regions (gen_harness.py), build the
# lock-step replay, run the same session scripts with every overlay flag unset (twice:
# the record must be deterministic), and diff base against candidate. The residual diff
# (residual.diff) lists every media_player.cpp difference outside the replayed regions.
# Exit 0 only when every flags-off record is identical, and every invalid lab flag value
# ("", "yes", "2", " 1", ...) fails closed: the base publisher and the base record, apart
# from the candidate's own "invalid ... -> off" notice lines. Lab-flag runs are informational.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(git -C "$here" rev-parse --show-toplevel)
base=${1:-dbcc42fa}
cand=${2:-HEAD}
out=${3:-$root/build/overlay_spi_replay}
CXX=${CXX:-g++}
mkdir -p "$out"

build_ref() {
    local ref=$1 tag=$2 src=$out/src-$2
    rm -rf "$src"
    mkdir -p "$src"
    git -C "$root" archive "$ref" host/libmisterplex | tar -x -C "$src"
    git -C "$root" show "$ref:arm/misterplexd/media_player.cpp" > "$src/media_player.cpp"
    python3 "$here/gen_harness.py" "$src/media_player.cpp" "$here/harness_template.cpp" \
        "$out/replay-$tag.cpp" "$(git -C "$root" rev-parse --short=8 "$ref")" | tee "$out/gen-$tag.txt"
    "$CXX" -std=c++17 -O1 -pthread -w -I"$src/host" -o "$out/replay-$tag" "$out/replay-$tag.cpp"
}

build_ref "$base" base
build_ref "$cand" cand
if python3 -c 'import sys; sys.exit(0 if open(sys.argv[1]).read() in open(sys.argv[2]).read() else 1)' \
    "$out/src-base/media_player.cpp.block" "$out/src-cand/media_player.cpp.block"; then
    echo "block: the base publisher block is in the candidate byte-for-byte"
else
    echo "block: the candidate's publisher block is not the base's"
fi
if diff -u "$out/src-base/media_player.cpp.residual" "$out/src-cand/media_player.cpp.residual" \
    > "$out/residual.diff"; then
    echo "residual: media_player.cpp outside the replayed regions is identical"
else
    echo "residual: $(grep -c '^[-+][^-+]' "$out/residual.diff" || true) changed lines outside the replayed regions (residual.diff)"
fi

unset MPX_OVERLAY_DECOUPLE MPX_OVERLAY_ONCHANGE MPX_OVERLAY_KEEPALIVE_MS MPX_OVERLAY_SKIP_NO_PLANE MPX_SPI_DIAG
fail=0
for sc in session eof failures exception; do
    for tag in base cand; do
        "$out/replay-$tag" "$sc" > "$out/$tag-$sc.log" 2> "$out/$tag-$sc.err"
        "$out/replay-$tag" "$sc" > "$out/$tag-$sc.run2.log" 2> /dev/null
        if ! cmp -s "$out/$tag-$sc.log" "$out/$tag-$sc.run2.log"; then
            echo "NONDETERMINISTIC $tag $sc"
            fail=1
        fi
    done
    sends=$(grep -c ' SPI sendFileTx ' "$out/base-$sc.log" || true)
    pub=$(grep -h 'publisher=' "$out/cand-$sc.err" | sed 's/.*publisher=//' | sort -u | tr '\n' ' ')
    if cmp -s "$out/base-$sc.log" "$out/cand-$sc.log"; then
        echo "IDENTICAL $sc: $(wc -l < "$out/base-$sc.log") records, $sends overlay SPI sends; candidate ran: $pub"
    else
        diff -u "$out/base-$sc.log" "$out/cand-$sc.log" > "$out/diff-$sc.txt" || true
        echo "DIFF $sc: $(grep -c '^[-+][^-+]' "$out/diff-$sc.txt" || true) lines differ (diff-$sc.txt); candidate ran: $pub"
        fail=1
    fi
done

# Invalid flag values fail closed (rel bug-nits 1004): with any one lab variable set to
# a value other than exactly "1", the candidate must run the same publisher as the base and
# record the base's flags-off record, apart from its own "<VAR>='<value>' invalid (want ...)"
# notice lines (counted). "" means unset: no notice.
echo "invalid flag values on the candidate (fail closed: the base publisher and record):"
inv_n=0
inv_bad=0
for var in MPX_OVERLAY_DECOUPLE MPX_OVERLAY_ONCHANGE MPX_OVERLAY_SKIP_NO_PLANE MPX_OVERLAY_KEEPALIVE_MS; do
    for val in "" "yes" "2" " 1" "1 " "true" "01"; do
        vtag=$(printf '%s' "$val" | od -An -tx1 | tr -d ' \n')
        vtag=${vtag:-empty}
        notes=0
        bad=""
        for sc in session eof failures exception; do
            log=$out/cand-inv-$var-$vtag-$sc.log
            rc=0
            env "$var=$val" "$out/replay-cand" "$sc" > "$log" 2> "$log.err" || rc=$?
            bpub=$( (grep -h 'publisher=' "$out/base-$sc.err" || true) | sed 's/.*publisher=//' | sort -u | tr '\n' ' ')
            pub=$( (grep -h 'publisher=' "$log.err" || true) | sed 's/.*publisher=//' | sort -u | tr '\n' ' ')
            (grep -v -E "^t=[0-9]+ LOG media: LAB $var='.*' invalid \\(want " "$log" || true) > "$log.strip"
            notes=$((notes + $(wc -l < "$log") - $(wc -l < "$log.strip")))
            if [ "$rc" != 0 ]; then
                bad="$bad $sc:rc=$rc"
            elif [ "$pub" != "$bpub" ]; then
                bad="$bad $sc:publisher=${pub% }(base ${bpub% })"
            elif ! cmp -s "$out/base-$sc.log" "$log.strip"; then
                bad="$bad $sc:$( (diff "$out/base-$sc.log" "$log.strip" || true) | grep -c '^[<>]' || true) lines differ"
            fi
        done
        inv_n=$((inv_n + 1))
        if [ -z "$bad" ]; then
            echo "  IDENTICAL $var='$val': base publisher, base record in all 4 scenarios; $notes notice line(s)"
        else
            echo "  DIFF $var='$val':$bad"
            inv_bad=$((inv_bad + 1))
            fail=1
        fi
    done
done
echo "invalid values: $((inv_n - inv_bad))/$inv_n fail closed"

echo "lab flags on the candidate (informational, session scenario):"
for build in 48005D45 4801F40C; do
    for flags in "MPX_OVERLAY_SKIP_NO_PLANE=1" "MPX_OVERLAY_DECOUPLE=1" "MPX_OVERLAY_ONCHANGE=1"; do
        log=$out/cand-lab-${build}-${flags%%=*}.log
        env "$flags" "$out/replay-cand" session "$build" > "$log" 2> /dev/null || true
        n=$(grep -c ' SPI sendFileTx ' "$log" || true)
        locked=$(grep ' SPI sendFileTx ' "$log" | grep -c 'thr=publisher presentMu=publisher' || true)
        echo "  0x$build $flags: $n overlay SPI sends ($locked by the publisher under presentMu_)"
    done
done
echo "OVERLAY_SPI_REPLAY $([ "$fail" = 0 ] && echo PASS || echo FAIL) base=$base cand=$cand out=$out"
exit "$fail"
