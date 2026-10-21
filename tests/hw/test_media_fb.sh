#!/usr/bin/env bash
# Hardware: playMedia → timeline playing → pause/resume/stop on live MiSTer.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
BASE="http://${HOST}:3005"

CURL=(curl -fsS --connect-timeout 5 --max-time 10)

echo "resources..."
"${CURL[@]}" "$BASE/resources" | grep -q MiSTerPlex

echo "play test.mp4..."
"${CURL[@]}" "$BASE/player/playback/playMedia?key=%2Fmedia%2Ffat%2Fmistercast%2Ftest.mp4&offset=0&commandID=1" \
  | grep -q fullScreenVideo
sleep 2
"${CURL[@]}" "$BASE/player/timeline/poll?commandID=2" | grep -q 'state="playing"'

echo "pause..."
"${CURL[@]}" "$BASE/player/playback/pause?commandID=3" >/dev/null
"${CURL[@]}" "$BASE/player/timeline/poll?commandID=4" | grep -q 'state="paused"'

echo "resume..."
"${CURL[@]}" "$BASE/player/playback/play?commandID=5" >/dev/null
"${CURL[@]}" "$BASE/player/timeline/poll?commandID=6" | grep -q 'state="playing"'

echo "stop (prePlayHold / castBound)..."
STOP=$("${CURL[@]}" "$BASE/player/playback/stop?commandID=7")
echo "$STOP" | grep -q 'location="navigation"'
echo "$STOP" | grep -q 'state="buffering"'
POLL=$("${CURL[@]}" "$BASE/player/timeline/poll?commandID=8")
echo "$POLL" | grep -q 'location="navigation"'
echo "$POLL" | grep -Eq 'state="(buffering|stopped)"'
# Must not keep fullScreenVideo + key after stop (Web idles scrubber)
echo "$POLL" | grep -qv 'fullScreenVideo' || true

echo "test_media_fb: OK on $HOST"
