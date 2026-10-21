#!/usr/bin/env bash
# Same-interval CPU attribution for misterplexd (and optional second process).
#
# Parent Sweep 113 proved misterplexd idle burns ~half of dual-core capacity.
# Earlier 136%/131% per-process splits were arithmetically impossible (withdrawn).
# This script samples /proc/stat AND /proc/<pid>/task/*/stat over ONE wall
# interval so jiffies are comparable.
#
# Usage (on DE10-Nano, as root or the misterplexd user):
#   scripts/daemon_thread_cpu.sh                  # auto-find misterplexd
#   scripts/daemon_thread_cpu.sh <pid>
#   scripts/daemon_thread_cpu.sh <pid> 5          # 5 second window
#   INTERVAL=10 scripts/daemon_thread_cpu.sh
#
# Output: system idle%, per-thread utime+stime jiffies and % of one core,
# thread comm names. Does not print tokens or mutate the device.

set -euo pipefail

INTERVAL="${INTERVAL:-${2:-3}}"
PID="${1:-}"

if [[ -z "$PID" ]]; then
  PID="$(pidof misterplexd 2>/dev/null | awk '{print $1}')" || true
fi
if [[ -z "$PID" || ! -d "/proc/$PID" ]]; then
  echo "daemon_thread_cpu: misterplexd pid not found (pass pid as \$1)" >&2
  exit 2
fi

if ! [[ "$INTERVAL" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "daemon_thread_cpu: INTERVAL must be a number of seconds" >&2
  exit 2
fi

read_stat_totals() {
  # /proc/stat first line: cpu user nice system idle iowait irq softirq ...
  # shellcheck disable=SC2034
  local cpu user nice system idle iowait irq softirq steal guest guest_nice
  read -r cpu user nice system idle iowait irq softirq steal guest guest_nice < /proc/stat
  local busy=$((user + nice + system + irq + softirq + steal))
  local idlej=$((idle + iowait))
  printf '%s %s\n' "$busy" "$idlej"
}

# task_id utime stime comm
read_tasks() {
  local p="$1"
  local tdir comm ut st
  for tdir in /proc/"$p"/task/*; do
    [[ -d "$tdir" ]] || continue
    local tid
    tid="$(basename "$tdir")"
    comm="$(cat "$tdir/comm" 2>/dev/null || echo '?')"
    # stat: pid (comm) state ppid ... utime stime — comm may contain spaces/parens
    # Field positions after the last ')': state ppid pgrp session tty tpgid flags
    # minflt cminflt majflt cmajflt utime stime ...
    local line rest
    line="$(cat "$tdir/stat" 2>/dev/null || true)"
    [[ -n "$line" ]] || continue
    rest="${line##*)}"
    # After last ')': $1=state ... $12=utime $13=stime
    # shellcheck disable=SC2086
    set -- $rest
    ut="${12:-0}"
    st="${13:-0}"
    printf '%s %s %s %s\n' "$tid" "$ut" "$st" "$comm"
  done
}

comm_of() {
  cat /proc/"$1"/comm 2>/dev/null || echo '?'
}

HZ="$(getconf CLK_TCK 2>/dev/null || echo 100)"
PROC_COMM="$(comm_of "$PID")"

echo "daemon_thread_cpu: pid=$PID comm=$PROC_COMM interval_s=$INTERVAL hz=$HZ"
echo "--- sample A ---"
STAT_A="$(read_stat_totals)"
mapfile -t TASKS_A < <(read_tasks "$PID")
busy_a=$(awk '{print $1}' <<<"$STAT_A")
idle_a=$(awk '{print $2}' <<<"$STAT_A")
echo "system busy_j=$busy_a idle_j=$idle_a"
echo "threads=${#TASKS_A[@]}"

sleep "$INTERVAL"

echo "--- sample B ---"
STAT_B="$(read_stat_totals)"
mapfile -t TASKS_B < <(read_tasks "$PID")
busy_b=$(awk '{print $1}' <<<"$STAT_B")
idle_b=$(awk '{print $2}' <<<"$STAT_B")

db=$((busy_b - busy_a))
di=$((idle_b - idle_a))
tot=$((db + di))
if [[ "$tot" -le 0 ]]; then
  echo "daemon_thread_cpu: zero system delta (clock stuck?)" >&2
  exit 1
fi
# integer percent *10 for one decimal
idle_pct10=$(( di * 1000 / tot ))
idle_pct=$((idle_pct10 / 10))
idle_frac=$((idle_pct10 % 10))
echo "system: delta_busy=$db delta_idle=$di idle%=${idle_pct}.${idle_frac}"

echo "--- per-thread (same interval) ---"
printf '%6s %8s %8s %8s %s\n' "tid" "d_ut" "d_st" "%core" "comm"

# Build associative deltas from B-A by tid
declare -A A_UT A_ST A_COMM
for row in "${TASKS_A[@]}"; do
  # tid ut st comm(maybe with spaces — only first three numeric fields matter)
  tid=$(awk '{print $1}' <<<"$row")
  ut=$(awk '{print $2}' <<<"$row")
  st=$(awk '{print $3}' <<<"$row")
  cm=$(awk '{print substr($0, index($0,$4))}' <<<"$row")
  A_UT[$tid]=$ut
  A_ST[$tid]=$st
  A_COMM[$tid]=$cm
done

sum_j=0
declare -a LINES=()
for row in "${TASKS_B[@]}"; do
  tid=$(awk '{print $1}' <<<"$row")
  ut=$(awk '{print $2}' <<<"$row")
  st=$(awk '{print $3}' <<<"$row")
  cm=$(awk '{print substr($0, index($0,$4))}' <<<"$row")
  aut=${A_UT[$tid]:-0}
  ast=${A_ST[$tid]:-0}
  du=$((ut - aut))
  ds=$((st - ast))
  dj=$((du + ds))
  [[ "$dj" -lt 0 ]] && dj=0
  sum_j=$((sum_j + dj))
  # % of one core = jiffies / (INTERVAL * HZ) * 100
  # use awk for float
  pct=$(awk -v j="$dj" -v iv="$INTERVAL" -v hz="$HZ" 'BEGIN{printf "%.1f", (j * 100.0) / (iv * hz)}')
  LINES+=("$(printf '%6s %8s %8s %7s%% %s' "$tid" "$du" "$ds" "$pct" "$cm")")
done

# Sort by total jiffies desc (third+second columns) — simple reprint sorted
printf '%s\n' "${LINES[@]}" | sort -k2,2nr -k3,3nr

proc_pct=$(awk -v j="$sum_j" -v iv="$INTERVAL" -v hz="$HZ" 'BEGIN{printf "%.1f", (j * 100.0) / (iv * hz)}')
echo "--- summary ---"
echo "pid=$PID comm=$PROC_COMM thread_jiffies=$sum_j approx_%one_core=$proc_pct system_idle%=${idle_pct}.${idle_frac}"
echo "note: %one_core is relative to a single core; on dual-core, 100% == one full core."
