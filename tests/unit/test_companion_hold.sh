#!/usr/bin/env bash
# Unit: bounded buffering@navigation wire hold (holdIdle RCA; review H1-H4, N1-N3).
# After Stop or mirror, a cast-bound player reports buffering@navigation only for
# MPX_COMPANION_STOP_HOLD_MS / MPX_COMPANION_MIRROR_HOLD_MS, then stopped; a poll
# never re-arms it; a cold poll-only start is stopped; one push follows expiry.
# Margins sized for load (N2): window 1000 ms, "within" < 300 ms, "after" > 2000 ms.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/build/misterplexd"
make -C "$ROOT" plexd >/dev/null
WORK=$(mktemp -d "${TMPDIR:-/tmp}/companion_hold.XXXXXX")
# T-DAEMON-LOCK-SHARED: a private lock dir (build/misterplexd is an MPX_TEST_BUILD).
export MPX_LOCK_DIR="$WORK/locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
lp_init

free_port() {
  python3 - "$1" <<'PY'
import socket, sys
lo = int(sys.argv[1])
for p in range(lo, lo + 145):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", p)); print(p); s.close(); break
    except OSError:
        s.close()
else:
    raise SystemExit("no free port")
PY
}

PID=""
SINK_PID=""
# misterplexd ignores SIGTERM once the companion is listening. INT, then KILL.
stop_pid() {
  local p=$1
  [ -n "$p" ] || return 0
  kill -0 "$p" 2>/dev/null || return 0
  kill -INT "$p" 2>/dev/null || true
  local i=0
  while [ "$i" -lt 20 ]; do
    kill -0 "$p" 2>/dev/null || return 0
    i=$((i + 1))
    sleep 0.1
  done
  kill -KILL "$p" 2>/dev/null || true
  wait "$p" 2>/dev/null || true
}
cleanup() {
  stop_pid "$PID"
  stop_pid "$SINK_PID"
  rm -rf "$WORK"
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

start_daemon() {  # $1 = log file; env passed by caller
  PORT=$(free_port 13150)
  "$BIN" --name MiSTerPlexHold --port "$PORT" >"$1" 2>&1 &
  PID=$!
  lp_expect "$1" 1 || fail "the daemon's lock paths"
  for _ in $(seq 1 50); do
    curl -fsS "http://127.0.0.1:${PORT}/resources" >/dev/null 2>&1 && return 0
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
  done
  fail "companion HTTP not up on :$PORT"
}
stop_daemon() { stop_pid "$PID"; PID=""; }

U() { echo "http://127.0.0.1:${PORT}$1"; }
poll() { curl -fsS "$(U "/player/timeline/poll?commandID=$1")"; }
state_of() { sed -n 's/.*<Timeline type="video" state="\([a-z]*\)".*/\1/p' <<<"$1"; }
expect() {  # $1 label, $2 xml, $3 state
  local s; s=$(state_of "$2")
  [ "$s" = "$3" ] || fail "$1: state=$s want $3: $2"
  grep -q 'location="navigation"' <<<"$2" || fail "$1: not navigation: $2"
  grep -q 'key="/library' <<<"$2" && fail "$1: media key on idle wire: $2"
  grep -q ' time="0"' <<<"$2" || fail "$1: time not 0: $2"
  return 0
}
ms_now() { date +%s%3N; }

# ---- N1: strict env parse / clamp ----
MPX_COMPANION_STOP_HOLD_MS=abc MPX_COMPANION_MIRROR_HOLD_MS=999999 start_daemon "$WORK/n1.log"
sleep 0.2
grep -q 'wire hold stop_ms=8000 mirror_ms=120000' "$WORK/n1.log" ||
  fail "N1 env parse: $(grep 'wire hold' "$WORK/n1.log" || echo none)"
stop_daemon
MPX_COMPANION_STOP_HOLD_MS=-5 MPX_COMPANION_MIRROR_HOLD_MS=12x start_daemon "$WORK/n1b.log"
sleep 0.2
grep -q 'wire hold stop_ms=8000 mirror_ms=30000' "$WORK/n1b.log" || fail "N1 negative/garbage"
stop_daemon

# ---- main run: 1000 ms windows ----
SINK_PORT=$(free_port 13300)
python3 - "$SINK_PORT" "$WORK/push.log" <<'PY' &
import http.server, sys, time
port, out = int(sys.argv[1]), sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n).decode("utf-8", "replace")
        st = body.split('<Timeline type="video" state="', 1)[-1].split('"', 1)[0]
        with open(out, "a") as f:
            f.write("%d %s\n" % (int(time.time() * 1000), st))
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", port), H).serve_forever()
PY
SINK_PID=$!
: > "$WORK/push.log"

