#!/usr/bin/env bash
# Hardware: multi-chunk F2 PCM append (no per-download flush) + live play f2≈bytes.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PMS_URL="${PLEX_BASE:-${PMS_URL:-}}"
PLAYER_ID="${MISTERPLEX_ID:-misterplex-dev}"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$USER@$HOST" "$@"
}

echo "=== core is Plex ==="
ssh_m 'grep -q Plex /tmp/CORENAME'

echo "=== ensure push_frame + 16 KiB PCM chunk ==="
if ! ssh_m 'test -x /media/fat/misterplex/bin/push_frame'; then
  echo "missing push_frame — deploy arm build first" >&2
  exit 1
fi
# 4096 stereo frames = 16384 bytes (two FIFO depths @ 2048)
python3 - <<'PY'
from pathlib import Path
import math, struct
Path("build").mkdir(exist_ok=True)
rate, n, amp = 48000, 4096, 10000
buf = bytearray()
for i in range(n):
    s = int(amp * math.sin(2 * math.pi * 440.0 * i / rate))
    buf += struct.pack("<hh", s, s)
Path("build/plex_f2_chunk16k.s16le").write_bytes(buf)
print(len(buf))
PY
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$ROOT/build/plex_f2_chunk16k.s16le" \
  "$USER@$HOST:/media/fat/plex_f2_chunk16k.s16le"

echo "=== 8× F2 append (SPI, no host flush) ==="
OUT=$(ssh_m '
ok=0
for i in 1 2 3 4 5 6 7 8; do
  /media/fat/misterplex/bin/push_frame --index 2 /media/fat/plex_f2_chunk16k.s16le >/media/fat/f2push.out || exit 1
  ok=$((ok+1))
done
echo chunks_ok=$ok
cat /media/fat/f2push.out
')
echo "$OUT"
echo "$OUT" | grep -q 'chunks_ok=8'
echo "$OUT" | grep -q 'OK'

echo "=== live testsrc: require F2 continuous (f2≈audio bytes) ==="
# Truncate log so we can match this session only
ssh_m 'cp /media/fat/misterplex/misterplexd.log /media/fat/misterplex/mpx.log.bak; : > /media/fat/misterplex/misterplexd.log; killall -HUP misterplexd 2>/dev/null || true'
# Restart daemon so log is clean and companion is up
PMS_ARG=""
if [[ -n "$PMS_URL" ]]; then
  PMS_ARG="--pms '$PMS_URL'"
fi
ssh_m "
killall misterplexd 2>/dev/null || true
sleep 1
fuser -k 3005/tcp 2>/dev/null || true
sleep 1
cd /media/fat/misterplex
nohup ./bin/misterplexd --name MiSTerPlex --id '$PLAYER_ID' --port 3005 \
  --conf /media/fat/misterplex/misterplex.conf \
  $PMS_ARG >> /media/fat/misterplex/misterplexd.log 2>&1 &
sleep 2
grep -q 'companion on' /media/fat/misterplex/misterplexd.log
"

curl -sS -m 5 "http://${HOST}:3005/player/playback/playMedia?key=testsrc&offset=0&protocol=http&address=127.0.0.1&port=32400&machineIdentifier=local&token=x" \
  -H 'X-Plex-Client-Identifier: f2append' >/dev/null
# Play ~6s then stop
sleep 6
curl -sS -m 3 "http://${HOST}:3005/player/playback/stop?commandID=9" \
  -H 'X-Plex-Client-Identifier: f2append' >/dev/null || true
sleep 3

LOG=$(ssh_m 'cat /media/fat/misterplex/misterplexd.log')
echo "$LOG" | grep -q 'F2 audio_fifo streaming enabled'
# Parse last audio pump line: bytes=N f2=M
LINE=$(echo "$LOG" | grep 'audio pump end' | tail -1)
echo "pump: $LINE"
echo "$LINE" | grep -qE 'bytes=([0-9]+) f2=\1'
BYTES=$(echo "$LINE" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
# At least ~0.5s of PCM @ 48k stereo (192000 B/s * 0.5)
python3 -c "import sys; b=int('$BYTES' or 0); sys.exit(0 if b >= 96000 else 1)"

echo "test_f2_append: OK on $HOST (f2 matched audio bytes=$BYTES)"
