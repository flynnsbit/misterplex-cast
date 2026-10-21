#!/usr/bin/env python3
"""Shared FPGA A/V start gate: hold 240p/480p/480i video until audio prefill.

Fails on c737a (no audioReadyForStart_, no fpgaAvStartGateBank wait).
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CPP = "arm/misterplexd/media_player.cpp"
HPP = "arm/misterplexd/media_player.hpp"


def read(root, rel):
    with open(os.path.join(root, rel)) as f:
        return f.read()


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def method_src(src, name):
    needle = f"void MediaPlayer::{name}"
    i = src.find(needle)
    if i < 0:
        return None
    j = src.find("void MediaPlayer::", i + len(needle))
    if j < 0:
        j = len(src)
    return src[i:j]


def check(root):
    fails = []
    cpp = read(root, CPP)
    hpp = read(root, HPP)
    pump = method_src(cpp, "fpgaAudioPump")
    video = method_src(cpp, "fpgaThreadMain")
    if pump is None:
        fails.append("fpgaAudioPump not found")
        return fails
    if video is None:
        fails.append("fpgaThreadMain not found")
        return fails
    pump_code = strip_comments(pump)
    video_code = strip_comments(video)
    hpp_code = strip_comments(hpp)

    if "fpgaAvStartGateBank" not in hpp:
        fails.append("media_player.hpp: missing fpgaAvStartGateBank")

    open_i = video.find("demux.open")
    reset_i = video.find("audioReadyForStart_.store(false)")
    if open_i < 0:
        fails.append("fpgaThreadMain: demux.open not found")
    elif reset_i < 0 or reset_i >= open_i:
        fails.append("fpgaThreadMain: audioReadyForStart_.store(false) must appear before demux.open")

    m = re.search(r"while\s*\(\s*!audioReadyForStart_\.load\(\)", video_code)
    if not m:
        fails.append("fpgaThreadMain: missing while (!audioReadyForStart_.load()) wait")
    else:
        pre = video_code[max(0, m.start() - 500) : m.start()]
        if "fpgaAvStartGateBank" not in pre:
            fails.append("fpgaThreadMain: wait is not gated on fpgaAvStartGateBank")
        wait = video_code[m.start() : m.start() + 400]
        if "playbackActiveUs()" not in wait:
            fails.append("fpgaThreadMain: wait has no playbackActiveUs() deadline")

    if "audioReadyForStart_.store(true)" not in pump_code:
        fails.append("fpgaAudioPump: missing audioReadyForStart_.store(true)")
    else:
        ready = pump_code.find("audioReadyForStart_.store(true)")
        vwait = pump_code.find("FpgaAvWait::VideoNotStarted")
        if vwait >= 0 and ready > vwait + 800:
            fails.append("fpgaAudioPump: ready flag is far from the VideoNotStarted prefill path")

    if "setAudioReadyForStart" in hpp_code:
        fails.append("media_player.hpp: public setAudioReadyForStart must not exist")
    if "audioReadyForStart_" not in hpp:
        fails.append("media_player.hpp: missing private audioReadyForStart_ member")

    return fails


def main():
    fails = check(ROOT)
    if fails:
        for f in fails:
            print("FAIL", f)
        return 1
    print("test_fpga_av_start_gate: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
