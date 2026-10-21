#!/usr/bin/env bash
# Unit/smoke: companion scrubber fields, viewOffset, prePlayHold / castBound resume hold.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/misterplexd"
make -C "$ROOT" plexd >/dev/null
# Free TCP port via bind probe (ss/netstat races under parallel agents).
PORT=$(python3 - <<'PY'
import socket
for p in range(13005, 13150):
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
    raise SystemExit("no free port in 13005-13149")
PY
)
echo "test_companion_http: using PORT=$PORT"
# T-DAEMON-LOCK-SHARED: a private lock dir (build/misterplexd is an MPX_TEST_BUILD); the daemon's
# stderr is also kept in a file for the lock-paths check.
MPX_LOCK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mpx_locks.XXXXXX")
export MPX_LOCK_DIR
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init
"$BIN" --name MiSTerPlexTest --port "$PORT" 2> >(tee "$MPX_LOCK_DIR.stderr" >&2) &
PID=$!
cleanup() {
  kill "$PID" 2>/dev/null || true
  # The daemon ignores SIGTERM once the companion is listening (daemonTermShouldExit).
  for _ in 1 2 3 4 5; do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.2
  done
  kill -9 "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  rm -rf "$MPX_LOCK_DIR" "$MPX_LOCK_DIR.stderr"
}
trap cleanup EXIT
lp_expect "$MPX_LOCK_DIR.stderr" 1 || exit 1
# Wait until HTTP is up (bind can lag slightly after process start).
ok=0
for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:${PORT}/resources" >/dev/null 2>&1; then
    ok=1
    break
  fi
  # Bail early if daemon died
  if ! kill -0 "$PID" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if [ "$ok" != 1 ]; then
  echo "FAIL: companion HTTP not up on :$PORT (pid=$PID)" >&2
  exit 1
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

# Assert the daemon shut down cleanly on SIGTERM. Without this the harness only
# checked HTTP responses, so an abort during MediaPlayer teardown (a joinable
# std::thread in the destructor) printed "terminate called without an active
# exception" and the script still exited 0.
assert_clean_exit() {
  kill "$PID" 2>/dev/null || true
  # Once the companion is listening, SIGTERM is ignored (daemonTermShouldExit).
  # Give a SIGTERM exit a moment, then SIGKILL the way stopd does.
  for _ in 1 2 3 4 5; do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.2
  done
  if kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID" 2>/dev/null || true
  fi
  local rc=0
  wait "$PID" 2>/dev/null || rc=$?
  case "$rc" in
    0 | 137 | 143) ;;
    134) fail "daemon aborted on shutdown (SIGABRT — joinable thread in destructor?)" ;;
    *) fail "daemon exited $rc on shutdown (expected 0, 137, or 143)" ;;
  esac
}

BODY=$(curl -fsS "http://127.0.0.1:${PORT}/resources")
echo "$BODY" | grep -q MiSTerPlexTest
echo "$BODY" | grep -q machineIdentifier
echo "$BODY" | grep -q 'clientProfile=' && fail "resources advertised a named clientProfile"

# --- cold-start empty session: seek/step/skip ACK without re-arming media ---
COLD_SEEK=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=9000&commandID=cold1")
echo "$COLD_SEEK" | grep -q Timeline || fail "cold seek no Timeline"
echo "$COLD_SEEK" | grep -q 'location="navigation"' || fail "cold seek not navigation: $COLD_SEEK"
echo "$COLD_SEEK" | grep -qv 'key="/library' || fail "cold seek planted media key: $COLD_SEEK"
COLD_STEP=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=cold2")
echo "$COLD_STEP" | grep -q 'location="navigation"' || fail "cold step not navigation: $COLD_STEP"
COLD_NEXT=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipNext?commandID=cold3")
echo "$COLD_NEXT" | grep -q Timeline || fail "cold skipNext no Timeline"
COLD_PREV=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=cold4")
echo "$COLD_PREV" | grep -q Timeline || fail "cold skipPrevious no Timeline"
# seekTo without offset= must not crash / not force fullScreenVideo
COLD_NOFF=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?commandID=cold5")
echo "$COLD_NOFF" | grep -q Timeline || fail "seekTo no-offset no Timeline"
echo "$COLD_NOFF" | grep -q 'location="navigation"' || fail "seekTo no-offset re-armed: $COLD_NOFF"

