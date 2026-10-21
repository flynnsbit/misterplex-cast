#!/usr/bin/env bash
# Sandbox rows for `scripts/misterplexd-boot-mast.sh mast-reload` (MAST-WEDGE hand-off; never touches
# a MiSTer). It uses the launcher's own overrides: ROOT, DRY_RUN, BOOT_PROC, BOOT_* waits,
# BOOT_EXPECT_MD5 and BOOT_MAST_LOADER.
#   A  marker requested, daemon gone, CORENAME Plex (DRY_RUN): one guarded load, then the verified
#      start (its env has MPX_MAST_RELOAD_CMD); marker -> attempted.
#   B  marker attempted (a second request): refused, restart only, marker unchanged.
#   C  daemon still alive: no load; marker refused why=daemon-alive (M2).
#   D  real run, the loader refuses (rc 97 or 98, the guard): no reload; marker refused
#      why=guard-rc<rc> (M2).
#   E  real run, the loader succeeds: called exactly once with the RBF, then the start path.
#   F  CORENAME is MENU, or another core, right before the load: no load, loud; marker refused
#      why=core-switched:<core>. review 21:52 M1 / review 21:52: never replace a core or
#      menu the user switched to.
#   G  real run, the guard passes and the loader fails (rc 1 or 99, not the guard's 97/98): one
#      attempt, loud 'reload attempted, loader rc=<rc>'; marker load-failed why=load-failed-rc<rc>,
#      never refused (review 22:00 N1).
#   H, I, I2, J, J2 (glass G1 23:49, review 23:53 R1): a hand-off while a start launcher holds
#      the lock waits for it and loads once (H); a lock that never frees refuses a fresh request
#      why=launcher-busy (I) and leaves any other marker alone (I2); a real start run whose daemon
#      hands off inside START_WAIT yields: no retry, no second start (J), but not to a stale one (J2).
#   J3, S, precheck (review 00:18 P1/P4): a hand-off deep in START_WAIT (>= 5 s) still yields
#      (J3); without usleep and fractional sleep the clock-bounded waits keep their length (S); the
#      R4 box precheck uses the launcher's own tick, and (review 03:16 N2) its MATH line runs
#      the loader's 64-bit check: MATH_OK here, MATH_BAD and rc 1 on a simulated 32-bit shell.
#   D, E, G and F's real-run case stop at the md5 gate, so nothing starts in the sandbox.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BOOT=${MAST_LAUNCHER:-$ROOT/scripts/misterplexd-boot-mast.sh}
LOADER_SRC=${MAST_LOADER_SRC:-$ROOT/scripts/misterplexd-mast-load.src.sh}
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
mk() { # sandbox root with a fake daemon, CORENAME and an empty fake /proc
    SB=$(mktemp -d /tmp/mast-boot-sbx.XXXXXX)
    mkdir -p "$SB/tmp" "$SB/media/fat/linux" "$SB/root" "$SB/proc"
    printf '#!/bin/sh\nexit 0\n' > "$SB/media/fat/linux/misterplexd-coregate-2164c4ad"
    chmod +x "$SB/media/fat/linux/misterplexd-coregate-2164c4ad"
    MD5=$(md5sum "$SB/media/fat/linux/misterplexd-coregate-2164c4ad" | cut -c1-32)
    echo "${1:-Plex}" > "$SB/tmp/CORENAME"
    MARK=$SB/tmp/misterplexd.mast-reload
}
run() { # dry md5 args...
    local dry=$1 md5=$2
    shift 2
    env DRY_RUN="$dry" ROOT="$SB" BOOT_PROC="$SB/proc" BOOT_STARTUP_DELAY=0 BOOT_NET_WAIT=0 \
        BOOT_CORE_WAIT=0 BOOT_MR_EXIT_WAIT="${EXITWAIT:-1}" BOOT_MR_SETTLE=0 BOOT_EXPECT_MD5="$md5" \
        BOOT_MR_LOCK_WAIT="${LOCKWAIT:-5}" BOOT_MAST_MARKER="$MARK" BOOT_PATH_PREFIX="${PATHPFX:-}" \
        BOOT_START_WAIT="${STARTWAIT:-20}" BOOT_TRIES="${TRY_N:-3}" \
        BOOT_MAST_LOADER="$SB/loader.sh" sh "$BOOT" "$@" > "$SB/out" 2>&1
    echo $?
}
blog() { cat "$SB/tmp/misterplexd-boot.log" 2>/dev/null; }
refused() { # why
    grep -q '^state=refused' "$MARK" && grep -q "^why=$1\$" "$MARK"
}

# A
mk
printf 'state=requested\ntime=1\nreason=gate-open probe\n' > "$MARK"
run 1 "$MD5" mast-reload 999999 "gate-open probe" "$MARK" > /dev/null
blog | grep -q 'MAST_RELOAD: daemon pid=999999 gone after 0.0s' || fail "A: no daemon-gone line"
blog | grep -q 'MAST_RELOAD: CORENAME=\[Plex\]; marker attempted' || fail "A: no CORENAME check line"
blog | grep -q "DRY_RUN would: sh $SB/loader.sh /media/fat/_Utility/Plex.rbf" || fail "A: no guarded load"
blog | grep -q 'MPX_MAST_RELOAD_CMD=/media/fat/linux/misterplexd-boot.sh setsid nohup' ||
    fail "A: the verified start lacks MPX_MAST_RELOAD_CMD"
grep -q '^state=attempted' "$MARK" || fail "A: marker not attempted"
[ "$(blog | grep -c 'DRY_RUN would: sh ')" = 1 ] || fail "A: not exactly one load"
rm -rf "$SB"

# B
mk
printf 'state=attempted\ntime=1\nreason=gate-open probe\n' > "$MARK"
run 1 "$MD5" mast-reload 999999 "begin" "$MARK" > /dev/null
blog | grep -q "MAST_RELOAD refused: marker state=\[attempted\]" || fail "B: second request not refused"
blog | grep -q 'DRY_RUN would: sh ' && fail "B: loaded on a second request"
grep -q 'refused' "$SB/tmp/misterplexd-boot.FAILED" || fail "B: refusal not loud (FAILED file)"
grep -q '^state=attempted' "$MARK" || fail "B: marker changed"
blog | grep -q 'DRY_RUN would: cd ' || fail "B: no restart"
rm -rf "$SB"

