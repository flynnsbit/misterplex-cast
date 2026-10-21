#!/bin/sh
# Single-instance supervisor. The RBF cannot exec Linux; this is the only
# respawn parent. A second copy exits immediately (mkdir lock).
set -eu
BIN="${MISTERPLEXD_BIN:-/media/fat/misterplex/bin/misterplexd}"
CONF="${MISTERPLEX_CONF:-/media/fat/misterplex/misterplex.conf}"
LOG="${MISTERPLEX_LOG:-/media/fat/misterplex/misterplexd.log}"
SUPLOG="${MISTERPLEX_SUPLOG:-/media/fat/misterplex/misterplexd_supervise.log}"
LAST="${MISTERPLEX_LAST:-/media/fat/misterplex/misterplexd.last}"
DEATH="${MISTERPLEX_DEATH:-/media/fat/misterplex/misterplexd.death}"
ID="${MISTERPLEX_ID:-misterplex-dev}"
PORT="${MISTERPLEX_PORT:-3005}"
NAME="${MISTERPLEX_NAME:-MiSTerPlex}"
LOCKDIR="${MISTERPLEX_SUP_LOCK:-/tmp/misterplexd-supervise.lock}"
MIN_BACKOFF=2
MAX_BACKOFF=60
backoff=$MIN_BACKOFF
HTTP_WAIT_TRIES="${MISTERPLEX_HTTP_WAIT_TRIES:-8}"
HTTP_WAIT_SLEEP_S="${MISTERPLEX_HTTP_WAIT_SLEEP_S:-0.5}"
HTTP_FAIL_LIMIT="${MISTERPLEX_HTTP_FAIL_LIMIT:-2}"

CAST_READY="${MISTERPLEX_CAST_READY:-}"
if [ -z "$CAST_READY" ]; then
  if [ -f "$(dirname "$0")/misterplex_cast_ready.sh" ]; then
    CAST_READY="$(dirname "$0")/misterplex_cast_ready.sh"
  elif [ -n "${ROOT:-}" ] && [ -f "$ROOT/bin/misterplex_cast_ready.sh" ]; then
    CAST_READY="$ROOT/bin/misterplex_cast_ready.sh"
  elif [ -f /media/fat/misterplex/bin/misterplex_cast_ready.sh ]; then
    CAST_READY=/media/fat/misterplex/bin/misterplex_cast_ready.sh
  fi
fi
if [ -z "${CAST_READY:-}" ] || [ ! -f "$CAST_READY" ]; then
  echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) SUPERVISE_ERROR missing misterplex_cast_ready.sh" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "$CAST_READY"

ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }

sig_name() {
  case "$1" in
    1) echo SIGHUP ;;
    2) echo SIGINT ;;
    3) echo SIGQUIT ;;
    4) echo SIGILL ;;
    6) echo SIGABRT ;;
    7) echo SIGBUS ;;
    8) echo SIGFPE ;;
    9) echo SIGKILL ;;
    11) echo SIGSEGV ;;
    13) echo SIGPIPE ;;
    15) echo SIGTERM ;;
    *) echo "SIG_$1" ;;
  esac
}

mkdir -p "$(dirname "$SUPLOG")" "$(dirname "$LOG")"

if ! mkdir "$LOCKDIR" 2>/dev/null; then
  oldpid=$(cat "$LOCKDIR/pid" 2>/dev/null || true)
  if [ -n "${oldpid:-}" ] && kill -0 "$oldpid" 2>/dev/null && [ "$oldpid" != "$$" ]; then
    echo "$(ts) SUPERVISE_DUP exit — lock held by pid=$oldpid" >>"$SUPLOG"
    exit 0
  fi
  rm -rf "$LOCKDIR"
  mkdir "$LOCKDIR"
fi
echo $$ >"$LOCKDIR/pid"
# TERM/INT must exit. A rm-only TERM trap leaves supervise running with no lock
# so a second copy can spawn another GDM advertiser.
cleanup_sup() { rm -rf "$LOCKDIR"; }
trap cleanup_sup EXIT
trap 'exit 0' INT TERM

# Refuse to spawn if a real companion already answers /resources.
if cast_http_ok; then
  others=$(ps w | awk '/[m]isterplexd --/ {c++} END{print c+0}')
  if [ "$others" -gt 0 ]; then
    echo "$(ts) SUPERVISE_DUP port :$PORT already served — not starting a second daemon" >>"$SUPLOG"
    exit 0
  fi
fi

echo "$(ts) SUPERVISE_START bin=$BIN id=$ID conf=$CONF" >>"$SUPLOG"