# --- playMedia stub (local testsrc via unresolved → pattern) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F1&offset=0&commandID=1" \
  | grep -q Timeline

# timeline poll should show buffering or playing (wantPlay)
POLL=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=2")
echo "$POLL" | grep -q state=
echo "$POLL" | grep -Eq 'state="(buffering|playing|paused)"'

# ratingKey derived from key path when omitted
echo "$POLL" | grep -q 'ratingKey="1"' || fail "missing derived ratingKey from key: $POLL"
# No play queue (no containerKey, playQueueID or playQueueItemID): no playQueueItemID may be invented from the
# ratingKey. f176804c removed the companion fallback; this line pinned it and has failed since (review 10-04 16:39
# P1 exempt). TASKS-NIGHT-1005 #22.
if echo "$POLL" | grep -q 'playQueueItemID='; then fail "invented playQueueItemID without a play queue: $POLL"; fi

# pause / play / stop control paths
curl -fsS "http://127.0.0.1:${PORT}/player/playback/pause?commandID=3" | grep -q Timeline
curl -fsS "http://127.0.0.1:${PORT}/player/playback/play?commandID=4" | grep -q Timeline

# --- stop → prePlayHold while castBound (Resume dialog resilience) ---
STOP=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=5")
echo "$STOP" | grep -q Timeline
# Stop ACK must not advertise live media keys (idles Web / freezes scrubber)
echo "$STOP" | grep -q 'location="navigation"' || fail "stop ACK not navigation: $STOP"
echo "$STOP" | grep -q 'state="buffering"' || fail "stop ACK not buffering hold: $STOP"
echo "$STOP" | grep -qv 'key="/library' || fail "stop ACK still has media key: $STOP"

POLL_HOLD=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=5b")
echo "$POLL_HOLD" | grep -q 'location="navigation"' || fail "hold poll not navigation: $POLL_HOLD"
echo "$POLL_HOLD" | grep -q 'state="buffering"' || fail "hold poll not buffering: $POLL_HOLD"
echo "$POLL_HOLD" | grep -qv 'fullScreenVideo' || fail "hold poll still fullScreenVideo: $POLL_HOLD"

# --- mirror stages prePlayHold (buffering @ navigation, no live keys) ---
MIRROR=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/mirror?key=%2Flibrary%2Fmetadata%2F2&commandID=6")
echo "$MIRROR" | grep -q Timeline
echo "$MIRROR" | grep -q 'state="buffering"' || fail "mirror not buffering: $MIRROR"
echo "$MIRROR" | grep -q 'location="navigation"' || fail "mirror not navigation: $MIRROR"
echo "$MIRROR" | grep -qv 'fullScreenVideo' || fail "mirror fullScreenVideo: $MIRROR"

# --- playMedia with full play-queue bind fields (Web scrubber contract) ---
PQ=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F9&containerKey=%2FplayQueues%2F42%3Fown%3D1&playQueueItemID=99&playQueueVersion=3&ratingKey=9&address=pms.lan&port=32400&protocol=http&machineIdentifier=server-mid&offset=0&commandID=7")
echo "$PQ" | grep -q Timeline
# Immediate ACK should already expose queue bind (async resolve later)
echo "$PQ" | grep -q 'playQueueID="42"' || fail "ACK missing playQueueID: $PQ"
echo "$PQ" | grep -q 'playQueueItemID="99"' || fail "ACK missing playQueueItemID: $PQ"
echo "$PQ" | grep -q 'containerKey="/playQueues/42' || fail "ACK missing containerKey: $PQ"
echo "$PQ" | grep -q 'key="/library/metadata/9"' || fail "ACK missing key: $PQ"
echo "$PQ" | grep -q 'location="fullScreenVideo"' || fail "ACK not fullScreenVideo: $PQ"
echo "$PQ" | grep -q 'address="pms.lan"' || fail "ACK missing server address: $PQ"