# C
mk
printf 'state=requested\ntime=1\nreason=begin\n' > "$MARK"
sleep 30 &
ALIVE=$!
run 1 "$MD5" mast-reload "$ALIVE" "begin" "$MARK" > /dev/null
kill "$ALIVE" 2>/dev/null
blog | grep -q "MAST_RELOAD refused (daemon-alive): misterplexd pid=$ALIVE still running after 1s" ||
    fail "C: live daemon not refused"
blog | grep -q 'DRY_RUN would: sh ' && fail "C: loaded with the daemon alive"
refused daemon-alive || fail "C: marker not refused why=daemon-alive: $(tr '\n' ' ' < "$MARK")"
rm -rf "$SB"

# D: the guard's two refusal codes
for grc in 97 98; do
    mk
    printf 'state=requested\ntime=1\nreason=begin\n' > "$MARK"
    printf '#!/bin/sh\necho "LOADCORE_GUARD_ABORT (sandbox fake)" >&2\nexit %s\n' "$grc" > "$SB/loader.sh"
    rc=$(run 0 wrongmd5 mast-reload 999999 "begin" "$MARK")
    blog | grep -q "MAST_RELOAD refused (guard-rc$grc): LOADCORE_GUARD refused the load (loader rc=$grc;" ||
        fail "D($grc): refusal not logged"
    blog | grep -q 'LOADCORE_GUARD_ABORT (sandbox fake)' || fail "D($grc): guard lines not in the boot log"
    blog | grep -q 'reload attempted' && fail "D($grc): a guard refusal called an attempted reload"
    refused "guard-rc$grc" || fail "D($grc): marker not refused why=guard-rc$grc: $(tr '\n' ' ' < "$MARK")"
    blog | grep -q 'DAEMON MD5 MISMATCH' || fail "D($grc): did not reach the start path"
    [ "$rc" = 2 ] || fail "D($grc): rc $rc"
    rm -rf "$SB"
done

# D96 (R6 PORT SUSPECT, review 02:17): the loader's rc 96 is a refusal (no load ran): marker
# refused why=r6-port-suspect, loud, reboot first, and the normal start follows.
mk
printf 'state=requested\ntime=1\nreason=begin\n' > "$MARK"
printf '#!/bin/sh\necho "!!! MAST_LOAD R6 PORT SUSPECT: (sandbox fake)"\nexit 96\n' > "$SB/loader.sh"
rc=$(run 0 wrongmd5 mast-reload 999999 "begin" "$MARK")
blog | grep -q 'MAST_RELOAD refused (r6-port-suspect): R6 PORT SUSPECT: .* - no load, restart only: HPS reboot first' ||
    fail "D96: refusal not logged: $(blog | grep 'MAST_RELOAD' | tail -n 1)"
blog | grep -q 'MAST_LOAD R6 PORT SUSPECT: (sandbox fake)' || fail "D96: loader lines not in the boot log"
blog | grep -q 'reload attempted' && fail "D96: a suspect port called an attempted reload"
refused r6-port-suspect || fail "D96: marker not refused why=r6-port-suspect: $(tr '\n' ' ' < "$MARK")"
grep -q 'r6-port-suspect' "$SB/tmp/misterplexd-boot.FAILED" || fail "D96: not loud"
blog | grep -q 'DAEMON MD5 MISMATCH' || fail "D96: did not reach the start path"
[ "$rc" = 2 ] || fail "D96: rc $rc"
rm -rf "$SB"

# E
mk
printf 'state=requested\ntime=1\nreason=gate-open probe\n' > "$MARK"
printf '#!/bin/sh\necho "loader $*" >> %s/loader.calls\necho "$MAST_LOAD_MARKER|$MAST_LOAD_CORENAME" > %s/loader.env\necho "LOADCORE_GUARD_OK (sandbox fake)"\nexit 0\n' "$SB" "$SB" > "$SB/loader.sh"
rc=$(run 0 wrongmd5 mast-reload 999999 "gate-open probe" "$MARK")
[ "$(cat "$SB/loader.calls" 2>/dev/null)" = "loader /media/fat/_Utility/Plex.rbf" ] ||
    fail "E: loader not called exactly once with the RBF: $(cat "$SB/loader.calls" 2>/dev/null)"
[ "$(cat "$SB/loader.env" 2>/dev/null)" = "$MARK|$SB/tmp/CORENAME" ] ||
    fail "E: the loader did not get MAST_LOAD_MARKER/MAST_LOAD_CORENAME: [$(cat "$SB/loader.env" 2>/dev/null)]"
blog | grep -q 'MAST_RELOAD: loaded; CORENAME=\[Plex\]' || fail "E: no loaded line"
blog | grep -q 'LOADCORE_GUARD_OK (sandbox fake)' || fail "E: guard line not in the boot log"
grep -q '^state=attempted' "$MARK" || fail "E: marker not attempted"
[ "$rc" = 2 ] || fail "E: rc $rc"
rm -rf "$SB"

# F: MENU (DRY_RUN), and another core in a real run with a loader that would succeed
for core in MENU NES; do
    mk "$core"
    printf 'state=requested\ntime=1\nreason=gate-open probe\n' > "$MARK"
    printf '#!/bin/sh\necho "loader $*" >> %s/loader.calls\nexit 0\n' "$SB" > "$SB/loader.sh"
    if [ "$core" = MENU ]; then dry=1; else dry=0; fi
    run "$dry" wrongmd5 mast-reload 999999 "gate-open probe" "$MARK" > /dev/null
    blog | grep -q "MAST_RELOAD refused (core-switched:$core): CORENAME=\[$core\] is not Plex" ||
        fail "F($core): switched core not refused"
    blog | grep -q 'DRY_RUN would: sh ' && fail "F($core): loaded over a switched core"
    [ -e "$SB/loader.calls" ] && fail "F($core): the loader ran over a switched core"
    grep -q "core-switched:$core" "$SB/tmp/misterplexd-boot.FAILED" || fail "F($core): not loud"
    refused "core-switched:$core" || fail "F($core): marker not refused: $(tr '\n' ' ' < "$MARK")"
    rm -rf "$SB"
done

