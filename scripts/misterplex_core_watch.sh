#!/bin/sh
# Bridge Main's /tmp/CORENAME → one matching misterplexd.
#
# The RBF cannot exec Linux. This watcher is the stand-in:
#   Plex loaded  → start the daemon that pairs with the live Plex.rbf md5
#   Plex left / reset → stop supervise + daemon (no orphans, no second copy)
# A second watcher exits on the mkdir lock.
set -eu

CORENAME_FILE="${CORENAME_FILE:-/tmp/CORENAME}"
RBFNAME_FILE="${RBFNAME_FILE:-/tmp/RBFNAME}"
ROOT="${MISTERPLEX_ROOT:-/media/fat/misterplex}"
PAIRS="${MISTERPLEX_PAIRS:-$ROOT/rbf_daemon_pairs.txt}"
DEFAULT_BIN="${MISTERPLEXD_BIN:-$ROOT/bin/misterplexd}"
SUP="${MISTERPLEX_SUPERVISE:-$ROOT/bin/misterplexd_supervise.sh}"
WATCHLOG="${MISTERPLEX_WATCHLOG:-$ROOT/misterplex_core_watch.log}"
POLL_S="${MISTERPLEX_WATCH_POLL_S:-2}"
ID="${MISTERPLEX_ID:-misterplex-dev}"
PORT="${MISTERPLEX_PORT:-3005}"
NAME="${MISTERPLEX_NAME:-MiSTerPlex}"
CONF="${MISTERPLEX_CONF:-$ROOT/misterplex.conf}"
LOCKDIR="${MISTERPLEX_WATCH_LOCK:-/tmp/misterplex-watch.lock}"
# Named Utility cores only. Generic Plex.rbf is forbidden: every sibling
# reports CORENAME/RBFNAME=Plex, so hashing _Utility/Plex.rbf mis-pairs
# Plex_720p24 onto the 480p daemon (colorbars/black, no chevron, no play).
LIVE_RBF="${MISTERPLEX_LIVE_RBF:-/media/fat/_Utility/Plex_480p.rbf}"
UTILITY_DIR="${MISTERPLEX_UTILITY:-/media/fat/_Utility}"
L4_PLXS_ADDR="${MISTERPLEX_L4_PLXS:-0x3047F100}"
HTTP_RESTART_COOLDOWN_S="${MISTERPLEX_HTTP_RESTART_COOLDOWN_S:-8}"
http_restart_s=0
PAIR_KEY_FILE="${MISTERPLEX_PAIR_KEY_FILE:-/tmp/misterplex-last-pair-key}"
last_pair_key=$(cat "$PAIR_KEY_FILE" 2>/dev/null || true)
SELECTED_BIN=

CAST_READY="${MISTERPLEX_CAST_READY:-}"
if [ -z "$CAST_READY" ]; then
  if [ -f "$(dirname "$0")/misterplex_cast_ready.sh" ]; then
    CAST_READY="$(dirname "$0")/misterplex_cast_ready.sh"
  elif [ -f "$ROOT/bin/misterplex_cast_ready.sh" ]; then
    CAST_READY="$ROOT/bin/misterplex_cast_ready.sh"
  elif [ -f "$ROOT/scripts/misterplex_cast_ready.sh" ]; then
    CAST_READY="$ROOT/scripts/misterplex_cast_ready.sh"
  fi
fi
if [ -z "${CAST_READY:-}" ] || [ ! -f "$CAST_READY" ]; then
  echo "misterplex_core_watch: missing misterplex_cast_ready.sh" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "$CAST_READY"

PAIR_CONF_LIB="${MISTERPLEX_PAIR_CONF:-}"
if [ -z "$PAIR_CONF_LIB" ]; then
  if [ -f "$(dirname "$0")/misterplex_pair_conf.sh" ]; then
    PAIR_CONF_LIB="$(dirname "$0")/misterplex_pair_conf.sh"
  elif [ -f "$ROOT/bin/misterplex_pair_conf.sh" ]; then
    PAIR_CONF_LIB="$ROOT/bin/misterplex_pair_conf.sh"
  elif [ -f "$ROOT/scripts/misterplex_pair_conf.sh" ]; then
    PAIR_CONF_LIB="$ROOT/scripts/misterplex_pair_conf.sh"
  fi