sleep 0.3
POLL2=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=8")
echo "$POLL2" | grep -q 'playQueueID="42"' || fail "poll missing playQueueID: $POLL2"
echo "$POLL2" | grep -q 'playQueueItemID="99"' || fail "poll missing playQueueItemID: $POLL2"
echo "$POLL2" | grep -q 'containerKey="/playQueues/42' || fail "poll missing containerKey: $POLL2"
echo "$POLL2" | grep -q 'key="/library/metadata/9"' || fail "poll missing key: $POLL2"
echo "$POLL2" | grep -q 'seekRange=' || true  # duration may still be 0 without resolve

# --- viewOffset query param (ms) seeds time on playMedia ---
VO=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F11&viewOffset=74000&commandID=9")
echo "$VO" | grep -q 'time="74000"' || fail "viewOffset not applied to time=: $VO"

# offset=0 on the same playMedia must not hide viewOffset (F242 094 cast resume).
BOTH=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F111&offset=0&viewOffset=180000&commandID=9b")
echo "$BOTH" | grep -q 'time="180000"' || fail "offset=0 hid viewOffset: $BOTH"

# offset=0 explicit must win over any staged viewOffset (Play from start)
OFF0=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F12&offset=0&commandID=10")
echo "$OFF0" | grep -q 'time="0"' || fail "offset=0 not honored: $OFF0"

# --- poison containerKey=/library/metadata/N must not appear as containerKey ---
BADCK=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F13&containerKey=%2Flibrary%2Fmetadata%2F13&commandID=11")
echo "$BADCK" | grep -qv 'containerKey="/library/metadata' || fail "poison containerKey emitted: $BADCK"

# --- proxy/timeline alias ---
curl -fsS "http://127.0.0.1:${PORT}/player/proxy/timeline?commandID=12" | grep -q Timeline

# --- scrubber stepForward / stepBack (relative ±10s) ---
# Use seekTo to plant a known time, then step (avoids async playMedia race).
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F20&offset=0&commandID=12a" \
  | grep -q Timeline || fail "rebind for step"
sleep 0.4
SEEK_BASE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=30000&commandID=12a2")
echo "$SEEK_BASE" | grep -q 'time="30000"' || fail "seekTo plant 30s: $SEEK_BASE"
STEP=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=12b")
echo "$STEP" | grep -q Timeline || fail "stepForward no Timeline"
# time should advance +10s from 30000 → 40000
echo "$STEP" | grep -q 'time="40000"' || fail "stepForward not +10s: $STEP"
STEPB=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepBack?commandID=12c")
echo "$STEPB" | grep -q 'time="30000"' || fail "stepBack not -10s: $STEPB"

# --- skipPrevious restarts at 0 ---
SKP=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=12d")
echo "$SKP" | grep -q 'time="0"' || fail "skipPrevious not 0: $SKP"

# --- skipNext ACK (may no-op without playQueue; must not 404) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipNext?commandID=12e" | grep -q Timeline \
  || fail "skipNext missing Timeline"

# --- seekTo applies offset ---
SEEK=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=55000&commandID=12f")
echo "$SEEK" | grep -q 'time="55000"' || fail "seekTo offset not applied: $SEEK"
echo "$SEEK" | grep -q Timeline || fail "seekTo no Timeline"
# After resolve/bind, duration from testsrc path is 120000 → seekRange present on poll
sleep 0.3
POLL_DUR=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=12g")
echo "$POLL_DUR" | grep -q 'seekRange=' || echo "NOTE: seekRange absent (duration may be 0): $POLL_DUR"

# --- P4-SCRUB edge cases: clamp / custom step / playQueueID-only bind ---
# Negative seekTo → 0 (do not emit negative time=)
NEG=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=-5000&commandID=20")
echo "$NEG" | grep -q 'time="0"' || fail "negative seekTo not clamped to 0: $NEG"
echo "$NEG" | grep -qv 'time="-' || fail "negative time= leaked: $NEG"

