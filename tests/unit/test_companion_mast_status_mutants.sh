#!/usr/bin/env bash
# Mutant for test_companion_mast_status (review 01:28/03:52 read-only rule): a copy of
# arm/misterplexd/companion.cpp with the GET /misterplex/mast route moved below the cast/token
# handling (right before the OPTIONS branch). The wiring test must FAIL on it (its read-only row).
# Usage: test_companion_mast_status_mutants.sh ROOT OUT_DIR
set -euo pipefail
ROOT=$1
OUT=$2
mkdir -p "$OUT"
python3 - "$ROOT/arm/misterplexd/companion.cpp" "$OUT/companion.cpp" << 'EOF'
import sys
src = open(sys.argv[1]).read()
start = src.index('        // GET /misterplex/mast (MAST R5b follow-up')
end = src.index('            continue;\n        }\n', start) + len('            continue;\n        }\n')
block, src = src[start:end], src[:start] + src[end:]
anchor = '        if (req.find("OPTIONS") == 0) {'
assert src.count(anchor) == 1, 'OPTIONS anchor'
open(sys.argv[2], 'w').write(src.replace(anchor, block + anchor))
EOF
if cmp -s "$ROOT/arm/misterplexd/companion.cpp" "$OUT/companion.cpp"; then
    echo "MUTANT NOT APPLIED: ROUTE_LATE"
    exit 1
fi
"${CXX:-g++}" -std=c++17 -O2 -I"$ROOT/host" -I"$ROOT/arm/misterplexd" -pthread \
    -DMPX_MAST_RELOAD_MARKER_PATH="\"$OUT/marker\"" -o "$OUT/test_route_late" \
    "$ROOT/tests/unit/test_companion_mast_status.cpp" "$OUT/companion.cpp" \
    "$ROOT/arm/misterplexd/pms_timeline.cpp" "$ROOT/arm/misterplexd/plex_resolve.cpp"
if timeout 60 "$OUT/test_route_late" > "$OUT/route_late.log" 2>&1; then
    echo "MUTANT SURVIVED: ROUTE_LATE (the status route below the cast/token handling)"
    exit 1
fi
echo "test_companion_mast_status: mutant ROUTE_LATE FAILS as required: $(grep -m1 '^FAIL' "$OUT/route_late.log")"
