#!/usr/bin/env python3
"""Measure A/V lipsync on the MiSTer over HDMI capture (flash <-> beep).

Casts a blip fixture to the MiSTer companion (the same playMedia call Plex Web
makes), captures HDMI video + audio from the USB grabber, and reports the offset
between each white flash and its 1 kHz beep.

The important mode is --drift: it samples TWO windows in a single playback (early
and late) and reports the SLOPE in ms/min. A single short window cannot see a
frame-rate pacing error -- 23.976 content paced at 24 fps drifts ~1 ms/s, which is
~12 ms across a 12 s window (invisible) but ~234 ms by 3:54.

offset_ms = (t_beep - t_flash) * 1000
  negative -> audio is EARLY (heard before the flash)
  positive -> audio is LATE  (heard after the flash; "audio behind the lips")
"""
from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

VIDEO_DEV = "/dev/video4"
# PipeWire/Pulse source (matches the harness the -60 ms RK10 baseline was measured
# with). Capturing ALSA hw: directly adds a large, silent A/V mux skew.
PULSE_SRC = "alsa_input.usb-MACROSILICON_2109-02.analog-stereo"
COMPANION = os.environ.get("MISTERPLEX_PLAYER", "")
PMS = os.environ.get("PLEX_BASE", "http://YOUR-PLEX-SERVER:32400")
PMS_HOST = os.environ.get("PMS_HOST", "YOUR-PLEX-SERVER")
PMS_MACHINE_ID = os.environ.get("PMS_MACHINE_ID", "server-mid")


def sh(cmd: list[str], timeout: int = 60) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.stdout


def timeline() -> dict:
    body = sh(["curl", "-sS", "-m", "5", f"{COMPANION}/player/timeline/poll?wait=0"], 10)
    out = {}
    for k in ("state", "time", "duration", "key"):
        m = re.search(rf'{k}="([^"]*)"', body)
        if m:
            out[k] = m.group(1)
    return out


def cast(rating_key: str, token: str, offset_ms: int = 0) -> None:
    """Issue the same playMedia the Plex Web UI sends to the companion."""
    url = (
        f"{COMPANION}/player/playback/playMedia"
        f"?key=/library/metadata/{rating_key}"
        f"&offset={offset_ms}"
        f"&machineIdentifier={PMS_MACHINE_ID}"
        f"&address={PMS_HOST}&port=32400&protocol=http"
        f"&X-Plex-Token={token}"
        f"&X-Plex-Client-Identifier=avsync-harness"
        f"&commandID=1"
    )
    sh(["curl", "-sS", "-m", "20", url], 30)


def stop() -> None:
    sh(["curl", "-sS", "-m", "5", f"{COMPANION}/player/playback/stop?commandID=99"], 10)


# Grabber modes. 720p60 halves the flash-timing quantisation, which is the
# limiting error when fitting a video rate (a 1-frame flash sampled at 30 fps
# carries +-33 ms of jitter).
CAP_SIZE = "1280x720"
CAP_FPS = "60"


def capture(dest: Path, seconds: float) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-thread_queue_size", "1024",
        "-f", "v4l2", "-input_format", "mjpeg", "-video_size", CAP_SIZE,
        "-framerate", CAP_FPS, "-i", VIDEO_DEV,
        "-thread_queue_size", "1024",
        "-f", "pulse", "-ac", "2", "-ar", "48000", "-i", PULSE_SRC,
        "-map", "0:v", "-map", "1:a", "-t", f"{seconds:.2f}",
        "-c:v", "mjpeg", "-q:v", "5", "-c:a", "pcm_s16le",
        str(dest),
    ]
    subprocess.run(cmd, capture_output=True, text=True, timeout=int(seconds) + 60)


def video_luma(cap: Path) -> tuple[np.ndarray, np.ndarray]:
    """Per-frame mean luma and presentation timestamps."""
    w, h = 32, 18
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(cap),
         "-map", "0:v", "-vf", f"scale={w}:{h}", "-pix_fmt", "gray",
         "-f", "rawvideo", "pipe:1"],
        capture_output=True, timeout=300,
    ).stdout
    frames = np.frombuffer(raw, dtype=np.uint8)
    n = frames.size // (w * h)
    luma = frames[: n * w * h].reshape(n, w * h).mean(axis=1)

    times = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "frame=pts_time", "-of", "csv=p=0", str(cap)],
        capture_output=True, text=True, timeout=300,
    ).stdout.split()
    t = np.array([float(x.rstrip(",")) for x in times if x.strip().rstrip(",")][:n])
    if t.size < n:  # fall back to nominal grabber rate
        t = np.arange(n) / float(CAP_FPS)
    return luma, t


def audio_env(cap: Path) -> tuple[np.ndarray, int]:
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(cap),
         "-map", "0:a", "-ac", "1", "-ar", "48000", "-f", "s16le", "pipe:1"],
        capture_output=True, timeout=300,
    ).stdout
    a = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    return np.abs(a), 48000


def rising_edges(sig: np.ndarray, t: np.ndarray, lo_q=20, hi_q=99.0, frac=0.5) -> list[float]:
    """Times of low->high transitions of a pulsed signal."""
    if sig.size == 0:
        return []
    floor = np.percentile(sig, lo_q)
    peak = np.percentile(sig, hi_q)
    if peak - floor < 1e-6:
        return []
    thr = floor + frac * (peak - floor)
    hot = sig > thr
    out = []
    for i in range(1, hot.size):
        if hot[i] and not hot[i - 1]:
            out.append(float(t[i]))
    return out


