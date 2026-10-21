#!/bin/sh
# Installer update against a fake release on 127.0.0.1 only.
# One HTTP zip case and one HTTPS zip case (local CA via --cacert).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT="$ROOT/scripts/update_ytdlp.sh"
fail() { echo "FAIL $*" >&2; exit 1; }
command -v python3 >/dev/null || fail "python3"
command -v sha256sum >/dev/null || fail "sha256sum"
command -v unzip >/dev/null || fail "unzip"
command -v openssl >/dev/null || fail "openssl"
grep -q -- '--cacert' "$SCRIPT" || fail "updater does not pass --cacert"
grep -q 'yt-dlp_linux_armv7l.zip' "$SCRIPT" || fail "updater still wants the bare asset"
grep -q 'trap cleanup EXIT' "$SCRIPT" || fail "updater has no EXIT trap"
grep -q -- '--speed-limit 1024' "$SCRIPT" || fail "updater has no speed-limit"
grep -q -- '--speed-time 30' "$SCRIPT" || fail "updater has no speed-time"
grep -q -- '--max-time 600' "$SCRIPT" || fail "updater still uses a short max-time"
grep -q 'yt-dlp-dl' "$SCRIPT" || fail "updater does not sweep stale download dirs"
work=$(mktemp -d)
srv=
tls=
tagpid=
trap 'kill $srv $tls ${tagpid:-} 2>/dev/null || true; rm -rf "$work"' EXIT
mkdir -p "$work/www" "$work/old/yt-dlp/_internal" "$work/tls"
printf 'OLD' > "$work/old/yt-dlp/yt-dlp_linux_armv7l"
printf 'old' > "$work/old/yt-dlp/_internal/marker"
python3 - "$work/www/yt-dlp_linux_armv7l.zip" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
info = zipfile.ZipInfo("yt-dlp_linux_armv7l")
info.external_attr = 0o755 << 16
z.writestr(info, b"NEWBYTES")
z.writestr("_internal/marker", b"m")
z.close()
PY
sum=$(sha256sum "$work/www/yt-dlp_linux_armv7l.zip" | awk '{print $1}')
printf '%s  yt-dlp_linux_armv7l.zip\n' "$sum" > "$work/www/SHA2-256SUMS"
python3 - "$work/www" <<'PY' &
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root = sys.argv[1]
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        name = self.path.split("?")[0].lstrip("/")
        path = root + "/" + name
        try:
            data = open(path, "rb").read()
        except OSError:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, *a):
        pass
