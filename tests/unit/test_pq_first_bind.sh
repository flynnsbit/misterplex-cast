#!/usr/bin/env bash
set -euo pipefail

if [ "$$" -ne 1 ]; then
  echo "VOID: test_pq_first_bind.sh must run as PID 1 inside a private pid namespace" >&2
  exit 77
fi

mount -t tmpfs -o size=1g,mode=1777 tmpfs /tmp
# Bar 2: a FRESH pycache prefix on this run's own tmpfs, whatever the caller passed in.
PYTHONPYCACHEPREFIX=$(mktemp -d /tmp/pq_first_bind_pyc.XXXXXX)
export PYTHONPYCACHEPREFIX
# The daemon reads its settings from MPX_* env: drop every inherited one, so the backend (and with it the resolve
# failure's log form) is the daemon default on every run.
while IFS= read -r v; do unset "$v"; done < <(compgen -e | grep '^MPX_' || true)
ip link set lo up
# L-3 (test_lock_paths_needle.py): the daemon's lock files go to this run's own dir, never a prod path.
export MPX_LOCK_DIR=/tmp/pq-first-bind-lock
mkdir -p "$MPX_LOCK_DIR"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FAKE="$ROOT/tests/fixtures/fake_pms/fake_pms.py"
BIN="${1:-$ROOT/build/misterplexd}"
EXPECT_ITEM="${2:-7002}"
TOKEN="pq25tok"
QID="25"
DAEMON_PORT="13025"
WORK="/tmp/pq-first-bind"
mkdir -p "$WORK"
DAEMON_LOG="$WORK/daemon.log"
PMS_LOG="$WORK/fake_pms.log"
PORT_FILE="$WORK/fake_pms.ports.json"

DAEMON_PID=""
PMS_PID=""
cleanup() {
  # A record keeps the run's logs (the run's own /tmp is a private tmpfs that ends with it).
  if [ -n "${PQ_FIRST_BIND_KEEP:-}" ]; then
    mkdir -p "$PQ_FIRST_BIND_KEEP" && cp -f "$DAEMON_LOG" "$PMS_LOG" "$PQ_FIRST_BIND_KEEP/" 2>/dev/null || true
  fi
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
  fi
  if [ -n "$PMS_PID" ] && kill -0 "$PMS_PID" 2>/dev/null; then
    kill "$PMS_PID" 2>/dev/null || true
    wait "$PMS_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ ! -x "$BIN" ]; then
  make -C "$ROOT" plexd >/dev/null
fi

python3 "$FAKE" --host 127.0.0.1 --host6 "" --port-file "$PORT_FILE" --log "$PMS_LOG" --token "$TOKEN" &
PMS_PID=$!
for _ in $(seq 1 200); do
  if [ -s "$PORT_FILE" ]; then
    break
  fi
  if ! kill -0 "$PMS_PID" 2>/dev/null; then
    echo "FAIL: fake PMS exited early" >&2
    exit 1
  fi
  sleep 0.05
done
if [ ! -s "$PORT_FILE" ]; then
  echo "FAIL: fake PMS did not publish port file" >&2
  exit 1
fi
PMS_PORT=$(python3 - "$PORT_FILE" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as fh:
    print(json.load(fh)["port4"])
PY
)
curl -fsS "http://127.0.0.1:${PMS_PORT}/__ping" >/dev/null

"$BIN" --name MisterPlex --port "$DAEMON_PORT" >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!
ready=0
for _ in $(seq 1 100); do
  if curl -fsS "http://127.0.0.1:${DAEMON_PORT}/resources" >/dev/null 2>&1; then
    ready=1
    break
  fi
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "FAIL: daemon exited before HTTP ready" >&2
    sed -n '1,160p' "$DAEMON_LOG" >&2 || true
    exit 1
  fi
  sleep 0.05
done
[ "$ready" = 1 ] || { echo "FAIL: daemon HTTP not ready" >&2; exit 1; }

PLAY_URL="http://127.0.0.1:${DAEMON_PORT}/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F123&containerKey=%2FplayQueues%2F${QID}%3Fown%3D1&address=127.0.0.1&port=${PMS_PORT}&protocol=http&token=${TOKEN}&machineIdentifier=fake-pms&commandID=pq25"
curl -fsS "$PLAY_URL" >/dev/null

# b3: a resolve that failed is VOID, never PASS or KILLED. Both log forms (main.cpp: 'misterplexd: resolve failed:'
# on the default backend, which still goes on to bind; 'misterplexd: FPGA resolve failed:' on fpga-h264, which
# returns before the bind) are checked BEFORE a bound line is accepted, and again on timeout.
RESOLVE_FAILED='misterplexd: (FPGA )?resolve failed:'
void_if_resolve_failed() {
  if grep -qE "$RESOLVE_FAILED" "$DAEMON_LOG"; then
    echo "VOID: resolve failed before bind"
    grep -E "$RESOLVE_FAILED" "$DAEMON_LOG" | tail -n 1
    exit 77
  fi
}
bound=""
for _ in $(seq 1 120); do
  void_if_resolve_failed
  if grep -q "play queue bound id=${QID} item=" "$DAEMON_LOG"; then
    void_if_resolve_failed
    bound=$(grep "play queue bound id=${QID} item=" "$DAEMON_LOG" | tail -n 1)
    break
  fi
  sleep 0.1
done
void_if_resolve_failed

if [ -z "$bound" ]; then
  echo "FAIL: no play queue bound line" >&2
  sed -n '1,220p' "$DAEMON_LOG" >&2 || true
  exit 1
fi
echo "$bound"
echo "$bound" | grep -q "item=${EXPECT_ITEM}" || {
  echo "FAIL: expected bound item=${EXPECT_ITEM}, got: $bound" >&2
  exit 1
}
# b1: the bound line names the pass that found the row (the key pass: /library/metadata/123 is row 1's key).
echo "$bound" | grep -q "index=1 by=key)" || {
  echo "FAIL: the bound line does not name the pass (want 'index=1 by=key'): $bound" >&2
  exit 1
}

poll=""
for _ in $(seq 1 80); do
  poll=$(curl -fsS "http://127.0.0.1:${DAEMON_PORT}/player/timeline/poll?commandID=pq25poll")
  if echo "$poll" | grep -q "playQueueItemID=\"${EXPECT_ITEM}\""; then
    echo "$poll"
    echo "test_pq_first_bind: OK item=${EXPECT_ITEM}"
    exit 0
  fi
  sleep 0.1
done
echo "FAIL: poll never carried playQueueItemID=\"${EXPECT_ITEM}\": $poll" >&2
exit 1