# G: the guard passed, the load failed (review 22:00 N1)
for lrc in 1 99; do
    mk
    printf 'state=requested\ntime=1\nreason=gate-open probe\n' > "$MARK"
    printf '#!/bin/sh\necho "loader $*" >> %s/loader.calls\necho "LOADCORE_GUARD_OK (sandbox fake)"\nexit %s\n' "$SB" "$lrc" > "$SB/loader.sh"
    rc=$(run 0 wrongmd5 mast-reload 999999 "gate-open probe" "$MARK")
    [ "$(cat "$SB/loader.calls" 2>/dev/null)" = "loader /media/fat/_Utility/Plex.rbf" ] ||
        fail "G($lrc): loader not called exactly once: $(cat "$SB/loader.calls" 2>/dev/null)"
    blog | grep -q "MAST_RELOAD FAILED (load-failed-rc$lrc): reload attempted, loader rc=$lrc " ||
        fail "G($lrc): no 'reload attempted, loader rc=$lrc' line"
    grep -q "load-failed-rc$lrc" "$SB/tmp/misterplexd-boot.FAILED" || fail "G($lrc): not loud"
    blog | grep -q 'MAST_RELOAD refused' && fail "G($lrc): a failed load called a refusal"
    blog | grep -q 'MAST_RELOAD: loaded;' && fail "G($lrc): a failed load called loaded"
    { grep -q '^state=load-failed$' "$MARK" && grep -q "^why=load-failed-rc$lrc\$" "$MARK"; } ||
        fail "G($lrc): marker not load-failed why=load-failed-rc$lrc: $(tr '\n' ' ' < "$MARK")"
    blog | grep -q 'DAEMON MD5 MISMATCH' || fail "G($lrc): did not reach the start path"
    [ "$rc" = 2 ] || fail "G($lrc): rc $rc"
    rm -rf "$SB"
done

# The R1 rows (glass G1 23:49, review 23:53): a hand-off made while a start launcher holds the
# lock is never dropped.
hold_lock() { # seconds: a stand-in start launcher that holds the lock, then releases it on exit
    sh -c 'mkdir "$1" && echo $$ > "$1/pid" && sleep "$2"; rm -rf "$1"' _ \
        "$SB/tmp/misterplexd-boot.lock.d" "$1" &
    HOLDER=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$SB/tmp/misterplexd-boot.lock.d/pid" ] && break; sleep 0.1; done
}

# H: the lock frees after ~1 s: the hand-off waits, then runs ONE load
mk
printf 'state=requested\ntime=%s\nreason=gate-open probe\n' "$(date +%s)" > "$MARK"
hold_lock 1
run 1 "$MD5" mast-reload 999999 "gate-open probe" "$MARK" > /dev/null
wait "$HOLDER" 2>/dev/null
blog | grep -q "MAST_RELOAD: launcher lock held by misterplexd-boot pid=$HOLDER; waiting up to 5s" ||
    fail "H: no lock-wait line"
blog | grep -q 'MAST_RELOAD: launcher lock taken after ' || fail "H: lock never taken"
blog | grep -q 'another misterplexd-boot' && fail "H: the hand-off was dropped on the lock"
[ "$(blog | grep -c 'DRY_RUN would: sh ')" = 1 ] || fail "H: not exactly one load"
grep -q '^state=attempted' "$MARK" || fail "H: marker not attempted: $(tr '\n' ' ' < "$MARK")"
rm -rf "$SB"

# I: the lock never frees within MR_LOCK_WAIT: a fresh request is refused loudly, no load;
# I2: the same with a marker that is no longer a fresh request: left alone, no load
for st in requested attempted; do
    mk
    printf 'state=%s\ntime=%s\nreason=begin\n' "$st" "$(date +%s)" > "$MARK"
    hold_lock 4
    rc=$(LOCKWAIT=1 run 1 "$MD5" mast-reload 999999 "begin" "$MARK")
    kill "$HOLDER" 2>/dev/null
    wait "$HOLDER" 2>/dev/null
    blog | grep -q 'DRY_RUN would: sh ' && fail "I($st): loaded without the lock"
    [ "$rc" = 0 ] || fail "I($st): rc $rc"
    if [ "$st" = requested ]; then
        blog | grep -q "MAST_RELOAD refused (launcher-busy): misterplexd-boot pid=$HOLDER still holds the launcher lock after 1s" ||
            fail "I: no launcher-busy refusal"
        grep -q 'launcher-busy' "$SB/tmp/misterplexd-boot.FAILED" || fail "I: not loud"
        refused launcher-busy || fail "I: marker not refused why=launcher-busy: $(tr '\n' ' ' < "$MARK")"
    else
        blog | grep -q 'marker state=\[attempted\] is not a fresh request - no load' ||
            fail "I2: no 'not a fresh request' line"
        grep -q '^state=attempted' "$MARK" || fail "I2: marker changed: $(tr '\n' ' ' < "$MARK")"
    fi
    rm -rf "$SB"
done

# J: a real start whose daemon hands off inside START_WAIT (the glass G1 race): the start
# launcher yields - no PROD_START_BAD, no attempt 2, no second daemon start, lock released.
# J2: a stale 'requested' (older than the launcher) is no pending hand-off: no yield.
for case in J J2; do
    mk
    D="$SB/media/fat/linux/misterplexd-coregate-2164c4ad"
    if [ "$case" = J ]; then
        printf '#!/bin/sh\necho "run $*" >> %s/daemon.calls\nprintf "state=requested\\ntime=%%s\\nreason=gate-open probe\\n" "$(date +%%s)" > %s.tmp && mv -f %s.tmp %s\nexit 86\n' \
            "$SB" "$MARK" "$MARK" "$MARK" > "$D"
    else
        printf 'state=requested\ntime=1\nreason=gate-open probe\n' > "$MARK"
        printf '#!/bin/sh\necho "run $*" >> %s/daemon.calls\nexit 0\n' "$SB" > "$D"
    fi
    DM=$(md5sum < "$D" | cut -c1-32)
    if [ "$case" = J ]; then
        rc=$(STARTWAIT=5 run 0 "$DM" start)
    else
        rc=$(STARTWAIT=1 TRY_N=1 run 0 "$DM" start)
    fi
    calls=$(wc -l < "$SB/daemon.calls" 2>/dev/null || echo 0)
    if [ "$case" = J ]; then
        blog | grep -q 'MAST_HANDOFF pending at .*start launcher yields' || fail "J: the start launcher did not yield"
        blog | grep -q 'PROD_START_BAD' && fail "J: PROD_START_BAD with a hand-off pending"
        blog | grep -q -- '--- attempt 2' && fail "J: retried with a hand-off pending"
        [ "$calls" = 1 ] || fail "J: daemon started $calls times, want 1"
        [ "$rc" = 0 ] || fail "J: rc $rc"
        [ -d "$SB/tmp/misterplexd-boot.lock.d" ] && fail "J: the lock was not released"
        grep -q '^state=requested' "$MARK" || fail "J: the start launcher touched the marker"
    else
        blog | grep -q 'MAST_HANDOFF pending' && fail "J2: yielded to a stale request"
        blog | grep -q -- '--- attempt 1' || fail "J2: no start attempt"
        [ "$calls" = 1 ] || fail "J2: daemon started $calls times, want 1"
    fi
    rm -rf "$SB"
