#!/usr/bin/env bash
# Deploy static ARM misterplexd to MiSTer and restart.
set -euo pipefail
# T-DEPLOYD-ARGS (review 19:40/19:41, review 19:41): this script takes no arguments, and the
# target host has no default. Any argument (e.g. --prebuilt-candidate), or an unset, empty or
# malformed MISTER_HOST, is refused here, before any external command (make, ssh) runs.
# Name the host with MISTER_HOST.
if (($#)); then
  printf 'deploy_misterplexd.sh: takes no arguments, refusing: %s (nothing built or deployed)\n' "$*" >&2
  exit 2
fi
HOST="${MISTER_HOST:-}"
if [[ -z "$HOST" ]]; then
  printf 'deploy_misterplexd.sh: MISTER_HOST is unset or empty; name the target host (nothing built or deployed)\n' >&2
  exit 2
fi
# C-1 (review 19:52): only a canonical IPv4 dotted quad is a host here (octets 0-255, no
# leading zeros; no user@, port, whitespace, trailing dot or hostname). C-2 (H264/review 20:07):
# explicit digit lists, never ranges: in a UTF-8 locale a bash [[ =~ ]] range like [0-9] also
# matches non-ASCII digits.
octet='(25[012345]|2[01234][0123456789]|1[0123456789][0123456789]|[123456789][0123456789]|[0123456789])'
canonical="^${octet}\.${octet}\.${octet}\.${octet}\$"
if [[ ! $HOST =~ $canonical ]]; then
  printf 'deploy_misterplexd.sh: MISTER_HOST=%q is a malformed host; give a canonical IPv4 dotted quad (nothing built or deployed)\n' "$HOST" >&2
  exit 2
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
BIN="$ROOT/build/arm/misterplexd"
PLAYER_ID="${MISTERPLEX_ID:-misterplex-dev}"
PMS_URL="${PLEX_BASE:-${PMS_URL:-}}"

# Always let make decide. Guarding this with `if [[ ! -f "$BIN" ]]` meant that
# once the binary existed it was never rebuilt again, so every subsequent deploy
# silently shipped a stale daemon and "verified" fixes that were not on the box.
export PATH="${PATH}:${ARM_TOOLCHAIN_BIN:-/opt/arm-gnu-toolchain/bin}"
make -C "$ROOT" arm-plexd ARM_PLEXD_AIRPLAY=1
if [[ ! -f "$BIN" ]]; then
  echo "arm-plexd did not produce $BIN" >&2
  exit 1
fi

sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$USER@$HOST" \
  'mkdir -p /media/fat/misterplex/bin /media/fat/misterplex/scripts
   if [ -f /media/fat/misterplex/bin/misterplexd ]; then
     cp -f /media/fat/misterplex/bin/misterplexd /media/fat/misterplex/bin/misterplexd.prev-c2
   fi
   # Stop watch/supervise/daemon cleanly before replace (numeric PIDs only).
   for p in $(pidof misterplexd 2>/dev/null) $(pidof ffmpeg 2>/dev/null); do
     kill -9 "$p" 2>/dev/null || true
   done
   for p in $(ps w | awk "/misterplexd_supervise\\.sh|misterplex_core_watch\\.sh/ && !/awk/ {print \$1}"); do
     kill -9 "$p" 2>/dev/null || true
   done
   sleep 0.4
   rm -f /media/fat/misterplex/bin/misterplexd'
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$BIN" "$USER@$HOST:/media/fat/misterplex/bin/misterplexd"
# On-device browse / menu + core-load autostart helpers
SCP_SCRIPTS=()
for s in plex_browse.sh plex_menu.sh misterplexd_supervise.sh misterplex_core_watch.sh misterplex_cast_ready.sh; do
  [[ -f "$ROOT/scripts/$s" ]] && SCP_SCRIPTS+=("$ROOT/scripts/$s")
done
if ((${#SCP_SCRIPTS[@]})); then
  sshpass -p "$PASS" scp -o StrictHostKeyChecking=no \
    "${SCP_SCRIPTS[@]}" \
    "$USER@$HOST:/media/fat/misterplex/scripts/"
fi
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$USER@$HOST" \
  "PLAYER_ID='$PLAYER_ID' PMS_URL='$PMS_URL' bash -s" <<'REMOTE'
set -e
# Helpers live in bin/ for boot path simplicity; scripts/ keeps copies for package layout.
for s in misterplexd_supervise.sh misterplex_core_watch.sh misterplex_cast_ready.sh; do
  if [ -f "/media/fat/misterplex/scripts/$s" ]; then
    cp -f "/media/fat/misterplex/scripts/$s" "/media/fat/misterplex/bin/$s"
  fi
done
chmod +x /media/fat/misterplex/bin/misterplexd
chmod +x /media/fat/misterplex/bin/misterplexd_supervise.sh \
         /media/fat/misterplex/bin/misterplex_core_watch.sh \
         /media/fat/misterplex/bin/misterplex_cast_ready.sh 2>/dev/null || true
chmod +x /media/fat/misterplex/scripts/*.sh 2>/dev/null || true

# Startup hook (idempotent): core watch starts/respawns daemon when Plex loads
# and also ensures it is up at boot (cast discovery). RBF cannot exec ARM.
HOOK=/media/fat/linux/_user-startup.sh
MPX_ID="${PLAYER_ID:-misterplex-dev}"
WATCH_LINE="MISTERPLEX_ID=${MPX_ID} nohup /media/fat/misterplex/bin/misterplex_core_watch.sh >>/media/fat/misterplex/misterplex_core_watch.log 2>&1 &"
mkdir -p /media/fat/linux /media/fat/misterplex
touch "$HOOK"
# Drop legacy bare-daemon lines (no supervise / no nohup) so we do not double-spawn.
if grep -qE '^[^#].*misterplex/bin/misterplexd ' "$HOOK" 2>/dev/null; then
  cp -f "$HOOK" "$HOOK.bak-before-core-watch-$(date -u +%Y%m%dT%H%M%SZ)"
  sed -i 's|^\([^#].*misterplex/bin/misterplexd .*\)|# LEGACY_DIRECT_DAEMON \1|' "$HOOK" || true
fi
if ! grep -q 'misterplex_core_watch\.sh' "$HOOK" 2>/dev/null; then
  printf '\n# MiSTerPlex: core-load + boot ensure (RBF cannot start ARM processes)\n%s\n' "$WATCH_LINE" >>"$HOOK"
  echo "Added core-watch startup hook"
else
  # Refresh hook line so MISTERPLEX_ID stays current
  if ! grep -q "MISTERPLEX_ID=${MPX_ID}.*misterplex_core_watch" "$HOOK" 2>/dev/null; then
    sed -i '/misterplex_core_watch\.sh/d' "$HOOK" || true
    printf '\n# MiSTerPlex: core-load + boot ensure (RBF cannot start ARM processes)\n%s\n' "$WATCH_LINE" >>"$HOOK"
    echo "Refreshed core-watch startup hook (id=$MPX_ID)"
  else
    echo "core-watch startup hook already present"
  fi
fi

# Ensure conf exists (token optional — cast can supply transient tokens)
if [[ ! -f /media/fat/misterplex/misterplex.conf ]]; then
  cat >/media/fat/misterplex/misterplex.conf <<'CONF'
# Set this to your Plex Media Server, for example:
# PLEX_BASE=http://YOUR-PLEX-SERVER:32400
# PLEX_TOKEN=
CONF
  if [[ -n "${PMS_URL:-}" ]]; then
    printf 'PLEX_BASE=%s\n' "$PMS_URL" >>/media/fat/misterplex/misterplex.conf
  fi
fi

export MISTERPLEX_ID="${PLAYER_ID:-misterplex-dev}"
: >>/media/fat/misterplex/misterplexd.log
nohup env MISTERPLEX_ID="$MISTERPLEX_ID" \
  /media/fat/misterplex/bin/misterplex_core_watch.sh \
  >>/media/fat/misterplex/misterplex_core_watch.log 2>&1 &
sleep 1.2
ps w | grep -E '[m]isterplexd|[m]isterplex_core_watch|[m]isterplexd_supervise' || true
wget -qO- http://127.0.0.1:3005/resources | head -c 300; echo
echo "CORENAME=$(tr -d '\000\r\n' </tmp/CORENAME 2>/dev/null || echo none)"
REMOTE
echo "Deployed misterplexd + core-watch → $HOST"
