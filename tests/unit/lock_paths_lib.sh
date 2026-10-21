# Sourced by the daemon e2e scripts (T-DAEMON-LOCK-SHARED A1: c3 no split brain, m1 captured
# stderr, k2 realpath). Every MPX_TEST_BUILD participant (daemon, set_status, ...) prints ONE line,
# first on its stderr (static init): 'mpx-lock-paths: source=<env|baked|default> daemon=<p> spi=<p>'.
# lp_expect, called right after a participant starts, checks that the captured log holds exactly the
# expected number of such lines and that every one names this script's own lock dir. A participant
# whose stderr is discarded, or one built without the flags (no line, or /tmp paths), is RED.

lp_init() {  # after 'export MPX_LOCK_DIR=...; mkdir -p "$MPX_LOCK_DIR"'
    LP_DIR_REAL=$(realpath "$MPX_LOCK_DIR") || {
        echo "FAIL [lock-paths]: realpath $MPX_LOCK_DIR failed" >&2
        exit 1
    }
    LP_WANT="daemon=$LP_DIR_REAL/misterplexd.lock spi=$LP_DIR_REAL/misterplex_spi.lock"
    LP_CHECKS=0
}

lp_expect() {  # lp_expect <log> <n>: wait up to 3 s for n lines; all must name LP_WANT
    local log=$1 n=$2 got=0 i other
    for i in $(seq 1 30); do
        got=$(grep -c '^mpx-lock-paths: ' "$log" 2>/dev/null || true)
        [ "${got:-0}" -ge "$n" ] && break
        sleep 0.1
    done
    other=$(grep '^mpx-lock-paths: ' "$log" 2>/dev/null | sed 's/^mpx-lock-paths: source=[a-z]* //' |
        grep -v -x -F -- "$LP_WANT" | head -n 1)
    if [ "${got:-0}" != "$n" ] || [ -n "$other" ]; then
        echo "FAIL [lock-paths]: $log: want $n x [$LP_WANT], got ${got:-0}${other:+; other: $other}" >&2
        return 1
    fi
    LP_CHECKS=$((LP_CHECKS + 1))
}