def beep_times(env: np.ndarray, sr: int) -> list[float]:
    # 1 ms RMS-ish envelope, then rising edges
    win = sr // 1000
    n = env.size // win
    if n == 0:
        return []
    e = env[: n * win].reshape(n, win).mean(axis=1)
    t = np.arange(n) / 1000.0
    return rising_edges(e, t, lo_q=20, hi_q=99.5, frac=0.35)


def match(flashes: list[float], beeps: list[float], window=0.5) -> list[float]:
    """Pair each flash with the nearest beep and return offsets in ms."""
    offs = []
    for f in flashes:
        if not beeps:
            break
        b = min(beeps, key=lambda x: abs(x - f))
        if abs(b - f) <= window:
            offs.append(round((b - f) * 1000.0, 1))
    return offs


def measure_window(cap: Path) -> dict:
    luma, t = video_luma(cap)
    env, sr = audio_env(cap)
    flashes = rising_edges(luma, t)
    beeps = beep_times(env, sr)
    offs = match(flashes, beeps)
    d = {
        "capture": str(cap),
        "flashes": len(flashes),
        "beeps": len(beeps),
        "matches": len(offs),
        "offsets_ms": offs,
        "flash_t": [round(x, 3) for x in flashes[:40]],
        "beep_t": [round(x, 3) for x in beeps[:40]],
    }
    if offs:
        med = statistics.median(offs)
        d.update(
            median_offset_ms=med,
            mad_ms=round(statistics.median([abs(o - med) for o in offs]), 2),
            mean_offset_ms=round(statistics.fmean(offs), 2),
            min_ms=min(offs),
            max_ms=max(offs),
            abs_median_le_42ms=abs(med) <= 42.0,
        )
    return d


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rating-key", default="11", help="PMS ratingKey to cast")
    ap.add_argument("--token", default=os.environ.get("PLEX_TOKEN", ""),
                    help="Plex auth token (default: $PLEX_TOKEN)")
    ap.add_argument("--out", type=Path, required=True, help="output dir")
    ap.add_argument("--label", default="run")
    ap.add_argument("--window", type=float, default=15.0, help="capture seconds")
    ap.add_argument("--drift", action="store_true",
                    help="two-point measurement -> slope ms/min")
    ap.add_argument("--early-at", type=float, default=30.0,
                    help="playback seconds at which to take the early sample")
    ap.add_argument("--late-at", type=float, default=300.0,
                    help="playback seconds at which to take the late sample")
    ap.add_argument("--no-cast", action="store_true", help="assume already playing")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    result: dict = {"label": args.label, "rating_key": args.rating_key}

    if not args.no_cast:
        stop()
        time.sleep(2)
        print(f"cast RK{args.rating_key} ...", flush=True)
        cast(args.rating_key, args.token)

    # Wait for playback to actually start
    t_start = time.time()
    for _ in range(60):
        tl = timeline()
        if tl.get("state") == "playing" and int(tl.get("time", "0") or 0) > 0:
            t_start = time.time() - int(tl["time"]) / 1000.0
            break
        time.sleep(1)
    else:
        print("WARN: never saw state=playing", flush=True)
    result["timeline_at_start"] = timeline()

    points = [("early", args.early_at)]
    if args.drift:
        points.append(("late", args.late_at))

    for name, at in points:
        target = t_start + at
        wait = target - time.time()
        if wait > 0:
            print(f"waiting {wait:.1f}s for {name} window (playback t={at:.0f}s)", flush=True)
            time.sleep(wait)
        tl = timeline()
        pos_ms = int(tl.get("time", "0") or 0)
        cap = args.out / f"{args.label}_{name}.mkv"
        print(f"capture {name} at playback {pos_ms/1000:.1f}s -> {cap.name}", flush=True)
        capture(cap, args.window)
        w = measure_window(cap)
        w["playback_pos_s"] = round(pos_ms / 1000.0, 2)
        w["state"] = tl.get("state")
        result[name] = w
        print(f"  {name}: matches={w['matches']} "
              f"median={w.get('median_offset_ms')} ms", flush=True)

    if args.drift and "late" in result:
        e, l = result["early"], result["late"]
        if "median_offset_ms" in e and "median_offset_ms" in l:
            dt_min = (l["playback_pos_s"] - e["playback_pos_s"]) / 60.0
            if dt_min > 0:
                slope = (l["median_offset_ms"] - e["median_offset_ms"]) / dt_min
                result["drift_slope_ms_per_min"] = round(slope, 2)
                result["drift_span_min"] = round(dt_min, 2)
                result["gate_G-AV6_slope_le_10"] = abs(slope) <= 10.0
                result["gate_G-AV7_both_le_42"] = (
                    abs(e["median_offset_ms"]) <= 42.0 and abs(l["median_offset_ms"]) <= 42.0
                )

    rep = args.out / f"{args.label}_report.json"
    rep.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("early", "late")}, indent=2))
    for k in ("early", "late"):
        if k in result:
            w = result[k]
            print(f"{k}: pos={w.get('playback_pos_s')}s matches={w['matches']} "
                  f"median={w.get('median_offset_ms')} mad={w.get('mad_ms')} "
                  f"range=[{w.get('min_ms')},{w.get('max_ms')}]")
    print(f"report {rep}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
