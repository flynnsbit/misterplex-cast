#!/usr/bin/env bash
# SPS-gate layer 3, T1f + bar b1: drives tests/unit/test_resolve_gate_fallback.cpp against the fake PMS, one case per
# run, and counts the fake PMS requests each case makes. Then the resolve mutants, each KILLED only by rc 1 and its
# pinned first FAIL line:
#   M9   forceLadder not ORed into the universal-skip branch (plex_resolve.cpp :2175 terms unchanged) -> C1 RED
#   M9b  forceLadder not in wantProxy: the branch is entered but the result is a direct Part (the b1 gap) -> C1 RED
#   M9c  forceLadder without a helper still skips PMS (helperOn not required) -> C3 RED (direct Part or refuse)
# usage: test_resolve_gate_fallback.sh <repo root> <out dir>. Loopback only; run it in the record's private net ns.
set -euo pipefail
ROOT=$(cd "${1:?usage: test_resolve_gate_fallback.sh <root> <out>}" && pwd)
OUT=${2:?out dir}
mkdir -p "$OUT"
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -I$ROOT/host}
# bar 2: one fresh pycache prefix before any python call.
if [ -z "${PYTHONPYCACHEPREFIX:-}" ]; then
    PYTHONPYCACHEPREFIX=$(mktemp -d "$OUT/pycache-prefix.XXXX")
    export PYTHONPYCACHEPREFIX
fi
T=$(mktemp -d)
PMS_PID=
cleanup() {
    if [ -n "$PMS_PID" ]; then
        kill "$PMS_PID" 2>/dev/null || true
        wait "$PMS_PID" 2>/dev/null || true
    fi
    rm -rf "$T"
}
trap cleanup EXIT

build() {   # build <name> <plex_resolve.cpp>
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS -I"$ROOT/arm/misterplexd" -o "$T/$1" "$ROOT/tests/unit/test_resolve_gate_fallback.cpp" "$2" \
        > "$OUT/build-$1.log" 2>&1
}
build base "$ROOT/arm/misterplexd/plex_resolve.cpp"

python3 "$ROOT/tests/fixtures/fake_pms/fake_pms.py" --port-file "$T/port.json" --log "$OUT/fake_pms.log" \
    > "$OUT/fake_pms.stdout" 2>&1 &
PMS_PID=$!
for _ in $(seq 1 100); do [ -s "$T/port.json" ] && break; sleep 0.1; done
[ -s "$T/port.json" ] || { echo "FAIL fake PMS never wrote its port file"; exit 1; }
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port4"])' "$T/port.json")
BASE="http://127.0.0.1:$PORT"
HELPER="http://127.0.0.1:9"   # never contacted by resolve: the ladder URL is built locally
lines() { wc -l < "$OUT/fake_pms.log" 2>/dev/null || echo 0; }

rc_all=0
run_case() {   # run_case <bin> <case> -> prints the case line + 'requests=N'
    local before after rc=0
    before=$(lines)
    "$T/$1" --base "$BASE" --helper "$HELPER" --case "$2" > "$OUT/$1-$2.out" 2> "$OUT/$1-$2.err" || rc=$?
    sleep 0.2
    after=$(lines)
    echo "$1 $2 rc=$rc requests=$((after - before)) $(grep -m1 '^CASE ' "$OUT/$1-$2.out" | sed 's/tok/REDACTED/g')"
    return $rc
}
for c in C0 C1 C2 C3 C4; do
    line=$(run_case base "$c") || rc_all=1
    echo "$line"
    case "$c" in
        C3) [[ "$line" == *" requests=0 "* ]] && { echo "FAIL C3: noproxy forceLadder made 0 PMS requests"; rc_all=1; } ;;
        C0) grep -q 'transcode/universal/decision' "$OUT/fake_pms.log" ||
                { echo "FAIL C0: no universal decision request"; rc_all=1; } ;;
    esac
done

mut() {   # mut <name> <sed expr> <case> <pinned first FAIL>
    local name=$1 expr=$2 c=$3 want=$4 rc=0 first changed
    sed "$expr" "$ROOT/arm/misterplexd/plex_resolve.cpp" > "$T/$name.cpp"
    changed=$(diff "$ROOT/arm/misterplexd/plex_resolve.cpp" "$T/$name.cpp" | grep -c '^[0-9]' || true)
    if [ "$changed" != 1 ]; then echo "FAIL mutant $name: $changed diff hunk(s), want 1"; rc_all=1; return; fi
    build "$name" "$T/$name.cpp"
    "$T/$name" --base "$BASE" --helper "$HELPER" --case "$c" > "$OUT/$name-$c.out" 2> "$OUT/$name-$c.err" || rc=$?
    first=$(grep -m1 '^FAIL ' "$OUT/$name-$c.err" || true)
    if [ "$rc" = 1 ] && [ "$first" = "$want" ]; then
        echo "  mutant $name: KILLED by its pinned check: $first"
    else
        echo "FAIL mutant $name: rc=$rc first='$first' (pinned '$want')"
        rc_all=1
    fi
}
mut M9-no-or-term 's/if ((forceLadder \&\& helperOn) || (remoteBase \&\& idrLadder) ||/if ((remoteBase \&\& idrLadder) ||/' C1 \
    'FAIL resolve gate fallback C1: forceLadder resolves to the ladder helper'
mut M9b-not-in-wantProxy 's/^                (forceLadder ||   \/\/ SPS-gate L3 b1/                (false ||   \/\/ SPS-gate L3 b1/' C1 \
    'FAIL resolve gate fallback C1: forceLadder resolves to the ladder helper'
mut M9c-forceLadder-skip-pms 's/if ((forceLadder \&\& helperOn) || (remoteBase \&\& idrLadder) ||/if (forceLadder || (remoteBase \&\& idrLadder) ||/' C3 \
    'FAIL resolve gate fallback C3: no helper: PMS universal, never a direct Part'
if [ "$rc_all" = 0 ]; then
    echo "test_resolve_gate_fallback: OK (C0-C4, C3 PMS universal noproxy, M9 M9b M9c KILLED)"
else
    echo "test_resolve_gate_fallback: FAILED"
fi
exit $rc_all
