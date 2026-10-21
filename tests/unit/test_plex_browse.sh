#!/usr/bin/env bash
# Smoke: plex_browse.sh player commands against local misterplexd (curl only).
# No PMS token required for status/stop/play-with-test path.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BROWSE="$ROOT/scripts/plex_browse.sh"
MENU="$ROOT/scripts/plex_menu.sh"
BIN="$ROOT/build/misterplexd"
chmod +x "$BROWSE" "$MENU" 2>/dev/null || true
make -C "$ROOT" plexd >/dev/null
# Free TCP port via bind probe (ss/netstat races under parallel agents).
PORT=$(python3 - <<'PY'
import socket
for p in range(13115, 13280):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", p))
        print(p)
        s.close()
        break
    except OSError:
        s.close()
else:
    raise SystemExit("no free port in 13115-13279")
PY
)
echo "test_plex_browse: using PORT=$PORT"
# T-DAEMON-LOCK-SHARED: a private lock dir (build/misterplexd is an MPX_TEST_BUILD); the daemon's
# stderr is also kept in a file for the lock-paths check. It goes through a tracked tee: the OK below
# first closes TEE_FD and waits for TEEPID, so the daemon's last stderr line cannot land after the OK
# (LOCK-SHARED r2 F2, H264/review 05:33).
MPX_LOCK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mpx_locks.XXXXXX")
export MPX_LOCK_DIR
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
exec {TEE_FD}> >(tee "$MPX_LOCK_DIR.stderr" >&2)
TEEPID=$!
"$BIN" --name MiSTerPlexBrowseTest --port "$PORT" 2>&"$TEE_FD" &
PID=$!
# misterplexd ignores SIGTERM once the companion is listening.
stop_pid() {
  local p=$1
  [ -n "$p" ] || return 0
  kill -0 "$p" 2>/dev/null || return 0
  kill -INT "$p" 2>/dev/null || true
  local i=0
  while [ "$i" -lt 50 ]; do
    kill -0 "$p" 2>/dev/null || return 0
    i=$((i + 1))
    sleep 0.1
  done
  kill -KILL "$p" 2>/dev/null || true
  wait "$p" 2>/dev/null || true
}
cleanup() {
  stop_pid "$PID"
  rm -rf "$MPX_LOCK_DIR" "$MPX_LOCK_DIR.stderr"
}
trap cleanup EXIT
lp_expect "$MPX_LOCK_DIR.stderr" 1 || exit 1
fail() { echo "FAIL: $*" >&2; exit 1; }

