#!/usr/bin/env bash
# TASKS #25 T-PQ-SUBSTRING-MATCH source needles. Exit 0 = clean, 1 = a needle hit.
set -euo pipefail

ROOT="${1:?usage: pq25_needles.sh <repo root>|--self-test <repo root>}"

hit() { echo "NEEDLE HIT: $*"; hits=$((hits + 1)); }

# A substring leg: .find( on a ratingKey, or key.find( on a playQueueItemId. Whitespace is allowed around '(' and
# after it, so the statement scan (newlines folded to spaces) sees a leg split after 'find(' (pq25-review finding 3).
NEEDLE='\.find[[:space:]]*\([[:space:]]*((q\.items\[i\]\.|item\.)?ratingKey)|key\.find[[:space:]]*\([[:space:]]*playQueueItemId[[:space:]]*\)'

run_scan() {
    local root="$1"
    local D="$root/arm/misterplexd"
    hits=0

    # 1. Line forms: identity substring matches must not appear in product .cpp files.
    while IFS= read -r l; do hit "line-substring: $l"; done < <(
        grep -HnE "$NEEDLE" "$D"/*.cpp || true
    )

    # 2. Statement forms: catch split lines inside one C++ statement.
    for f in "$D"/*.cpp; do
        while IFS= read -r s; do hit "statement-substring: ${f##*/}: $s"; done < <(
            tr '\n' ' ' < "$f" |
                grep -oE "($NEEDLE)[^;]*" || true
        )
    done

    # fetchPlayQueue itself, or the BUG2-PREROLL split: fetchPlayQueue returns
    # parsePlayQueueXml(...) and that function calls selectQueueIndex.
    local select_calls
    select_calls=$(awk '/PlayQueue fetchPlayQueue\(/ { s = 1 }
                        s && /selectQueueIndex\(/ { n++ }
                        s && /return parsePlayQueueXml\(/ { d = 1 }
                        s && /^}/ { e = 1; exit }
                        END { print (s && e) ? n + 0 : "no-function" }' "$D/plex_resolve.cpp")
    if [ "$select_calls" = 0 ]; then
        local delegated
        delegated=$(awk '/PlayQueue fetchPlayQueue\(/ { s = 1 }
                         s && /return parsePlayQueueXml\(/ { d = 1 }
                         s && /^}/ { exit }
                         END { print d + 0 }' "$D/plex_resolve.cpp")
        if [ "$delegated" = 1 ]; then
            select_calls=$(awk '/PlayQueue parsePlayQueueXml\(/ { s = 1 }
                                s && /selectQueueIndex\(/ { n++ }
                                s && /^}/ { e = 1; exit }
                                END { print (s && e) ? n + 0 : "no-function" }' "$D/plex_resolve.cpp")
        fi
    fi
    case "$select_calls" in
        no-function) hit "call: fetchPlayQueue not found in plex_resolve.cpp" ;;
        0) hit "call: fetchPlayQueue does not call selectQueueIndex" ;;
    esac

    local bind_calls
    bind_calls=$(awk '/auto doPlay = \[/ && !s { s = NR }
                      s && !e && NR > s && /^    };/ { e = NR }
                      s && !e && /firstResolveBinds\(/ { n++ }
                      END { print (s && e) ? n + 0 : "no-lambda" }' "$D/main.cpp")
    case "$bind_calls" in
        no-lambda) hit "call: doPlay lambda not found in main.cpp" ;;
        0) hit "call: doPlay does not call firstResolveBinds" ;;
    esac

    local step_calls
    step_calls=$(awk '/auto tryQueueStep = \[/ && !s { s = NR }
                      s && !e && NR > s && /^    };/ { e = NR }
                      s && !e && /queueStepDest\(/ { n++ }
                      END { print (s && e) ? n + 0 : "no-lambda" }' "$D/main.cpp")
    case "$step_calls" in
        no-lambda) hit "call: tryQueueStep lambda not found in main.cpp" ;;
        0) hit "call: tryQueueStep does not call queueStepDest" ;;
    esac

    local defs
    defs=$(grep -HnE '^[[:space:]]*(inline[[:space:]]+)?std::string[[:space:]]+ratingKeyFromKey[[:space:]]*\(' "$D"/*.cpp "$D"/*.hpp || true)
    local def_count
    def_count=$(printf "%s\n" "$defs" | grep -c . || true)
    if [ "$def_count" -ne 1 ]; then
        hit "definition-count: ratingKeyFromKey definitions=$def_count ${defs//$'\n'/ | }"
    fi

    if [ "$hits" -ne 0 ]; then
        echo "pq25_needles: $hits hit(s)"
        return 1
    fi
    echo "pq25_needles: OK (substring line 0, statement 0, selectQueueIndex calls=$select_calls, firstResolveBinds calls=$bind_calls, queueStepDest calls=$step_calls, ratingKeyFromKey definitions=1)"
}

if [ "$ROOT" = "--self-test" ]; then
    ROOT="${2:?usage: pq25_needles.sh --self-test <repo root>}"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/c2" "$tmp/dup"
    git -C "$ROOT" archive c2aed83b arm/misterplexd | tar -x -C "$tmp/c2"
    if run_scan "$tmp/c2"; then
        echo "NEG FAIL: c2aed83b fixture did not hit needles"
        exit 1
    fi
    mkdir -p "$tmp/dup/arm"
    cp -R "$ROOT/arm/misterplexd" "$tmp/dup/arm/"
    cat >> "$tmp/dup/arm/misterplexd/plex_resolve.hpp" <<'EOF'
inline std::string ratingKeyFromKey(const std::string& key) { return key; }
EOF
    if run_scan "$tmp/dup"; then
        echo "NEG FAIL: duplicate ratingKeyFromKey fixture did not hit needle"
        exit 1
    fi
    # Split-line legs (pq25-review finding 3): only the statement scan can see them, and it must.
    for leg in doPlay fetchPlayQueue; do
        rm -rf "$tmp/split"
        mkdir -p "$tmp/split/arm"
        cp -R "$ROOT/arm/misterplexd" "$tmp/split/arm/"
        if [ "$leg" = doPlay ]; then
            printf '%s\n' 'static bool pq25NegSplitA(const std::string& key, const PlayQueueItem& item) { return key.find(' \
                '    item.ratingKey) != std::string::npos; }' >> "$tmp/split/arm/misterplexd/main.cpp"
        else
            printf '%s\n' 'static bool pq25NegSplitB(const std::string& currentKey, const PlayQueue& q, size_t i) {' \
                '    return currentKey.find(' '        q.items[i].ratingKey) != std::string::npos; }' \
                >> "$tmp/split/arm/misterplexd/plex_resolve.cpp"
        fi
        out=$(run_scan "$tmp/split" || true)
        if ! grep -q 'NEEDLE HIT: statement-substring' <<<"$out" || grep -q 'NEEDLE HIT: line-substring' <<<"$out"; then
            echo "NEG FAIL: split-line $leg fixture: want a statement hit and no line hit, got:"
            echo "$out"
            exit 1
        fi
    done
    echo "pq25_needles: self-test OK (c2aed83b NEG, duplicate-definition NEG, split-line NEGs doPlay + fetchPlayQueue)"
    exit 0
fi

run_scan "$ROOT"