fi
if [ -n "${PAIR_CONF_LIB:-}" ] && [ -f "$PAIR_CONF_LIB" ]; then
  # shellcheck disable=SC1090
  . "$PAIR_CONF_LIB"
fi
pair_conf_dirty=0

NAMED_RBF_LIB="${MISTERPLEX_NAMED_RBF:-}"
if [ -z "$NAMED_RBF_LIB" ]; then
  if [ -f "$(dirname "$0")/misterplex_named_rbf.sh" ]; then
    NAMED_RBF_LIB="$(dirname "$0")/misterplex_named_rbf.sh"
  elif [ -f "$ROOT/bin/misterplex_named_rbf.sh" ]; then
    NAMED_RBF_LIB="$ROOT/bin/misterplex_named_rbf.sh"
  elif [ -f "$ROOT/scripts/misterplex_named_rbf.sh" ]; then
    NAMED_RBF_LIB="$ROOT/scripts/misterplex_named_rbf.sh"
  fi
fi
if [ -z "${NAMED_RBF_LIB:-}" ] || [ ! -f "$NAMED_RBF_LIB" ]; then
  echo "misterplex_core_watch: missing misterplex_named_rbf.sh" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "$NAMED_RBF_LIB"

ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }
log() { echo "$(ts) $*" >>"$WATCHLOG"; }

if ! mkdir "$LOCKDIR" 2>/dev/null; then
  oldpid=$(cat "$LOCKDIR/pid" 2>/dev/null || true)
  if [ -n "${oldpid:-}" ] && kill -0 "$oldpid" 2>/dev/null && [ "$oldpid" != "$$" ]; then
    echo "$(ts) WATCH_DUP exit — lock held by pid=$oldpid" >>"$WATCHLOG"
    exit 0
  fi
  rm -rf "$LOCKDIR"
  mkdir "$LOCKDIR"
fi
echo $$ >"$LOCKDIR/pid"
# A second watcher used to survive the mkdir lock (stale pid / race) and
# PAIR_INSTALL the 480p daemon over 720p24. Cull any other core_watch.
for p in $(ps w | awk '/misterplex_core_watch\.sh/ && !/awk/ {print $1}'); do
  [ "$p" = "$$" ] && continue
  log "WATCH_CULL dup pid=$p"
  kill "$p" 2>/dev/null || true
done
# TERM/INT must actually exit. `trap 'rm -rf lock' TERM` swallows SIGTERM and
# the loop keeps running (duplicate watchers + PAIR_OK spam; lock stolen).
cleanup_watch() { rm -rf "$LOCKDIR"; }
trap cleanup_watch EXIT
trap 'exit 0' INT TERM

is_plex_core() {
  name=$(tr -d '\000\r\n' <"$1" 2>/dev/null || true)
  name=$(echo "$name" | sed 's|.*/||; s|\.rbf$||; s|\.RBF$||')
  case "$name" in
    [Pp]lex|[Pp]lex_*|[Pp]lex-* ) return 0 ;;
    *) return 1 ;;
  esac
}

plex_loaded() {
  if [ -f "$CORENAME_FILE" ] && is_plex_core "$CORENAME_FILE"; then
    return 0
  fi
  if [ -f "$RBFNAME_FILE" ] && is_plex_core "$RBFNAME_FILE"; then
    return 0
  fi
  return 1
}

daemon_count() {
  # pidof is the ground truth. `ps | awk /misterplexd/` also matches this
  # watcher's own command line and false-counts 2–3 daemons.
  if command -v pidof >/dev/null 2>&1; then
    n=$(pidof misterplexd 2>/dev/null | wc -w)
    echo "${n:-0}"
    return 0
  fi
  ps w | awk '/\/misterplexd --name/ && !/awk/ && !/core_watch/ && !/supervise/ {c++} END{print c+0}'
}

