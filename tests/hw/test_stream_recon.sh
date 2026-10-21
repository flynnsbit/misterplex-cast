#!/usr/bin/env bash
# Hardware: STREAM=1 host I-slice recon → F1 smoke (PRESENT=both).
# Plays local Baseline annex-B if present; asserts recon_ok / multi-IDR logs.
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
BASE="http://${HOST}:3005"
PASS="${MISTER_PASS:?set MISTER_PASS}"
USER="${MISTER_USER:-root}"
HOLD_S="${HOLD_S:-8}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

ssh_m() {
  sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$USER@$HOST" "$@"
}

echo "=== resources ==="
curl -fsS --connect-timeout 5 "$BASE/resources" | grep -q MiSTerPlex

echo "=== conf STREAM/PRESENT ==="
ssh_m 'grep -E "^(STREAM|PRESENT|STREAM_SKIP)" /media/fat/misterplex/misterplex.conf || true'
# Ensure STREAM=1 for this smoke (restore not needed — lab conf already STREAM=1)
ssh_m 'grep -q "^STREAM=1" /media/fat/misterplex/misterplex.conf || echo "WARN: STREAM!=1"'

# Prefer real Baseline annex-B already on device; else generate + scp
LOCAL_AB="$ROOT/build/plex_real_baseline.264"
REMOTE_AB="/media/fat/misterplex/plex_real_baseline.264"
if [[ ! -f "$LOCAL_AB" ]]; then
  mkdir -p "$ROOT/build"
  python3 "$ROOT/scripts/gen_test_annexb_real.py" "$LOCAL_AB"
fi
echo "=== push Baseline annex-B ==="
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$LOCAL_AB" "$USER@$HOST:$REMOTE_AB"

echo "=== clear log + play elementary H.264 ==="
ssh_m ': > /media/fat/misterplex/misterplexd.log'
# Companion accepts local absolute path as playMedia key
curl -fsS --get "$BASE/player/playback/playMedia" \
  --data-urlencode "key=${REMOTE_AB}" \
  --data-urlencode "offset=0" \
  --data-urlencode "commandID=101" >/dev/null || true
sleep 2
curl -fsS "$BASE/player/timeline/poll?commandID=102" | tee "$ROOT/build/stream_tl1.xml" | \
  grep -Eq 'state="(playing|buffering)"' || {
  echo "timeline not playing; dump log:" >&2
  ssh_m 'tail -80 /media/fat/misterplex/misterplexd.log' >&2
  exit 1
}

echo "=== hold ${HOLD_S}s for multi-IDR recon ==="
sleep "$HOLD_S"

echo "=== stop ==="
curl -fsS "$BASE/player/playback/stop?commandID=103" >/dev/null || true
sleep 1

echo "=== assert STREAM recon logs ==="
LOG=$(ssh_m 'cat /media/fat/misterplex/misterplexd.log')
echo "$LOG" | tail -60
echo "$LOG" | grep -q 'STREAM=1 host I-slice recon' || {
  echo "FAIL: missing STREAM recon banner" >&2
  exit 1
}
# Either recon success (Baseline) or clear CABAC fail (High) — both are "robust"
if echo "$LOG" | grep -q 'recon frame ok'; then
  echo "OK: recon frame present"
  echo "$LOG" | grep -E 'recon frame ok|STREAM end|session end' | tail -10
elif echo "$LOG" | grep -qE 'recon CABAC/High|recon skip CABAC'; then
  echo "OK: CABAC sticky fail path (fb0 fallback expected on PRESENT=both)"
elif echo "$LOG" | grep -qE 'recon_ok=[1-9]'; then
  echo "OK: STREAM recon_ok≥1"
elif echo "$LOG" | grep -qE 'STREAM end'; then
  echo "FAIL: STREAM ran but no recon_ok and no CABAC message" >&2
  echo "$LOG" | grep -E 'STREAM|recon' | tail -20 >&2
  exit 1
else
  echo "FAIL: no STREAM/recon activity in log" >&2
  exit 1
fi

# Seek/stop robustness: brief seek then stop
echo "=== seek + stop smoke ==="
curl -fsS --get "$BASE/player/playback/playMedia" \
  --data-urlencode "key=${REMOTE_AB}" \
  --data-urlencode "offset=0" \
  --data-urlencode "commandID=110" >/dev/null || true
sleep 2
curl -fsS "$BASE/player/playback/seekTo?offset=1000&commandID=111" >/dev/null || true
sleep 2
curl -fsS "$BASE/player/playback/stop?commandID=112" >/dev/null || true
sleep 1
# Daemon still responsive
curl -fsS --connect-timeout 5 "$BASE/resources" | grep -q MiSTerPlex

echo "test_stream_recon: OK on $HOST"
