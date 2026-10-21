#!/usr/bin/env bash
# Parse a live 720p24 Star Trek soak log. FAIL costume/prefetch/second-HTTP.
# Usage: test_p720_startrek_soak_parse.sh FILE
set -u
FILE="${1:-}"
if [ -z "$FILE" ] || [ ! -f "$FILE" ]; then
  echo "FAIL: need soak log file" >&2
  exit 1
fi
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

if grep -q 'farpoint_1280x720' "$FILE"; then
  fail "spawn must not use farpoint_1280x720 identity"
fi
grep -q 'prefetched=0' "$FILE" || fail "missing prefetched=0"
grep -q 'url_local=0' "$FILE" || fail "missing url_local=0"
grep -q 'inproc_audio=remux_pcm' "$FILE" || fail "missing inproc_audio=remux_pcm"
grep -q 'videoResolution=1280x720' "$FILE" || fail "missing videoResolution=1280x720"
grep -q 'decode=1280x720' "$FILE" || fail "missing decode=1280x720"
if grep -v 'no spawnAudioOnly' "$FILE" | grep -qE 'spawnAudioOnly|audio-only fork failed'; then
  fail "720p must not spawn second HTTP audio"
fi
if grep -q 'wait_ok=1 spawn=pipe-from-tmpfs' "$FILE"; then
  fail "must not prefetch-to-tmpfs"
fi

# Last frames= line with pfps/audio/wall/drift/hw.
line=$(grep -E 'media: frames=.*pfps=' "$FILE" | tail -1)
[ -n "$line" ] || fail "missing media: frames= soak line"
if [ -n "$line" ]; then
  pfps=$(echo "$line" | sed -n 's/.* pfps=\([0-9.]*\).*/\1/p')
  hw=$(echo "$line" | sed -n 's/.* hw_fps=\([0-9.]*\).*/\1/p')
  audio=$(echo "$line" | sed -n 's/.* audio_s=\([0-9.]*\).*/\1/p')
  wall=$(echo "$line" | sed -n 's/.* wall_s=\([0-9.]*\).*/\1/p')
  drift=$(echo "$line" | sed -n 's/.* av_drift_ms=\([-0-9]*\).*/\1/p')
  drops=$(echo "$line" | sed -n 's/.* drops=\([0-9]*\).*/\1/p')
  python3 - "$pfps" "$hw" "$audio" "$wall" "$drift" "$drops" <<'PY' || fail "numeric gates"
import sys
pfps, hw, audio, wall = map(float, sys.argv[1:5])
drift, drops = int(sys.argv[5]), int(sys.argv[6])
ok = True
if pfps < 23.5:
    print("FAIL pfps", pfps, file=sys.stderr); ok = False
if hw < 23.5:
    print("FAIL hw_fps", hw, file=sys.stderr); ok = False
if wall <= 0 or audio / wall < 0.95:
    print("FAIL audio/wall", audio, wall, file=sys.stderr); ok = False
if abs(drift) > 80:
    print("FAIL |av_drift_ms|", drift, file=sys.stderr); ok = False
if drops != 0:
    print("FAIL drops", drops, file=sys.stderr); ok = False
sys.exit(0 if ok else 1)
PY
fi

if [ "$fails" -ne 0 ]; then
  echo "test_p720_startrek_soak_parse: $fails failures ($FILE)"
  exit 1
fi
echo "test_p720_startrek_soak_parse: OK $FILE"
exit 0
