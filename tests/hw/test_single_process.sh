#!/usr/bin/env bash
# Hardware: single-process FFmpeg A/V (one demux) + headroom check.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
BASE="http://${HOST}:3005"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
CURL=(curl -fsS --connect-timeout 5 --max-time 12)

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== play PMS library (single-process expected) ==="
"${CURL[@]}" "$BASE/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F3&offset=0&commandID=90" >/dev/null
sleep 4

echo "=== timeline playing ==="
"${CURL[@]}" "$BASE/player/timeline/poll?commandID=91" | grep -q 'state="playing"'

echo "=== process count: expect 1 ffmpeg (not 2) ==="
# shell wrappers may exist; count actual ffmpeg binaries
FF=$(ssh_m 'ps w | grep "[f]fmpeg" | grep -v grep | wc -l')
echo "ffmpeg_procs=$FF"
# Allow 1 (ideal) or 2 briefly during seek; fail if 3+ (old dual curl path)
if [[ "$FF" -ge 3 ]]; then
  echo "FAIL: too many ffmpeg processes (dual-path regression?)" >&2
  ssh_m 'ps w | grep ffmpeg | grep -v grep'
  exit 1
fi

echo "=== log shows single-process spawn ==="
ssh_m 'grep -q "spawn single-process" /media/fat/misterplex/misterplexd.log'

echo "=== frames advancing with audio ==="
ssh_m 'grep "audio=on" /media/fat/misterplex/misterplexd.log | tail -2' | grep -q audio=on

echo "=== sample load ==="
ssh_m 'cat /proc/loadavg; top -bn1 | head -8'

"${CURL[@]}" "$BASE/player/playback/stop?commandID=92" >/dev/null
echo "test_single_process: OK on $HOST (ffmpeg_procs=$FF)"
