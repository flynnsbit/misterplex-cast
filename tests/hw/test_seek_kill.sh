#!/usr/bin/env bash
# Hardware: seek mid-play + kill daemon recovery on live MiSTer.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
BASE="http://${HOST}:3005"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
PMS_URL="${PLEX_BASE:-${PMS_URL:-}}"
PLAYER_ID="${MISTERPLEX_ID:-misterplex-dev}"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== resources ==="
curl -fsS --connect-timeout 5 "$BASE/resources" | grep -q MiSTerPlex

echo "=== play testsrc ==="
curl -fsS "$BASE/player/playback/playMedia?key=testsrc&offset=0&commandID=10" >/dev/null
sleep 2
curl -fsS "$BASE/player/timeline/poll?commandID=11" | tee /tmp/tl1.xml | grep -q 'state="playing"'

echo "=== seekTo 5000ms ==="
curl -fsS "$BASE/player/playback/seekTo?offset=5000&commandID=12" >/dev/null
sleep 2
# timeline should still be playing (may restart stream)
curl -fsS "$BASE/player/timeline/poll?commandID=13" | tee /tmp/tl2.xml | grep -Eq 'state="(playing|buffering)"'

echo "=== kill daemon mid-play ==="
ssh_m 'killall -9 misterplexd 2>/dev/null || true; sleep 0.5'
# Companion dead
if curl -fsS --connect-timeout 2 "$BASE/resources" >/dev/null 2>&1; then
  echo "FAIL: daemon still responding after kill" >&2
  exit 1
fi

echo "=== restart daemon ==="
PMS_ARG=""
if [[ -n "$PMS_URL" ]]; then
  PMS_ARG="--pms '$PMS_URL'"
fi
ssh_m "nohup /media/fat/misterplex/bin/misterplexd --name MiSTerPlex --id '$PLAYER_ID' --port 3005 --conf /media/fat/misterplex/misterplex.conf $PMS_ARG >>/media/fat/misterplex/misterplexd.log 2>&1 &"
sleep 1.5
curl -fsS --connect-timeout 5 "$BASE/resources" | grep -q MiSTerPlex

echo "=== play again after recovery ==="
curl -fsS "$BASE/player/playback/playMedia?key=testsrc&offset=0&commandID=20" >/dev/null
sleep 2
curl -fsS "$BASE/player/timeline/poll?commandID=21" | grep -q 'state="playing"'
curl -fsS "$BASE/player/playback/stop?commandID=22" >/dev/null

echo "test_seek_kill: OK on $HOST"
