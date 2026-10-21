#!/usr/bin/env bash
# Live HDMI glass floor regression.
#
# Default: pin the v0.2.0 glass pair, conf 320x240, menu bounce, idle chevron + B6 play.
# GLASS_CANDIDATE=1: do not pin md5s; only run behavioral gates on whatever is live
#   (use this to prove a new misterplexd+Plex.rbf pair matches the floor).
#
# Env:
#   MISTER_HOST (set MISTER_HOST)
#   MISTER_PASS (required)
#   HDMI_DEV    (default /dev/video0)
#   HDMI_SIZE   (default 1280x720)
#   GLASS_CANDIDATE=1  skip pin; test live pair only
#   GLASS_SKIP_PIN=1   same as candidate for pin skip
#   GLASS_SKIP_SEEK=1  skip optional seek gate
#   GLASS_OUT          evidence dir (default /tmp/misterplex-baseline-<ts>)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
POLICY="$ROOT/tests/fixtures/glass_baseline/pair.json"
ART="$ROOT/release_artifacts/v0.2.0-glass-baseline"
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
DEV="${HDMI_DEV:-/dev/video0}"
SIZE="${HDMI_SIZE:-1280x720}"
CANDIDATE="${GLASS_CANDIDATE:-0}"
SKIP_PIN="${GLASS_SKIP_PIN:-0}"
SKIP_SEEK="${GLASS_SKIP_SEEK:-0}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${GLASS_OUT:-/tmp/misterplex-baseline-$TS}"
mkdir -p "$OUT"

SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o PreferredAuthentications=password
  -o PubkeyAuthentication=no -o ConnectTimeout=12 "root@$HOST")
SCP=(sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -o PreferredAuthentications=password
  -o PubkeyAuthentication=no)

log() { echo "glass-baseline: $*"; }
fail() { echo "glass-baseline: FAIL - $*" | tee -a "$OUT/VERDICT.txt" >&2; exit 1; }

if [[ ! -f "$POLICY" ]]; then
  fail "missing policy $POLICY"
fi

RBF_MD5="$(python3 -c "import json; print(json.load(open('$POLICY'))['pair']['rbf_md5'])")"
DAE_MD5="$(python3 -c "import json; print(json.load(open('$POLICY'))['pair']['daemon_md5'])")"
B6_PATH="$(python3 -c "import json; print(json.load(open('$POLICY'))['content']['local_mp4'])")"
MIN_ORANGE="$(python3 -c "import json; print(json.load(open('$POLICY'))['gates']['idle']['min_orange_px_1280x720'])")"
MIN_PFPS="$(python3 -c "import json; print(json.load(open('$POLICY'))['present_path']['min_pfps'])")"
MIN_UNIQUE="$(python3 -c "import json; print(json.load(open('$POLICY'))['gates']['play']['min_unique_coarse_means'])")"
MIN_MOTION="$(python3 -c "import json; print(json.load(open('$POLICY'))['gates']['play']['min_motion_mean_delta'])")"

log "OUT=$OUT host=$HOST candidate=$CANDIDATE"

remote() { "${SSH[@]}" "$@"; }

# --- optional pin ---
if [[ "$CANDIDATE" != "1" && "$SKIP_PIN" != "1" ]]; then
  [[ -f "$ART/Plex.rbf" && -f "$ART/misterplexd" ]] || fail "artifacts missing under $ART"
  act_r="$(md5sum "$ART/Plex.rbf" | awk '{print $1}')"
  act_d="$(md5sum "$ART/misterplexd" | awk '{print $1}')"
  [[ "$act_r" == "$RBF_MD5" ]] || fail "local artifact RBF md5 $act_r != $RBF_MD5"
  [[ "$act_d" == "$DAE_MD5" ]] || fail "local artifact daemon md5 $act_d != $DAE_MD5"
  log "pinning floor pair to device"
  "${SCP[@]}" "$ART/Plex.rbf" "root@$HOST:/media/fat/_Utility/Plex.rbf"
  "${SCP[@]}" "$ART/Plex.rbf" "root@$HOST:/media/fat/_Utility/Plex.GOODBASELINE.dfebf2bf.rbf"
  "${SCP[@]}" "$ART/misterplexd" "root@$HOST:/media/fat/misterplex/bin/misterplexd"
  "${SCP[@]}" "$ART/misterplexd" "root@$HOST:/media/fat/misterplex_v2/bin/misterplexd.GOODBASELINE.7cd10b4d"
  remote "chmod +x /media/fat/misterplex/bin/misterplexd
