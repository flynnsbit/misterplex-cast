#!/usr/bin/env bash
# Cast-ready policy: GDM/HTTP gate + watch/supervise contracts.
# RED twin: any-HTTP-200 (wget -O /dev/null) or process-up is not Cast-ready.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
READY="$ROOT/scripts/misterplex_cast_ready.sh"
WATCH="$ROOT/scripts/misterplex_core_watch.sh"
SUP="$ROOT/scripts/misterplexd_supervise.sh"
fails=0

fail() {
  echo "FAIL: $*" >&2
  fails=$((fails + 1))
}
ok() { echo "OK: $*"; }

[ -f "$READY" ] || { echo "missing $READY" >&2; exit 1; }
# shellcheck disable=SC1090
PORT="${PORT:-13005}"
export PORT
. "$READY"

# --- Body oracle --------------------------------------------------------------
good_xml='<?xml version="1.0" encoding="UTF-8"?><MediaContainer><Player title="MiSTerPlex" product="MiSTerPlex" machineIdentifier="misterplex-dev" version="0.4.1"/></MediaContainer>'
empty=''
plain_ok='ok'
wrong_player='<?xml version="1.0"?><MediaContainer><Player title="Other" product="Plexamp" machineIdentifier="x"/></MediaContainer>'
container_only='<MediaContainer></MediaContainer>'

tmp=$(mktemp)
printf '%s\n' "$good_xml" >"$tmp"
cast_resources_looks_ready "$tmp" || fail "good companion XML rejected"
printf '%s\n' "$empty" >"$tmp"
cast_resources_looks_ready "$tmp" && fail "empty body accepted"
printf '%s\n' "$plain_ok" >"$tmp"
cast_resources_looks_ready "$tmp" && fail "plain 200 body accepted (RED twin)"
printf '%s\n' "$wrong_player" >"$tmp"
cast_resources_looks_ready "$tmp" && fail "non-MiSTerPlex player accepted"
printf '%s\n' "$container_only" >"$tmp"
cast_resources_looks_ready "$tmp" && fail "MediaContainer-only accepted"
rm -f "$tmp"
ok "body oracle"

# --- Live HTTP: dummy 200 vs companion XML -----------------------------------
py_port=$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)
PORT=$py_port
export PORT

python3 - "$py_port" <<'PY' &
import sys, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
    body = b"ok\n"
    ctype = "text/plain"
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", self.ctype)
        self.end_headers()
        self.wfile.write(self.body)
    def log_message(self, *args):
        pass

httpd = HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
t = threading.Thread(target=httpd.serve_forever, daemon=True)
t.start()
open("/tmp/mplex-cast-dummy.ready", "w").write("1")
threading.Event().wait()
PY
dummy_pid=$!
for _ in $(seq 1 40); do
  [ -f /tmp/mplex-cast-dummy.ready ] && break
  sleep 0.05
done
rm -f /tmp/mplex-cast-dummy.ready

if cast_ready_legacy_any_http; then
  ok "RED twin: wget -O /dev/null accepts dummy 200"
else
  fail "RED twin wget did not see dummy 200 (server down?)"
fi
if cast_http_ok; then
  fail "cast_http_ok accepted dummy 200 (must require companion XML)"
else
  ok "cast_http_ok rejects dummy 200"
fi
kill "$dummy_pid" 2>/dev/null || true
wait "$dummy_pid" 2>/dev/null || true

python3 - "$py_port" <<'PY' &
import sys, threading
from http.server import BaseHTTPRequestHandler, HTTPServer

BODY = (
    b'<?xml version="1.0" encoding="UTF-8"?>'
    b'<MediaContainer>'
    b'<Player title="MiSTerPlex" product="MiSTerPlex" '
    b'machineIdentifier="misterplex-dev" version="0.4.1"/>'
    b'</MediaContainer>'
)

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/xml")
        self.end_headers()
        self.wfile.write(BODY)
    def log_message(self, *args):
        pass

httpd = HTTPServer(("127.0.0.1", int(sys.argv[1])), H)
t = threading.Thread(target=httpd.serve_forever, daemon=True)
t.start()
open("/tmp/mplex-cast-good.ready", "w").write("1")
threading.Event().wait()
PY
good_pid=$!
for _ in $(seq 1 40); do
  [ -f /tmp/mplex-cast-good.ready ] && break
  sleep 0.05
done
rm -f /tmp/mplex-cast-good.ready
if cast_http_ok; then
  ok "cast_http_ok accepts companion XML"
else
  fail "cast_http_ok rejected companion XML"
fi
kill "$good_pid" 2>/dev/null || true
wait "$good_pid" 2>/dev/null || true

# --- Script contracts ---------------------------------------------------------
grep -q 'misterplex_cast_ready.sh' "$WATCH" || fail "watch does not source cast_ready"
grep -q 'ENSURE_DEAD_HTTP' "$WATCH" || fail "watch missing ENSURE_DEAD_HTTP"
if grep -n 'BIN=$(select_bin_for_live_rbf)' "$WATCH"; then
  fail "select_bin_for_live_rbf in \$() drops last_pair_key (PAIR_OK spam)"
else
  ok "select_bin not in subshell"
fi
grep -q 'cast_http_ok' "$WATCH" || fail "watch missing cast_http_ok"
# TERM must exit. A live `trap 'rm -rf ...' TERM` swallows SIGTERM.
if grep -n "^trap 'rm -rf" "$WATCH" "$SUP" | grep -E 'INT|TERM'; then
  fail "watch/supervise TERM trap swallows signal (must exit)"
else
  ok "TERM trap exits"
fi
grep -q "trap 'exit 0' INT TERM" "$WATCH" || fail "watch missing exit-on-TERM trap"
grep -q "trap 'exit 0' INT TERM" "$SUP" || fail "supervise missing exit-on-TERM trap"
if grep -n 'pidof misterplexd' "$WATCH" | grep -v '^#' >/dev/null; then
  # pidof as sole ready is the old defect
  if grep -A5 'daemon_alive' "$WATCH" | grep -q 'pidof'; then
    fail "watch still treats pidof as ready"
  fi
fi
grep -q 'cast_http_ok' "$SUP" || fail "supervise missing cast_http_ok"
grep -q 'SUPERVISE_HTTP_DEAD' "$SUP" || fail "supervise missing SUPERVISE_HTTP_DEAD"
grep -q 'SUPERVISE_HTTP_LOST' "$SUP" || fail "supervise missing SUPERVISE_HTTP_LOST"
# Ready must not be wget -O /dev/null to /resources (any 200).
if grep -n 'wget .*-O /dev/null.*"http://127.0.0.1:\${PORT}/resources"' "$WATCH" "$SUP"; then
  fail "watch/supervise still treat any HTTP 200 as ready"
else
  ok "no wget -O /dev/null /resources ready check"
fi

# Companion must not advertise GDM until HTTP listen succeeds.
COMP="$ROOT/arm/misterplexd/companion.cpp"
grep -q 'gdmMayReply' "$COMP" || fail "companion gdmLoop missing gdmMayReply"
grep -q 'gdmMayAdvertise' "$COMP" || fail "companion gdmLoop missing gdmMayAdvertise"
grep -q 'openHttpListen' "$COMP" || fail "companion missing openHttpListen"
grep -q 'not advertising GDM' "$COMP" || fail "companion start() missing HTTP-fail mute"
ok "companion GDM-after-HTTP"

if [ "$fails" -ne 0 ]; then
  echo "test_cast_http_ready_policy: $fails failure(s)" >&2
  exit 1
fi
echo "test_cast_http_ready_policy: OK"
exit 0
