#!/usr/bin/env python3
"""Separate the audio clock error from the video clock error on the MiSTer.

The two-point flash<->beep measurement gives a drift SLOPE but cannot say which
side is wrong. The blip fixture has flashes and beeps at exactly 1.000 s spacing,
so a single long HDMI capture lets us fit each cadence independently:

    played_period = slope of (event_index -> event_time)

  audio_ppm > 0  -> MiSTer plays audio SLOWER than realtime (period > 1 s)
  video_ppm > 0  -> MiSTer presents video SLOWER than realtime

Their difference is the lipsync drift, in ms per minute:

    drift_ms_per_min = (audio_period - video_period) * 60000

A constant capture-rig offset cancels completely here, so this is a much stronger
measurement than the absolute flash<->beep offset.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import avsync_measure as av  # noqa: E402


def fit_period(times: list[float]) -> tuple[float, int, float]:
    """Least-squares period of events nominally 1 s apart.

    Uses the round()ed index so a missed detection does not shift the fit.
    """
    if len(times) < 8:
        return (0.0, len(times), 0.0)
    t = np.asarray(times, dtype=float)
    idx = np.round(t - t[0])
    a = np.polyfit(idx, t, 1)
    resid = t - np.polyval(a, idx)
    return (float(a[0]), len(t), float(np.max(np.abs(resid))))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rating-key", default="11")
    ap.add_argument("--token", default=os.environ.get("PLEX_TOKEN", ""),
                    help="Plex auth token (default: $PLEX_TOKEN)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--label", default="rate")
    ap.add_argument("--settle", type=float, default=45.0,
                    help="playback seconds to skip before capturing")
    ap.add_argument("--window", type=float, default=150.0, help="capture seconds")
    ap.add_argument("--no-cast", action="store_true")
    ap.add_argument("--reuse", action="store_true",
                    help="analyse an existing capture instead of grabbing a new one")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if args.reuse:
        args.no_cast = True
    if not args.no_cast:
        print(f"cast RK{args.rating_key} ...")
        av.cast(args.rating_key, args.token, 0)
        deadline = time.time() + 30
        while time.time() < deadline:
            if av.timeline().get("state") == "playing":
                break
            time.sleep(0.5)
        print(f"settling {args.settle}s")
        time.sleep(args.settle)

    cap = args.out / f"{args.label}.mkv"
    if args.reuse:
        print(f"reusing {cap}")
    else:
        print(f"capturing {args.window}s -> {cap.name}")
        av.capture(cap, args.window)

    luma, vt = av.video_luma(cap)
    flashes = av.rising_edges(luma, vt)
    env, sr = av.audio_env(cap)
    beeps = av.beep_times(env, sr)

    # Offset drift measured INSIDE one capture. Two separate captures cannot be
    # compared: each ffmpeg invocation starts the v4l2 and pulse streams with its
    # own pipeline latency, and that per-capture A/V skew (tens of ms) swamps a
    # real drift of a few ms/min.
    pairs = []
    for f in flashes:
        cand = [b for b in beeps if abs(b - f) < 0.5]
        if cand:
            pairs.append((f, (min(cand, key=lambda b: abs(b - f)) - f) * 1000.0))
    off_slope = off_mid = 0.0
    if len(pairs) >= 8:
        ft = np.asarray([p[0] for p in pairs])
        fo = np.asarray([p[1] for p in pairs])
        c = np.polyfit(ft, fo, 1)
        off_slope = float(c[0]) * 60.0
        off_mid = float(np.polyval(c, float(np.median(ft))))

    vper, vn, vres = fit_period(flashes)
    aper, an, ares = fit_period(beeps)
    drift = (aper - vper) * 60000.0 if (vper and aper) else 0.0

    rep = {
        "label": args.label,
        "rating_key": args.rating_key,
        "window_s": args.window,
        "video": {"period_s": round(vper, 6), "events": vn,
                  "ppm": round((vper - 1.0) * 1e6, 1), "max_resid_ms": round(vres * 1000, 1)},
        "audio": {"period_s": round(aper, 6), "events": an,
                  "ppm": round((aper - 1.0) * 1e6, 1), "max_resid_ms": round(ares * 1000, 1)},
        "drift_ms_per_min": round(drift, 2),
        "offset_fit": {"slope_ms_per_min": round(off_slope, 2),
                       "offset_ms_mid": round(off_mid, 1),
                       "pairs": len(pairs),
                       "median_ms": round(statistics.median([p[1] for p in pairs]), 1)
                       if pairs else None},
        "gate_G-AV6_slope_le_10": abs(off_slope) <= 10.0,
    }
    print(json.dumps(rep, indent=2))
    (args.out / f"{args.label}_rate.json").write_text(json.dumps(rep, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
