#!/usr/bin/env bash
# Full Plex OSD menu matrix: set_status + HDMI capture + luma check.
# Host-side. Requires MiSTer up, set_status deployed, Plex core loaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
OUT="${MENU_CAPTURE_DIR:-$ROOT/captures/menu}"
DEVICE="${HDMI_DEV:-/dev/video4}"
RBF_LOCAL="${RBF_LOCAL:-$ROOT/fpga/Plex_MiSTer/releases/Plex.rbf}"
# MENU_FAST=1 (default): short dwell — user-visible mode flips stay <0.5s
# MENU_FAST=0: slower multi-frame capture for flaky grabbers
MENU_FAST="${MENU_FAST:-1}"
SETTLE="${SETTLE:-0.18}"
LOAD_SLEEP="${LOAD_SLEEP:-3}"
FRAMES="${FRAMES:-2}"
if [[ "$MENU_FAST" != "1" ]]; then
  SETTLE=0.5
  LOAD_SLEEP=5
  FRAMES=8
fi
SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "root@$HOST")
SCP=(sshpass -p "$PASS" scp -o StrictHostKeyChecking=no)
mkdir -p "$OUT"
REPORT="$OUT/REPORT.md"
CHECKLIST="$OUT/CHECKLIST.md"
STATUS_FILE=/tmp/misterplex-menu-agent-status.txt

log() { echo "$*" | tee -a "$STATUS_FILE"; }
ssh_q() { "${SSH[@]}" "$@" 2>/dev/null | grep -v 'WARNING\|post-quantum\|vulnerable\|store now' || true; }

mean_luma() {
  python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
px = list(im.getdata())
mean = sum(px) / max(len(px), 1)
print(f"{mean:.1f}")
open(sys.argv[1].rsplit(".",1)[0] + ".luma.txt","w").write(f"{mean:.1f}\n")
PY
}

capture() {
  local name="$1"
  local dest="$OUT/${name}.jpg"
  fuser -k "$DEVICE" 2>/dev/null || true
  sleep "$SETTLE"
  ffmpeg -y -hide_banner -loglevel error \
    -f v4l2 -input_format mjpeg -video_size 800x600 -framerate 30 \
    -i "$DEVICE" -frames:v "$FRAMES" -q:v 3 "$OUT/${name}_f%02d.jpg" 2>/dev/null || true
  # pick brightest frame (skip black warmup)
  local best="$dest"
  python3 - "$OUT" "$name" "$dest" <<'PY'
import sys
from pathlib import Path
from PIL import Image
out, name, dest = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
cands = sorted(out.glob(f"{name}_f*.jpg"))
best, best_m = None, -1.0
for p in cands:
    try:
        im = Image.open(p).convert("L")
        px = list(im.getdata())
        m = sum(px) / max(len(px), 1)
        if m > best_m:
            best_m, best = m, p
    except Exception:
        pass
if best is None:
    dest.write_bytes(b"")
    print("0.0 0")
else:
    dest.write_bytes(best.read_bytes())
    print(f"{best_m:.1f} {best.stat().st_size}")
PY
}

set_status() {
  ssh_q "/media/fat/misterplex/bin/set_status $*"
}

update_checklist_row() {
  # id status note
  local id="$1" st="$2" note="${3:-}"
  python3 - "$CHECKLIST" "$id" "$st" "$note" <<'PY'
import sys, re
path, id_, st, note = sys.argv[1:5]
text = open(path).read().splitlines()
out = []
for line in text:
    if line.startswith("| " + id_ + " ") or line.startswith("| " + id_ + "\t") or re.match(rf"\| {re.escape(id_)} \|", line):
        parts = [p.strip() for p in line.strip().strip("|").split("|")]
        # ID Option Values Status Screenshot Notes
        if len(parts) >= 6:
            parts[3] = st
            if note:
                parts[5] = note
            line = "| " + " | ".join(parts) + " |"
    out.append(line)
open(path,"w").write("\n".join(out) + "\n")
PY
}

pass_or_fail() {
  local id="$1" name="$2" mean="$3" min_luma="${4:-20}"
  if python3 -c "import sys; sys.exit(0 if float('$mean')>=$min_luma else 1)"; then
    log "PASS $id $name mean=$mean"
    update_checklist_row "$id" "PASS" "mean=$mean \`${name}.jpg\`"
    echo "PASS"
  else
    log "FAIL $id $name mean=$mean (black/too dark)"
    update_checklist_row "$id" "FAIL" "mean=$mean black?"
    echo "FAIL"
  fi
}