done

# J3 (review 00:18 P4): the slow case, a hand-off deep in START_WAIT (>= 5 s, as on glass where
# the verify loop ran 58 s): the start launcher still yields, no PROD_START_BAD, one daemon start.
mk
D="$SB/media/fat/linux/misterplexd-coregate-2164c4ad"
printf '#!/bin/sh\necho "run $*" >> %s/daemon.calls\nsleep 5\nprintf "state=requested\\ntime=%%s\\nreason=gate-open probe\\n" "$(date +%%s)" > %s.tmp && mv -f %s.tmp %s\nexit 86\n' \
    "$SB" "$MARK" "$MARK" "$MARK" > "$D"
rc=$(STARTWAIT=12 run 0 "$(md5sum < "$D" | cut -c1-32)" start)
at=$(blog | sed -n 's/.*MAST_HANDOFF pending at START_WAIT \([0-9]*\)s.*/\1/p' | head -n 1)
[ -n "$at" ] && [ "$at" -ge 4 ] || fail "J3: no yield deep in START_WAIT (at=[$at]): $(blog | grep -E 'MAST_HANDOFF|PROD_' | tail -n 2)"
blog | grep -q 'PROD_START_BAD' && fail "J3: PROD_START_BAD with a hand-off pending"
[ "$(wc -l < "$SB/daemon.calls" 2>/dev/null || echo 0)" = 1 ] || fail "J3: not exactly one daemon start"
[ "$rc" = 0 ] || fail "J3: rc $rc"
rm -rf "$SB"

# S (review 00:18 P1): a busybox without usleep whose sleep rejects fractions. The waits are
# clock-bounded, so they keep their length: the hand-off waits ~1 s for the lock and ~2 s for
# the daemon and still loads once. Before P1, 'sleep 0.1' failed at once and the 5 s lock wait
# collapsed into a launcher-busy refusal.
mk
mkdir -p "$SB/strict"
printf '#!/bin/sh\ncase "$1" in *.*) echo "sleep: invalid number '"'"'$1'"'"'" >&2; exit 1 ;; esac\nexec %s "$@"\n' \
    "$(command -v sleep)" > "$SB/strict/sleep"
printf '#!/bin/sh\necho "usleep: not found" >&2\nexit 127\n' > "$SB/strict/usleep"
chmod +x "$SB/strict/sleep" "$SB/strict/usleep"
printf 'state=requested\ntime=%s\nreason=gate-open probe\n' "$(date +%s)" > "$MARK"
sleep 3 &
DPID=$!
hold_lock 1
PATHPFX="$SB/strict" EXITWAIT=5 run 1 "$MD5" mast-reload "$DPID" "gate-open probe" "$MARK" > /dev/null
wait "$HOLDER" "$DPID" 2>/dev/null
lt=$(blog | sed -n 's/.*launcher lock taken after \([0-9]*\)\.\([0-9]\)s.*/\1\2/p' | head -n 1)
gt=$(blog | sed -n 's/.*gone after \([0-9]*\)\.\([0-9]\)s.*/\1\2/p' | head -n 1)
[ -n "$lt" ] && [ "$lt" -ge 8 ] || fail "S: the lock wait did not last (taken after [$lt] ds): $(blog | grep -E 'launcher-busy|lock' | tail -n 1)"
[ -n "$gt" ] && [ "$gt" -ge 8 ] || fail "S: the exit wait did not last (gone after [$gt] ds)"
blog | grep -q 'launcher-busy' && fail "S: the lock wait collapsed into launcher-busy"
[ "$(blog | grep -c 'DRY_RUN would: sh ')" = 1 ] || fail "S: not exactly one load"
grep -q '^state=attempted' "$MARK" || fail "S: marker not attempted: $(tr '\n' ' ' < "$MARK")"
rm -rf "$SB"

# The R4 box precheck paces with the launcher's own tick (review 00:18 P1).
PRE=$ROOT/scripts/misterplexd-tick-precheck.sh
[ "$(grep '^tick()' "$PRE")" = "$(grep '^tick()' "$BOOT")" ] ||
    fail "precheck: its tick() is not the launcher's"
pout=$(sh "$PRE") || fail "precheck: TICK_BAD or MATH_BAD on this host: $(tr '\n' '|' <<< "$pout")"
# review 03:16 N2: the precheck's MATH line runs the loader's own 64-bit check; a shell whose
# math is 32-bit (simulated: the expression yields -1, as a 32-bit ash) gives MATH_BAD and rc 1.
grep -q '^m=$(echo "$((0xFFFFFFFF))" 2>/dev/null)$' "$PRE" &&
    grep -q '^  if \[ "$((0xFFFFFFFF))" != 4294967295 \]; then$' "$ROOT/scripts/misterplexd-mast-load.src.sh" ||
    fail "precheck: its math line is not the loader's 64-bit check"
grep -q '^MATH_OK $((0xFFFFFFFF)) = 4294967295 (64-bit shell math: the R6 drain can judge the ring)$' <<< "$pout" ||
    fail "precheck: no MATH_OK line on this host: $(tr '\n' '|' <<< "$pout")"
pm=$(mktemp /tmp/mast-precheck-m.XXXXXX)
sed 's/^m=$(echo "$((0xFFFFFFFF))" 2>\/dev\/null)$/m=$(echo "-1")/' "$PRE" > "$pm"
if cmp -s "$pm" "$PRE"; then
    fail "precheck: the 32-bit simulation did not apply"