supervise_count() {
  ps w | awk '/misterplexd_supervise\.sh/ && !/awk/ {c++} END{print c+0}'
}

kill_pids() {
  # $1 = awk pattern
  for p in $(ps w | awk "$1"' && !/awk/ {print $1}'); do
    kill "$p" 2>/dev/null || true
  done
}

stop_plex_arm() {
  # Supervise first so it cannot respawn while we kill the child.
  kill_pids '/misterplexd_supervise\.sh/'
  sleep 0.2
  kill_pids '/[m]isterplexd --/'
  sleep 0.3
  kill_pids '/misterplexd_supervise\.sh/'
  kill_pids '/[m]isterplexd --/'
  # leftover ffmpeg from a torn session
  kill_pids '/[f]fmpeg /'
  cast_kill_misterplexd_on_port
  rm -rf /tmp/misterplexd-supervise.lock
  log "ARM_STOP daemons=$(daemon_count) supervise=$(supervise_count) listen=$(cast_port_listen && echo up || echo down)"
}

select_bin_for_live_rbf() {
  # Must not run in $() — last_pair_key / SELECTED_BIN live in this shell.
  rbf=$(resolve_loaded_rbf)
  SELECTED_BIN="$DEFAULT_BIN"
  sum="missing"
  if [ -f "$rbf" ]; then
    sum=$(md5sum "$rbf" | awk '{print $1}')
    if [ -f "$PAIRS" ]; then
      pair=$(awk -v s="$sum" '$1==s && $1 !~ /^#/ {print $2; exit}' "$PAIRS")
      if [ -n "${pair:-}" ] && [ -x "$pair" ]; then
        SELECTED_BIN="$pair"
        # Pairs col3 is l4/true480. Only hashing 03f1b95a as L4 pinned
        # 28cb5a75 (and every later 720p24 RBF) glass=true480 so the daemon
        # GLASS-MAX'd DECODE=1280x720 → bank=640x480.
        glass_col=$(awk -v s="$sum" '$1==s && $1 !~ /^#/ {print tolower($3); exit}' "$PAIRS")
        if [ "$glass_col" = "l4" ]; then
          echo L4 > /tmp/misterplex-live-glass
        else
          echo true480 > /tmp/misterplex-live-glass
        fi
        pair_key="$sum $SELECTED_BIN"
        if [ "$pair_key" != "$last_pair_key" ]; then
          log "PAIR_OK file=$rbf rbf=$sum bin=$SELECTED_BIN glass=$(cat /tmp/misterplex-live-glass)"
          last_pair_key=$pair_key
          printf '%s\n' "$pair_key" >"$PAIR_KEY_FILE"
          if [ -n "${PAIR_CONF_LIB:-}" ]; then
            misterplex_apply_pair_conf "$sum" "$CONF"
            pair_conf_dirty=1
            log "PAIR_CONF $CONF rbf=$sum"
          fi
        fi
        return 0
      fi
    fi
    log "PAIR_DEFAULT file=$rbf rbf=$sum bin=$SELECTED_BIN"
  else
    log "PAIR_DEFAULT no live rbf bin=$SELECTED_BIN"
  fi
}

