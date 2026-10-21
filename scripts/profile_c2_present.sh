#!/usr/bin/env bash
# Bounded present-path profiler for the MiSTer. Prefer perf, fall back to
# strace -c, then to /proc thread CPU deltas. Does not touch SPI or restart.
set -euo pipefail

HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
DURATION="${DURATION:-30}"
OUT_DIR="/media/fat/misterplex/profiles"

sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$USER@$HOST" bash -s -- "$DURATION" "$OUT_DIR" <<'REMOTE'
set -euo pipefail
dur="$1"
out_dir="$2"
pid="$(ps | awk '/misterplexd --name/{print $1; exit}')"
if [[ -z "$pid" ]]; then
  echo "misterplexd not running" >&2
  exit 1
fi
mkdir -p "$out_dir"
echo "pid=$pid duration=${dur}s"

if command -v perf >/dev/null 2>&1; then
  data="$out_dir/c2_perf.data"
  perf record -F 99 -p "$pid" -o "$data" -- sleep "$dur" || true
  perf report -i "$data" --stdio --no-children | head -80 || true
  exit 0
fi

if command -v strace >/dev/null 2>&1; then
  out="$out_dir/c2_strace.txt"
  strace -qq -c -f -p "$pid" -o "$out" &
  spid=$!
  sleep "$dur"
  kill -INT "$spid" 2>/dev/null || true
  wait "$spid" 2>/dev/null || true
  cat "$out"
  exit 0
fi

hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
snap=""
for t in /proc/"$pid"/task/*; do
  tid="${t##*/}"
  vals="$(cut -d' ' -f14,15 "$t/stat")"
  snap="$snap $tid:${vals% *}:${vals#* }"
done
sleep "$dur"
for t in /proc/"$pid"/task/*; do
  tid="${t##*/}"
  vals="$(cut -d' ' -f14,15 "$t/stat")"
  u="${vals% *}"
  s="${vals#* }"
  rec=""
  for x in $snap; do
    case "$x" in "$tid":*) rec="$x" ;; esac
  done
  [[ -z "$rec" ]] && continue
  ou="${rec#*:}"
  os="${ou#*:}"
  ou="${ou%%:*}"
  du=$((u - ou))
  ds=$((s - os))
  cpu=$(((du + ds) * 100 / hz / dur))
  printf 'tid=%s user_ticks=%d sys_ticks=%d cpu_pct=%d comm=%s wchan=%s\n' \
    "$tid" "$du" "$ds" "$cpu" "$(cat "$t/comm")" "$(cat "$t/wchan" 2>/dev/null)"
done | sort -k4 -nr
REMOTE
