#!/usr/bin/env bash
# R-m m8 and m10 (review 08:17 (2)) get their own mutants (review 11:36 R2). Each mutant edits a
# SCRATCH copy of host/libmisterplex (sed on one line of r1_plxh.hpp's PlayRequestHold destructor,
# anchored to exactly one line), never the tree, so the daemon and its ARM do not change.
# test_r1_idle_entry is built against each copy (the copy is the only host include path), and the
# named case runs alone (R1_HOLD_CASE):
#   M8_EXIT_DISOWNS       a doPlay that ends without a session only disowns its hold (no RAII
#                         release): R1_HOLD_CASE=m8 (a throw inside doPlay) must FAIL
#   M10_STARTED_RELEASES  a doPlay that started its session releases at exit instead of disowning:
#                         R1_HOLD_CASE=m10 (held until the session's enter) must FAIL
# The unmutated copy must PASS both cases. Output follows h264-add7-mutants.sh: SRC_MD5, an
# N1 before/after line per mutant (exit 3 = INVALID, an anchor not where it must be), CHANGED_LINES
# and TEMP_MD5; exit 1 = a mutant survived or the base failed.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${R1_HOLD_MUT_OUT:-$ROOT/build/r1_hold_mutants}
CXX=${CXX:-g++}
HDR=host/libmisterplex/r1_plxh.hpp
ENDED='            policy_.playRequestEnded(gen_);'
DONE='            policy_.playRequestDone(gen_);'
fail() { echo "FAIL $*"; exit 1; }
invalid() { echo "N1 $*: INVALID"; exit 3; }   # an anchor that is not exactly where expected (h264-add7 rule)
echo "SRC_MD5 $(md5sum < "$ROOT/$HDR" | cut -c1-32)"
for l in "$ENDED" "$DONE"; do
    [ "$(grep -cxF "$l" "$ROOT/$HDR")" = 1 ] || invalid "anchor not exactly once in $HDR: '$l'"
done
rm -rf "$OUT"
mkdir -p "$OUT"
build() { # name sed-expression|"" [the line it removes] [the line it adds]
    local d=$OUT/$1
    mkdir -p "$d/host"
    cp -r "$ROOT/host/libmisterplex" "$d/host/"
    if [ -n "$2" ]; then
        sed -i "$2" "$d/$HDR"
        local gone added
        gone=$(grep -cxF "$3" "$d/$HDR")
        added=$(grep -cxF "$4" "$d/$HDR")
        [ "$gone" = 0 ] && [ "$added" = 2 ] || invalid "$1 after: '$3' x$gone (want 0), '$4' x$added (want 2)"
        echo "N1 $1 before=1 after=1"
        echo "CHANGED_LINES $1 $(diff "$ROOT/$HDR" "$d/$HDR" | grep -c '^>' || true)"
    fi
    echo "TEMP_MD5 $1 $(md5sum < "$d/$HDR" | cut -c1-32)"
    "$CXX" -std=c++17 -O2 -Wall -Wextra -I"$d/host" -I"$ROOT/arm/misterplexd" -pthread \
        -o "$d/test_r1_idle_entry" "$ROOT/tests/unit/test_r1_idle_entry.cpp" -lz > "$d/build.log" 2>&1 ||
        { cat "$d/build.log"; fail "$1: build"; }
}
run() { # name case want(pass|fail)
    local d=$OUT/$1 rc=0
    R1_HOLD_CASE=$2 "$d/test_r1_idle_entry" > "$d/$2.log" 2>&1 || rc=$?
    echo "  $1 R1_HOLD_CASE=$2: rc=$rc $(tail -n 1 "$d/$2.log")"
    case $3 in
        pass) [ "$rc" = 0 ] || fail "$1: case $2 must PASS" ;;
        fail) [ "$rc" != 0 ] || fail "$1: case $2 must FAIL (mutant survived)" ;;
    esac
}
echo "test_r1_hold_mutants: scratch copies of host/libmisterplex under $OUT"
build base ""
build M8_EXIT_DISOWNS "s/^${ENDED//./\\.}\$/${DONE}/" "$ENDED" "$DONE"
build M10_STARTED_RELEASES "s/^${DONE//./\\.}\$/${ENDED}/" "$DONE" "$ENDED"
run base m8 pass
run base m10 pass
run M8_EXIT_DISOWNS m8 fail
run M10_STARTED_RELEASES m10 fail
echo "test_r1_hold_mutants: base passes m8 and m10; M8_EXIT_DISOWNS fails m8; M10_STARTED_RELEASES fails m10"
