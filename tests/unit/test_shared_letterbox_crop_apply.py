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


check("inline bool plexSharedLetterboxCrop" in hdr, "need plexSharedLetterboxCrop")
check("std::string selectCoreRbfForMode(int width, int height) const;" in hdr,
      "declare selectCoreRbfForMode in hpp")
check("uint32_t coreId480i() const" in hdr, "coreId480i in hpp")
check("uint32_t coreId240p() const" in hdr, "coreId240p in hpp")
check("uint32_t MediaPlayer::coreId480p()" not in src, "no extra coreId480p cpp stub")
check("uint32_t MediaPlayer::coreId480i()" not in src, "coreId480i is inline in hpp")
check("setLetterboxCrop(letterboxCropTop_, letterboxCropBottom_)" not in src, "no identity re-store")
fn = hdr.split("inline bool plexSharedLetterboxCrop")[1].split("inline ")[0] if "inline bool plexSharedLetterboxCrop" in hdr else ""
check("if (codedH <= 0" in fn, "zero guard")
check("if (codedH >= 720)" in fn, "never x3 / refuse 720p")
check("const int raster = codedH >= 480 ? 480 : (codedH >= 240 ? 240 : codedH)" in fn, "480 then 240 raster")
check("const int lbTop = ((raster - displayH) / 2) & ~1;" in fn, "G2 even-top")
check("480-bank 404 -> 38/38" in hdr, "480i 404 is 38/38")
check("350 -> 64/66" in hdr, "640x350 G2 is 64/66 not truncating 65")
check("240-bank 180 -> 30/30" in hdr, "240p short title into 240")
check("codedH >= 720 -> false" in hdr, "never x3 documented")
check("* 3" not in fn and "9 / 16" not in fn, "no x3 and no DAR 16:9 crop")
check("plexSharedLetterboxCrop(g.coded_height, sourceMediaH_, top, bot)" in src,
      "playback applies shared crop")
check("kPlex480iCodedWidth" in src and "kPlex480pPresentedWidth" in src and "kPlex240pPresentedWidth" in src,
      "three banks in sharedLbBank")
check("g.display_height = g.coded_height - top - bot" in src, "display_height must shrink")
check("g.crop_top = top" in src and "g.crop_bottom = bot" in src, "set crop from helper")
check("g.crop_top += " not in src, "must not add onto existing bars")
check("media: letterboxCrop top=" in src, "log keeps letterboxCrop token")
check("media: selectCoreRbfForMode " in src, "play logs selectCoreRbfForMode")
check("inline int plex240pActiveHeight" in hdr, "keep 240p active-height name")

if fails:
    print("test_shared_letterbox_crop_apply: %d failures" % fails)
    sys.exit(1)
print("test_shared_letterbox_crop_apply: OK")
