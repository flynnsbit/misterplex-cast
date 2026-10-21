#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
hdr = (ROOT / "arm/misterplexd/media_player.hpp").read_text()
src = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
fails = 0

def check(cond, msg):
    global fails
    if not cond:
        print("FAIL", msg)
        fails += 1

check("inline int plex240pActiveHeight" in hdr, "need plex240pActiveHeight")
check("return coded_height >= 240 ? 240 : coded_height" in hdr, "240p cap is 240")
check("kPlexActiveHeights" in hdr, "need shared mode table")
check('{"240p", 240, 0}' in hdr, "240p row 240")
check('{"480p", 480, 0}' in hdr, "480p row 480")
check('{"480i", 480, 240}' in hdr, "480i row frame 480 field 240")
check(hdr.count('{"240p"') + hdr.count('{"480p"') + hdr.count('{"480i"') >= 3, "three mode rows")
fn = hdr.split("inline int plexSharedActiveHeight")[1].split("int plex")[0]
check("if (coded_height <= 0)" in fn, "zero guard")
check("return 0;" in fn, "zero must not invent 240")
check("if (coded_height >= 480)" in fn, "480-line cap")
check("return 480;" in fn, "480i/480p active height is 480")
check("if (coded_height >= 240)" in fn, "240-line cap")
check("return 240;" in fn, "240p active height is 240")
check("* 3" not in fn and "720" not in fn, "never x3 / never store 720")
check(src.count("plexSharedActiveHeight(outH_)") == 2, "240p and 480 arms both call shared")
check("media: 480 active_height=" in src, "480p/480i arm logs shared height")
check("media: 240p active_height=" in src, "240p arm still logs")
check("g.crop_top = crop_top" not in src, "must not mutate crop_top")
check("g.display_height = active_height" not in src, "must not mutate display_height")

if fails:
    print("test_shared_active_height: %d failures" % fails)
    sys.exit(1)
print("test_shared_active_height: OK")
