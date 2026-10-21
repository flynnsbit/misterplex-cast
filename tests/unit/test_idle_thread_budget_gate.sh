#!/usr/bin/env bash
# Host-side gate for scripts/check_idle_thread_budget.sh (L30 regression barrier).
# Does NOT need a live misterplexd. Asserts the script encodes the Sweep 114
# failure modes: hot unnamed thread + nonvoluntary-dominated spin ratio.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/scripts/check_idle_thread_budget.sh"
fail=0
check() {
  if ! grep -qE "$2" "$SCRIPT"; then
    echo "FAIL: $1 — pattern not found: $2"
    fail=1
  else
    echo "OK: $1"
  fi
}
[[ -x "$SCRIPT" ]] || chmod +x "$SCRIPT"
check "exists" "."
check "max core pct" 'MAX_CORE_PCT'
check "nonvoluntary ratio" 'nonvoluntary_ctxt_switches'
check "voluntary ratio" 'voluntary_ctxt_switches'
check "unnamed hot thread fail" 'mpx-\*'
check "spin signature fail" 'spin signature'
check "soft-skip 77" 'exit 77'
# Negative: script must not treat skip as pass in its own OK path
if grep -n 'exit 0' "$SCRIPT" | grep -qi skip; then
  echo "FAIL: skip path must not exit 0"
  fail=1
fi
# RED twin: a naive script without unnamed check would fail this source gate
if ! grep -q "unnamed" "$SCRIPT" && ! grep -q 'mpx-\*' "$SCRIPT"; then
  echo "FAIL: missing unnamed/mpx-* guard"
  fail=1
fi
if [[ "$fail" -ne 0 ]]; then
  echo "test_idle_thread_budget_gate: FAIL"
  exit 1
fi
echo "test_idle_thread_budget_gate: OK"
exit 0