# Assert the daemon shut down cleanly on SIGINT. SIGTERM is ignored once the
# companion is listening. Without this the harness only checked HTTP responses,
# so an abort during MediaPlayer teardown (a joinable std::thread in the
# destructor) printed "terminate called without an active exception" and the
# script still exited 0.
assert_clean_exit() {
  kill -INT "$PID" 2>/dev/null || true
  local i=0
  while [ "$i" -lt 50 ]; do
    kill -0 "$PID" 2>/dev/null || break
    i=$((i + 1))
    sleep 0.1
  done
  if kill -0 "$PID" 2>/dev/null; then
    kill -KILL "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    fail "daemon ignored SIGINT"
  fi
  local rc=0
  wait "$PID" 2>/dev/null || rc=$?
  # 0 = clean handler exit, 130 = died on SIGINT before the handler.
  case "$rc" in
    0 | 130) ;;
    134) fail "daemon aborted on shutdown (SIGABRT — joinable thread in destructor?)" ;;
    *) fail "daemon exited $rc on shutdown (expected 0 or 130)" ;;
  esac
  PID=""
}
# Wait for our HTTP bind
ok=0
for _ in $(seq 1 50); do
  if out=$(curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/resources" 2>/dev/null); then
    if echo "$out" | grep -q MiSTerPlexBrowseTest; then
      ok=1
      break
    fi
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
[ "$ok" = 1 ] || fail "browse test daemon not ready on :${PORT}"

# help exits 0
"$BROWSE" --help >/dev/null || fail "browse --help"

# status against local player
out=$("$BROWSE" --player "127.0.0.1:${PORT}" status) || fail "status"
echo "$out" | grep -q 'state=' || fail "status missing state: $out"

# play local key (resolve fails → test pattern; companion still ACKs)
out=$("$BROWSE" --player "127.0.0.1:${PORT}" play /library/metadata/99) || fail "play"
echo "$out" | grep -qE 'state=|key=' || fail "play ACK: $out"

# status after play should show media or buffering
sleep 0.3
out=$("$BROWSE" --player "127.0.0.1:${PORT}" status) || fail "status2"
echo "$out" | grep -q 'state=' || fail "status2: $out"

# seek
"$BROWSE" --player "127.0.0.1:${PORT}" seek 12000 || fail "seek"
out=$("$BROWSE" --player "127.0.0.1:${PORT}" status) || fail "status-seek"
# time may still be buffering at 12000
echo "$out" | grep -q 'time=' || fail "status after seek: $out"

# step ±10s + skip (companion routes; no PMS required)
"$BROWSE" --player "127.0.0.1:${PORT}" step || fail "step"
"$BROWSE" --player "127.0.0.1:${PORT}" stepBack || fail "stepBack"
"$BROWSE" --player "127.0.0.1:${PORT}" next || fail "skipNext"
"$BROWSE" --player "127.0.0.1:${PORT}" prev || fail "skipPrevious"

# CLI validation: reject non-numeric / negative seek and zero step
if "$BROWSE" --player "127.0.0.1:${PORT}" seek -1 2>/dev/null; then
  fail "seek -1 should fail validation"
fi
if "$BROWSE" --player "127.0.0.1:${PORT}" seek abc 2>/dev/null; then
  fail "seek abc should fail validation"
fi
if "$BROWSE" --player "127.0.0.1:${PORT}" step 0 2>/dev/null; then
  fail "step 0 should fail validation"
fi
if "$BROWSE" --player "127.0.0.1:${PORT}" stepBack -5 2>/dev/null; then
  fail "stepBack -5 should fail validation"
fi
# custom step size + huge step capped at 120s (still succeeds)
"$BROWSE" --player "127.0.0.1:${PORT}" step 5000 || fail "custom step 5s"
"$BROWSE" --player "127.0.0.1:${PORT}" step 999999999 || fail "huge step should cap not fail"
"$BROWSE" --player "127.0.0.1:${PORT}" stepBack 5000 || fail "custom stepBack 5s"
"$BROWSE" --player "127.0.0.1:${PORT}" stepBack 999999999 || fail "huge stepBack should cap not fail"
# aliases
"$BROWSE" --player "127.0.0.1:${PORT}" ff || fail "ff alias"
"$BROWSE" --player "127.0.0.1:${PORT}" rw || fail "rw alias"
"$BROWSE" --player "127.0.0.1:${PORT}" skipNext || fail "skipNext alias"
"$BROWSE" --player "127.0.0.1:${PORT}" skipPrevious || fail "skipPrevious alias"
# huge absolute seek still accepted (companion clamps to duration when known)
"$BROWSE" --player "127.0.0.1:${PORT}" seek 999999999 || fail "huge seek should succeed"
# reject empty / junk step size
if "$BROWSE" --player "127.0.0.1:${PORT}" step abc 2>/dev/null; then
  fail "step abc should fail validation"
fi

# pause / resume / stop
"$BROWSE" --player "127.0.0.1:${PORT}" pause || fail "pause"
# seek while paused (wantPlay still latched)
"$BROWSE" --player "127.0.0.1:${PORT}" seek 8000 || fail "seek while paused"
"$BROWSE" --player "127.0.0.1:${PORT}" resume || fail "resume"
"$BROWSE" --player "127.0.0.1:${PORT}" stop || fail "stop"

# empty session: seek/step/next/prev still ACK (no crash; no re-arm required)
"$BROWSE" --player "127.0.0.1:${PORT}" seek 5000 || fail "seek after stop"
"$BROWSE" --player "127.0.0.1:${PORT}" step || fail "step after stop"
"$BROWSE" --player "127.0.0.1:${PORT}" stepBack || fail "stepBack after stop"
"$BROWSE" --player "127.0.0.1:${PORT}" next || fail "next after stop"
"$BROWSE" --player "127.0.0.1:${PORT}" prev || fail "prev after stop"
# pause/resume after stop must not re-arm fullScreenVideo (wantPlay gate)
"$BROWSE" --player "127.0.0.1:${PORT}" pause || fail "pause after stop"
"$BROWSE" --player "127.0.0.1:${PORT}" resume || fail "resume after stop"
# status stays navigation-ish (no fullScreenVideo keys required after stop)
out=$("$BROWSE" --player "127.0.0.1:${PORT}" status) || fail "status after empty scrub"
echo "$out" | grep -q 'state=' || fail "status after empty scrub: $out"
echo "$out" | grep -qv 'fullScreenVideo' || fail "empty session still fullScreenVideo: $out"

# play + instant stop race: late resolve must not leave fullScreenVideo
"$BROWSE" --player "127.0.0.1:${PORT}" play /library/metadata/88 || fail "play before race-stop"
"$BROWSE" --player "127.0.0.1:${PORT}" stop || fail "race stop"
sleep 0.5
out=$("$BROWSE" --player "127.0.0.1:${PORT}" status) || fail "status after race-stop"
echo "$out" | grep -qv 'fullScreenVideo' || fail "race-stop still fullScreenVideo: $out"
"$BROWSE" --player "127.0.0.1:${PORT}" next || fail "next after race-stop"
"$BROWSE" --player "127.0.0.1:${PORT}" prev || fail "prev after race-stop"

# E-P4h: rapid re-cast + seek still ACKs (async seek path)
"$BROWSE" --player "127.0.0.1:${PORT}" play /library/metadata/92 || fail "play A rapid"
"$BROWSE" --player "127.0.0.1:${PORT}" play /library/metadata/93 || fail "play B rapid"
"$BROWSE" --player "127.0.0.1:${PORT}" seek 25000 || fail "seek after re-cast"
"$BROWSE" --player "127.0.0.1:${PORT}" step || fail "step after re-cast"
"$BROWSE" --player "127.0.0.1:${PORT}" stop || fail "stop after re-cast"

# menu script exists and --help works (non-interactive)
"$MENU" --help >/dev/null || fail "menu --help"

# servers with example conf (no token ok)
"$BROWSE" --conf "$ROOT/assets/misterplex.conf.example" servers | grep -q selected_base \
  || fail "servers"

assert_clean_exit
exec {TEE_FD}>&-
wait "$TEEPID" || fail "the stderr tee (pid $TEEPID) exited $?"

echo "test_plex_browse: OK"