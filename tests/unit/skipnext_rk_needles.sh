#!/usr/bin/env bash
# TASKS-NIGHT-1005 #22 source needles (review 05:19 b1). The test_resolve tooth covers queueStepRequest only, so a
# call site that rebuilds the request inline, or re-adds the ratingKey fallback, would still pass it. These pin the
# call sites: tryQueueStep (main.cpp) builds its request with queueStepRequest, and no arm/misterplexd/*.cpp assigns
# playQueueItemId from a ratingKey. queue_step.hpp is not scanned: its MUT_SKIPNEXT_RK_FALLBACK branch holds the
# fallback on purpose.
# Usage: skipnext_rk_needles.sh <repo root>. Exit 0 = clean, 1 = a needle hit.
set -euo pipefail
D="${1:?usage: skipnext_rk_needles.sh <repo root>}/arm/misterplexd"
hits=0
hit() { echo "NEEDLE HIT: $*"; hits=$((hits + 1)); }

# 1. Line form, as bound.
while IFS= read -r l; do hit "line: $l"; done < <(grep -Hn 'playQueueItemId *=.*ratingKey' "$D"/*.cpp || true)

# 2. Statement form. The line form misses the pre-#22 fallback, which spans two lines (main.cpp:1873-1874
#    @c2aed83b): an assignment ('=', not '==') to playQueueItemId whose statement mentions ratingKey.
for f in "$D"/*.cpp; do
    while IFS= read -r s; do hit "statement: ${f##*/}: $s"; done < <(tr '\n' ' ' < "$f" |
        grep -o 'playQueueItemId[[:space:]]*=[^=;][^;]*ratingKey' || true)
done

# 3. tryQueueStep calls queueStepRequest. The lambda runs from 'auto tryQueueStep = [' to its closing '    };'.
calls=$(awk '/auto tryQueueStep = \[/ && !s { s = NR }
             s && !e && NR > s && /^    };/ { e = NR }
             s && !e && /queueStepRequest\(/ { n++ }
             END { print (s && e) ? n + 0 : "no-lambda" }' "$D/main.cpp")
case "$calls" in
    no-lambda) hit "call: tryQueueStep lambda not found in main.cpp" ;;
    0) hit "call: tryQueueStep does not call queueStepRequest" ;;
esac

if [ "$hits" -ne 0 ]; then
    echo "skipnext_rk_needles: $hits hit(s)"
    exit 1
fi
echo "skipnext_rk_needles: OK (line 0, statement 0, tryQueueStep calls queueStepRequest $calls time(s))"
