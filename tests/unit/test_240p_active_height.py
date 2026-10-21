#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
hdr = (ROOT / "arm/misterplexd/media_player.hpp").read_text()
fails = 0

def check(cond, msg):
    global fails
    if not cond:
        print("FAIL", msg)
        fails += 1

check("ddrFrameGeometryForDecodeSize" not in src, "must not wrap DDR geom in 240p mutator")
check(src.count("ddrFrameGeometryForPresentedSize(") >= 5, "play/idle/aspect must use presented-size")
check("nativeScalerPresent ? ddrFrameGeometryForPresentedSize(outW_, outH_)" in src,
      "threadMain 480p/240p geom must stay presented-size")
check("(void)active_crop_top" not in src, "must not discard the 240p crop rect")
check("g.crop_top = crop_top" not in src, "must not mutate crop_top")
check("g.display_height = active_height" not in src, "must not mutate display_height")
check("224" not in hdr, "must not document 224 overscan as 240p active height")
check("plex240pActiveHeight" in hdr, "need plex240pActiveHeight")
check("return coded_height >= 240 ? 240 : coded_height" in hdr,
      "active height for 240-line output is 240")
fn = hdr.split("inline int plex240pActiveHeight")[1].split("}")[0]
check("if (coded_height <= 0)" in fn, "must guard zero source height")
check("return 0;" in fn, "zero source height must not invent 240")
check("240p active_height=" in src, "240p path must apply/log active_height")
check("isPlex480pDdrFrameGeometry(ddrGeometry)" in src, "480p matcher still used")

if fails:
    print("test_240p_active_height: %d failures" % fails)
    sys.exit(1)
print("test_240p_active_height: OK")