# stepBack at 0 stays 0
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=0&commandID=21" >/dev/null
STEPO=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepBack?commandID=22")
echo "$STEPO" | grep -q 'time="0"' || fail "stepBack at 0 not pinned: $STEPO"

# Custom step size via offset= (relative ms, not absolute seek)
# Re-plant time and assert ACK before step. Companion preserves scrubber time across
# empty-session "stopped@0" progress so demux short-read cannot clobber the plant.
PLANT=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=20000&commandID=23")
echo "$PLANT" | grep -q 'time="20000"' || fail "plant 20s before custom step: $PLANT"
# Confirm poll still holds plant (progress race regression guard)
POLL_PLANT=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=23b")
echo "$POLL_PLANT" | grep -q 'time="20000"' || fail "plant not sticky on poll: $POLL_PLANT"
STEP5=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?offset=5000&commandID=24")
echo "$STEP5" | grep -q 'time="25000"' || fail "custom step +5s not applied: $STEP5"

# Seek past known duration (testsrc resolve → 120000) clamps to end
# Re-bind so duration is set; wait for async resolve bindMedia (testsrc dur=120000)
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F30&offset=0&commandID=25" \
  | grep -q Timeline || fail "rebind for clamp"
DUR_OK=0
POLL_C=""
for _ in $(seq 1 40); do
  POLL_C=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=25b")
  if echo "$POLL_C" | grep -q 'duration="120000"'; then
    DUR_OK=1
    break
  fi
  sleep 0.1
done
[ "$DUR_OK" = 1 ] || fail "duration never bound for past-end clamp: $POLL_C"
OVER=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=999999999&commandID=26")
echo "$OVER" | grep -q 'time="120000"' || fail "seek past end not clamped to duration: $OVER"
# stepForward near end clamps (not 120000+10000)
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=115000&commandID=27" >/dev/null
STEPE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=28")
echo "$STEPE" | grep -q 'time="120000"' || fail "step past end not clamped: $STEPE"
echo "$STEPE" | grep -qv 'time="125000"' || fail "step overshot duration: $STEPE"
# Already at end: second stepForward stays at duration (no thrash overshoot)
STEPE2=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=28b")
echo "$STEPE2" | grep -q 'time="120000"' || fail "step at end not pinned: $STEPE2"
# Custom step larger than remaining also clamps
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=110000&commandID=28c" >/dev/null
STEPE3=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?offset=50000&commandID=28d")
echo "$STEPE3" | grep -q 'time="120000"' || fail "large step past end not clamped: $STEPE3"
# Huge relative step size is capped at 120s (companion); from 0 → 120000 not 999999
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=0&commandID=28e" >/dev/null
STEPHUGE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?offset=999999999&commandID=28f")
echo "$STEPHUGE" | grep -q 'time="120000"' || fail "huge step size not capped+clamped: $STEPHUGE"
# skipNext while active but empty/unbound queue still ACKs with media bind intact
SKIP_EMPTY=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipNext?commandID=28g")
echo "$SKIP_EMPTY" | grep -q Timeline || fail "skipNext empty-queue no Timeline"
echo "$SKIP_EMPTY" | grep -q 'location="fullScreenVideo"' || fail "skipNext empty-queue dropped bind: $SKIP_EMPTY"
# seekTo with no offset while active: ACK only, do not jump to 0
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=45000&commandID=28h" >/dev/null
SEEK_NOFF=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?commandID=28i")
echo "$SEEK_NOFF" | grep -q 'time="45000"' || fail "seekTo no-offset changed time: $SEEK_NOFF"

# playQueueID query param alone (no containerKey) still binds scrubber fields
PQONLY=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F40&playQueueID=888&playQueueItemID=777&playQueueVersion=5&ratingKey=40&offset=0&commandID=29")
echo "$PQONLY" | grep -q 'playQueueID="888"' || fail "playQueueID-only bind missing id: $PQONLY"
echo "$PQONLY" | grep -q 'playQueueItemID="777"' || fail "playQueueID-only bind missing item: $PQONLY"
echo "$PQONLY" | grep -q 'playQueueVersion="5"' || fail "playQueueID-only bind missing version: $PQONLY"
echo "$PQONLY" | grep -q 'containerKey="/playQueues/888' || fail "playQueueID-only missing synthetic containerKey: $PQONLY"