MPX_COMPANION_STOP_HOLD_MS=1000 MPX_COMPANION_MIRROR_HOLD_MS=1000 start_daemon "$WORK/main.log"
grep -q 'wire hold stop_ms=1000 mirror_ms=1000' "$WORK/main.log" || { sleep 0.3;
  grep -q 'wire hold stop_ms=1000 mirror_ms=1000' "$WORK/main.log" || fail "main env not applied"; }

# H2: fresh start, poll only (Web selected the player, no media) -> stopped
expect "H2 cold poll" "$(poll c1)" stopped
expect "H2 cold poll again" "$(poll c2)" stopped

# H3: mirror -> buffering within the window, stopped after it (polls don't extend)
MIRROR=$(curl -fsS "$(U "/player/timeline/mirror?key=%2Flibrary%2Fmetadata%2F2&commandID=m1")")
expect "mirror ACK" "$MIRROR" buffering
expect "mirror poll <300ms" "$(poll m2)" buffering
for i in 1 2 3 4 5 6 7 8 9 10; do poll "m3$i" >/dev/null; sleep 0.2; done  # 2 s of polling
expect "mirror poll >2000ms" "$(poll m4)" stopped

# Stop hold: subscribe (push sink), play, Stop
curl -fsS "$(U "/player/timeline/subscribe?protocol=http&port=${SINK_PORT}&commandID=s1")" >/dev/null
curl -fsS "$(U "/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F1&offset=0&commandID=p1")" | grep -q Timeline
P=$(poll p2)
grep -Eq 'state="(buffering|playing|paused)"' <<<"$P" || fail "play not active: $P"
grep -q 'fullScreenVideo' <<<"$P" || fail "play not fullScreenVideo: $P"
sleep 0.3
STOP=$(curl -fsS "$(U "/player/playback/stop?commandID=x1")")
T_STOP=$(ms_now)
expect "N3 stop ACK" "$STOP" buffering
expect "stop poll <300ms" "$(poll x2)" buffering
for i in 1 2 3 4 5 6 7 8 9 10; do poll "x3$i" >/dev/null; sleep 0.2; done
expect "stop poll >2000ms" "$(poll x4)" stopped
for i in 1 2 3 4 5; do expect "no re-arm $i" "$(poll "x5$i")" stopped; sleep 0.2; done

# H4: one push after expiry carries stopped, within 2.5 s of Stop
LATE=$(awk -v t="$T_STOP" '$1 > t + 900 {print}' "$WORK/push.log")
FIRST_STOPPED=$(awk -v t="$T_STOP" '$1 > t + 900 && $2 == "stopped" {print $1; exit}' "$WORK/push.log")
[ -n "$FIRST_STOPPED" ] || fail "H4 no stopped push after expiry: $(tail -5 "$WORK/push.log")"
[ $((FIRST_STOPPED - T_STOP)) -le 2500 ] || fail "H4 expiry push late: $((FIRST_STOPPED - T_STOP)) ms"
grep -q buffering <<<"$LATE" && fail "H4 buffering pushed after expiry: $LATE"
N_LATE=$(wc -l <<<"$LATE")
[ "$N_LATE" -le 3 ] || fail "H4 push storm after expiry: $N_LATE pushes"

# Next session: mirror -> buffering, playMedia -> live (no regression)
expect "re-mirror" "$(curl -fsS "$(U "/player/timeline/mirror?key=%2Flibrary%2Fmetadata%2F3&commandID=r1")")" buffering
curl -fsS "$(U "/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F3&offset=0&commandID=r2")" | grep -q Timeline
P=$(poll r3)
grep -q 'fullScreenVideo' <<<"$P" || fail "next session not fullScreenVideo: $P"
curl -fsS "$(U "/player/playback/stop?commandID=r4")" >/dev/null
stop_daemon

# Stop hold 0 = no hold: the Stop ACK is already stopped (documented)
MPX_COMPANION_STOP_HOLD_MS=0 start_daemon "$WORK/zero.log"
poll z0 >/dev/null
curl -fsS "$(U "/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F1&offset=0&commandID=z1")" >/dev/null
sleep 0.3
expect "hold 0 stop ACK" "$(curl -fsS "$(U "/player/playback/stop?commandID=z2")")" stopped
stop_daemon

echo "test_companion_hold: OK"
