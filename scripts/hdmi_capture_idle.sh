#!/usr/bin/env bash
# Capture a settled HDMI frame from the lab grabber and report its visual stats.
#
# WHY THIS EXISTS: a bare `ffmpeg -frames:v 1` returns a FALSE BLACK. The
# MacroSilicon MJPEG grabber needs ~15 frames to lock, so the first frames decode
# as uniform black regardless of what the MiSTer is actually showing. That recipe
# has already produced at least two false RED calls in this lab (see
# docs/ddr-daily-promotion.md). Always discard the warm-up.
#
# Usage:  scripts/hdmi_capture_idle.sh OUT.png [FRAMES]
# Env:    HDMI_DEV (default /dev/video0), HDMI_SIZE (default 1920x1080),
#         HDMI_TRIES (default 3)
#
# Exit:   0  captured, stats printed
#         1  GRABBER_NOT_READY  (device busy/absent, or every try was black)
#
# Prints: MEAN=… STD=… ORANGE_PX=… ACTIVE=WxH FILE=…
#   MEAN 15..70 with a healthy ORANGE_PX is the good Plex chevron idle.
#   MEAN >= 100 near-uniform is the documented mixed-pair GREEN failure.
#   MEAN == 0 with STD == 0 across all tries is a genuine black screen — which
#   is the mixed-pair failure mode where the core never frees a DDR bank.
set -uo pipefail
OUT="${1:?usage: hdmi_capture_idle.sh OUT.png [FRAMES]}"
FRAMES="${2:-45}"
DEV="${HDMI_DEV:-/dev/video0}"
SIZE="${HDMI_SIZE:-1920x1080}"
TRIES="${HDMI_TRIES:-3}"

# Operator error must never be reported as a device observation. Passing a
# directory here used to raise IsADirectoryError inside the stats step, which
# then fell through to "GRABBER_NOT_READY reason=all_tries_uniform_black" -- a
# device-shaped verdict for a bad argument. In this lab a black-screen verdict
# gets acted on (rollbacks, agent lanes, retracted milestones), so a usage
# mistake that renders as one is a real hazard. Exit 2: not a device claim.
if [ -d "$OUT" ]; then
  echo "USAGE_ERROR: OUT must be a .png FILE, not a directory: $OUT" >&2
  echo "             usage: hdmi_capture_idle.sh OUT.png [FRAMES]" >&2
  exit 2
fi
case "$OUT" in
  */) echo "USAGE_ERROR: OUT looks like a directory path: $OUT" >&2; exit 2 ;;
esac
_out_dir="$(dirname "$OUT")"
if [ ! -d "$_out_dir" ]; then
  echo "USAGE_ERROR: output directory does not exist: $_out_dir" >&2
  exit 2
fi

if [ ! -e "$DEV" ]; then
  echo "GRABBER_NOT_READY reason=no_device dev=$DEV" >&2
  exit 1
fi
if command -v fuser >/dev/null 2>&1 && fuser "$DEV" >/dev/null 2>&1; then
  echo "GRABBER_NOT_READY reason=device_busy dev=$DEV (close OBS/preview; fuser -v $DEV)" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for try in $(seq 1 "$TRIES"); do
  rm -f "$TMP"/f_*.png
  ffmpeg -v error -f v4l2 -input_format mjpeg -video_size "$SIZE" \
    -i "$DEV" -frames:v "$FRAMES" -y "$TMP/f_%03d.png" >/dev/null 2>&1
  last="$(ls "$TMP"/f_*.png 2>/dev/null | tail -1)"
  [ -n "$last" ] || { sleep 2; continue; }
  cp -f "$last" "$OUT"
  if python3 - "$OUT" "$try" <<'PY'
import sys
import numpy as np
from PIL import Image
im = np.array(Image.open(sys.argv[1]).convert("RGB")).astype(int)
lum = im.sum(axis=2) / 3.0
R, G, B = im[:, :, 0], im[:, :, 1], im[:, :, 2]
orange = int(((R > 150) & (G > 90) & (G < 200) & (B < 90)).sum())
tot = im.sum(axis=2)
rows = np.where(tot.max(axis=1) > 60)[0]
cols = np.where(tot.max(axis=0) > 60)[0]
if len(rows) and len(cols):
    active = f"{cols[-1]-cols[0]+1}x{rows[-1]-rows[0]+1}"
    ax = f" ACTIVE_X=[{cols[0]},{cols[-1]}]"
else:
    active, ax = "0x0", ""
mean, std = lum.mean(), lum.std()
print(f"MEAN={mean:.1f} STD={std:.1f} ORANGE_PX={orange} ACTIVE={active}{ax} "
      f"TRY={sys.argv[2]} FILE={sys.argv[1]}")
# A perfectly uniform black frame is indistinguishable from a grabber that never
# locked, so report it as not-yet-settled and let the caller retry.
sys.exit(3 if (mean == 0.0 and std == 0.0) else 0)
PY
  then
    exit 0
  fi
  sleep 2
done

echo "GRABBER_NOT_READY reason=all_tries_uniform_black dev=$DEV tries=$TRIES" >&2
echo "  NOTE: if the grabber is healthy this means the device really is outputting" >&2
echo "  black — e.g. a mixed core/daemon pair where the core never frees a DDR bank." >&2
echo "  Confirm by rolling back to the stable pair and re-capturing on the SAME chain." >&2
exit 1
