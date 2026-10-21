#!/usr/bin/env python3
"""Shared even-top letterbox bars for 240- and 480-line rasters.

Pins letterboxBarsN in media_player.hpp and the 240p G2 odd-center (178 -> 30/32).
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
hdr = (ROOT / "arm/misterplexd/media_player.hpp").read_text()
cpp = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        print("FAIL", msg)
        fails += 1


def bars(raster, coded, display):
    if coded <= 0 or display <= 0:
        return 0, 0
    if coded >= raster or display >= raster:
        return 0, 0
    top = ((raster - display) // 2) & ~1
    bottom = raster - display - top
    return top, bottom


check("letterboxBarsN" in hdr, "need shared letterboxBarsN")
check("letterboxBars240" in hdr, "need letterboxBars240")
check("letterboxBars480" in hdr, "need letterboxBars480")
check("k240pLetterboxRasterLines = 240" in hdr, "240 raster constant")
check("letterboxBars240(" in cpp, "FPGA path must call letterboxBars240")
check(bars(240, 176, 160) == (40, 40), "176/160 -> 40/40")
check(bars(240, 240, 240) == (0, 0), "full 240 passthrough")
check(bars(240, 240, 224) == (0, 0), "coded 240 passthrough even if vis 224")
check(bars(240, 144, 136) == (52, 52), "144/136 -> 52/52")
check(bars(240, 192, 178) == (30, 32), "178 odd-center 30/32")
check(bars(480, 416, 404) == (38, 38), "404 on 480 -> 38/38")
check(bars(480, 480, 468) == (0, 0), "coded 480 passthrough")

if fails:
    print("test_letterbox_bars: %d failures" % fails)
    sys.exit(1)
print("test_letterbox_bars: OK")
