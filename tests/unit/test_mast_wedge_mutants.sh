#!/usr/bin/env bash
# MAST-WEDGE e2e mutants (review 21:14; release 21:16/21:17). Each test-only MUT_MAST_* daemon
# build (AUDIO_RECOVERY_DEFS, its own output dir) must FAIL its audio-recovery row:
#   MUT_MAST_NO_GATE_PROBE  no gate-open probe                    -> wedge_reload
#   MUT_MAST_STUCK_IS_LIVE  a decodable but stuck MAST is "live"  -> wedge_reload
#   MUT_MAST_KEEP_FDS       the launcher inherits the daemon's fds -> wedge_reload (0-misterplexd)
#   MUT_MAST_BLIND_RETRY    a wedged begin is retried 5 times     -> wedge_twice
#   MUT_MAST_RELOAD_LOOP    the marker is ignored                 -> wedge_twice (second hand-off)
#   MUT_MAST_RECOVER_FROM_REQUESTED  a live probe calls a hand-off that was never taken
#                           recovered (glass G1 23:49)            -> handoff_lost
#   MUT_MAST_SILENT_NO_GATE a gate that never opens after the reload is never a failed cure
#                           (glass G2 01:03; R5a/R5d)             -> reload_no_answer
#   MUT_MAST_NO_EXIT_WITHDRAW  an exit leaves the audio command valid in DDR (R5f) -> wedge_reload
# Launcher mutants (a sed copy of scripts/misterplexd-boot-mast.sh run by boot_race, MAST_BOOT_SCRIPT):
#   LAUNCHER_DROP_ON_LOCK   a busy launcher lock drops the hand-off (glass G1 23:49) -> boot_race
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${MAST_MUT_OUT:-$ROOT/build/mast_wedge_mutants}
MUTANTS=${MAST_MUTANTS:-MUT_MAST_NO_GATE_PROBE MUT_MAST_STUCK_IS_LIVE MUT_MAST_KEEP_FDS MUT_MAST_BLIND_RETRY MUT_MAST_RELOAD_LOOP MUT_MAST_RECOVER_FROM_REQUESTED MUT_MAST_SILENT_NO_GATE MUT_MAST_NO_EXIT_WITHDRAW LAUNCHER_DROP_ON_LOCK}
declare -A row=(
    [MUT_MAST_NO_GATE_PROBE]=wedge_reload
    [MUT_MAST_STUCK_IS_LIVE]=wedge_reload
    [MUT_MAST_KEEP_FDS]=wedge_reload
    [MUT_MAST_BLIND_RETRY]=wedge_twice
    [MUT_MAST_RELOAD_LOOP]=wedge_twice
    [MUT_MAST_RECOVER_FROM_REQUESTED]=handoff_lost
    [MUT_MAST_SILENT_NO_GATE]=reload_no_answer
    [MUT_MAST_NO_EXIT_WITHDRAW]=wedge_reload
    [LAUNCHER_DROP_ON_LOCK]=boot_race
)
mkdir -p "$OUT"
n=0
for m in $MUTANTS; do
    out=$OUT/$m.out
    rc=0
    defs=-D$m
    boot=$ROOT/scripts/misterplexd-boot-mast.sh
    if [ "$m" = LAUNCHER_DROP_ON_LOCK ]; then
        defs=
        boot=$OUT/misterplexd-boot.$m.sh
        sed 's/^    \[ "\$MODE" = mast-reload \] || { log "another/    false || { log "another/' \
            "$ROOT/scripts/misterplexd-boot-mast.sh" > "$boot"
        if cmp -s "$boot" "$ROOT/scripts/misterplexd-boot-mast.sh"; then
            echo "MUTANT NOT APPLIED: $m (the sed matched nothing)" >&2
            exit 1
        fi
    fi
    AUDIO_RECOVERY_DEFS=$defs AUDIO_RECOVERY_OUT=$OUT/$m AUDIO_RECOVERY_SCENARIOS=${row[$m]} \
        MAST_BOOT_SCRIPT=$boot bash "$ROOT/tests/unit/test_audio_recovery_e2e.sh" > "$out" 2>&1 || rc=$?
    if grep -q 'test_audio_recovery_e2e: SKIP' "$out"; then
        echo "test_mast_wedge_mutants: SKIP (needs host libav and ffmpeg)"
        exit 0
    fi
    if [ "$rc" = 0 ]; then
        echo "MUTANT SURVIVED: $m (${row[$m]})" >&2
        exit 1
    fi
    echo "  $m RED on ${row[$m]}: $(grep -m1 '^FAIL' "$out" | cut -c1-200)"
    n=$((n + 1))
done
echo "test_mast_wedge_mutants: OK ($n mutants RED)"
