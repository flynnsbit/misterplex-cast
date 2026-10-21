#!/usr/bin/env python3
"""Pins companionTimelineTick: valid PMS state, no double-fire, FPGA ACK + host ticks."""
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


check("void companionTimelineTick();" in hpp, "hpp must declare companionTimelineTick")
check("void MediaPlayer::companionTimelineTick()" in cpp, "cpp must define companionTimelineTick")
check('"timeline"' not in cpp.split("void MediaPlayer::companionTimelineTick()", 1)[1].split("void MediaPlayer::", 1)[0],
      "helper must not pass state timeline")
body = cpp.split("void MediaPlayer::companionTimelineTick()", 1)[1].split("void MediaPlayer::", 1)[0]
check('paused_.load() ? "paused" : "playing"' in body, "helper must send playing/paused")
check("companionTimelineTick();" in cpp, "must have a call site")
# FPGA ACK used to call onProgress_ directly; now the helper.
ack = cpp[cpp.find("if (firstPresentation)"):cpp.find("return true;", cpp.find("if (firstPresentation)"))]
check("companionTimelineTick()" in ack, "FPGA ACK must tick companion via helper")
check('onProgress_(paused_.load() ? "paused" : "playing"' not in ack,
      "FPGA ACK must not double-fire onProgress_")

if fails:
    print("test_companion_timeline_tick: %d failures" % fails)
    sys.exit(1)
print("test_companion_timeline_tick: OK")
