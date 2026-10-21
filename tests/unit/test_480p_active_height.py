#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
fails = 0

def check(cond, msg):
    global fails
    if not cond:
        print("FAIL", msg)
        fails += 1

check("kActiveHeight480p = 478" not in src, "must not hardcode 478")
check("const DdrFrameGeometry ddrGeometry" in src, "ddrGeometry must stay const")
check("active_height" in src, "need active_height token")
check("kActiveHeight480p = ddrGeometry.display_height" in src,
      "active height must be stored display_height")
check("ddrGeometry.display_height = kActiveHeight480p" not in src,
      "must not mutate display_height")
check("ddrGeometry.crop_top =" not in src, "must not mutate crop_top")
check("ddrGeometry.crop_bottom =" not in src, "must not mutate crop_bottom")
check("isPlex480pDdrFrameGeometry(ddrGeometry)" in src, "480p matcher still used")

if fails:
    print("test_480p_active_height: %d failures" % fails)
    sys.exit(1)
print("test_480p_active_height: OK")