CFG=/media/fat/misterplex/misterplex.conf
grep -q '^PRESENT=' \"\$CFG\" && sed -i 's/^PRESENT=.*/PRESENT=fpga/' \"\$CFG\" || echo PRESENT=fpga >> \"\$CFG\"
grep -q '^DECODE=' \"\$CFG\" && sed -i 's/^DECODE=.*/DECODE=320x240/' \"\$CFG\" || echo DECODE=320x240 >> \"\$CFG\"
grep -q '^STREAM=' \"\$CFG\" && sed -i 's/^STREAM=.*/STREAM=0/' \"\$CFG\" || echo STREAM=0 >> \"\$CFG\"
grep -q '^IDLE_SCREEN=' \"\$CFG\" && sed -i 's/^IDLE_SCREEN=.*/IDLE_SCREEN=logo/' \"\$CFG\" || echo IDLE_SCREEN=logo >> \"\$CFG\"
grep -q '^DISPLAY_RES=' \"\$CFG\" && sed -i 's/^DISPLAY_RES=.*/DISPLAY_RES=240p/' \"\$CFG\" || true
grep -q '^CONTENT_RES=' \"\$CFG\" && sed -i 's/^CONTENT_RES=.*/CONTENT_RES=240p/' \"\$CFG\" || true
killall misterplexd 2>/dev/null || true
sleep 0.4
killall -9 misterplexd 2>/dev/null || true
echo load_core /media/fat/menu.rbf > /dev/MiSTer_cmd
sleep 4
echo load_core /media/fat/_Utility/Plex.rbf > /dev/MiSTer_cmd
sleep 5
: > /tmp/misterplexd_glass_baseline.log
cd /media/fat/misterplex
nohup /media/fat/misterplex/bin/misterplexd --name MiSTerPlex --id misterplex-dev --port 3005 \
  --conf /media/fat/misterplex/misterplex.conf > /tmp/misterplexd_glass_baseline.log 2>&1 &
sleep 3
md5sum /media/fat/_Utility/Plex.rbf /media/fat/misterplex/bin/misterplexd
head -25 /tmp/misterplexd_glass_baseline.log
" | tee "$OUT/pin.log"
else
  log "candidate/skip-pin mode — not restoring floor md5s"
fi

# --- live md5 report ---
LIVE="$(remote 'md5sum /media/fat/_Utility/Plex.rbf /media/fat/misterplex/bin/misterplexd 2>/dev/null; grep -E "^(PRESENT|DECODE|STREAM|IDLE)" /media/fat/misterplex/misterplex.conf 2>/dev/null || true')"
echo "$LIVE" | tee "$OUT/live_stack.txt"
LIVE_RBF="$(echo "$LIVE" | awk '/Plex.rbf/{print $1; exit}')"
LIVE_DAE="$(echo "$LIVE" | awk '/misterplexd$/{print $1; exit}')"
if [[ "$CANDIDATE" != "1" && "$SKIP_PIN" != "1" ]]; then
  [[ "$LIVE_RBF" == "$RBF_MD5" ]] || fail "live RBF $LIVE_RBF != floor $RBF_MD5"
  [[ "$LIVE_DAE" == "$DAE_MD5" ]] || fail "live daemon $LIVE_DAE != floor $DAE_MD5"
  log "live pair matches floor md5s"
else
  log "live RBF=$LIVE_RBF daemon=$LIVE_DAE (candidate under gate)"
fi

# companion up
for i in $(seq 1 15); do
  if remote 'wget -qO- "http://127.0.0.1:3005/player/timeline/poll?wait=0" 2>/dev/null | head -c 40' | grep -q MediaContainer; then
    break
  fi
  sleep 1
  [[ $i -eq 15 ]] && fail "companion :3005 not up"
done

# stop any play for idle
remote 'wget -qO- "http://127.0.0.1:3005/player/playback/stop" >/dev/null 2>&1 || true; sleep 1' || true

# --- idle capture ---
[[ -e "$DEV" ]] || fail "no grabber $DEV"
if ! command -v ffmpeg >/dev/null; then fail "ffmpeg missing on host"; fi
ffmpeg -hide_banner -loglevel error -y -f v4l2 -input_format mjpeg -video_size "$SIZE" -i "$DEV" \
  -frames:v 60 -q:v 2 "$OUT/idle_%03d.jpg" 2>"$OUT/idle_cap.err" || fail "idle capture failed"
