#!/usr/bin/env python3
"""Capture or load the MiSTer HDMI output and grade the luma edge-marker frame.

Companion to gen_edge_markers.py. Assumes the marker frame is already on screen.
Captures the grabber in uncompressed YUYV -- MJPEG's chroma subsampling and block
artifacts destroy the 1-pixel edge detail we are measuring -- and reports, for
each axis, where the first and last source column/row actually landed.

The core upscales 320 store columns across 529 display pixels, so a correctly
displayed edge column occupies 1-2 display pixels (roughly 4-7 pixels of a 1920
wide capture). A markedly wider run means that column is being repeated, which is
exactly the "bar" symptom on the right/bottom edge.

Exit code 0 = all four edges correct.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import os
from pathlib import Path

import numpy as np
from PIL import Image

DEFAULT_DEV = "/dev/video4"
DEFAULT_CAP = "build/edge_cap.png"
DEFAULT_SIZE = "1920x1080"
WARMUP = 60

MAX_EDGE_PX = 9  # widest a single source column may legitimately appear
SYNTH_W = 1920
SYNTH_H = 1080
SRC_SCALE_X = 6
SRC_SCALE_Y = 5


class CaptureError(RuntimeError):
    """A capture could not be acquired or validated."""


class StaleCaptureError(CaptureError):
    """The current frame is byte-identical to the baseline frame."""


def load_image(path):
    try:
        return np.array(Image.open(path).convert("RGB")).astype(np.uint8)
    except Exception as e:
        raise CaptureError(f"could not load frame {path}: {e}") from e


def save_image(path, frame):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(frame.astype(np.uint8)).save(path)


def assert_frame_changed(previous, current):
    if previous.shape == current.shape and np.array_equal(previous, current):
        raise StaleCaptureError(
            "STALE capture: current frame is byte-identical to the previous grab; "
            "refusing to grade a possibly buffered/stale frame"
        )


def check_device_ready(dev):
    if not Path(dev).exists():
        raise CaptureError(f"capture device {dev} is absent")
    try:
        r = subprocess.run(["fuser", dev], capture_output=True, text=True, timeout=5)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return
    holders = (r.stdout + r.stderr).strip()
    if r.returncode == 0 and holders:
        raise CaptureError(f"capture device {dev} is busy/exclusive-open ({holders})")


def capture_v4l2_frame(path, dev, video_size, warmup):
    check_device_ready(dev)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-f", "v4l2",
        "-input_format", "yuyv422",
        "-video_size", video_size,
        "-i", dev,
        "-vf", f"select=gte(n\\,{warmup})",
        "-frames:v", "1",
        "-y", str(path),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30 + warmup)
    if r.returncode != 0:
        detail = (r.stderr or r.stdout or "").strip()
        raise CaptureError(
            f"ffmpeg capture failed for {dev} using yuyv422 {video_size}: {detail}"
        )
    if not path.exists() or path.stat().st_size == 0:
        raise CaptureError(f"ffmpeg reported success but produced no image at {path}")
    return load_image(path)


def capture_v4l2(path, dev, video_size, warmup, previous_path=None, capture_only=False):
    path = Path(path)
    previous = None
    if previous_path:
        previous = load_image(previous_path)
    elif path.exists() and not capture_only:
        previous = load_image(path)

    current = capture_v4l2_frame(path, dev, video_size, warmup)
    if capture_only:
        return current
    if previous is None:
        raise CaptureError(
            "no previous frame baseline for stale detection; run once with "
            "--capture-only before changing the screen, or pass --previous"
        )
    assert_frame_changed(previous, current)
    return current


def synthetic_frame(kind="good", width=SYNTH_W, height=SYNTH_H):
    frame = np.zeros((height, width, 3), dtype=np.uint8)
    frame[:, :, :] = 16
    xw = max(1, SRC_SCALE_X)
    yw = max(1, SRC_SCALE_Y)

    if kind == "baseline":
        frame[:, :, :] = 24
        frame[height // 3: 2 * height // 3, width // 3: 2 * width // 3, :] = 90
        return frame

    if kind not in {"good", "hwrap", "vshift"}:
        raise ValueError(f"unknown synthetic frame kind: {kind}")

    # Good frame: first source column/row touches the leading display edge and
    # last source column/row touches the trailing display edge.
    frame[:, :xw, :] = 255
    frame[:, width - xw:, :] = 128
    frame[:yw, :, :] = 255
    frame[height - yw:, :, :] = 128

    if kind == "hwrap":
        frame[:, :, :] = 16
        frame[:, :xw, :] = 128          # last source column wrapped to the left
        frame[:, xw: 2 * xw, :] = 255   # first source column is one source px late
        frame[:, width - xw:, :] = 128
        frame[:yw, :, :] = 255
        frame[height - yw:, :, :] = 128
    elif kind == "vshift":
        frame[:, :, :] = 16
        frame[:, :xw, :] = 255
        frame[:, width - xw:, :] = 128
        frame[yw: 2 * yw, :, :] = 255   # first source row is one source px late
        frame[height - yw:, :, :] = 128
    return frame


def synthetic_capture(case):
    if case == "stale":
        current = synthetic_frame("good")
        return current.copy(), current
    return synthetic_frame("baseline"), synthetic_frame(case)


def file_capture(inputs, previous_path=None):
    if previous_path:
        previous = load_image(previous_path)
        if not inputs:
            raise CaptureError("--source file requires a current --input frame")
        current = load_image(inputs[-1])
    else:
        if len(inputs) < 2:
            raise CaptureError(
                "--source file requires two --input images (previous, current) "
                "so stale capture can be checked"
            )
        previous = load_image(inputs[-2])
        current = load_image(inputs[-1])
    assert_frame_changed(previous, current)
    return current


def find_runs(line, lo, hi, min_len=2):
    """Return [(start, end)] runs of samples inside [lo, hi]."""
    out, start = [], None
    for i, v in enumerate(line):
        if lo <= v <= hi:
            if start is None:
                start = i
        elif start is not None:
            if i - start >= min_len:
                out.append((start, i - 1))
            start = None
    if start is not None and len(line) - start >= min_len:
        out.append((start, len(line) - 1))
    return out


def grade(line, axis):
    n = len(line)
    # The grabber applies limited-range expansion plus some gamma, so match on
    # broad bands rather than exact values: white is simply "very bright", and the
    # grey marker sits well above the near-black body.
    # Vertical scaling (240 -> 1080) interpolates hard enough that the top row can
    # survive as a single bright sample, so a 1-sample white run is a real hit. Only
    # column/row 0 is this bright, and we sample away from the reference bar band.
    whites = find_runs(line, 200, 300, min_len=1)
    greys = find_runs(line, 100, 175, min_len=2)
    problems, info = [], []

    if not whites:
        problems.append(f"{axis} leading edge: first column/row (white) is NOT displayed")
    else:
        s, e = whites[0]
        info.append(f"first@{s}-{e} (w={e-s+1})")
        if s > 3:
            problems.append(
                f"{axis} leading edge: first column/row starts at {s}, expected 0 "
                f"-- {s} pixels of something else are shown before it")
        if e - s + 1 > MAX_EDGE_PX:
            problems.append(
                f"{axis} leading edge: first column/row is {e-s+1}px wide "
                f"(max {MAX_EDGE_PX}) -- it is being repeated")

    if not greys:
        problems.append(f"{axis} trailing edge: last column/row (grey) is NOT displayed")
    else:
        s, e = greys[-1]
        info.append(f"last@{s}-{e} (w={e-s+1})")
        if e < n - 4:
            problems.append(
                f"{axis} trailing edge: last column/row ends at {e}, expected {n-1} "
                f"-- {n-1-e} pixels of something else are shown after it")
        if e - s + 1 > MAX_EDGE_PX:
            problems.append(
                f"{axis} trailing edge: last column/row is {e-s+1}px wide "
                f"(max {MAX_EDGE_PX}) -- it is repeated (this is the edge bar)")

    if whites and greys and greys[0][0] < whites[0][0]:
        problems.append(
            f"{axis} WRAP: the last column/row appears at {greys[0][0]}, before the first")

    return info, problems


def grade_frame(im):
    h, w, _ = im.shape
    lum = im.mean(axis=2)

    allprob = []
    lines = [f"capture {w}x{h}"]
    vx = int(w * 200 / 320)  # away from the reference bar band
    for axis, line in (("H", lum[h // 2, :]), ("V", lum[:, vx])):
        info, problems = grade(line, axis)
        lines.append(f"{axis}: " + "  ".join(info))
        allprob += problems
    return lines, allprob


def parse_inputs(env_value):
    if not env_value:
        return []
    return [x for x in env_value.split(",") if x]


def load_capture(args):
    if args.source == "synthetic":
        previous, current = synthetic_capture(args.synthetic_case)
        assert_frame_changed(previous, current)
        return current
    if args.source == "file":
        return file_capture(args.inputs, args.previous)
    return capture_v4l2(
        args.out, args.device, args.video_size, args.warmup,
        previous_path=args.previous, capture_only=args.capture_only,
    )


def build_arg_parser():
    env_inputs = parse_inputs(os.environ.get("EDGE_INPUT", ""))
    default_source = os.environ.get("EDGE_SOURCE")
    if default_source is None:
        default_source = "file" if env_inputs else "v4l2"

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", choices=("v4l2", "file", "synthetic"),
                    default=default_source,
                    help="frame source (default: EDGE_SOURCE, file if EDGE_INPUT is set, else v4l2)")
    ap.add_argument("--input", dest="inputs", action="append", default=env_inputs,
                    help="file-backed frame; pass previous then current, or use EDGE_INPUT=a,b")
    ap.add_argument("--synthetic-case", choices=("good", "hwrap", "vshift", "stale"),
                    default=os.environ.get("EDGE_SYNTHETIC_CASE", "good"))
    ap.add_argument("--previous", default=os.environ.get("EDGE_PREV"),
                    help="previous frame baseline for stale detection")
    ap.add_argument("--out", default=os.environ.get("EDGE_CAP", DEFAULT_CAP),
                    help="capture output path for v4l2/capture-only")
    ap.add_argument("--device", default=os.environ.get("HDMI_DEV", DEFAULT_DEV))
    ap.add_argument("--video-size", default=os.environ.get("EDGE_VIDEO_SIZE", DEFAULT_SIZE))
    ap.add_argument("--warmup", type=int, default=int(os.environ.get("EDGE_WARMUP", WARMUP)),
                    help="frames to discard before a v4l2 grab")
    ap.add_argument("--capture-only", action="store_true",
                    help="v4l2 only: save a baseline frame and do not grade it")
    return ap


def main(argv=None):
    args = build_arg_parser().parse_args(argv)
    try:
        im = load_capture(args)
    except StaleCaptureError as e:
        print(str(e), file=sys.stderr)
        return 2
    except CaptureError as e:
        print(f"CAPTURE ERROR: {e}", file=sys.stderr)
        return 2

    if args.capture_only:
        print(f"captured baseline {args.out}")
        return 0

    lines, allprob = grade_frame(im.astype(int))
    for line in lines:
        print(line)

    if allprob:
        print("\nFAIL")
        for p in allprob:
            print("  -", p)
        return 1
    print("\nPASS: all four edges correct")
    return 0


if __name__ == "__main__":
    sys.exit(main())