else
    pout=$(sh "$pm")
    prc=$?
    [ "$prc" = 1 ] &&
        grep -q '^MATH_BAD $((0xFFFFFFFF)) = \[-1\], want 4294967295 - the R6 drain cannot judge - STOP: run no MAST row$' <<< "$pout" ||
        fail "precheck: a 32-bit shell did not give MATH_BAD and rc 1: rc $prc $(tr '\n' '|' <<< "$pout")"
fi
rm -f "$pm"

# P / P2 (P-a, review 01:25/01:28) and P3-P7 (R6, review 02:08/02:17, H264 r6 input §5): the
# loader drains, waits 200 ms, withdraws the stale audio command, then loads. devmem has width 32
# on every call and writes only the commit word 0x30140128-12F, then reads it back. The fake
# devmem serves the 0x30140000 page (page.bin) and the D10 page 0x300FF000 (page.bin.ff), logs
# each call with a monotonic time, can drain READ toward CTRL (FAKE_DRAIN_STEP, as the core
# consuming), and records the ring at the first commit write (page.bin.pa).
pa_run() { # stuck corename seedspec [NAME=VALUE for the loader...]
    local stuck=$1 cn=$2 spec=$3
    shift 3
    SB=$(mktemp -d /tmp/mast-pa-sbx.XXXXXX)
    cat > "$SB/devmem.py" << 'EOF'
import os, struct, sys, time
page, args = sys.argv[1], sys.argv[2:]
with open(page + '.argv', 'a') as f:
    f.write(' '.join(args) + '\n')
with open(page + '.times', 'a') as f:
    f.write('%.6f %s\n' % (time.monotonic(), ' '.join(args)))
addr, width = int(args[0], 0), (int(args[1]) if len(args) > 1 else 32)
if width != 32:
    sys.exit('fake devmem: unsupported ' + ' '.join(args))
if 0x30140000 <= addr <= 0x30140FFC:
    path, off = page, addr - 0x30140000
elif 0x300FF000 <= addr <= 0x300FFFFC:
    path, off = page + '.ff', addr - 0x300FF000
else:
    sys.exit('fake devmem: unsupported ' + ' '.join(args))
rd = lambda d, o: struct.unpack_from('<I', d, o)[0]
with open(path, 'r+b') as f:
    d = bytearray(f.read())
    if len(args) >= 3:
        if path == page and off == 0x128 and not os.path.exists(page + '.pa'):
            with open(page + '.pa', 'w') as g:
                g.write('read=0x%08X ctrl=0x%08X level=0x%08X\n' % (rd(d, 0x0C), rd(d, 0x04), rd(d, 0x1C)))
        if os.environ.get('FAKE_DEVMEM_STUCK') != '1':
            struct.pack_into('<I', d, off, int(args[2], 0) & 0xffffffff)
    else:
        print('0x%08X' % rd(d, off))
        step = int(os.environ.get('FAKE_DRAIN_STEP', '0'), 0)
        if path == page and off == 0x0C and step:
            w, r = rd(d, 0x04) & 0x7fffffff, rd(d, 0x0C) & 0x7fffffff
            r = min(w, r + step)
            struct.pack_into('<I', d, 0x0C, r)
            struct.pack_into('<I', d, 0x1C, w - r)
    f.seek(0)
    f.write(d)
EOF
    mkdir -p "$SB/bin"
    printf '#!/bin/sh\nexec python3 %s/devmem.py %s/page.bin "$@"\n' "$SB" "$SB" > "$SB/bin/devmem"
    chmod +x "$SB/bin/devmem"
    python3 - "$SB/page.bin" "$spec" << 'EOF'
import struct, sys
b = bytearray((i * 7 + 3) & 0xff for i in range(4096))
b[0x100:0x108] = struct.pack('<Q', (1 << 32) | 0x4D414354)  # a stale MACT body word
b[0x128:0x130] = struct.pack('<Q', (3 << 32) | 0x4D414343)  # its MACC commit, publication 3
v = dict(ctrl_magic=0x504C5842, ctrl=0x1000, read_magic=0x504C5852, read=0x1000,
         plxt_magic=0x504C5854, level=0, caps_magic=0x4D504334, build=0x48005D45)
for kv in sys.argv[2].split():
    k, x = kv.split('=')
    v[k] = int(x, 0)
for off, k in ((0x00, 'ctrl_magic'), (0x04, 'ctrl'), (0x08, 'read_magic'), (0x0C, 'read'),
               (0x18, 'plxt_magic'), (0x1C, 'level'), (0x70, 'caps_magic'), (0x74, 'build')):
    struct.pack_into('<I', b, off, v[k])
open(sys.argv[1], 'wb').write(bytes(b))
ff = bytearray(4096)
for off, m in ((0x100, 0x504C5853), (0x108, 0x504C5849), (0x110, 0x504C584D), (0x118, 0x504C5846),
               (0x128, 0x504C5844), (0x130, 0x504C584A)):
    struct.pack_into('<I', ff, off, m)
open(sys.argv[1] + '.ff', 'wb').write(bytes(ff))
EOF
    printf '%s\n' "$cn" > "$SB/CORENAME"
    printf 'state=attempted\ntime=1\nreason=gate-open probe\n' > "$SB/marker"
    cp "$SB/page.bin" "$SB/page.seed"
    sed "s#/dev/MiSTer_cmd#$SB/MiSTer_cmd#" "$LOADER_SRC" > "$SB/loader.sh"
    mkfifo "$SB/MiSTer_cmd"
    (
        IFS= read -r line < "$SB/MiSTer_cmd"
        printf '%s\n' "$line" > "$SB/load_line"
        python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(); print("0x%016X" % struct.unpack("<Q", d[0x128:0x130])[0])' \
            "$SB/page.bin" > "$SB/commit_at_load"
    ) &
    READER=$!
    env FAKE_DEVMEM_STUCK="$stuck" PATH="$SB/bin:$PATH" MAST_LOAD_FAILED="$SB/FAILED" \
        MAST_LOAD_MARKER="$SB/marker" MAST_LOAD_CORENAME="$SB/CORENAME" "$@" \
        timeout 30 sh "$SB/loader.sh" /media/fat/_Utility/Plex.rbf > "$SB/out" 2>&1
    PA_RC=$?
    for _ in $(seq 1 30); do kill -0 "$READER" 2>/dev/null || break; sleep 0.1; done
    kill "$READER" 2>/dev/null
    wait "$READER" 2>/dev/null
}
pa_gap() { # seconds from the last ring read (PLXT high) to the first commit write
    python3 - "$SB/page.bin.times" << 'EOF'
import sys
last = None
for line in open(sys.argv[1]):
    t, a = line.split(' ', 1)
    a = a.strip()
    if a == '0x3014001C 32':
        last = float(t)
    elif a == '0x30140128 32 0':
        print('%.3f' % (float(t) - last) if last is not None else 'none')
        break
EOF
}
pa_loaded() { [ "$(cat "$SB/load_line" 2>/dev/null)" = "load_core /media/fat/_Utility/Plex.rbf" ]; }
R6_SAMPLE='0x30140000 32
0x30140004 32
0x30140008 32
0x3014000C 32
0x30140018 32
0x3014001C 32'
# P: a drained ring: quiesced on 2 reads, the 200 ms quiet wait, P-a, the ONE load.
pa_run 0 Plex ""
[ "$PA_RC" = 0 ] || fail "P: loader rc $PA_RC: $(tr '\n' '|' < "$SB/out")"
[ "$(cat "$SB/commit_at_load" 2>/dev/null)" = 0x0000000000000000 ] ||
    fail "P: the commit at load_core is [$(cat "$SB/commit_at_load" 2>/dev/null)], want 0"