while true; do
  if [ ! -x "$BIN" ]; then
    echo "$(ts) SUPERVISE_ERROR missing executable $BIN — sleep ${backoff}s" >>"$SUPLOG"
    sleep "$backoff"
    if [ "$backoff" -lt "$MAX_BACKOFF" ]; then
      backoff=$((backoff * 2))
      [ "$backoff" -gt "$MAX_BACKOFF" ] && backoff=$MAX_BACKOFF
    fi
    continue
  fi

  echo "$(ts) SUPERVISE_SPAWN id=$ID port=$PORT" >>"$SUPLOG"
  # Live 40868: inproc HTTP unique~21.5 audio/wall~0.97. Combined pipe
  # unique~14.5 (1.38 MiB I420 pipe stall). 480p bank rejects inproc.
  export MPX_INPROC_DECODE="${MPX_INPROC_DECODE:-1}"
  # 720p WC dest-bank ingest (geometry-gated). Dual-memcpy after swap is
  # banned. 480p ignores. Set 0 to restore heap memcpy.
  export MPX_STICK_I420="${MPX_STICK_I420:-1}"
  "$BIN" --name "$NAME" --id "$ID" --port "$PORT" --conf "$CONF" >>"$LOG" 2>&1 &
  child=$!
  echo "$(ts) SUPERVISE_CHILD pid=$child" >>"$SUPLOG"

  # Gold daemon advertises GDM immediately even if bind :3005 fails.
  # Kill inside ~4s so the Cast picker cannot linger on a dead port.
  i=0
  http_ok=0
  while [ "$i" -lt "$HTTP_WAIT_TRIES" ]; do
    if cast_http_ok; then
      http_ok=1
      break
    fi
    kill -0 "$child" 2>/dev/null || break
    i=$((i + 1))
    sleep "$HTTP_WAIT_SLEEP_S"
  done
  if [ "$http_ok" != 1 ]; then
    echo "$(ts) SUPERVISE_HTTP_DEAD pid=$child port=:$PORT — killing for respawn" >>"$SUPLOG"
    kill "$child" 2>/dev/null || true
    sleep 0.3
    kill -9 "$child" 2>/dev/null || true
    cast_kill_misterplexd_on_port
  else
    backoff=$MIN_BACKOFF
    echo "$(ts) SUPERVISE_HTTP_OK pid=$child port=:$PORT" >>"$SUPLOG"
    # Stay on the child; if /resources dies while the PID lives, GDM is a lie.
    http_fail=0
    while kill -0 "$child" 2>/dev/null; do
      if cast_http_ok; then
        http_fail=0
      else
        http_fail=$((http_fail + 1))
        if [ "$http_fail" -ge "$HTTP_FAIL_LIMIT" ]; then
          echo "$(ts) SUPERVISE_HTTP_LOST pid=$child port=:$PORT fails=$http_fail — killing" >>"$SUPLOG"
          kill "$child" 2>/dev/null || true
          sleep 0.3
          kill -9 "$child" 2>/dev/null || true
          break
        fi
      fi
      s=0
      while [ "$s" -lt 4 ]; do
        kill -0 "$child" 2>/dev/null || break
        sleep 0.5
        s=$((s + 1))
      done
    done
  fi

  set +e
  wait "$child"
  st=$?
  set -e

  last_snap="(none)"
  death_snap="(none)"
  if [ -f "$LAST" ]; then
    last_snap=$(tr '\n' ' ' <"$LAST" | sed 's/[[:space:]]\+/ /g')
  fi
  if [ -f "$DEATH" ]; then
    death_snap=$(tr '\n' ' ' <"$DEATH" | sed 's/[[:space:]]\+/ /g')
  fi

  if [ "$st" -ge 128 ]; then
    sig=$((st - 128))
    sname=$(sig_name "$sig")
    echo "$(ts) SUPERVISE_EXIT pid=$child wait_rc=$st signal=$sig ($sname) — RESPAWN ${backoff}s last={$last_snap} death={$death_snap}" >>"$SUPLOG"
  else
    echo "$(ts) SUPERVISE_EXIT pid=$child wait_rc=$st exit_status=$st — RESPAWN ${backoff}s last={$last_snap} death={$death_snap}" >>"$SUPLOG"
  fi

  sleep "$backoff"
  if [ "$backoff" -lt "$MAX_BACKOFF" ]; then
    backoff=$((backoff * 2))
    [ "$backoff" -gt "$MAX_BACKOFF" ] && backoff=$MAX_BACKOFF
  fi
done