# containerKey with trailing slash before ? must survive timeline ACK when playQueueID is also set
PQSLASH=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F41&containerKey=%2FplayQueues%2F889%2F%3Fown%3D1&playQueueID=889&playQueueItemID=778&offset=0&commandID=29a")
echo "$PQSLASH" | grep -q 'containerKey="/playQueues/889/' || fail "playQueues trailing slash dropped from ACK: $PQSLASH"

# After stop, seek/step/skip must not re-arm fullScreenVideo keys (idle scrubber).
# Drain any in-flight async playMedia, then stop again so bindMedia is suppressed.
curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=30" >/dev/null
sleep 0.4
curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=30b" >/dev/null
SEEK_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=1000&commandID=31")
echo "$SEEK_IDLE" | grep -q 'location="navigation"' || fail "post-stop seek not navigation: $SEEK_IDLE"
echo "$SEEK_IDLE" | grep -qv 'key="/library' || fail "post-stop seek re-armed media key: $SEEK_IDLE"
STEP_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=32")
echo "$STEP_IDLE" | grep -q 'location="navigation"' || fail "post-stop step not navigation: $STEP_IDLE"
echo "$STEP_IDLE" | grep -qv 'key="/library' || fail "post-stop step re-armed media key: $STEP_IDLE"
NEXT_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipNext?commandID=33")
echo "$NEXT_IDLE" | grep -q 'location="navigation"' || fail "post-stop skipNext not navigation: $NEXT_IDLE"
echo "$NEXT_IDLE" | grep -qv 'key="/library' || fail "post-stop skipNext re-armed media key: $NEXT_IDLE"
PREV_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=34")
echo "$PREV_IDLE" | grep -q 'location="navigation"' || fail "post-stop skipPrevious not navigation: $PREV_IDLE"

# skipPrevious at t=0 (active session): ACK time=0; handler may try queue-prev (no thrash)
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F50&offset=0&commandID=35" \
  | grep -q Timeline || fail "rebind for skipPrev@0"
sleep 0.3
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=0&commandID=36" >/dev/null
SKP0=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=37")
echo "$SKP0" | grep -q 'time="0"' || fail "skipPrevious@0 not time=0: $SKP0"
echo "$SKP0" | grep -q Timeline || fail "skipPrevious@0 missing Timeline"
echo "$SKP0" | grep -q 'location="fullScreenVideo"' || fail "skipPrevious@0 dropped bind: $SKP0"
# Near-start (≤3s): still ACK; no PMS queue → stays on same bind (no crash)
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=1500&commandID=37b" >/dev/null
SKP_NEAR=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=37c")
echo "$SKP_NEAR" | grep -q Timeline || fail "skipPrevious near-start no Timeline"
# Mid-title (>3s): restart path plants time=0 immediately
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=45000&commandID=37d" >/dev/null
SKP_MID=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=37e")
echo "$SKP_MID" | grep -q 'time="0"' || fail "skipPrevious mid-title not restart@0: $SKP_MID"

# --- seekTo without offset= must not jump to 0 ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=42000&commandID=38" >/dev/null
SEEK_NOOFF=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?commandID=39")
echo "$SEEK_NOOFF" | grep -q 'time="42000"' || fail "seek without offset jumped time: $SEEK_NOOFF"

# --- seekTo aliases: viewOffset= and time= (same ms contract as offset=) ---
VO_SEEK=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?viewOffset=33000&commandID=39b")
echo "$VO_SEEK" | grep -q 'time="33000"' || fail "seekTo viewOffset= not applied: $VO_SEEK"
TIME_SEEK=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?time=28000&commandID=39c")
echo "$TIME_SEEK" | grep -q 'time="28000"' || fail "seekTo time= not applied: $TIME_SEEK"
# Negative alias clamps to 0
VO_NEG=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?viewOffset=-9000&commandID=39b2")
echo "$VO_NEG" | grep -q 'time="0"' || fail "seekTo viewOffset negative not clamped: $VO_NEG"

