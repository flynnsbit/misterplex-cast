#!/usr/bin/env python3
"""Generate a real H.264 Baseline annex-B clip (SPS/PPS/IDR) via ffmpeg for 3.3c SPS parse + F3 tests."""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="plex_real_baseline.264")
    ap.add_argument("--w", type=int, default=320)
    ap.add_argument("--h", type=int, default=240)
    ap.add_argument("--frames", type=int, default=1)
    args = ap.parse_args()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ffmpeg not found", file=sys.stderr)
        return 1
    cmd = [
        ffmpeg,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={args.w}x{args.h}:rate=1",
        "-frames:v",
        str(args.frames),
        "-c:v",
        "libx264",
        "-profile:v",
        "baseline",
        "-level",
        "3.0",
        "-pix_fmt",
        "yuv420p",
        "-x264-params",
        "keyint=1:min-keyint=1:scenecut=0:cabac=0",
        "-bsf:v",
        "h264_mp4toannexb",
        "-f",
        "h264",
        str(out),
    ]
    subprocess.check_call(cmd)
    data = out.read_bytes()
    # Quick NAL inventory
    i = 0
    types = []
    while i + 3 < len(data):
        if data[i : i + 4] == b"\x00\x00\x00\x01":
            sc = 4
        elif data[i : i + 3] == b"\x00\x00\x01":
            sc = 3
        else:
            i += 1
            continue
        j = i + sc
        while j + 3 < len(data):
            if data[j : j + 3] == b"\x00\x00\x01" or data[j : j + 4] == b"\x00\x00\x00\x01":
                break
            j += 1
        if j + 3 >= len(data):
            j = len(data)
        types.append(data[i + sc] & 0x1F)
        i = j
    print(
        f"wrote {out} ({len(data)} bytes) NALs types={types} expect SPS=7 PPS=8 IDR=5 dims={args.w}x{args.h}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
