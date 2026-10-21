#!/usr/bin/env python3
"""Generate deterministic H.264 Constrained Baseline Annex-B with IDR+P frames for inter-prediction tests."""
from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


def nal_types(data: bytes) -> list[int]:
    out: list[int] = []
    i = 0
    while i + 3 < len(data):
        if data[i : i + 4] == b"\x00\x00\x00\x01":
            sc = 4
        elif data[i : i + 3] == b"\x00\x00\x01":
            sc = 3
        else:
            i += 1
            continue
        if i + sc < len(data):
            out.append(data[i + sc] & 0x1F)
        i += sc
        while i + 3 < len(data) and data[i : i + 3] != b"\x00\x00\x01" and data[i : i + 4] != b"\x00\x00\x00\x01":
            i += 1
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="build/plex_inter_p16_baseline_320x240_12f.264")
    ap.add_argument("--w", type=int, default=320)
    ap.add_argument("--h", type=int, default=240)
    ap.add_argument("--rate", type=int, default=24)
    ap.add_argument("--frames", type=int, default=12)
    args = ap.parse_args()
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg not found")
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    params = ":".join(
        [
            "keyint=12",
            "min-keyint=12",
            "scenecut=0",
            "cabac=0",
            "bframes=0",
            "ref=1",
            "weightp=0",
            "8x8dct=0",
            "open-gop=0",
            "force-cfr=1",
            # Hardware-scope lever: disable sub-MB P partitions so this vector
            # exercises P16x16 motion compensation only.
            "partitions=none",
        ]
    )
    cmd = [
        ffmpeg,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={args.w}x{args.h}:rate={args.rate}",
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
        "-g",
        "12",
        "-bf",
        "0",
        "-refs",
        "1",
        "-x264-params",
        params,
        "-bsf:v",
        "h264_mp4toannexb",
        "-f",
        "h264",
        str(out),
    ]
    subprocess.check_call(cmd)
    data = out.read_bytes()
    print(f"wrote {out} ({len(data)} bytes) NALs types={nal_types(data)} dims={args.w}x{args.h} frames={args.frames}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