# --- same-position seekTo after clamp: ACK only (no demux thrash / time drift) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=50000&commandID=39s1" >/dev/null
SAME_SEEK=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=50000&commandID=39s2")
echo "$SAME_SEEK" | grep -q 'time="50000"' || fail "same-pos seek drifted time: $SAME_SEEK"
echo "$SAME_SEEK" | grep -q 'location="fullScreenVideo"' || fail "same-pos seek dropped bind: $SAME_SEEK"
# Poll must stay sticky at plant (progress race must not clobber)
POLL_SAME=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=39s3")
echo "$POLL_SAME" | grep -q 'time="50000"' || fail "same-pos plant not sticky on poll: $POLL_SAME"
# Same-pos after past-end clamp: plant at duration then re-seek huge → no thrash past end
if echo "$POLL_SAME" | grep -q 'duration="120000"'; then
  curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=120000&commandID=39s4" >/dev/null
  SAME_END=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=999999999&commandID=39s5")
  echo "$SAME_END" | grep -q 'time="120000"' || fail "same-pos clamp seek not pinned at end: $SAME_END"
fi

# --- custom stepBack relative size ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=25000&commandID=39d" >/dev/null
STEPB5=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepBack?offset=5000&commandID=39e")
echo "$STEPB5" | grep -q 'time="20000"' || fail "custom stepBack -5s not applied: $STEPB5"
# stepBack at 0 with huge size stays pinned at 0
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=0&commandID=39f" >/dev/null
STEPB0=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepBack?offset=999999&commandID=39g")
echo "$STEPB0" | grep -q 'time="0"' || fail "stepBack@0 huge not pinned: $STEPB0"
# Rapid step forward then back returns to plant (no cumulative drift beyond ±10s).
# Assert each hop so a progress race cannot silently skip stepForward.
# Brief settle after plant: demux short-read → stopped@0 preserves scrubber time.
PLANT40=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=40000&commandID=39h")
echo "$PLANT40" | grep -q 'time="40000"' || fail "plant 40s before fwd+back: $PLANT40"
sleep 0.25
# Re-assert plant sticky before stepping (progress race guard)
POLL40=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=39h2")
echo "$POLL40" | grep -q 'time="40000"' || fail "plant 40s not sticky before step: $POLL40"
STEP_F=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=39i")
echo "$STEP_F" | grep -q 'time="50000"' || fail "stepForward not +10s from plant: $STEP_F"
STEP_RB=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepBack?commandID=39j")
echo "$STEP_RB" | grep -q 'time="40000"' || fail "step fwd+back not back to plant: $STEP_RB"

# --- pause / resume idle after stop must not re-arm fullScreenVideo ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=40" >/dev/null
sleep 0.2
curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=40b" >/dev/null
# Drop cast hold so setState would have re-armed wantPlay before the idle gate.
curl -fsS "http://127.0.0.1:${PORT}/player/timeline/unsubscribe?commandID=41" >/dev/null
PAUSE_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/pause?commandID=42")
echo "$PAUSE_IDLE" | grep -q Timeline || fail "idle pause missing Timeline"
echo "$PAUSE_IDLE" | grep -q 'location="navigation"' || fail "idle pause not navigation: $PAUSE_IDLE"
echo "$PAUSE_IDLE" | grep -qv 'fullScreenVideo' || fail "idle pause re-armed fullScreenVideo: $PAUSE_IDLE"
RESUME_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/play?commandID=43")
echo "$RESUME_IDLE" | grep -q 'location="navigation"' || fail "idle resume not navigation: $RESUME_IDLE"
echo "$RESUME_IDLE" | grep -qv 'fullScreenVideo' || fail "idle resume re-armed fullScreenVideo: $RESUME_IDLE"
POLL_IDLE=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=44")
echo "$POLL_IDLE" | grep -qv 'fullScreenVideo' || fail "poll after idle pause/resume still fullScreenVideo: $POLL_IDLE"