ensure_daemon() {
  select_bin_for_live_rbf
  BIN=$SELECTED_BIN
  if [ "${pair_conf_dirty:-0}" = 1 ]; then
    pair_conf_dirty=0
    if [ "$(daemon_count)" -ge 1 ]; then
      log "PAIR_CONF restart so daemon rereads $CONF"
      stop_plex_arm
    fi
  fi
  n=$(daemon_count)
  if [ "$n" -gt 1 ]; then
    log "ENSURE_CULL extras daemons=$n"
    stop_plex_arm
    n=0
  fi
  if [ "$n" -eq 1 ]; then
    if cast_http_ok; then
      return 0
    fi
    now=$(date +%s)
    if [ "$http_restart_s" -gt 0 ] && [ $((now - http_restart_s)) -lt "$HTTP_RESTART_COOLDOWN_S" ]; then
      log "ENSURE_DEAD_HTTP cooldown port=:$PORT"
      return 1
    fi
    http_restart_s=$now
    log "ENSURE_DEAD_HTTP daemons=1 port=:$PORT — cull and restart"
    stop_plex_arm
    n=0
  fi
  if [ ! -x "$BIN" ]; then
    log "ENSURE_FAIL missing $BIN"
    return 1
  fi
  if [ "$(supervise_count)" -gt 0 ]; then
    i=0
    while [ "$i" -lt 8 ]; do
      if [ "$(daemon_count)" -eq 1 ] && cast_http_ok; then
        log "ENSURE_OK supervise respawn http=:$PORT"
        return 0
      fi
      i=$((i + 1))
      sleep 1
    done
    log "ENSURE_WARN supervise up but Cast HTTP down — restarting"
    stop_plex_arm
  fi
  RUN="$ROOT/bin/misterplexd"
  if [ "$BIN" != "$RUN" ] && [ -x "$BIN" ]; then
    src=$(md5sum "$BIN" | awk '{print $1}')
    dst=$(md5sum "$RUN" 2>/dev/null | awk '{print $1}')
    if [ "$src" != "$dst" ]; then
      cp -f "$BIN" "$RUN.tmp" && mv -f "$RUN.tmp" "$RUN"
      chmod +x "$RUN"
      log "PAIR_INSTALL $BIN -> $RUN md5=$src"
    fi
    BIN="$RUN"
  fi
  log "ENSURE_START supervise=$SUP bin=$BIN"
  # Do not force MPX_INPROC_DECODE=1. Inproc remux of PMS universal 720p
  # hung buffering. Do not set MPX_STICK_I420: stick ingest into uncached
  # HPS DDR was ~67 ms/f and locked L4 at 12 unique (P5_960 CLOSED).
  # 1500k + 2-slot cached ring (stick=0) is the 480p overlap path.
  MISTERPLEX_ID="$ID" MISTERPLEX_PORT="$PORT" MISTERPLEX_NAME="$NAME" \
    MISTERPLEX_CONF="$CONF" MISTERPLEXD_BIN="$BIN" \
    nohup "$SUP" >>"$WATCHLOG" 2>&1 &
  i=0
  while [ "$i" -lt 10 ]; do
    if [ "$(daemon_count)" -eq 1 ] && cast_http_ok; then
      log "ENSURE_OK via supervise http=:$PORT"
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
  log "ENSURE_FAIL daemon_count=$(daemon_count) http=$(cast_http_ok && echo ok || echo down)"
  return 1
}

mkdir -p "$ROOT"
log "WATCH_START poll=${POLL_S}s id=$ID (stop daemon when Plex unloads)"

# Drop leftover doubles / silent GDM, but do not bounce a healthy Cast listener
# (watch redeploy used to kill :3005 even when /resources was already 200).
n0=$(daemon_count)
if [ "$n0" -gt 1 ]; then
  log "WATCH_START cull extras daemons=$n0"
  stop_plex_arm
elif [ "$n0" -eq 1 ] && ! cast_http_ok; then
  log "WATCH_START cull silent daemon (GDM-without-HTTP class)"
  stop_plex_arm
elif [ "$n0" -eq 0 ]; then
  rm -rf /tmp/misterplexd-supervise.lock
fi
last_plex=0
if plex_loaded; then
  ensure_daemon || true
  last_plex=1
else
  log "CORE_NOT_PLEX — ARM left down until Plex loads"
fi

while true; do
  if plex_loaded; then
    if [ "$last_plex" -eq 0 ]; then
      log "CORE_PLEX_ENTER name=$(tr -d '\000\r\n' <"$CORENAME_FILE" 2>/dev/null || echo '?')"
      last_plex=1
    fi
    ensure_daemon || true
  else
    if [ "$last_plex" -eq 1 ]; then
      log "CORE_PLEX_LEAVE — stopping daemon (RBF no longer on glass)"
      last_plex=0
      stop_plex_arm
    fi
  fi
  sleep "$POLL_S"
done
