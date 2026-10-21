#!/usr/bin/env bash
# Serialize heavy local validation and wait for the existing resource preflight
# to pass. This never overrides a refusal; it only backs off until the hazard
# clears or exits with the same refusal class.
set -euo pipefail

usage() {
  cat <<USAGE
Usage: $(basename "$0") -- command [args...]

Environment:
  MISTERPLEX_PREFLIGHT_BACKOFF_ATTEMPTS  Attempts before refusing (default: 20)
  MISTERPLEX_PREFLIGHT_BACKOFF_SLEEP     Seconds between attempts (default: 30)
  MISTERPLEX_PREFLIGHT_LOCK              Lock path (default: build/resource-preflight.lock)

This is a queue/backoff wrapper, not an override. A preflight refusal remains a
refusal; the wrapper just waits under a repository-local lock so multiple
Verilator-heavy runs do not stampede the same 16 GB host.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ "${1:-}" == "--" ]]; then
  shift
fi
if [[ $# -eq 0 ]]; then
  usage >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ATTEMPTS="${MISTERPLEX_PREFLIGHT_BACKOFF_ATTEMPTS:-20}"
SLEEP_SECONDS="${MISTERPLEX_PREFLIGHT_BACKOFF_SLEEP:-30}"
LOCK_PATH="${MISTERPLEX_PREFLIGHT_LOCK:-$ROOT/build/resource-preflight.lock}"

if ! [[ "$ATTEMPTS" =~ ^[0-9]+$ ]] || (( ATTEMPTS < 1 )); then
  echo "MISTERPLEX_PREFLIGHT_BACKOFF_ATTEMPTS must be a positive integer" >&2
  exit 2
fi
if ! [[ "$SLEEP_SECONDS" =~ ^[0-9]+$ ]]; then
  echo "MISTERPLEX_PREFLIGHT_BACKOFF_SLEEP must be a non-negative integer" >&2
  exit 2
fi
if ! command -v flock >/dev/null 2>&1; then
  echo "RESOURCE_PREFLIGHT_REFUSED: flock is required for serialized heavy validation" >&2
  exit 4
fi

mkdir -p "$(dirname "$LOCK_PATH")"
exec 9>"$LOCK_PATH"
echo "resource-preflight: waiting for exclusive heavy-validation slot: $LOCK_PATH" >&2
flock 9
echo "resource-preflight: acquired slot for: $*" >&2

for attempt in $(seq 1 "$ATTEMPTS"); do
  rc=0
  bash "$ROOT/scripts/test_resource_preflight.sh" || rc=$?
  if [[ "$rc" -eq 0 ]]; then
    echo "resource-preflight: preflight OK on attempt $attempt; running command" >&2
    exec "$@"
  fi
  if [[ "$rc" -ne 3 ]]; then
    echo "resource-preflight: non-resource preflight failure rc=$rc; not retrying" >&2
    exit "$rc"
  fi
  if (( attempt == ATTEMPTS )); then
    echo "resource-preflight: still refused after $ATTEMPTS attempts; not overriding" >&2
    exit 3
  fi
  mem="$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo 2>/dev/null || echo unknown)"
  swap="$(awk '/^SwapFree:/{print int($2/1024)}' /proc/meminfo 2>/dev/null || echo unknown)"
  echo "resource-preflight: refusal is correct; backoff ${SLEEP_SECONDS}s (attempt $attempt/$ATTEMPTS, MemAvailable=${mem}MB SwapFree=${swap}MB)" >&2
  sleep "$SLEEP_SECONDS"
done
