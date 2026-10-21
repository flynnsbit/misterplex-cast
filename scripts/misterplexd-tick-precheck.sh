#!/bin/sh
# misterplexd-tick-precheck.sh - read-only R4 lease-start precheck (review 00:18 P1, release 00:26).
# misterplexd-boot-mast.sh (the MAST-WEDGE lab launcher) paces its waits with tick() (below, the same line). Every wait is bounded by
# the clock, but a tick that does not pace would spin. On this box, 10 ticks must take about 1 s
# (0.8-3.0 s). Bare 'sleep 0.1' is timed too, for the record; the launcher no longer depends on it.
# review 03:16 N2: the guarded loader's R6 drain (misterplexd-mast-load.sh) needs 64-bit shell
# math, the same expression as its own check: $((0xFFFFFFFF)) must be 4294967295, or every load
# logs 'R6 NOT QUIESCED (this shell's math is not 64-bit ...)' and judges nothing.
# Prints TICK_OK and MATH_OK; any TICK_BAD or MATH_BAD exits 1: then STOP, run no MAST row.
tick() { usleep 100000 2>/dev/null || sleep 0.1 2>/dev/null || sleep 1; }
up_cs() { awk '{ printf "%d", $1 * 100 }' /proc/uptime; }
fmt() { echo "$(($1 / 100)).$(printf '%02d' $(($1 % 100)))s"; }
via=sleep
usleep 1 2>/dev/null && via=usleep
t0=$(up_cs)
i=0
while [ $i -lt 10 ]; do tick; i=$((i + 1)); done
e=$(($(up_cs) - t0))
t0=$(up_cs)
i=0
while [ $i -lt 10 ]; do sleep 0.1 2>/dev/null; i=$((i + 1)); done
s=$(($(up_cs) - t0))
info="10 ticks = $(fmt "$e") via $via; 10 x 'sleep 0.1' = $(fmt "$s") (info)"
rc=0
if [ "$e" -ge 80 ] && [ "$e" -le 300 ]; then
    echo "TICK_OK $info"
else
    echo "TICK_BAD $info - STOP: run no MAST row"
    rc=1
fi
m=$(echo "$((0xFFFFFFFF))" 2>/dev/null)
if [ "$m" = 4294967295 ]; then
    echo "MATH_OK \$((0xFFFFFFFF)) = $m (64-bit shell math: the R6 drain can judge the ring)"
else
    echo "MATH_BAD \$((0xFFFFFFFF)) = [$m], want 4294967295 - the R6 drain cannot judge - STOP: run no MAST row"
    rc=1
fi
exit $rc
