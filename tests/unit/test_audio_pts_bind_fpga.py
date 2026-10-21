#!/usr/bin/env python3
"""Shared FPGA audio PTS bind for 240p/480p/480i.

Fails on c737a: no audioPtsBindEnabled_, no once-loaded videoPts overwrite.
Passes when the flag defaults on, video PTS loads once, FPGA banks overwrite,
kNoTimestamp video skips the bind, and the 2 s gap stays after the bind.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HPP = ROOT / "arm" / "misterplexd" / "media_player.hpp"
CPP = ROOT / "arm" / "misterplexd" / "media_player.cpp"
MAIN = ROOT / "arm" / "misterplexd" / "main.cpp"

fails = 0


def fail(msg: str) -> None:
    global fails
    print(f"FAIL: {msg}", file=sys.stderr)
    fails += 1


def ok(msg: str) -> None:
    print(f"OK: {msg}")


def pump_body(cpp: str) -> str | None:
    i = cpp.find("void MediaPlayer::fpgaAudioPump")
    if i < 0:
        fail("MediaPlayer::fpgaAudioPump missing")
        return None
    j = cpp.find("audioActive_.store(false)", i)
    if j < 0:
        fail("fpgaAudioPump missing audioActive_.store(false) tail")
        return None
    return cpp[i:j]


def main() -> int:
    hpp = HPP.read_text(encoding="utf-8")
    cpp = CPP.read_text(encoding="utf-8")
    main_txt = MAIN.read_text(encoding="utf-8")

    if hpp.count("audioPtsBindEnabled_ = true") != 1:
        fail("audioPtsBindEnabled_ must default true exactly once")
    else:
        ok("audioPtsBindEnabled_ = true exactly once")

    if "fpgaAudioPtsBindBank" not in hpp or "fpgaAudioPtsBindBank" not in cpp:
        fail("shared fpgaAudioPtsBindBank must exist in hpp and cpp")
    else:
        ok("shared fpgaAudioPtsBindBank")

    pump = pump_body(cpp)
    if pump is None:
        return 1

    if "const int64_t videoPts = fpgaFirstVideoPtsUs_.load();" not in pump:
        fail("fpgaAudioPump must load fpgaFirstVideoPtsUs_ once into videoPts")
    else:
        ok("video PTS loaded once")

    if "videoPts != ddr_bitstream_ring::kNoTimestamp" not in pump:
        fail("bind must refuse kNoTimestamp video")
    else:
        ok("kNoTimestamp video refused")

    if "audioPts = videoPts;" not in pump:
        fail("bind must assign audioPts = videoPts")
    else:
        ok("audioPts = videoPts overwrite")

    if "audioPts = fpgaFirstVideoPtsUs_.load();" in pump:
        fail("bind must not load fpgaFirstVideoPtsUs_ again on assign")
    else:
        ok("no second .load() on the overwrite")

    if "static_cast<long double>(audioPts) - videoPts" not in pump:
        fail("timestampDelta must subtract the once-loaded videoPts")
    else:
        ok("timestampDelta uses once-loaded videoPts")

    if "timestampDelta < -2000000 || timestampDelta > 2000000" not in pump:
        fail("2 s gap |timestampDelta|>2000000 must stay after the bind")
    else:
        ok("2 s gap check kept")

    if "fpgaAudioPtsBindBank(outW_, outH_)" not in pump:
        fail("bind must use shared fpgaAudioPtsBindBank(outW_, outH_)")
    else:
        ok("shared bank gate")

    if "setAudioPtsBind" in main_txt:
        fail("main.cpp must not call setAudioPtsBind")
    else:
        ok("main.cpp has no setAudioPtsBind")

    if fails:
        print(f"test_audio_pts_bind_fpga: {fails} failure(s)", file=sys.stderr)
        return 1
    print("test_audio_pts_bind_fpga: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