ThreadingHTTPServer.allow_reuse_address = True
s = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(root + "/port", "w").write(str(s.server_address[1]))
s.serve_forever()
PY
srv=$!
i=0
while [ ! -s "$work/www/port" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || fail "server did not bind"
  sleep 0.05
done
port=$(cat "$work/www/port")
rm -rf "$work/old/yt-dlp"
mkdir -p "$work/old/yt-dlp/_internal"
printf 'OLD' > "$work/old/yt-dlp/yt-dlp_linux_armv7l"
YTDLP_RELEASE_BASE="http://127.0.0.1:$port" \
YTDLP_DEST="$work/old/yt-dlp" \
  sh "$SCRIPT" || fail "good update rc=$?"
got=$(cat "$work/old/yt-dlp/yt-dlp_linux_armv7l")
prev=$(cat "$work/old/yt-dlp.prev/yt-dlp_linux_armv7l")
[ "$got" = "NEWBYTES" ] || fail "dest not swapped ($got)"
[ "$prev" = "OLD" ] || fail "previous not kept ($prev)"
[ -f "$work/old/yt-dlp/_internal/marker" ] || fail "onedir _internal missing"
grep -q 'yt-dlp_linux_armv7l' "$work/old/yt-dlp/yt-dlp" || fail "launcher missing"
[ ! -L "$work/old/yt-dlp/yt-dlp" ] || fail "launcher is a symlink"

python3 - "$work/www/yt-dlp_linux_armv7l.zip" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
z.writestr("yt-dlp_linux_armv7l", b"TAMPER")
z.close()
PY
if YTDLP_RELEASE_BASE="http://127.0.0.1:$port" \
   YTDLP_DEST="$work/old/yt-dlp" \
   sh "$SCRIPT"; then
  fail "bad checksum was accepted"
fi
got=$(cat "$work/old/yt-dlp/yt-dlp_linux_armv7l")
[ "$got" = "NEWBYTES" ] || fail "bad checksum replaced dest ($got)"

# HTTPS with a test CA. curl must use --cacert (plain curl is rc=60 on MiSTer).
openssl req -x509 -newkey rsa:2048 -keyout "$work/tls/ca.key" -out "$work/tls/ca.pem" \
  -days 1 -nodes -subj /CN=MisterplexTestCA >/dev/null 2>&1
openssl req -newkey rsa:2048 -keyout "$work/tls/srv.key" -out "$work/tls/srv.csr" \
  -nodes -subj /CN=127.0.0.1 >/dev/null 2>&1
printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\n' > "$work/tls/san.cnf"
openssl x509 -req -in "$work/tls/srv.csr" -CA "$work/tls/ca.pem" -CAkey "$work/tls/ca.key" \
  -CAcreateserial -out "$work/tls/srv.crt" -days 1 -extfile "$work/tls/san.cnf" >/dev/null 2>&1
cp "$work/www/SHA2-256SUMS" "$work/tls/SHA2-256SUMS"
# Restore a good zip; the tamper case replaced the http copy.
python3 - "$work/tls/yt-dlp_linux_armv7l.zip" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w")
info = zipfile.ZipInfo("yt-dlp_linux_armv7l")
info.external_attr = 0o755 << 16
z.writestr(info, b"TLSBYTES")
z.writestr("_internal/marker", b"t")
z.close()
PY
sum=$(sha256sum "$work/tls/yt-dlp_linux_armv7l.zip" | awk '{print $1}')
printf '%s  yt-dlp_linux_armv7l.zip\n' "$sum" > "$work/tls/SHA2-256SUMS"
python3 - "$work/tls" <<'PY' &
import ssl, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root = sys.argv[1]
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        name = self.path.split("?")[0].lstrip("/")
        path = root + "/" + name
        try:
            data = open(path, "rb").read()
        except OSError:
            self.send_response(404); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, *a):
        pass