python3 - "$OUT" "$MIN_ORANGE" <<'PY' | tee "$OUT/idle_score.txt"
import sys, json
from pathlib import Path
import numpy as np
from PIL import Image
out = Path(sys.argv[1])
min_orange = int(sys.argv[2])
frames = sorted(out.glob("idle_*.jpg"))
if len(frames) < 20:
    print("FAIL idle few frames", len(frames))
    sys.exit(2)

def orange_px(a):
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    return int(((r > 140) & (g > 40) & (g < 200) & (b < 120) & (r > g) & (r > b * 1.2)).sum())

best = None
best_op = -1
for p in frames[15:]:
    a = np.asarray(Image.open(p).convert("RGB"))
    op = orange_px(a)
    if op > best_op:
        best_op = op
        best = (p, a)
assert best is not None
p, a = best
mean = a.mean(axis=(0, 1))
luma = float(a.astype(np.float32).mean())
std = float(a.std())
Image.open(p).save(out / "idle_best.png")
print(f"idle_best={p.name} ORANGE_PX={best_op} mean={mean.tolist()} luma={luma:.1f} std={std:.1f}")
# colorbar-ish: high mid chroma columns low texture
h, w = a.shape[:2]
row = a[h // 2]
col_stds = [float(row[int(i * w / 8) : int((i + 1) * w / 8)].std()) for i in range(8)]
avg_col_std = float(np.mean(col_stds))
if best_op < min_orange:
    print(f"FAIL idle ORANGE_PX {best_op} < {min_orange}")
    sys.exit(3)
if not (10 <= luma <= 90):
    print(f"FAIL idle luma {luma:.1f} not in 10..90 (chevron-on-dark class)")
    sys.exit(4)
if avg_col_std < 8 and std > 40:
    print("FAIL idle looks colorbar-like")
    sys.exit(5)
print("PASS_IDLE")
sys.exit(0)
PY
[[ ${PIPESTATUS[0]} -eq 0 ]] || fail "idle chevron gate"

# --- play B6 ---
KEY="$(python3 -c "import urllib.parse; print(urllib.parse.quote('''$B6_PATH''', safe=''))")"
remote "test -f $(printf %q "$B6_PATH")" || fail "missing media on device: $B6_PATH"
remote "wget -qO- 'http://127.0.0.1:3005/player/playback/playMedia?key=${KEY}&offset=2000' >/dev/null" \
  || fail "playMedia failed"
sleep 6
# protocol pfps from log if available
remote 'tail -40 /tmp/misterplexd_glass_baseline.log 2>/dev/null; tail -20 /media/fat/misterplex/misterplexd.log 2>/dev/null' \
  | tee "$OUT/play_log_snip.txt" || true

ffmpeg -hide_banner -loglevel error -y -f v4l2 -input_format mjpeg -video_size "$SIZE" -framerate 30 -i "$DEV" \
  -frames:v 90 -q:v 3 "$OUT/play_%03d.jpg" 2>"$OUT/play_cap.err" || fail "play capture failed"

python3 - "$OUT" "$MIN_UNIQUE" "$MIN_MOTION" <<'PY' | tee "$OUT/play_score.txt"
import sys
from pathlib import Path
from collections import Counter
import numpy as np
from PIL import Image
out = Path(sys.argv[1])
min_unique = int(sys.argv[2])
min_motion = float(sys.argv[3])
frames = sorted(out.glob("play_*.jpg"))
if len(frames) < 40:
    print("FAIL play few frames", len(frames))
    sys.exit(2)
body = frames[25:]

def classify(a):
    mean = a.mean(axis=(0, 1))
    std = float(a.std())
    chroma = float(np.std(mean))
    # colorbar heuristic mid band
    h = a.shape[0]
    mid = a[2 * h // 5 : 3 * h // 5]
    row = a[h // 2]
    w = a.shape[1]
    col_stds = [float(row[int(i * w / 8) : int((i + 1) * w / 8)].std()) for i in range(8)]
    avg_col = float(np.mean(col_stds))
    if mean.mean() < 20 and std < 8:
        return "BLACK"
    if avg_col < 12 and std > 40 and chroma > 15:
        return "COLORBARS"
    # rainbow noise: high chroma of channel means + high std, low orange structure
    if chroma > 40 and std > 70 and mean[1] < 40:
        return "RAINBOW"
    return "CONTENT"

labels = []
means = []
arrs = []
for p in body:
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float32)
    labels.append(classify(a))
    means.append(tuple(np.round(a.mean(axis=(0, 1)), 1)))
    arrs.append(a)
print("label_counts", dict(Counter(labels)))
content_frac = sum(1 for l in labels if l == "CONTENT") / len(labels)
unique = len(set(means))
mots = [float(np.abs(arrs[i] - arrs[i - 1]).mean()) for i in range(1, len(arrs))]
motion = float(np.mean(mots)) if mots else 0.0
print(f"content_frac={content_frac:.2f} unique={unique} motion_mean={motion:.3f}")
Image.fromarray(arrs[-1].astype(np.uint8)).save(out / "play_last.png")
Image.fromarray(arrs[len(arrs) // 2].astype(np.uint8)).save(out / "play_mid.png")
if content_frac < 0.5:
    print("FAIL play content_frac low (bars/rainbow/black)")
    sys.exit(3)
if unique < min_unique:
    print(f"FAIL unique {unique} < {min_unique}")
    sys.exit(4)
if motion < min_motion:
    print(f"FAIL motion {motion:.3f} < {min_motion}")
    sys.exit(5)
print("PASS_PLAY")
sys.exit(0)
PY
[[ ${PIPESTATUS[0]} -eq 0 ]] || fail "play content gate"

# protocol pfps check (best-effort from snip)
if grep -EEo 'pfps=[0-9.]+' "$OUT/play_log_snip.txt" >/dev/null 2>&1; then
  PFPS="$(grep -EEo 'pfps=[0-9.]+' "$OUT/play_log_snip.txt" | tail -1 | cut -d= -f2)"
  python3 -c "import sys; p=float('$PFPS'); m=float('$MIN_PFPS');
sys.exit(0 if p>=m else 1)" || fail "pfps $PFPS < min $MIN_PFPS"
  log "pfps=$PFPS OK"
else
  log "WARN no pfps in log snip (candidate may log elsewhere); HDMI gates still PASS"
fi

# optional seeks
if [[ "$SKIP_SEEK" != "1" ]]; then
  for OFF in 60000 180000; do
    remote "wget -qO- 'http://127.0.0.1:3005/player/timeline/seekTo?offset=${OFF}' >/dev/null || true"
    sleep 2
    ffmpeg -hide_banner -loglevel error -y -f v4l2 -input_format mjpeg -video_size "$SIZE" -i "$DEV" \
      -frames:v 20 -q:v 3 "$OUT/seek${OFF}_%02d.jpg" 2>/dev/null || true
  done
  python3 - "$OUT" <<'PY' | tee "$OUT/seek_score.txt" || true
from pathlib import Path
import numpy as np
from PIL import Image
out = Path(__import__("sys").argv[1])
lasts = []
for off in (60000, 180000):
    fs = sorted(out.glob(f"seek{off}_*.jpg"))
    if not fs:
        print("seek missing", off)
        continue
    a = np.asarray(Image.open(fs[-1]).convert("RGB")).astype(np.float32)
    lasts.append(a)
    print(f"seek{off} mean={a.mean(axis=(0,1))}")
if len(lasts) == 2:
    d = float(np.abs(lasts[1] - lasts[0]).mean())
    print(f"cross_seek_delta={d:.2f}")
    print("PASS_SEEK" if d >= 10 else "WARN_SEEK_LOW_DELTA")
PY
fi

{
  echo "VERDICT=PASS_GLASS_BASELINE"
  echo "when=$TS"
  echo "candidate=$CANDIDATE"
  echo "live_rbf=$LIVE_RBF"
  echo "live_daemon=$LIVE_DAE"
  echo "floor_rbf=$RBF_MD5"
  echo "floor_daemon=$DAE_MD5"
  echo "out=$OUT"
} | tee "$OUT/VERDICT.txt"

# durable copy under Memory if present
MEM="$ROOT/Memory/lab/hdmi-proof/glass_baseline_$TS"
if mkdir -p "$MEM" 2>/dev/null; then
  cp -a "$OUT/VERDICT.txt" "$OUT/idle_best.png" "$OUT/play_mid.png" "$OUT/play_last.png" \
    "$OUT/live_stack.txt" "$OUT/idle_score.txt" "$OUT/play_score.txt" "$MEM/" 2>/dev/null || true
  cp -f "$OUT/VERDICT.txt" "$ROOT/Memory/lab/status/glass-baseline-last.txt" 2>/dev/null || true
  log "evidence also at $MEM"
fi

log "PASS_GLASS_BASELINE"
exit 0
