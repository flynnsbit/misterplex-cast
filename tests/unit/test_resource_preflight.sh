#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFLIGHT="$ROOT/scripts/test_resource_preflight.sh"
FIX="$ROOT/tests/fixtures/preflight"

run_preflight() {
  # Gate runners export the low-memory override; the fixtures must see the real guard.
  env -u MISTERPLEX_ALLOW_LOW_MEMORY_TESTS \
  MISTERPLEX_PREFLIGHT_SAMPLE_SECONDS=1 \
  MISTERPLEX_PREFLIGHT_VMSTAT="$FIX/vmstat_quiet" \
  MISTERPLEX_PREFLIGHT_MEMINFO="$1" \
  "$PREFLIGHT"
}

run_preflight "$FIX/meminfo_high_headroom" >/dev/null
echo "PASS preflight accepts synthetic high-headroom host"

run_preflight "$FIX/meminfo_low_steady_state" >/dev/null
echo "PASS preflight accepts synthetic 6269MB steady-state host with swap slack"

set +e
LOW_OUT="$(run_preflight "$FIX/meminfo_swap_exhausted" 2>&1)"
LOW_RC=$?
set -e
printf '%s\n' "$LOW_OUT"
if [[ "$LOW_RC" -ne 3 ]]; then
  echo "FAIL preflight swap-exhausted fixture: got rc=$LOW_RC want rc=3" >&2
  exit 1
fi
if ! grep -q "swap free" <<<"$LOW_OUT"; then
  echo "FAIL preflight swap-exhausted fixture: refusal did not cite swap exhaustion" >&2
  exit 1
fi
echo "PASS preflight refuses synthetic swap-exhausted host"
