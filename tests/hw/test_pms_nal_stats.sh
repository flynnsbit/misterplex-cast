#!/usr/bin/env bash
# Live PMS NAL sizing/jitter probe for sizing the host→FPGA bitstream ring.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONF_FILE="${MISTERPLEX_CONF:-${MISTER_CONF:-}}"
if [[ -z "$CONF_FILE" ]]; then
  for c in "$ROOT/assets/misterplex.conf" "$HOME/.config/misterplex/misterplex.conf"; do
    [[ -f "$c" ]] && CONF_FILE="$c" && break
  done
fi
conf_val() {
  local key="$1" file="$2"
  [[ -n "$file" && -f "$file" ]] || return 0
  awk -F= -v k="$key" '$1==k {sub(/^[ \t]+/, "", $2); sub(/[ \t\r]+$/, "", $2); print $2; exit}' "$file"
}
PLEX_BASE="${PLEX_BASE:-$(conf_val PLEX_BASE "$CONF_FILE")}"
PLEX_TOKEN="${PLEX_TOKEN:-$(conf_val PLEX_TOKEN "$CONF_FILE")}"
MISTERPLEX_BASELINE_KEY="${MISTERPLEX_BASELINE_KEY:-${PLEX_KEY:-}}"
if [[ -z "${PLEX_BASE:-}" || -z "${PLEX_TOKEN:-}" || -z "${MISTERPLEX_BASELINE_KEY:-}" ]]; then
  echo "SKIP-NOT-PASS test_pms_nal_stats: set PLEX_BASE, PLEX_TOKEN, and MISTERPLEX_BASELINE_KEY for the live PMS NAL stats probe." >&2
  echo "SKIP-NOT-PASS: this target is intentionally outside make unit and a skip must not be reported as pass." >&2
  exit 77
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "SKIP-NOT-PASS test_pms_nal_stats: ffmpeg is required to extract the delivered H.264 stream." >&2
  exit 77
fi
exec "$ROOT/build/pms_nal_stats" \
  --base "$PLEX_BASE" \
  --token "$PLEX_TOKEN" \
  --key "$MISTERPLEX_BASELINE_KEY" \
  --seconds "${MISTERPLEX_NAL_STATS_SECONDS:-30}"