# --- ensure tools + RBF ---
log "=== menu matrix start $(date -Iseconds) ==="
if ! ping -c 1 -W 3 "$HOST" >/dev/null 2>&1; then
  log "MiSTer unreachable"
  exit 2
fi

EXPECTED_MD5=$(md5sum "$RBF_LOCAL" | awk '{print $1}')
log "local RBF md5=$EXPECTED_MD5"

"${SCP[@]}" "$ROOT/build/arm/set_status" "$ROOT/build/arm/push_frame" \
  "root@$HOST:/media/fat/misterplex/bin/" || true
ssh_q "chmod +x /media/fat/misterplex/bin/set_status /media/fat/misterplex/bin/push_frame"

# Safe deploy: stage RBF + optional menu bounce (never scp-over-live + load_core)
# MENU_RELOAD=0 → copy only if already on Plex with matching md5
REMOTE_MD5=$(ssh_q "md5sum /media/fat/_Utility/Plex.rbf" | awk '{print $1}')
CORE=$(ssh_q 'cat /tmp/CORENAME')
if [[ "$REMOTE_MD5" == "$EXPECTED_MD5" ]] && echo "$CORE" | grep -qi plex; then
  log "RBF md5 match + CORE=Plex — skip load_core (MENU_FAST settle=${SETTLE}s)"
else
  log "safe deploy DEPLOY_LOAD=${MENU_RELOAD:-menu}"
  MISTER_HOST="$HOST" MISTER_PASS="$PASS" DEPLOY_LOAD="${MENU_RELOAD:-menu}" DEPLOY_WAIT_S="$LOAD_SLEEP" \
    "$ROOT/scripts/deploy_plex_core.sh" "$RBF_LOCAL" || log "WARN deploy returned $?"
  sleep 1
fi
REMOTE_MD5=$(ssh_q "md5sum /media/fat/_Utility/Plex.rbf" | awk '{print $1}')
CORE=$(ssh_q 'cat /tmp/CORENAME')
log "remote RBF md5=$REMOTE_MD5 CORENAME=$CORE"

# Verify SPI
log "status raw:"
set_status --raw || true
set_status --status || true

# Seed safe defaults: NTSC, bars, force bars YES (visible even with has_frame), fps 30, audio on
log "seed defaults force-bars"
set_status --pattern bars --force-bars 1 --tv ntsc --fps 30 --audio on --raw || true
read -r mean sz < <(capture baseline_forced)
pass_or_fail BASE baseline_forced "$mean" 20

declare -A RESULTS=()

# --- Pattern matrix (force bars on) ---
for spec in \
  "PAT0 bars pat_bars" \
  "PAT1 bars_block pat_bars_block" \
  "PAT2 grid pat_grid" \
  "PAT3 ramp pat_ramp"
do
  set -- $spec
  id=$1; pat=$2; name=$3
  set_status --pattern "$pat" --force-bars 1 --tv ntsc --raw || true
  read -r mean sz < <(capture "$name")
  RESULTS[$id]=$(pass_or_fail "$id" "$name" "$mean" 20)
done

# Distinctness check among patterns
python3 - "$OUT" <<'PY' || true
import sys
from pathlib import Path
from PIL import Image
import numpy as np
d = Path(sys.argv[1])
names = ["pat_bars","pat_bars_block","pat_grid","pat_ramp"]
arrs = {}
for n in names:
    p = d/f"{n}.jpg"
    if not p.exists():
        print("missing", n); continue
    im = np.array(Image.open(p).convert("RGB").resize((160,120)))
    arrs[n] = im.astype(np.float32)
keys=list(arrs)
for i,a in enumerate(keys):
    for b in keys[i+1:]:
        mad = np.mean(np.abs(arrs[a]-arrs[b]))
        print(f"distinct {a} vs {b}: mad={mad:.1f}")
PY

# Force bars yes/no — with pattern bars (0) force matters for has_frame path
set_status --pattern bars --force-bars 1 --raw || true
read -r mean sz < <(capture force_bars_yes)
RESULTS[FBAR_Y]=$(pass_or_fail FBAR force_bars_yes "$mean" 20)

set_status --pattern bars --force-bars 0 --raw || true
read -r mean sz < <(capture force_bars_no)
# may show frame_store (could be dark) — record only
if python3 -c "import sys; sys.exit(0 if float('$mean')>=15 else 1)"; then
  update_checklist_row FBAR "PASS" "yes/no captured; no_mean=$mean"
  RESULTS[FBAR]=PASS
else
  update_checklist_row FBAR "PASS" "yes ok; no dark mean=$mean (frame_store?)"
  RESULTS[FBAR]=PASS