[ "$(cat "$SB/page.bin.argv" 2>/dev/null)" = "$(printf '0x30140070 32\n0x30140074 32\n%s\n%s\n0x30140128 32 0\n0x3014012C 32 0\n0x30140128 32\n0x3014012C 32' "$R6_SAMPLE" "$R6_SAMPLE")" ] ||
    fail "P: devmem argv [$(tr '\n' '|' < "$SB/page.bin.argv" 2>/dev/null)]"
gap=$(pa_gap)
awk -v g="$gap" 'BEGIN { exit !(g + 0 >= 0.200) }' ||
    fail "P: ${gap}s from the second matching read to P-a, want >= 0.200 (the R6 quiet wait)"
pa_loaded || fail "P: load line [$(cat "$SB/load_line" 2>/dev/null)]"
python3 - "$SB/page.seed" "$SB/page.bin" << 'EOF' || fail "P: bytes outside 0x30140128-12F changed"
import sys
a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
sys.exit(0 if a[:0x128] == b[:0x128] and a[0x130:] == b[0x130:] else 1)
EOF
grep -q '^MAST_LOAD: R6 quiesced (consumed == written, level 0: PLXB 0x504C5842:0x00001000 PLXR 0x504C5852:0x00001000 PLXT 0x504C5854:0x00000000, on 2 reads after [0-9]* ms); 200 ms quiet, then P-a and the ONE load$' "$SB/out" ||
    fail "P: no R6 quiesced line: $(grep 'R6' "$SB/out" | head -n 1)"
grep -q '^r6=quiesced (consumed == written, level 0: ' "$SB/marker" || fail "P: marker r6: $(tr '\n' ' ' < "$SB/marker")"
grep -q 'MAST_LOAD: audio command withdrawn before load_core' "$SB/out" || fail "P: no withdrawn line"
grep -q '!!!' "$SB/out" && fail "P: a loud line on a clean quiesce and withdraw"
P_GAP=$gap
rm -rf "$SB"
# P2: a read-back that stays non-zero gives ONE loud line, and the load still happens.
pa_run 1 Plex ""
[ "$(grep -c '^!!! MAST_LOAD withdraw failed' "$SB/out")" = 1 ] ||
    fail "P2: not ONE loud withdraw-failed line: $(tr '\n' '|' < "$SB/out")"
grep -q 'withdraw failed' "$SB/FAILED" 2>/dev/null || fail "P2: not in the FAILED file"
pa_loaded || fail "P2: the ONE load did not proceed"
rm -rf "$SB"
# P3: consumed never reaches written (a wedged decoder): ONE loud 'R6 NOT QUIESCED', marked, and
# the load still happens after the bound.
pa_run 0 Plex "read=0xF00 level=0x100" MAST_LOAD_DRAIN_MS=400
[ "$PA_RC" = 0 ] || fail "P3: loader rc $PA_RC"
[ "$(grep -c '^!!! MAST_LOAD R6 NOT QUIESCED: not-quiesced (consumed != written or level != 0 within 400 ms: PLXB 0x504C5842:0x00001000 PLXR 0x504C5852:0x00000F00 PLXT 0x504C5854:0x00000100) - the ONE load proceeds$' "$SB/out")" = 1 ] ||
    fail "P3: not ONE loud NOT QUIESCED line: $(grep 'R6' "$SB/out" | head -n 2 | tr '\n' '|')"
grep -q 'R6 NOT QUIESCED' "$SB/FAILED" 2>/dev/null || fail "P3: not in the FAILED file"
grep -q '^r6=not-quiesced (' "$SB/marker" || fail "P3: marker r6: $(tr '\n' ' ' < "$SB/marker")"
pa_loaded || fail "P3: the ONE load did not proceed"
[ "$(cat "$SB/commit_at_load" 2>/dev/null)" = 0x0000000000000000 ] || fail "P3: commit at load_core not 0"
rm -rf "$SB"
# P4: the core is still consuming: P-a only once consumed == written and level 0 (twice), then 200 ms.
pa_run 0 Plex "read=0xF00 level=0x100" FAKE_DRAIN_STEP=0x80
[ "$PA_RC" = 0 ] || fail "P4: loader rc $PA_RC"
[ "$(cat "$SB/page.bin.pa" 2>/dev/null)" = "read=0x00001000 ctrl=0x00001000 level=0x00000000" ] ||
    fail "P4: P-a ran with the ring at [$(cat "$SB/page.bin.pa" 2>/dev/null)], want consumed == written, level 0"
