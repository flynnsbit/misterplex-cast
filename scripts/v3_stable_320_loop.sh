#!/usr/bin/env bash
# Isolated v0.3.0 lab-stable @320 loop: deploy pair, chevron + B6 gates.
# Does not run product H5 / Quartus. Does not delete glass-floor pins.
#
# Env:
#   MISTER_HOST / MISTER_PASS
#   V3_STABLE_PROMOTE=1  — after PASS, leave v0.3 as live default (still keep floor bak pins)
#   V3_STABLE_FORCE=1    — allow deploy even if live RBF looks like experiment thrash
#   HDMI_DEV (default /dev/video0)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
POLICY="$ROOT/tests/fixtures/v030_lab_stable/pair.json"
ART="$ROOT/release_artifacts/v0.3.0-lab-stable"
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
DEV="${HDMI_DEV:-/dev/video0}"
PROMOTE="${V3_STABLE_PROMOTE:-0}"
FORCE="${V3_STABLE_FORCE:-0}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${V3_OUT:-/tmp/misterplex-v030-$TS}"
mkdir -p "$OUT"

SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o PreferredAuthentications=password
  -o PubkeyAuthentication=no -o ConnectTimeout=12 "root@$HOST")
SCP=(sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -o PreferredAuthentications=password
  -o PubkeyAuthentication=no)

log() { echo "v3-stable-320: $*"; }
fail() { echo "v3-stable-320: FAIL - $*" | tee -a "$OUT/VERDICT.txt" >&2; exit 1; }

[[ -f "$POLICY" && -f "$ART/Plex.rbf" && -f "$ART/misterplexd" ]] || fail "missing policy/artifacts"

RBF_MD5="$(python3 -c "import json; print(json.load(open('$POLICY'))['pair']['rbf_md5'])")"
DAE_MD5="$(python3 -c "import json; print(json.load(open('$POLICY'))['pair']['daemon_md5'])")"
act_r="$(md5sum "$ART/Plex.rbf" | awk '{print $1}')"
act_d="$(md5sum "$ART/misterplexd" | awk '{print $1}')"
[[ "$act_r" == "$RBF_MD5" ]] || fail "artifact RBF $act_r != $RBF_MD5"
[[ "$act_d" == "$DAE_MD5" ]] || fail "artifact daemon $act_d != $DAE_MD5"
log "artifacts OK rbf=$RBF_MD5 daemon=$DAE_MD5"

# Refuse if exclusive product fit is LIVE (parent loop thrash)
if [[ -f /tmp/misterplex-loop-status.txt ]] && grep -q 'exclusive=LIVE' /tmp/misterplex-loop-status.txt 2>/dev/null; then
  if [[ "$FORCE" != "1" ]]; then
    fail "product exclusive=LIVE in /tmp/misterplex-loop-status.txt — wait or V3_STABLE_FORCE=1"
  fi
  log "WARN exclusive LIVE but FORCE=1"
fi

