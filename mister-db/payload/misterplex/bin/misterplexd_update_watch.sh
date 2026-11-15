#!/bin/sh
# GH#59: init-wrapper T+60 restore of misterplexd.prev when a swapped daemon
# never writes the tmpfs health marker. Pure functions; sourced by
# misterplexd_supervise.sh. Default timeout 60 s (spec). Never touches RBF.
#
# decide: wait | commit | rollback
# restore: mv BIN.prev over BIN (via .tmp), drop the journal.

update_watch_decide() {
    # journal marker elapsed timeout -> wait|commit|rollback
    _j=$1
    _m=$2
    _el=${3:-0}
    _to=${4:-60}
    if [ "${MUT_NO_T60_ROLLBACK:-}" = 1 ] || [ "${MISTERPLEX_UPDATE_NO_T60:-}" = 1 ]; then
        echo wait
        return 0
    fi
    if [ -z "$_j" ] || [ ! -f "$_j" ]; then
        echo wait
        return 0
    fi
    _st=$(sed -n '1s/^state=//p' "$_j" 2>/dev/null || true)
    if [ "$_st" != "PENDING" ]; then
        echo wait
        return 0
    fi
    _ver=$(sed -n '2s/^version=//p' "$_j" 2>/dev/null || true)
    if [ -n "$_m" ] && [ -f "$_m" ] && [ -n "$_ver" ]; then
        _got=$(sed -n '1p' "$_m" 2>/dev/null || true)
        if [ "$_got" = "$_ver" ]; then
            echo commit
            return 0
        fi
    fi
    if [ "$_el" -ge "$_to" ] 2>/dev/null; then
        echo rollback
        return 0
    fi
    echo wait
}

update_watch_restore() {
    # bin journal  — kill the child first. Returns 0 if .prev became BIN.
    _bin=$1
    _j=$2
    [ -n "$_bin" ] || return 1
    _prev="${_bin}.prev"
    [ -f "$_prev" ] || return 1
    _tmp="${_bin}.tmp"
    cp -f "$_prev" "$_tmp" || return 1
    mv -f "$_tmp" "$_bin" || return 1
    rm -f "$_j" "$_tmp"
    return 0
}