grep -q '^MAST_LOAD: R6 quiesced (consumed == written, level 0: ' "$SB/out" || fail "P4: no quiesced line"
gap=$(pa_gap)
awk -v g="$gap" 'BEGIN { exit !(g + 0 >= 0.200) }' || fail "P4: ${gap}s from the last read to P-a, want >= 0.200"
[ "$(grep -c '^0x3014000C 32$' "$SB/page.bin.argv")" -ge 4 ] || fail "P4: fewer than 4 ring samples"
pa_loaded || fail "P4: no load"
rm -rf "$SB"
# P5: nothing was ever fed (no PLXB): nothing to drain; quiet wait, P-a, load.
pa_run 0 Plex "ctrl_magic=0"
grep -q '^MAST_LOAD: R6 no-feed (CTRL \[0x00000000\] is not PLXB: nothing was fed, nothing to drain); 200 ms quiet' "$SB/out" ||
    fail "P5: no no-feed line: $(grep 'R6' "$SB/out" | head -n 1)"
grep -q '^r6=no-feed (' "$SB/marker" || fail "P5: marker r6"
pa_loaded || fail "P5: no load"
rm -rf "$SB"
# P6: R6 PORT SUSPECT (review 02:17): CORENAME Plex, CAPS 0x48005D45, a neighbour magic (PLXF) where
# PLXR belongs: ONE loud line, the D10 and 0x30140000-01F words, P-a, NO load, rc 96.
pa_run 0 Plex "read_magic=0x504C5846"
[ "$PA_RC" = 96 ] || fail "P6: loader rc $PA_RC, want 96"
pa_loaded && fail "P6: loaded on a suspect port"
[ -e "$SB/load_line" ] && fail "P6: something was written to MiSTer_cmd: $(cat "$SB/load_line")"
[ "$(grep -c '^!!! MAST_LOAD R6 PORT SUSPECT: port-suspect (PLXR/PLXT magic wrong on 2 reads, CORENAME Plex, CAPS build 0x48005D45: PLXB 0x504C5842:0x00001000 PLXR 0x504C5846:0x00001000 PLXT 0x504C5854:0x00000000) - NO core load: HPS reboot first' "$SB/out")" = 1 ] ||
    fail "P6: not ONE loud PORT SUSPECT line: $(grep 'R6' "$SB/out" | head -n 2 | tr '\n' '|')"
grep -q '^MAST_LOAD R6 D10 0x300FF100-13F: 0x504C5853 0x[0-9A-F]\{8\} 0x504C5849 0x[0-9A-F]\{8\} 0x504C584D 0x[0-9A-F]\{8\} 0x504C5846 0x[0-9A-F]\{8\} 0x[0-9A-F]\{8\} 0x[0-9A-F]\{8\} 0x504C5844 0x[0-9A-F]\{8\} 0x504C584A 0x[0-9A-F]\{8\} 0x[0-9A-F]\{8\} 0x[0-9A-F]\{8\}$' "$SB/out" ||
    fail "P6: no D10 dump: $(grep 'D10' "$SB/out")"
grep -q '^MAST_LOAD R6 0x30140000-01F: 0x504C5842 0x00001000 0x504C5846 0x00001000 0x[0-9A-F]\{8\} 0x[0-9A-F]\{8\} 0x504C5854 0x00000000$' "$SB/out" ||
    fail "P6: no 0x30140000-01F dump: $(grep '0x30140000-01F' "$SB/out")"
grep -q 'R6 PORT SUSPECT' "$SB/FAILED" 2>/dev/null || fail "P6: not in the FAILED file"
grep -q '^r6=port-suspect (' "$SB/marker" || fail "P6: marker r6"
[ "$(python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(); print("0x%016X" % struct.unpack("<Q", d[0x128:0x130])[0])' "$SB/page.bin")" = 0x0000000000000000 ] ||
    fail "P6: the stale commit was not withdrawn"
rm -rf "$SB"
# P7: the same bad magic is never flagged without the gate: CORENAME MENU, or Plex with a CAPS
# build outside kRecognisedFpgaBuilds. Not quiesced within the bound, and the load proceeds.
for g in "MENU 0x48005D45" "Plex 0x12345678"; do
    pa_run 0 "${g% *}" "read_magic=0x504C5846 build=${g#* }" MAST_LOAD_DRAIN_MS=400
    grep -q 'PORT SUSPECT' "$SB/out" && fail "P7(${g% *}): flagged without the gate"
    grep -q '^!!! MAST_LOAD R6 NOT QUIESCED: ' "$SB/out" || fail "P7(${g% *}): no NOT QUIESCED line"
    [ "$PA_RC" = 0 ] && pa_loaded || fail "P7(${g% *}): the ONE load did not proceed (rc $PA_RC)"
    rm -rf "$SB"
done
# The shipped loader is the guard's wrap of this source: r6_quiesce and pa_withdraw are the same
# bodies; pa_load runs them in order, each behind a guard check, and the load only after rc != 96.
W=$ROOT/scripts/misterplexd-mast-load.sh
for fn in r6_quiesce pa_withdraw; do
    [ "$(sed -n "/^$fn() {/,/^}/p" "$W")" = "$(sed -n "/^$fn() {/,/^}/p" "$ROOT/scripts/misterplexd-mast-load.src.sh")" ] ||
        fail "P: $fn differs between the source and the shipped loader"
done
pl=$(sed -n '/^pa_load() {$/,/^}$/p' "$W")
[ "$(grep -c . <<< "$pl")" = 7 ] &&
    grep -q '^  { _mlc_ok=; mlc_pre C:r6_quiesce .*; r6_quiesce; mlc_rc \$?; mlc_flagrc \$?; }$' <<< "$(sed -n 2p <<< "$pl")" &&
    [ "$(sed -n 3p <<< "$pl")" = '  r6rc=$?' ] &&
    grep -q '^  { _mlc_ok=; mlc_pre C:pa_withdraw .*; pa_withdraw; mlc_rc \$?; mlc_flagrc \$?; }$' <<< "$(sed -n 4p <<< "$pl")" &&
    [ "$(sed -n 5p <<< "$pl")" = '  if [ "$r6rc" = 96 ]; then return 96; fi' ] &&
    grep -q '^  { _mlc_ok=; mlc_guard "\$1" .*; echo "load_core \$1" > /dev/MiSTer_cmd; }$' <<< "$(sed -n 6p <<< "$pl")" ||
    fail "P: the shipped pa_load is not drain, withdraw, then the guarded load: $(tr '\n' '|' <<< "$pl" | cut -c1-200)"