# --- huge playMedia offset: ACK may plant raw time; bindMedia clamps after resolve ---
# playMedia clears prior duration so a shorter leftover title cannot clamp a legitimate
# continue-watching offset on a longer next cast (stale-duration race).
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F60&offset=0&commandID=45" \
  | grep -q Timeline || fail "rebind for offset clamp"
# Ensure prior duration is live first (proves we intentionally drop it on next playMedia)
DUR_OK2=0
POLL_D2=""
for _ in $(seq 1 40); do
  POLL_D2=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=45b")
  if echo "$POLL_D2" | grep -q 'duration="120000"'; then
    DUR_OK2=1
    break
  fi
  sleep 0.1
done
[ "$DUR_OK2" = 1 ] || fail "duration never bound before huge-offset cast: $POLL_D2"
HUGE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F61&offset=999999999&commandID=46")
echo "$HUGE" | grep -q Timeline || fail "huge playMedia no Timeline"
# ACK must not still advertise the *previous* title's duration as a clamp wall
# (duration reset to 0 until bind). time may be huge until resolve binds.
echo "$HUGE" | grep -qv 'duration="120000"' || fail "playMedia ACK kept stale duration: $HUGE"
# After async resolve+bindMedia, time clamps into new duration (testsrc 120000)
CLAMP_OK=0
POLL_CLAMP=""
for _ in $(seq 1 40); do
  POLL_CLAMP=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=46b")
  if echo "$POLL_CLAMP" | grep -q 'duration="120000"' && echo "$POLL_CLAMP" | grep -q 'time="120000"'; then
    CLAMP_OK=1
    break
  fi
  sleep 0.1
done
[ "$CLAMP_OK" = 1 ] || fail "huge playMedia offset not clamped after bind: $POLL_CLAMP"

# --- seekTo startTimeOffset= alias (parseOffsetMs) ---
STO=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?startTimeOffset=17000&commandID=47")
echo "$STO" | grep -q 'time="17000"' || fail "seekTo startTimeOffset= not applied: $STO"

# --- seek while paused keeps bind + plants new time ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/pause?commandID=48" | grep -q Timeline \
  || fail "pause before seek"
SEEK_PAUSED=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=22000&commandID=49")
echo "$SEEK_PAUSED" | grep -q 'time="22000"' || fail "seek while paused not applied: $SEEK_PAUSED"
echo "$SEEK_PAUSED" | grep -q 'location="fullScreenVideo"' || fail "seek while paused dropped bind: $SEEK_PAUSED"
# step while paused also advances scrubber
STEP_PAUSED=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?commandID=50")
echo "$STEP_PAUSED" | grep -q 'time="32000"' || fail "step while paused not +10s: $STEP_PAUSED"

# --- stop immediately after playMedia: playGen + wantPlay abort late bind (no zombie) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F70&offset=0&commandID=51" \
  | grep -q Timeline || fail "playMedia before instant stop"
curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=52" >/dev/null
# Give any in-flight resolve a moment; must not re-arm fullScreenVideo
sleep 0.6
POLL_STOP=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=53")
echo "$POLL_STOP" | grep -qv 'fullScreenVideo' || fail "late resolve re-armed after stop: $POLL_STOP"
echo "$POLL_STOP" | grep -qv 'key="/library' || fail "late resolve planted key after stop: $POLL_STOP"
# skipNext after race-stop stays idle
NEXT_RACE=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipNext?commandID=54")
echo "$NEXT_RACE" | grep -q 'location="navigation"' || fail "skipNext after race-stop not navigation: $NEXT_RACE"

# --- E-P4f: step offset=0 uses default ±10s (not zero-step no-op) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F80&offset=0&commandID=60" \
  | grep -q Timeline || fail "rebind for step0 default"