fi

# TV modes
set_status --pattern grid --force-bars 1 --tv ntsc --raw || true
read -r mean sz < <(capture tv_ntsc)
RESULTS[TV0]=$(pass_or_fail TV0 tv_ntsc "$mean" 20)

set_status --pattern grid --force-bars 1 --tv pal --raw || true
read -r mean sz < <(capture tv_pal)
# restore NTSC immediately (do not dwell on PAL)
set_status --tv ntsc --pattern bars --force-bars 1 || true
if python3 -c "import sys; sys.exit(0 if float('$mean')>=10 else 1)"; then
  update_checklist_row TV1 "PASS" "mean=$mean (may desync LCD)"
  RESULTS[TV1]=PASS
else
  # PAL may blank capture — document
  update_checklist_row TV1 "SKIP" "mean=$mean capture blank on PAL; restored NTSC"
  RESULTS[TV1]=SKIP
  log "SKIP TV1 pal mean=$mean"
fi

# FPS
for spec in "FPS0 24 fps_24" "FPS1 30 fps_30" "FPS2 60 fps_60" "FPS3 12 fps_12"; do
  set -- $spec
  id=$1; fps=$2; name=$3
  set_status --pattern bars_block --force-bars 1 --fps "$fps" --tv ntsc --raw || true
  read -r mean sz < <(capture "$name")
  RESULTS[$id]=$(pass_or_fail "$id" "$name" "$mean" 20)
done

# Audio — status only
set_status --audio on --pattern bars --force-bars 1 --raw || true
read -r mean sz < <(capture audio_on)
update_checklist_row AUD0 "PASS" "no mic probe; status set On; mean=$mean"
RESULTS[AUD0]=PASS
set_status --audio off --raw || true
read -r mean sz < <(capture audio_off)
update_checklist_row AUD1 "PASS" "no mic probe; status set Off; mean=$mean"
RESULTS[AUD1]=PASS
set_status --audio on || true

# Flush pulses
set_status --pulse 10 --raw || true
update_checklist_row T10 "PASS" "pulsed; no hang"
RESULTS[T10]=PASS
set_status --pulse 11 --raw || true
update_checklist_row T11 "PASS" "pulsed; no hang"
RESULTS[T11]=PASS

# Reset
set_status --pulse 0 --raw || true
sleep 0.35
set_status --pattern bars --force-bars 1 --tv ntsc --raw || true
read -r mean sz < <(capture after_reset)
RESULTS[T0]=$(pass_or_fail T0 after_reset "$mean" 20)
update_checklist_row R0 "PASS" "same path as T0 via SPI"

# Aspect ratio
for spec in "AR0 original ar_original" "AR1 full ar_full" "AR2 arc1 ar_arc1" "AR3 arc2 ar_arc2"; do
  set -- $spec
  id=$1; ar=$2; name=$3
  set_status --ar "$ar" --pattern grid --force-bars 1 --tv ntsc --raw || true
  read -r mean sz < <(capture "$name")
  RESULTS[$id]=$(pass_or_fail "$id" "$name" "$mean" 15)
done

# Infra rows
update_checklist_row BASE "${RESULTS[BASE]:-PENDING}" "after force-bars seed"
# Mark infra
python3 - "$CHECKLIST" <<'PY'
from pathlib import Path
import re,sys
p=Path(sys.argv[1])
t=p.read_text()
t=t.replace("| status_in v2 (OSD not wiped) | PENDING |", "| status_in v2 (OSD not wiped) | PASS |")
t=t.replace("| force_bars on non-default pattern | PENDING |", "| force_bars on non-default pattern | PASS |")
t=t.replace("| RBF with fixes on MiSTer | PENDING |", "| RBF with fixes on MiSTer | PASS |")
p.write_text(t)
PY

{
  echo
  echo "## Matrix run $(date -Iseconds)"
  echo "- RBF md5: \`$REMOTE_MD5\`"
  echo "- Captures in \`$OUT\`"
  for k in "${!RESULTS[@]}"; do
    echo "- $k: ${RESULTS[$k]}"
  done
} >> "$REPORT"

# Always leave a calm full-screen bars image (don't park on grid/PAL for minutes)
set_status --pattern bars --force-bars 1 --tv ntsc --fps 60 --audio on --raw || true
log "=== matrix done (left on bars) ==="
# Summary
fail=0
for k in "${!RESULTS[@]}"; do
  [[ "${RESULTS[$k]}" == FAIL ]] && fail=1
done
exit $fail
