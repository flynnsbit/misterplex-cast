#!/usr/bin/env bash
# Mutants for host/libmisterplex/gate_caps.hpp (T-SPS-GATE-FALLBACK pre-reg v1.4.3 b57a87fb: M7, M8, M10, M14 and the
# section 1 (i) bound). Each mutated header copy is compiled with the base's CXXFLAGS and run against
# tests/unit/test_gate_caps.cpp. Grammar (H264 c1 / review 02:56, SUPP-2):
#   KILLED        rc 1 and the FIRST "FAIL gate caps:" line equals the mutant's pinned line
#   KILLED-OTHER  rc 1 but another first FAIL line: a FINDING, the run fails
#   SURVIVED      rc 0: the run fails
#   INVALID       rc >= 128 (signal/crash), a timeout (rc 124), or any other rc: the run fails
# A mutation that does not apply (sed leaves the file unchanged) is a vacuous mutant: the run fails.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
H=$ROOT/host/libmisterplex/gate_caps.hpp
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -I$ROOT/host}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
n=0
mut() {
    local name=$1 expr=$2 want=$3 rc=0 first
    mkdir -p "$T/$name/libmisterplex"
    sed "$expr" "$H" > "$T/$name/libmisterplex/gate_caps.hpp"
    if cmp -s "$H" "$T/$name/libmisterplex/gate_caps.hpp"; then
        echo "FAIL mutant $name: the edit did not apply (vacuous)" >&2
        exit 1
    fi
    # The mutant dir comes first, so its gate_caps.hpp wins over host/.
    # shellcheck disable=SC2086
    "$CXX" -I"$T/$name" $CXXFLAGS -o "$T/$name/t" "$ROOT/tests/unit/test_gate_caps.cpp"
    timeout 60 "$T/$name/t" > "$T/$name/out" 2> "$T/$name/err" || rc=$?
    first=$(grep -m1 '^FAIL gate caps:' "$T/$name/err" || true)
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
mut M7-unknown-bits-ignored 's/if (unknownLow || unknownHigh) {/if (false) {/' \
    'FAIL gate caps: T11a: fallback, got Ok'
mut M7b-unlisted-level-ignored 's/if (levelIdc < 0) {/if (false) {/' \
    'FAIL gate caps: T11b: fallback, got Ok'
mut M8-floor-trusts-raw-low-half 's/v1Floor(c, unknownLow == 0)/v1Floor(c, true)/' \
    'FAIL gate caps: T11c/d constants floor: inter 1'
mut M10-constants-floor-keeps-inter 's/g.inter = lowTrusted && (c.features & ring::Inter) != 0;/g.inter = (c.features \& ring::Inter) != 0;/' \
    'FAIL gate caps: T11c/d constants floor: inter 1'
mut M14-orphan-grant-honoured 's/if (orphan) {/if (false) {/' \
    'FAIL gate caps: T11g: fallback, got Ok'
mut Mi-no-mpc2-bound 's/if (c.max_width > kPlex720pCodedWidth || c.max_height > kPlex720pCodedHeight) {/if (false) {/' \
    'FAIL gate caps: T11e: NoPlay, got Ok'
echo "test_gate_caps_mutants: OK ($n mutants KILLED by their pinned check)"