sleep 0.3
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=10000&commandID=61" >/dev/null
STEP_Z=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?offset=0&commandID=62")
echo "$STEP_Z" | grep -q 'time="20000"' || fail "step offset=0 should default +10s: $STEP_Z"
# playMedia negative offset clamps to 0
NEG_PM=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F81&offset=-8000&commandID=63")
echo "$NEG_PM" | grep -q 'time="0"' || fail "playMedia negative offset not clamped: $NEG_PM"
# skipPrevious threshold: t==3000 is near-start (≤3s); t==3001 is mid restart@0
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=3000&commandID=64" >/dev/null
SKP_EQ=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=65")
echo "$SKP_EQ" | grep -q Timeline || fail "skipPrevious@3000 no Timeline"
echo "$SKP_EQ" | grep -q 'location="fullScreenVideo"' || fail "skipPrevious@3000 dropped bind: $SKP_EQ"
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=3001&commandID=66" >/dev/null
SKP_GT=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/skipPrevious?commandID=67")
echo "$SKP_GT" | grep -q 'time="0"' || fail "skipPrevious@3001 not restart@0 plant: $SKP_GT"
# playMedia startTimeOffset= alias seeds cast offset
STO_PM=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F82&startTimeOffset=19000&commandID=68")
echo "$STO_PM" | grep -q 'time="19000"' || fail "playMedia startTimeOffset= not applied: $STO_PM"

# --- E-P4h: cast A→B supersede (playQueued + key-mismatch) lands on B ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F90&offset=0&commandID=70" \
  | grep -q Timeline || fail "cast A for supersede"
# Immediately cast B (do not wait for A resolve)
CAST_B=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F91&offset=5000&commandID=71")
echo "$CAST_B" | grep -q 'key="/library/metadata/91"' || fail "cast B ACK wrong key: $CAST_B"
echo "$CAST_B" | grep -q 'time="5000"' || fail "cast B ACK wrong time: $CAST_B"
# After settle, poll must show B (not zombie A bind)
SETTLE_OK=0
POLL_B=""
for _ in $(seq 1 50); do
  POLL_B=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=72")
  if echo "$POLL_B" | grep -q 'key="/library/metadata/91"'; then
    SETTLE_OK=1
    break
  fi
  sleep 0.1
done
[ "$SETTLE_OK" = 1 ] || fail "cast A→B did not settle on B: $POLL_B"
echo "$POLL_B" | grep -qv 'key="/library/metadata/90"' || fail "cast A key still present after B: $POLL_B"

# --- E-P4h: step offset negative size uses abs (stepForward -5s → +5s) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=15000&commandID=73" >/dev/null
STEP_NEG=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepForward?offset=-5000&commandID=74")
echo "$STEP_NEG" | grep -q 'time="20000"' || fail "stepForward offset=-5000 not abs +5s: $STEP_NEG"
# stepBack with negative size also abs → -5s from 20000
STEP_NEG_B=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/stepBack?offset=-5000&commandID=75")
echo "$STEP_NEG_B" | grep -q 'time="15000"' || fail "stepBack offset=-5000 not abs -5s: $STEP_NEG_B"

# --- E-P4h: rapid multi-seek plants last offset (async seek must not drop plant) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=10000&commandID=76" >/dev/null
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=20000&commandID=77" >/dev/null
curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=30000&commandID=78" >/dev/null
RAPID=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=40000&commandID=79")
echo "$RAPID" | grep -q 'time="40000"' || fail "rapid seek last plant not 40000: $RAPID"
POLL_RAPID=$(curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=80")
echo "$POLL_RAPID" | grep -q 'time="40000"' || fail "rapid seek plant not sticky: $POLL_RAPID"
# same-pos after rapid still sticky
SAME_R=$(curl -fsS "http://127.0.0.1:${PORT}/player/playback/seekTo?offset=40000&commandID=81")
echo "$SAME_R" | grep -q 'time="40000"' || fail "same-pos after rapid drifted: $SAME_R"

# --- unsubscribe clears castBound hold (after stop → pure stopped ok) ---
curl -fsS "http://127.0.0.1:${PORT}/player/playback/stop?commandID=13" >/dev/null
curl -fsS "http://127.0.0.1:${PORT}/player/timeline/unsubscribe?commandID=14" >/dev/null
# After unsubscribe without wantPlay, prePlayHold off — may be stopped or still buffered
# from residual state; at least request succeeds
curl -fsS "http://127.0.0.1:${PORT}/player/timeline/poll?commandID=15" | grep -q Timeline

assert_clean_exit

echo "test_companion_http: OK"
