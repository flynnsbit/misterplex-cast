#!/usr/bin/env bash
# Mutants for host/libmisterplex/gate_owner.hpp (SPS-gate layer 3, L3 paper v1 c1f55425; review 06:17): the
# decision-level mutants M1, M5, M6, M7, M8 of pre-reg v1.4.3 section 8, plus Mflip (Q1: L3 must not do the mid-item
# flip), Mq2 (Q2: no helper must not fall back), Mq3 (Q3: MALFORMED must not hard-error at AU0), Mstr (the shared
# text) and Mhs (a helper-stream first play must not fall back to the same ladder). M10 and M14 stay with gate_caps.hpp
# (test_gate_caps_mutants.sh). Each mutated header copy is compiled with the
# base's CXXFLAGS and run against tests/unit/test_gate_owner.cpp. Grammar (as test_gate_caps_mutants.sh):
#   KILLED        rc 1 and the FIRST "FAIL gate owner:" line equals the mutant's pinned line
#   KILLED-OTHER  rc 1 but another first FAIL line: a FINDING, the run fails
#   SURVIVED      rc 0: the run fails
#   INVALID       rc >= 128 (signal/crash), a timeout (rc 124), or any other rc: the run fails
# An edit must make exactly ONE diff hunk in the header (0 = vacuous, more = ambiguous): otherwise the run fails.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
H=$ROOT/host/libmisterplex/gate_owner.hpp
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -I$ROOT/host}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
n=0
mut() {
    local name=$1 expr=$2 want=$3 rc=0 first changed
    mkdir -p "$T/$name/libmisterplex"
    sed "$expr" "$H" > "$T/$name/libmisterplex/gate_owner.hpp"
    changed=$(diff "$H" "$T/$name/libmisterplex/gate_owner.hpp" | grep -c '^[0-9]' || true)
    if [ "$changed" != 1 ]; then
        echo "FAIL mutant $name: the edit made $changed diff hunk(s) in the header, want exactly 1" >&2
        exit 1
    fi
    # The mutant dir comes first, so its gate_owner.hpp wins over host/; gate_caps.hpp still comes from host/.
    # shellcheck disable=SC2086
    "$CXX" -I"$T/$name" $CXXFLAGS -o "$T/$name/t" "$ROOT/tests/unit/test_gate_owner.cpp"
    timeout 60 "$T/$name/t" > "$T/$name/out" 2> "$T/$name/err" || rc=$?
    first=$(grep -m1 '^FAIL gate owner:' "$T/$name/err" || true)
    if [ "$rc" = 0 ]; then
        echo "FAIL mutant $name: SURVIVED (rc 0)" >&2
        exit 1
    elif [ "$rc" != 1 ]; then
        echo "FAIL mutant $name: INVALID (rc $rc)" >&2
        exit 1
    elif [ "$first" != "$want" ]; then
        echo "FAIL mutant $name: KILLED-OTHER (FINDING): first '$first', pinned '$want'" >&2
        exit 1
    fi
    echo "  mutant $name: KILLED by its pinned check: $first"
    n=$((n + 1))
}
mut M1-gate-bypassed 's/^    if (s == StreamOutcome::In)$/    if (true)/' \
    'FAIL gate owner: R6 first play + helper, OUT: want R6/fallback, got R5/feed'
mut M5-second-fallback 's/return {GateAction::Error, GateRow::R8};/return {GateAction::Fallback, GateRow::R6};/' \
    'FAIL gate owner: R8 fallback play + helper, OUT: want R8/error, got R6/fallback'
mut M6-helper-failure-retries 's/return {GateAction::Error, GateRow::R10h};/return {GateAction::Fallback, GateRow::R10h};/' \
    'FAIL gate owner: R10h fallback play + helper: want R10h/error, got R10h/fallback'
mut M7-caps-fallback-ignored 's/if (caps == CapsOutcome::FallbackOnFloor) {/if (false) {/' \
    'FAIL gate owner: R2 first play + helper, CAPS FallbackOnFloor: want R2/fallback, got R4/gate-on-caps'
mut M8-helper-on-raw-word 's/return {GateAction::GateOnFloor, GateRow::R3};/return {GateAction::GateOnCaps, GateRow::R3};/' \
    'FAIL gate owner: R3 fallback play + helper, CAPS FallbackOnFloor: want R3/gate-on-floor, got R3/gate-on-caps'
mut Mflip-l3-flips 's/^    if (auWritten > 0)$/    if (false)/' \
    'FAIL gate owner: R9 first play + helper, OUT after 1 AU: want R9/stop-logged, got R6/fallback'
mut Mq2-no-helper-falls-back 's/^    return {GateAction::Error, GateRow::R7};$/    return {GateAction::Fallback, GateRow::R7};/' \
    'FAIL gate owner: R7 first play, no helper, OUT: want R7/error, got R7/fallback'
mut Mq3-malformed-hard-errors 's/^    if (auWritten > 0)$/    if (s == StreamOutcome::Malformed)\n        return {GateAction::Error, GateRow::R7};\n    if (auWritten > 0)/' \
    'FAIL gate owner: R6 first play + helper, MALFORMED: want R6/fallback, got R7/error'
mut Mstr-value-gt-limit 's/" (limit " + std::to_string(v.limit) + ")"/" > " + std::to_string(v.limit)/' \
    'FAIL gate owner: phrase refs: max_num_ref_frames 3 > 1'
mut Mhs-helper-stream-ignored 's/return ctx.fallbackUsed || ctx.helperStream;/return ctx.fallbackUsed;/' \
    "FAIL gate owner: R3 first play on the helper's stream, CAPS FallbackOnFloor: want R3/gate-on-floor, got R2/fallback"
echo "test_gate_owner_mutants: OK ($n mutants KILLED by their pinned check)"
