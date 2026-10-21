#!/usr/bin/env bash
# Device-side idle thread budget gate for misterplexd.
#
# Fails (rc=1) if any thread burns more than MAX_CORE_PCT of one core over
# INTERVAL seconds while the daemon should be idle (no playback). This is the
# test that would have caught Sweep 114's 108% unnamed GDM spin — the unit
# suite alone cannot see a live busy-loop on the DE10.
#
# Usage (on device, daemon running, no cast/play):
#   INTERVAL=5 MAX_CORE_PCT=15 ./scripts/check_idle_thread_budget.sh
#   ./scripts/check_idle_thread_budget.sh <pid>
#
# Also fails if a thread's lifetime nonvoluntary_ctxt_switches dominate
# voluntary by more than NV_RATIO (default 50) AND the thread used >5% core
# in the sample — the mechanical signature of a pure busy-spin.
#
# Exit 77 = soft-skip (no daemon). Exit 0 = within budget. Exit 1 = FAIL.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INTERVAL="${INTERVAL:-5}"
MAX_CORE_PCT="${MAX_CORE_PCT:-15}"
NV_RATIO="${NV_RATIO:-50}"
PID="${1:-}"

if [[ -z "$PID" ]]; then
  PID="$(pidof misterplexd 2>/dev/null | awk '{print $1}')" || true
fi
if [[ -z "$PID" || ! -d "/proc/$PID" ]]; then
  echo "check_idle_thread_budget: SKIP-NOT-PASS no misterplexd (exit 77)"
  exit 77
fi

HZ="$(getconf CLK_TCK 2>/dev/null || echo 100)"

read_tasks() {
  local p="$1"
  for tdir in /proc/"$p"/task/*; do
    [[ -d "$tdir" ]] || continue
    local tid comm line rest ut st
    tid="$(basename "$tdir")"
    comm="$(cat "$tdir/comm" 2>/dev/null || echo '?')"
    line="$(cat "$tdir/stat" 2>/dev/null || true)"
    [[ -n "$line" ]] || continue
    rest="${line##*)}"
    # shellcheck disable=SC2086
    set -- $rest
    ut="${12:-0}"
    st="${13:-0}"
    printf '%s %s %s %s\n' "$tid" "$ut" "$st" "$comm"
  done
}

echo "check_idle_thread_budget: pid=$PID interval=${INTERVAL}s max_core_pct=$MAX_CORE_PCT nv_ratio=$NV_RATIO"

mapfile -t A < <(read_tasks "$PID")
sleep "$INTERVAL"
mapfile -t B < <(read_tasks "$PID")

declare -A AUT AST
for row in "${A[@]}"; do
  tid=$(awk '{print $1}' <<<"$row")
  AUT[$tid]=$(awk '{print $2}' <<<"$row")
  AST[$tid]=$(awk '{print $3}' <<<"$row")
done

fail=0
printf '%6s %8s %8s %8s %s\n' "tid" "d_ut" "d_st" "%core" "comm"
for row in "${B[@]}"; do
  tid=$(awk '{print $1}' <<<"$row")
  ut=$(awk '{print $2}' <<<"$row")
  st=$(awk '{print $3}' <<<"$row")
  cm=$(awk '{print substr($0, index($0,$4))}' <<<"$row")
  du=$((ut - ${AUT[$tid]:-0}))
  ds=$((st - ${AST[$tid]:-0}))
  [[ "$du" -lt 0 ]] && du=0
  [[ "$ds" -lt 0 ]] && ds=0
  dj=$((du + ds))
  pct=$(awk -v j="$dj" -v iv="$INTERVAL" -v hz="$HZ" 'BEGIN{printf "%.1f", (j*100.0)/(iv*hz)}')
  printf '%6s %8s %8s %7s%% %s\n' "$tid" "$du" "$ds" "$pct" "$cm"

  hot=$(awk -v p="$pct" -v m="$MAX_CORE_PCT" 'BEGIN{exit !(p+0 > m+0)}' && echo 1 || echo 0)
  if [[ "$hot" == "1" ]]; then
    echo "FAIL thread tid=$tid comm=$cm used ${pct}% one-core > max ${MAX_CORE_PCT}%"
    fail=1
  fi

  # Sweep 114: hot thread kept default comm "misterplexd" and evaded named
  # analysis. Any thread using >5% core MUST have an mpx-* name (or mpx-main).
  significant_name=$(awk -v p="$pct" 'BEGIN{exit !(p+0 > 5.0)}' && echo 1 || echo 0)
  if [[ "$significant_name" == "1" ]]; then
    case "$cm" in
      mpx-*) ;;
      *)
        echo "FAIL tid=$tid comm='$cm' burns ${pct}% but is unnamed (expected mpx-*)"
        fail=1
        ;;
    esac
  fi

  # Lifetime voluntary vs nonvoluntary (spin signature).
  stfile="/proc/$PID/task/$tid/status"
  if [[ -r "$stfile" ]]; then
    vol=$(awk '/^voluntary_ctxt_switches:/{print $2}' "$stfile")
    nvol=$(awk '/^nonvoluntary_ctxt_switches:/{print $2}' "$stfile")
    vol=${vol:-0}
    nvol=${nvol:-0}
    # Only enforce ratio when the thread actually burned CPU this interval.
    significant=$(awk -v p="$pct" 'BEGIN{exit !(p+0 > 5.0)}' && echo 1 || echo 0)
    if [[ "$significant" == "1" && "$vol" -ge 0 ]]; then
      # nvol > NV_RATIO * max(vol,1)
      base="$vol"
      if [[ "$base" -lt 1 ]]; then
        base=1
      fi
      lim=$((NV_RATIO * base))
      if [[ "$nvol" -gt "$lim" ]]; then
        echo "FAIL tid=$tid comm=$cm spin signature nonvoluntary=$nvol voluntary=$vol ratio_cap=$lim"
        fail=1
      fi
    fi
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "check_idle_thread_budget: FAIL"
  exit 1
fi
echo "check_idle_thread_budget: OK"
exit 0
