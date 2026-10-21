#!/usr/bin/env bash
# Hardware: playMedia with play-queue fields → timeline scrubber bind.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
BASE="http://${HOST}:3005"
PMS_HOST="${PMS_HOST:-${PLEX_HOST:-YOUR-PLEX-SERVER}}"
PMS_MACHINE_ID="${PMS_MACHINE_ID:-server-mid}"

echo "=== playMedia with full queue bind ==="
# Simulate Plex Web cast URL fields
BODY=$(curl -fsS --get "$BASE/player/playback/playMedia" \
  --data-urlencode "key=/library/metadata/3" \
  --data-urlencode "containerKey=/playQueues/777?own=1" \
  --data-urlencode "playQueueItemID=555" \
  --data-urlencode "playQueueVersion=2" \
  --data-urlencode "ratingKey=3" \
  --data-urlencode "address=${PMS_HOST}" \
  --data-urlencode "port=32400" \
  --data-urlencode "protocol=http" \
  --data-urlencode "machineIdentifier=${PMS_MACHINE_ID}" \
  --data-urlencode "offset=0" \
  --data-urlencode "commandID=80")
echo "$BODY" | grep -q Timeline

sleep 2
POLL=$(curl -fsS "$BASE/player/timeline/poll?commandID=81")
echo "$POLL"
echo "$POLL" | grep -q 'playQueueID="777"'
echo "$POLL" | grep -q 'playQueueItemID="555"'
echo "$POLL" | grep -q 'containerKey="/playQueues/777'
echo "$POLL" | grep -q 'key="/library/metadata/3"'
echo "$POLL" | grep -Eq 'state="(playing|buffering)"'
echo "$POLL" | grep -q 'location="fullScreenVideo"'

curl -fsS "$BASE/player/playback/stop?commandID=82" >/dev/null
echo "test_playqueue_bind: OK on $HOST"
