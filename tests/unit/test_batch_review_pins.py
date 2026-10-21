#!/usr/bin/env python3
"""Source pins for c937 batched review findings (would fail on c737a)."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
cpp = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
hpp = (ROOT / "arm/misterplexd/media_player.hpp").read_text()
fails = 0


def check(cond, msg):
    global fails
    if not cond:
        print("FAIL", msg)
        fails += 1


check("defaultSourceAspectForBank(" in cpp, "720x480 empty DAR must use bank 4:3 not square 3:2")
check("source aspect fallback coded" not in cpp, "must not log square coded W:H fallback")
check("ptsWrap_.apply" in cpp, "PTS wrap must be applied to content-us")
check("ptsWrap_.reset()" in cpp, "PTS wrap must reset per session")
check("directProxyPolicy" in hpp, "MediaPlayer::directProxyPolicy declared")
check("level > 21" not in cpp, "must not invent level 21")
check("level > 30" in cpp, "FPGA direct cap is level 30")
check("offset = srcCount" in cpp, "audio queue bound must drop PCM")
check("stalled picture detected" in cpp, "500ms stall log")
check("first_picture_ms=" in hpp, "TTFP log includes first_picture_ms")
check("fpgaAvStartGateBank" in cpp, "shared A/V start gate")
check("audioPtsBindEnabled_" in cpp, "shared audio PTS bind")
check("ptsWrap_.apply(packet.pts)" in cpp, "PTS wrap must apply to compressed pace")
check("ptsJumpIsAcceptedNFrames" in cpp, "Gate A N-frame jumps must pass through")
check("audioQueueBound240p(" in cpp, "240p 120 ms audio queue drop")
check("audioLeadClamp240p(" in cpp, "240p DMA lead ceiling")
check("audioLeadClamp480p(" in cpp, "480p audio lead wait-to-bound")
check("spsRejectCode480pFromField" in cpp, "480p SPS reject coarse code")
check("spsReject480i(" in cpp, "480i SPS reject 720x480")
check("urlWithBitrateFloor480i" in cpp, "480i seek/play bitrate floor")
check("companionTimelineTick();" in cpp, "companion timeline tick call site")
check("written -= drop" not in cpp, "must not rewind DMA written on audio bound")

if fails:
    print("test_batch_review_pins: %d failures" % fails)
    sys.exit(1)
print("test_batch_review_pins: OK")
