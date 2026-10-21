#!/usr/bin/env bash
# Token-window helper: enable PRESENT_PROFILE, deploy the current daemon, run the
# fixed measurement script, then restore the original conf and redeploy so normal
# playback has zero profiling overhead.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
CONF="/media/fat/misterplex/misterplex.conf"
BACKUP="/media/fat/misterplex/misterplex.conf.c2profile.bak"

KEY=""
OFFSET_MS=0
SETTLE_SEC=30
DURATION_SEC=180

usage() {
  cat <<'USAGE'
Usage: scripts/run_c2_present_profile_window.sh --key /library/metadata/N [options]

Options are forwarded to measure_c2_pixel_path.sh:
  --offset-ms MS
  --settle-sec N
  --duration-sec N
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --key) KEY="${2:?}"; shift 2 ;;
    --offset-ms) OFFSET_MS="${2:?}"; shift 2 ;;
    --settle-sec) SETTLE_SEC="${2:?}"; shift 2 ;;
    --duration-sec) DURATION_SEC="${2:?}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$KEY" ]]; then
  echo "--key is required" >&2
  usage >&2
  exit 2
fi

ssh_mister() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$USER@$HOST" "$@"
}

restore_conf() {
  ssh_mister "if [ -f '$BACKUP' ]; then cp -f '$BACKUP' '$CONF'; rm -f '$BACKUP'; fi" || true
}

trap restore_conf EXIT

ssh_mister "cp -f '$CONF' '$BACKUP'; if grep -q '^PRESENT_PROFILE=' '$CONF'; then sed -i 's/^PRESENT_PROFILE=.*/PRESENT_PROFILE=1/' '$CONF'; else printf '\nPRESENT_PROFILE=1\n' >>'$CONF'; fi"

MISTER_HOST="$HOST" MISTER_USER="$USER" MISTER_PASS="$PASS" "$ROOT/scripts/deploy_misterplexd.sh"
MISTER_HOST="$HOST" MISTER_USER="$USER" MISTER_PASS="$PASS" \
  "$ROOT/scripts/measure_c2_pixel_path.sh" \
  --key "$KEY" --offset-ms "$OFFSET_MS" --settle-sec "$SETTLE_SEC" \
  --duration-sec "$DURATION_SEC" --command-id c2presentprofile

restore_conf
trap - EXIT
MISTER_HOST="$HOST" MISTER_USER="$USER" MISTER_PASS="$PASS" "$ROOT/scripts/deploy_misterplexd.sh"
