#!/usr/bin/env python3
"""TTFP breakdown: four stages on one clock; first packet recorded once."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
src = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
hpp = (ROOT / "arm/misterplexd/media_player.hpp").read_text()
fails = []
def expect(c, m):
    if not c:
        fails.append(m)
expect("ttfpOpenTimeUs_" in hpp and "ttfpFirstPacketTimeUs_" in hpp
       and "ttfpFirstDecodeTimeUs_" in hpp and "ttfpFirstPicTimeUs_" in hpp,
       "hpp missing ttfp atomics")
expect("ttfpFirstPicTimeUs_.store(us);" not in src,
       "first pic must not store content PTS us")
expect("ttfpFirstPicTimeUs_.store(anchorMonoUs);" in src,
       "first pic must store anchorMonoUs (steady_clock)")
# playInternal zeros the packet stamp; the first-packet store uses chrono.
i = src.find("ttfpFirstPacketTimeUs_.store")
while i >= 0 and "store(0" in src[i:i + 40]:
    i = src.find("ttfpFirstPacketTimeUs_.store", i + 1)
# The firstAu gate still wraps the store. The block in front of it is the High/CABAC
# and 240p/480i checks, so the lookback has to reach that if.
w = src[max(0, i - 3600):i] if i >= 0 else ""
expect("if (firstAu)" in w, "first-packet store must be gated on firstAu")
expect("firstAu = false" not in w[-120:],
       "first-packet store must run before firstAu = false")
open_i = src.find("ttfpOpenTimeUs_.store")
region = src[open_i:open_i + 900]
for n in ("ttfpFirstPacketTimeUs_.store(0", "ttfpFirstDecodeTimeUs_.store(0",
          "ttfpFirstPicTimeUs_.store(0"):
    expect(n in region, "playInternal must reset " + n)
expect("t_pic_to_display_us=" not in src, "drop mixed-clock t_pic_to_display_us")
expect("t_open_to_pic_us=" in src, "log total TTFP t_open_to_pic_us")
expect("openUs != 0 && pktUs != 0 && decUs != 0 && picUs != 0" in src,
       "log only when all four stages are set")
expect("media: ttfp-breakdown" in src, "breakdown log line present")
if fails:
    print("FAIL test_ttfp_breakdown_log:")
    for f in fails:
        print(" ", f)
    sys.exit(1)
print("test_ttfp_breakdown_log: PASS")
