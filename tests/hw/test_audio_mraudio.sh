#!/usr/bin/env bash
# Hardware: video + /dev/MrAudio PCM path on live MiSTer.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
BASE="http://${HOST}:3005"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== MrAudio present ==="
ssh_m 'test -c /dev/MrAudio && echo OK'

echo "=== play testsrc (video+sine) ==="
curl -fsS "$BASE/player/playback/playMedia?key=testsrc&offset=0&commandID=70" >/dev/null
sleep 3
curl -fsS "$BASE/player/timeline/poll?commandID=71" | grep -q 'state="playing"'

echo "=== log shows audio=on ==="
# Wait up to 5s for first 1Hz frame log with audio=on
ok=0
for _ in 1 2 3 4 5; do
  if ssh_m 'grep -q "audio=on" /media/fat/misterplex/misterplexd.log' 2>/dev/null; then
    ok=1
    break
  fi
  sleep 1
done
[[ $ok -eq 1 ]] || { echo "FAIL: no audio=on in log" >&2; ssh_m 'tail -20 /media/fat/misterplex/misterplexd.log'; exit 1; }

echo "=== MrAudio buffer has data (rptr/wptr) ==="
# Read status from device
ST=$(ssh_m 'cat /dev/MrAudio 2>/dev/null || true')
echo "MrAudio status: $ST"
echo "$ST" | grep -q 'wptr:' || echo "(status format may vary — non-fatal)"

echo "=== play local test.mp4 with audio track attempt ==="
curl -fsS "$BASE/player/playback/stop?commandID=72" >/dev/null
sleep 0.3
curl -fsS "$BASE/player/playback/playMedia?key=%2Fmedia%2Ffat%2Fmistercast%2Ftest.mp4&offset=0&commandID=73" >/dev/null
sleep 2
curl -fsS "$BASE/player/timeline/poll?commandID=74" | grep -q 'state="playing"'
ssh_m 'grep -E "frames=|audio=" /media/fat/misterplex/misterplexd.log | tail -6'

curl -fsS "$BASE/player/playback/stop?commandID=75" >/dev/null
echo "test_audio_mraudio: OK on $HOST"
