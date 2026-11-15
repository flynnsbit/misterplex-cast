#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# test_offline_install.sh - install the MisterPlex db into a throwaway fake SD card with the
# real MiSTer Downloader, served from 127.0.0.1 (no device, no GitHub), then run
# Scripts/misterplex_setup.sh against that card (ROOT=<fake>) and check the result.
#
# DEBUG=true is what lets Downloader accept a base_path outside /media (as downloader_test.py does).
# usage: test_offline_install.sh DOWNLOADER_SRC_DIR [YTDLP_ZIP]
#   DOWNLOADER_SRC_DIR  a checkout of MiSTer-devel/Downloader_MiSTer (its src/ is run with python3)
#   YTDLP_ZIP           local copy of the external yt-dlp zip (served instead of github.com)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DL=${1:?Downloader_MiSTer checkout}; YT=${2:-}
[[ -s "$HERE/db.json.zip" ]] || { echo "run build_db.sh first" >&2; exit 1; }
T=$(mktemp -d); SRV=; trap '[[ -n "$SRV" ]] && kill "$SRV" 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/www/payload" "$T/www/ext" "$T/root/media/fat/linux"
cp -RL "$HERE/payload/." "$T/www/payload/"
[[ -n "$YT" ]] && cp "$YT" "$T/www/ext/yt-dlp_linux_armv7l.zip"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
# Downloader refuses loopback/private URLs, so the db names a reserved .test host and
# Downloader reaches it through HTTP_PROXY=127.0.0.1:PORT; this tiny proxy serves www/.
cat > "$T/proxy.py" <<'PYX'
import http.server, os, sys, urllib.parse
root = sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=root, **k)
    def translate_path(self, path):
        return super().translate_path(urllib.parse.urlsplit(path).path or "/")
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PYX
python3 "$T/proxy.py" "$PORT" "$T/www" >/dev/null 2>&1 & SRV=$!
HOST=misterplex-db.test
# Same db, URLs pointed at the local server.
python3 - "$HERE/db.json.zip" "$T/www/db.json" "$HOST" "${YT:+1}" <<'PY'
import json, sys, zipfile
src, dst, host, yt = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else ""
db = json.loads(zipfile.ZipFile(src).read("db.json"))
db["base_files_url"] = "http://%s/payload/" % host
for p, d in list(db["files"].items()):
    if "url" in d:
        if yt and p.endswith("yt-dlp_linux_armv7l.zip"):
            d["url"] = "http://%s/ext/yt-dlp_linux_armv7l.zip" % host
        else:
            del db["files"][p]   # external file not served offline: leave it out of this test
json.dump(db, open(dst, "w"))
PY
(cd "$T/www" && zip -q db.json.zip db.json)
FAT="$T/root/media/fat"
# A card that already has user settings, an old core name, a root core copy and a user-startup.sh.
printf 'FRIENDLY_NAME=keep-me\nOVERSCAN=7\n' > "$FAT/linux/misterplex.conf"
printf '#!/bin/sh\n# user line\n' > "$FAT/linux/user-startup.sh"
mkdir -p "$FAT/_Utility"; echo old > "$FAT/_Utility/PlexOB.rbf"; echo old > "$FAT/PlexOB.rbf"; echo old > "$FAT/MiSTerPlex.rbf"
DB_ID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["db_id"])' "$T/www/db.json")
cat > "$T/downloader.ini" <<INI
[mister]
base_path = $FAT/
base_system_path = $FAT/
update_linux = false
allow_reboot = 0
verbose = false
downloader_retries = 0

[$DB_ID]
db_url = http://$HOST/db.json.zip
INI
sleep 1
(cd "$T" && DOWNLOADER_INI_PATH="$T/downloader.ini" DEBUG=true SKIP_FREE_SPACE_CHECKS=true CURL_SSL='' \
   UPDATE_LINUX=false ALLOW_REBOOT=0 HTTP_PROXY="http://127.0.0.1:$PORT" python3 "$DL/src/__main__.py") > "$T/downloader.log" 2>&1 || {
  tail -40 "$T/downloader.log"; echo "test: downloader failed" >&2; exit 1; }
cp "$T/downloader.log" "${MPX_TEST_LOG:-/dev/null}" 2>/dev/null || true; tail -15 "$T/downloader.log"
python3 - "$HERE/db.json.zip" "$FAT" "${YT:+1}" <<'PY'
import hashlib, json, os, sys, zipfile
db = json.loads(zipfile.ZipFile(sys.argv[1]).read("db.json")); fat = sys.argv[2]; yt = sys.argv[3] if len(sys.argv) > 3 else ""
bad = 0
for p, d in db["files"].items():
    if "url" in d and not yt: continue
    fp = os.path.join(fat, p)
    if not os.path.isfile(fp) or hashlib.md5(open(fp, "rb").read()).hexdigest() != d["hash"]:
        print("MISSING/BAD after downloader:", p); bad += 1
print("downloader tree:", "OK" if not bad else "FAIL", len(db["files"]), "files listed")
sys.exit(1 if bad else 0)
PY
# Post-install step, exactly as a user runs it, against the fake card.
ROOT="$T/root" sh "$FAT/Scripts/misterplex_setup.sh" > "$T/setup.log" 2>&1 || { cat "$T/setup.log"; echo "test: setup failed" >&2; exit 1; }
ROOT="$T/root" sh "$FAT/Scripts/misterplex_setup.sh" > "$T/setup2.log" 2>&1 || { cat "$T/setup2.log"; echo "test: setup re-run failed" >&2; exit 1; }
cat "$T/setup.log"
fail=0
chk() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi; }
chk "conf kept (FRIENDLY_NAME, OVERSCAN)" "grep -qx 'FRIENDLY_NAME=keep-me' '$FAT/linux/misterplex.conf' && grep -qx 'OVERSCAN=7' '$FAT/linux/misterplex.conf'"
chk "PlexOB.rbf removed (_Utility and root)" "[ ! -e '$FAT/_Utility/PlexOB.rbf' ] && [ ! -e '$FAT/PlexOB.rbf' ]"
chk "root MiSTerPlex.rbf moved aside" "[ ! -e '$FAT/MiSTerPlex.rbf' ] && ls '$FAT/misterplex/backup/'MiSTerPlex.rbf.root.* >/dev/null 2>&1"
chk "one core in _Utility" "[ -f '$FAT/_Utility/MiSTerPlex.rbf' ]"
chk "CA bundle in linux/" "cmp -s '$FAT/misterplex/linux/misterplex-cacert.pem' '$FAT/linux/misterplex-cacert.pem'"
chk "gcast + airplay helpers in linux/ (MD5SUMS ok)" "(cd '$FAT/linux/gcast' && md5sum -c --quiet MD5SUMS) && (cd '$FAT/linux/airplay' && md5sum -c --quiet MD5SUMS)"
chk "boot line added exactly once" "[ \$(grep -c 'misterplexd-boot-rc3.sh' '$FAT/linux/user-startup.sh') -eq 1 ]"
chk "user line in user-startup.sh kept" "grep -q '# user line' '$FAT/linux/user-startup.sh'"
rm "$FAT/linux/misterplex.conf"; ROOT="$T/root" sh "$FAT/Scripts/misterplex_setup.sh" >/dev/null 2>&1
chk "fresh card: conf created from example" "cmp -s '$FAT/misterplex/conf/misterplex.conf.example' '$FAT/linux/misterplex.conf'"
echo "--- user-startup.sh"; cat "$FAT/linux/user-startup.sh"
[[ $fail = 0 ]] && echo "test_offline_install: PASS" || { echo "test_offline_install: FAIL"; exit 1; }
