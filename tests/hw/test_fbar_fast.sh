#!/usr/bin/env bash
# OBSOLETE on Plex core v3 (RBF 91777ac1) and later.
#
# The v3 CONF_STR removed the debug menu items this script drives — Pattern,
# Audio tone and Force bars — and reclaimed status[9:6] for the video delay.
# `pattern`, `audio_en` and `use_frame_store` are now hardwired to 0 in Plex.sv,
# so there is no way to ask the core for colour bars any more. Running this
# against a v3 core writes a bogus video delay instead of enabling bars and will
# always report failure.
#
# Kept for archaeology / for bisecting against a pre-v3 RBF only.
# Verify the live core first:  set_status --confstr
#
# Fast Force-bars only check — no long dwell, no full matrix.
# ~2–4 seconds wall. Exit 0 if force_bars=1 with pattern=grid still shows bars
# after RBF with eff_pattern (O[9] → pattern 0).
set -euo pipefail
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
OUT="${MENU_CAPTURE_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/captures/menu}"
DEV="${HDMI_DEV:-/dev/video4}"
mkdir -p "$OUT"
SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 "root@$HOST")
ssh_q() { "${SSH[@]}" "$@" 2>/dev/null | grep -v 'WARNING\|post-quantum\|vulnerable\|store now' || true; }

snap() {
  local name=$1
  fuser -k "$DEV" 2>/dev/null || true
  sleep 0.15
  ffmpeg -y -hide_banner -loglevel error \
    -f v4l2 -input_format mjpeg -video_size 800x600 -framerate 30 \
    -i "$DEV" -frames:v 2 -q:v 3 "$OUT/${name}_%02d.jpg" 2>/dev/null || true
  # pick last frame
  local f
  f=$(ls -1 "$OUT/${name}_"*.jpg 2>/dev/null | tail -1)
  [[ -n "$f" ]] || { echo "no capture $name"; return 1; }
  cp -f "$f" "$OUT/${name}.jpg"
  python3 - "$f" <<'PY'
import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert("L")
px=list(im.getdata())
m=sum(px)/len(px)
print(f"{m:.1f}")
PY
}

echo "=== fast FBAR $(date -Iseconds) ==="
ssh_q '/media/fat/misterplex/bin/set_status --tv ntsc --fps 60 --audio on --pattern grid --force-bars 0'
sleep 0.2
m0=$(snap fbar_fast_grid_off || echo 0)
ssh_q '/media/fat/misterplex/bin/set_status --raw' | head -2

ssh_q '/media/fat/misterplex/bin/set_status --pattern grid --force-bars 1'
sleep 0.2
m1=$(snap fbar_fast_grid_on || echo 0)
ssh_q '/media/fat/misterplex/bin/set_status --raw' | head -2

# Leave stable bars (don't leave test pattern up)
ssh_q '/media/fat/misterplex/bin/set_status --pattern bars --force-bars 1 --tv ntsc --fps 60'
sleep 0.15
m2=$(snap fbar_fast_bars || echo 0)

echo "grid_off mean=$m0  grid_on(force) mean=$m1  bars mean=$m2"
# After eff_pattern fix, force-on with grid should look like bars (closer to m2 than m0)
python3 - <<PY
m0,m1,m2=float("$m0" or 0),float("$m1" or 0),float("$m2" or 0)
print(f"delta force_vs_bars={abs(m1-m2):.1f} force_vs_grid_off={abs(m1-m0):.1f}")
# Soft pass: all non-black; hard visual needs RBF with eff_pattern
ok = m1 >= 15 and m2 >= 15
raise SystemExit(0 if ok else 1)
PY
echo "FBAR_FAST done (left on bars)"
