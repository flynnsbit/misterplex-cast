#!/usr/bin/env python3
"""Generate A/V sync blip fixtures (flash + beep every 1.0 s, file-aligned).

product-class: 320x240 @ 24/30/60 (MiSTer DECODE path)
trekmatch:     1080p24 ~8 Mbps (TNG Blu-ray-class source) + 320x240@24 weak twin

Visual: white full-frame flash (~2 frames), frame counter, FLASH label, mouth bar.
Audio: 1 kHz beep 50 ms at each integer second.
"""
from __future__ import annotations

import argparse
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT = "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"


def write_pcm(path: Path, duration_s: float, sample_rate: int = 48000) -> None:
    n = int(duration_s * sample_rate)
    with path.open("wb") as f:
        for i in range(n):
            t = i / sample_rate
            sec = int(t)
            in_beep = (t - sec) < 0.050 and sec < int(duration_s)
            if in_beep:
                v = int(0.9 * 32767 * math.sin(2 * math.pi * 1000.0 * t))
            else:
                v = 0
            f.write(struct.pack("<hh", v, v))


def gen_one(
    out: Path,
    *,
    width: int,
    height: int,
    fps: str | int | float,
    duration_s: float,
    vbitrate: str,
    audio_bitrate: str,
    label: str,
) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    # fps may be a rational string ("24000/1001") so fixtures can carry a real NTSC
    # film rate. Integer-only fixtures are exactly why the 23.976 pacing bug hid:
    # a 24.000 fps blip makes a hardcoded fps=24 assumption true.
    fps_str = str(fps)
    if "/" in fps_str:
        num, den = fps_str.split("/", 1)
        fps_val = float(num) / float(den)
    else:
        fps_val = float(fps_str)
    flash_s = 2.0 / fps_val
    fs = max(14, height // 18)
    fs_flash = max(24, height // 10)
    font = FONT if Path(FONT).is_file() else "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"

    with tempfile.TemporaryDirectory(prefix="avsync_") as td:
        pcm = Path(td) / "a.s16"
        write_pcm(pcm, duration_s, 48000)

        # Single filter_complex; enable= for flash/mouth windows.
        fc = (
            f"[0:v]"
            f"drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:"
            f"enable='lt(mod(t\\,1)\\,{flash_s:.6f})',"
            f"drawbox=x=(iw-iw/4)/2:y=ih*3/4:w=iw/4:h=ih/10:color=red:t=fill:"
            f"enable='lt(mod(t\\,1)\\,0.05)',"
            f"drawtext=fontfile={font}:text='{label} n=%{{n}}':"
            f"x=8:y=8:fontsize={fs}:fontcolor=yellow:borderw=2:bordercolor=black,"
            f"drawtext=fontfile={font}:text='FLASH':x=(w-tw)/2:y=h/3:"
            f"fontsize={fs_flash}:fontcolor=black:"
            f"enable='lt(mod(t\\,1)\\,{flash_s:.6f})',"
            f"format=yuv420p[v]"
        )

        cmd = [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            f"color=c=black:s={width}x{height}:r={fps_str}:d={duration_s}",
            "-f",
            "s16le",
            "-ar",
            "48000",
            "-ac",
            "2",
            "-i",
            str(pcm),
            "-filter_complex",
            fc,
            "-map",
            "[v]",
            "-map",
            "1:a:0",
            "-c:v",
            "libx264",
            "-profile:v",
            "baseline",
            "-pix_fmt",
            "yuv420p",
            "-b:v",
            vbitrate,
            "-maxrate",
            vbitrate,
            "-bufsize",
            str(int(vbitrate.rstrip("kK")) * 2) + "k" if vbitrate[-1] in "kK" else vbitrate,
            "-r",
            fps_str,
            "-g",
            str(int(round(fps_val * 2))),
            "-c:a",
            "aac",
            "-b:a",
            audio_bitrate,
            "-ar",
            "48000",
            "-ac",
            "2",
            "-shortest",
            "-movflags",
            "+faststart",
            str(out),
        ]
        print("GEN", out.name, f"{width}x{height}@{fps_str}", vbitrate, flush=True)
        subprocess.check_call(cmd)
        print("OK", out, out.stat().st_size, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", type=Path, default=ROOT / "assets" / "avsync")
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument(
        "--only",
        choices=("all", "product", "trekmatch", "24", "30", "60", "2397"),
        default="all",
    )
    args = ap.parse_args()
    d = args.duration
    od = args.out_dir
    jobs = []
    if args.only in ("all", "product", "24"):
        jobs.append(
            dict(
                out=od / "sync_24fps_blip.mp4",
                width=320,
                height=240,
                fps=24,
                duration_s=d,
                vbitrate="500k",
                audio_bitrate="96k",
                label="PLEX24",
            )
        )
    if args.only in ("all", "30"):
        jobs.append(
            dict(
                out=od / "sync_30fps_blip.mp4",
                width=320,
                height=240,
                fps=30,
                duration_s=d,
                vbitrate="500k",
                audio_bitrate="96k",
                label="PLEX30",
            )
        )
    if args.only in ("all", "60"):
        jobs.append(
            dict(
                out=od / "sync_60fps_blip.mp4",
                width=320,
                height=240,
                fps=60,
                duration_s=d,
                vbitrate="800k",
                audio_bitrate="96k",
                label="PLEX60",
            )
        )
    if args.only in ("all", "trekmatch"):
        jobs.append(
            dict(
                out=od / "sync_trekmatch_1080p24_blip.mp4",
                width=1920,
                height=1080,
                fps=24,
                duration_s=d,
                vbitrate="8000k",
                audio_bitrate="192k",
                label="TREK24",
            )
        )
        jobs.append(
            dict(
                out=od / "sync_trekmatch_320x240_24_blip.mp4",
                width=320,
                height=240,
                fps=24,
                duration_s=d,
                vbitrate="1500k",
                audio_bitrate="128k",
                label="TREK24p",
            )
        )
    if args.only in ("all", "2397"):
        # Real NTSC film rate (24000/1001 = 23.976). Drift against a hardcoded fps=24
        # is ~1 ms/s, so this fixture only tells the truth over a long run — keep it
        # several minutes long and measure the SLOPE, not a single 12 s window.
        jobs.append(
            dict(
                out=od / "sync_2397fps_blip.mp4",
                width=320,
                height=240,
                fps="24000/1001",
                duration_s=max(d, 360.0),
                vbitrate="1500k",
                audio_bitrate="128k",
                label="NTSC2397",
            )
        )
    for j in jobs:
        gen_one(**j)
    return 0


if __name__ == "__main__":
    sys.exit(main())