ThreadingHTTPServer.allow_reuse_address = True
s = ThreadingHTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(root + "/srv.crt", root + "/srv.key")
s.socket = ctx.wrap_socket(s.socket, server_side=True)
open(root + "/port", "w").write(str(s.server_address[1]))
s.serve_forever()
PY
tls=$!
i=0
while [ ! -s "$work/tls/port" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || fail "tls server did not bind"
  sleep 0.05
done
tport=$(cat "$work/tls/port")
if MPX_CA_BUNDLE="$work/tls/missing.pem" \
   YTDLP_RELEASE_BASE="https://127.0.0.1:$tport" \
   YTDLP_DEST="$work/old/yt-dlp" \
   sh "$SCRIPT"; then
  fail "https without a CA bundle was accepted"
fi
MPX_CA_BUNDLE="$work/tls/ca.pem" \
YTDLP_RELEASE_BASE="https://127.0.0.1:$tport" \
YTDLP_DEST="$work/old/yt-dlp" \
  sh "$SCRIPT" || fail "tls update rc=$?"
got=$(cat "$work/old/yt-dlp/yt-dlp_linux_armv7l")
[ "$got" = "TLSBYTES" ] || fail "tls dest not swapped ($got)"
[ -f "$work/old/yt-dlp/_internal/marker" ] || fail "tls onedir _internal missing"

# A failed fetch must not leave .yt-dlp-dl.* behind (set -e used to skip the rm).
mkdir -p "$work/old/.yt-dlp-dl.stale"
printf 'junk' > "$work/old/.yt-dlp-dl.stale/partial"
if YTDLP_RELEASE_BASE="http://127.0.0.1:$port/missing" \
   YTDLP_DEST="$work/old/yt-dlp" \
   YTDLP_TAG_URL="http://127.0.0.1:$port/no-tag" \
   sh "$SCRIPT"; then
  fail "missing release was accepted"
fi
left=$(find "$work/old" -maxdepth 1 -name '.yt-dlp-dl.*' | wc -l)
[ "$left" -eq 0 ] || fail "stale or failed download dir left behind ($left)"
got=$(cat "$work/old/yt-dlp/yt-dlp_linux_armv7l")
[ "$got" = "TLSBYTES" ] || fail "failed fetch replaced dest ($got)"

# Installed version equals the latest tag: no zip download.
tagport=""
python3 - "$work/tag" <<'PY' &
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(302)
        self.send_header("Location", "http://127.0.0.1/tag/2026.08.19")
        self.end_headers()
    def log_message(self, *a):
        pass
ThreadingHTTPServer.allow_reuse_address = True
s = ThreadingHTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1] + ".port", "w").write(str(s.server_address[1]))
s.serve_forever()
PY
tagpid=$!
i=0
while [ ! -s "$work/tag.port" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || fail "tag server did not bind"
  sleep 0.05
done
tagport=$(cat "$work/tag.port")
mkdir -p "$work/old/yt-dlp"
printf '%s\n' '#!/bin/sh' 'echo 2026.08.19' > "$work/old/yt-dlp/yt-dlp"
chmod +x "$work/old/yt-dlp/yt-dlp"
printf 'KEEP' > "$work/old/yt-dlp/yt-dlp_linux_armv7l"
mkdir -p "$work/old/.yt-dlp-dl.older"
before=$(cat "$work/old/yt-dlp/yt-dlp_linux_armv7l")
YTDLP_TAG_URL="http://127.0.0.1:$tagport/latest" \
YTDLP_RELEASE_BASE="http://127.0.0.1:$port" \
YTDLP_DEST="$work/old/yt-dlp" \
  sh "$SCRIPT" || fail "version skip rc=$?"
after=$(cat "$work/old/yt-dlp/yt-dlp_linux_armv7l")
[ "$after" = "$before" ] || fail "version skip replaced dest ($after)"
[ "$after" = "KEEP" ] || fail "version skip dest bytes ($after)"
left=$(find "$work/old" -maxdepth 1 -name '.yt-dlp-dl.*' | wc -l)
[ "$left" -eq 0 ] || fail "version skip left a download dir"

# A dir whose .lock is held is an in-progress updater. The sweep removes an
# unlocked stale dir and leaves the live one, payload included.
mkdir -p "$work/old/.yt-dlp-dl.live" "$work/old/.yt-dlp-dl.dead"
printf live > "$work/old/.yt-dlp-dl.live/partial"
printf dead > "$work/old/.yt-dlp-dl.dead/partial"
exec 7>"$work/old/.yt-dlp-dl.live/.lock"
flock -x 7
if YTDLP_RELEASE_BASE="http://127.0.0.1:$port/missing" \
   YTDLP_DEST="$work/old/yt-dlp" \
   YTDLP_TAG_URL="http://127.0.0.1:$port/no-tag" \
   sh "$SCRIPT"; then
  fail "fetch with a live lock dir was accepted"
fi
[ -d "$work/old/.yt-dlp-dl.live" ] || fail "sweep removed a dir holding a live lock"
[ "$(cat "$work/old/.yt-dlp-dl.live/partial")" = "live" ] || fail "live lock dir was emptied"
[ ! -e "$work/old/.yt-dlp-dl.dead" ] || fail "unlocked stale dir survived the sweep"
flock -u 7
exec 7>&-
kill "$tagpid" 2>/dev/null || true
echo "test_update_ytdlp: ok"