log "deploying v0.3 lab pair (PRESENT=both, 320x240) — ship recipe"
"${SSH[@]}" 'killall misterplexd 2>/dev/null || true; sleep 0.4; killall -9 misterplexd 2>/dev/null || true; sleep 0.3'
"${SCP[@]}" "$ART/Plex.rbf" "root@$HOST:/media/fat/_Utility/Plex.rbf"
# Keep named pin (do not overwrite GOODBASELINE dfebf2bf pin)
"${SCP[@]}" "$ART/Plex.rbf" "root@$HOST:/media/fat/_Utility/Plex.v030_lab_stable.rbf"
"${SSH[@]}" 'mv -f /media/fat/misterplex/bin/misterplexd /media/fat/misterplex/bin/misterplexd.prev-before-v030loop 2>/dev/null || true'
"${SCP[@]}" "$ART/misterplexd" "root@$HOST:/media/fat/misterplex/bin/misterplexd"
"${SSH[@]}" "chmod +x /media/fat/misterplex/bin/misterplexd
CFG=/media/fat/misterplex/misterplex.conf
cp -a \"\$CFG\" /tmp/misterplex.conf.bak_v030loop
for kv in PRESENT=both DECODE=320x240 STREAM=0 AUDIO=on OSD_CONTROL=1 IDLE_SCREEN=logo DISPLAY_RES=240p CONTENT_RES=240p STREAM_SKIP_RGB=off; do
  k=\${kv%%=*}; v=\${kv#*=}
  if grep -q \"^\${k}=\" \"\$CFG\"; then sed -i \"s|^\${k}=.*|\${k}=\${v}|\" \"\$CFG\"
  else echo \"\${k}=\${v}\" >> \"\$CFG\"; fi
done
grep -q '^PRESENT=both' \"\$CFG\" || { echo PRESENT must be both; exit 2; }
md5sum /media/fat/_Utility/Plex.rbf /media/fat/misterplex/bin/misterplexd
grep -E '^(PRESENT|DECODE|STREAM|OSD|IDLE)' \"\$CFG\"
echo load_core /media/fat/menu.rbf > /dev/MiSTer_cmd
"
sleep 4
"${SSH[@]}" 'echo load_core /media/fat/_Utility/Plex.rbf > /dev/MiSTer_cmd'
sleep 5
"${SSH[@]}" ': > /tmp/misterplexd_v030loop.log
cd /media/fat/misterplex
nohup /media/fat/misterplex/bin/misterplexd --name MiSTerPlex --id misterplex-dev --port 3005 \
  --conf /media/fat/misterplex/misterplex.conf > /tmp/misterplexd_v030loop.log 2>&1 &
sleep 3
head -35 /tmp/misterplexd_v030loop.log
md5sum /media/fat/_Utility/Plex.rbf /media/fat/misterplex/bin/misterplexd
' | tee "$OUT/deploy.txt"

LIVE_R="$(grep -E 'Plex.rbf$' "$OUT/deploy.txt" | tail -1 | awk '{print $1}')"
LIVE_D="$(grep -E 'misterplexd$' "$OUT/deploy.txt" | tail -1 | awk '{print $1}')"
# fallback parse from md5sum lines
if [[ -z "${LIVE_R:-}" ]]; then
  LIVE_R="$("${SSH[@]}" 'md5sum /media/fat/_Utility/Plex.rbf' | awk '{print $1}')"
  LIVE_D="$("${SSH[@]}" 'md5sum /media/fat/misterplex/bin/misterplexd' | awk '{print $1}')"
fi
[[ "$LIVE_R" == "$RBF_MD5" ]] || fail "live RBF $LIVE_R != $RBF_MD5"
[[ "$LIVE_D" == "$DAE_MD5" ]] || fail "live daemon $LIVE_D != $DAE_MD5"
log "live pair matches policy"

for i in $(seq 1 25); do
  if "${SSH[@]}" 'wget -qO- "http://127.0.0.1:3005/player/timeline/poll?wait=0" 2>/dev/null | head -c 40' | grep -q MediaContainer; then
    log "companion up try=$i"
    break
  fi
  sleep 1
  [[ $i -eq 25 ]] && fail "companion not up"
done

"${SSH[@]}" 'wget -qO- "http://127.0.0.1:3005/player/playback/stop" >/dev/null 2>&1 || true; sleep 1' || true

[[ -e "$DEV" ]] || fail "no grabber $DEV"
ffmpeg -hide_banner -loglevel error -y -f v4l2 -input_format mjpeg -video_size 1280x720 -i "$DEV" \
  -frames:v 50 -q:v 2 "$OUT/idle_%03d.jpg" 2>"$OUT/idle.err" || fail "idle capture failed"

python3 - "$OUT" <<'PY' | tee "$OUT/idle_score.txt"
import sys
from pathlib import Path
import numpy as np
from PIL import Image
out = Path(sys.argv[1])
frames = sorted(out.glob("idle_*.jpg"))
if len(frames) < 15:
    print("FAIL few frames"); sys.exit(2)

def orange_px(a):
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    return int(((r > 140) & (g > 40) & (g < 200) & (b < 120) & (r > g) & (r > b * 1.2)).sum())

best_op, best = -1, None
for p in frames[12:]:
    a = np.asarray(Image.open(p).convert("RGB"))
    op = orange_px(a)
    if op > best_op:
        best_op, best = op, (p, a)
p, a = best
Image.open(p).save(out / "idle_best.png")
luma = float(a.mean())
print(f"ORANGE_PX={best_op} luma={luma:.1f} mean={a.mean(axis=(0,1))}")
if best_op >= 8000 and 10 <= luma <= 90:
    print("PASS_CHEVRON"); sys.exit(0)
print("FAIL_CHEVRON"); sys.exit(3)
PY
[[ ${PIPESTATUS[0]} -eq 0 ]] || fail "chevron gate"

KEY="$(python3 -c "import urllib.parse; print(urllib.parse.quote('/media/fat/Games/Plex/MiSTerPlex B6 RealGlass 720x480 24fps 300s (2026).mp4', safe=''))")"
"${SSH[@]}" "test -f /media/fat/Games/Plex/MiSTerPlex\\ B6\\ RealGlass\\ 720x480\\ 24fps\\ 300s\\ \\(2026\\).mp4"
"${SSH[@]}" "wget -qO- 'http://127.0.0.1:3005/player/playback/playMedia?key=${KEY}&offset=5000' >/dev/null" || fail "playMedia"
sleep 6
ffmpeg -hide_banner -loglevel error -y -f v4l2 -input_format mjpeg -video_size 1280x720 -i "$DEV" \
  -frames:v 60 -q:v 3 "$OUT/play_%03d.jpg" 2>"$OUT/play.err" || fail "play capture"
"${SSH[@]}" 'tail -25 /tmp/misterplexd_v030loop.log' | tee "$OUT/play_log.txt" || true

python3 - "$OUT" <<'PY' | tee "$OUT/play_score.txt"
import sys
from pathlib import Path
import numpy as np
from PIL import Image
out = Path(sys.argv[1])
frames = sorted(out.glob("play_*.jpg"))
if len(frames) < 25:
    print("FAIL few play frames"); sys.exit(2)
body = frames[15:]
means = []
arrs = []
for p in body:
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
    means.append(tuple(np.round(a.mean(axis=(0, 1)), 1)))
    arrs.append(a)
um = len(set(means))
mot = float(np.abs(arrs[-1] - arrs[0]).mean())
print(f"unique={um} motion={mot:.3f} last_mean={arrs[-1].mean(axis=(0,1))}")
Image.fromarray(arrs[-1].astype(np.uint8)).save(out / "play_last.png")
if um >= 5 and mot >= 2.0 and float(arrs[-1].mean()) > 25:
    print("PASS_PLAY"); sys.exit(0)
print("FAIL_PLAY"); sys.exit(3)
PY
[[ ${PIPESTATUS[0]} -eq 0 ]] || fail "play gate"

{
  echo "VERDICT=PASS_V3_STABLE_320"
  echo "when=$TS"
  echo "rbf=$RBF_MD5"
  echo "daemon=$DAE_MD5"
  echo "present=fb0"
  echo "promote=$PROMOTE"
  echo "out=$OUT"
  echo "github_release=none (tag only; not repeating GH ship)"
} | tee "$OUT/VERDICT.txt"

if [[ -d "$ROOT/Memory/lab" ]]; then
  MEM="$ROOT/Memory/lab/hdmi-proof/v030_$TS"
  mkdir -p "$MEM"
  cp -a "$OUT/VERDICT.txt" "$OUT/idle_best.png" "$OUT/play_last.png" "$OUT/deploy.txt" \
    "$OUT/idle_score.txt" "$OUT/play_score.txt" "$MEM/" 2>/dev/null || true
  cp -f "$OUT/VERDICT.txt" "$ROOT/Memory/lab/status/v3-stable-320-last.txt" 2>/dev/null || true
  log "evidence $MEM"
fi

if [[ "$PROMOTE" != "1" ]]; then
  log "PASS — live is v0.3 lab pair for this session; glass floor pins still on SD"
  log "To make v0.3 the sticky default: V3_STABLE_PROMOTE=1 (already live after PASS)"
else
  log "PROMOTE=1 — leaving v0.3 pair as live Plex.rbf + misterplexd"
fi

log "PASS_V3_STABLE_320"
exit 0