grep -q '^{ _mlc_ok=; mlc_pre C:pa_load ' "$W" || fail "P: the shipped loader's pa_load call is not guarded"
# R6_BUILDS is kRecognisedFpgaBuilds, exactly.
want=$(grep -oE '^\s*\{0x[0-9A-F]{8}u, "' "$ROOT/host/libmisterplex/fpga_build_ids.hpp" | grep -oE '0x[0-9A-F]{8}' | sort | tr '\n' ' ')
have=$(sed -n 's/^R6_BUILDS="\(.*\)"$/\1/p' "$ROOT/scripts/misterplexd-mast-load.src.sh" | tr ' ' '\n' | sort | tr '\n' ' ')
[ -n "$want" ] && [ "$want" = "$have" ] || fail "P: R6_BUILDS [$have] is not kRecognisedFpgaBuilds [$want]"

# Launcher mutants (sed from the real script): each must make this test FAIL.
#   no-corename        : the M1 check never refuses                 -> F red
#   no-refused-state   : mr_refuse writes no marker (M2)            -> C, D, F red
#   no-loadfail-split  : every non-zero loader rc is a refusal (N1) -> G red
#   drop-on-lock       : a busy lock drops the hand-off (glass G1)  -> H red
#   no-yield           : the start launcher never yields (R1)       -> J red
#   no-withdraw        : the loader skips the P-a withdraw          -> P red (a loader mutant)
#   no-r6-quiet        : no 200 ms quiet wait before P-a (R6 guard)  -> P red (loader)
#   no-r6-drain        : no drain: P-a while the core still consumes -> P, P4 red (loader)
#   r6-ignore-magic    : a wrong PLXR/PLXT magic is never suspect    -> P6 red (loader)
#   r6-no-gate         : PORT SUSPECT without the CORENAME/CAPS gate -> P7 red (loader)
#   no-r6-refusal      : the launcher calls loader rc 96 load-failed -> D96 red
if [ -z "${MAST_LAUNCHER_MUTANT:-}" ]; then
    for m in no-corename no-refused-state no-loadfail-split drop-on-lock no-yield no-withdraw \
        no-r6-quiet no-r6-drain r6-ignore-magic r6-no-gate no-r6-refusal; do
        mut=$(mktemp /tmp/mast-launcher-mut.XXXXXX)
        orig=$BOOT
        case $m in
            no-corename) sed 's/if \[ "\$cn" != Plex \]; then/if false; then/' "$BOOT" > "$mut" ;;
            no-refused-state) sed "s/^  printf 'state=refused/  false \&\& printf 'state=refused/" "$BOOT" > "$mut" ;;
            no-loadfail-split) sed 's/^    97|98) mr_refuse/    *) mr_refuse/' "$BOOT" > "$mut" ;;
            drop-on-lock) sed 's/^    \[ "\$MODE" = mast-reload \] || { log "another/    false || { log "another/' "$BOOT" > "$mut" ;;
            no-yield) sed 's/^  \[ "\$MODE" = start \] || return 1$/  return 1/' "$BOOT" > "$mut" ;;
            no-r6-refusal) sed 's/^    96) mr_refuse "r6-port-suspect"/    96) mr_loadfail "$rc"; return 0; mr_refuse "r6-port-suspect"/' "$BOOT" > "$mut" ;;
            no-withdraw) orig=$LOADER_SRC; sed '/^pa_load() {$/,/^}$/{/^  pa_withdraw$/d}' "$LOADER_SRC" > "$mut" ;;
            no-r6-quiet) orig=$LOADER_SRC; sed '/^r6_quiesce() {$/,/^}$/{/^  usleep 200000 /d}' "$LOADER_SRC" > "$mut" ;;
            no-r6-drain) orig=$LOADER_SRC; sed 's/^  while \[ -z "\$R6" \]; do$/  R6="quiesced (mutant: no drain)"; while [ -z "$R6" ]; do/' "$LOADER_SRC" > "$mut" ;;
            r6-ignore-magic) orig=$LOADER_SRC; sed 's/^    if \[ "\$r0" != 0x504C5852 \] || \[ "\$l0" != 0x504C5854 \]; then$/    if false; then/' "$LOADER_SRC" > "$mut" ;;
            r6-no-gate) orig=$LOADER_SRC; sed 's/^      if \[ "\$gate" = 1 \] && \[ "\$bad" -ge 2 \]; then$/      if [ "$bad" -ge 2 ]; then/' "$LOADER_SRC" > "$mut" ;;
        esac
        if cmp -s "$mut" "$orig"; then
            fail "mutant $m: the sed did not apply"
        elif if [ "$orig" = "$LOADER_SRC" ]; then MAST_LOADER_SRC=$mut MAST_LAUNCHER_MUTANT=$m bash "$0"; else MAST_LAUNCHER=$mut MAST_LAUNCHER_MUTANT=$m bash "$0"; fi > "$mut.out" 2>&1; then
            fail "mutant $m survived"
        else
            echo "  mutant $m RED: $(grep -m1 '^FAIL' "$mut.out" | cut -c1-120)"
        fi
        rm -f "$mut" "$mut.out"
    done
fi

if [ "$fails" = 0 ]; then
    echo "PASS misterplexd-boot mast-reload sandbox (A one guarded load, B second request refused," \
        "C live daemon refused, D guard refusal (rc 97, 98) refused, D96 R6 PORT SUSPECT (rc 96)" \
        "refused, reboot first, E loader called once with the marker and CORENAME, F switched" \
        "core (MENU, NES) refused, no load, G failed load (rc 1, 99) load-failed, not refused," \
        "H hand-off waits for a busy lock then loads once, I busy lock -> launcher-busy, I2 stale" \
        "request untouched, J start launcher yields to a pending hand-off, J2 no yield to a stale one," \
        "J3 yields deep in START_WAIT, S waits keep their length without fractional sleep, precheck" \
        "(tick, 64-bit math: MATH_OK, simulated 32-bit MATH_BAD rc 1)," \
        "P R6 quiesced on 2 reads, ${P_GAP:-?}s quiet, P-a withdraws the stale commit before load_core," \
        "P2 failed withdraw loud, load proceeds, P3 R6 NOT QUIESCED loud, load proceeds, P4 P-a only" \
        "after the core drained, P5 no feed, P6 R6 PORT SUSPECT: dump, P-a, no load, P7 no flag" \
        "without CORENAME Plex and a known CAPS build)"
else
    echo "FAIL misterplexd-boot mast-reload sandbox ($fails)"
    exit 1
fi
