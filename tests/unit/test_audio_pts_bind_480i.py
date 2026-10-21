#!/usr/bin/env python3
"""480i audio PTS even-field bind pin (C611 / review-259).

Fails on cc88b90f34: pacer is still frameContentMs(frameIndex, fpsNum, fpsDen),
the log heuristic is rawH==480 && fpsNum==60 && fpsDen==1, and the log flag is
process-lifetime static. Passes when media_player.cpp binds 2*F-1 at 2*N for
NTSC 29.97/30 frame rates and logs once per threadMain start.
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA = ROOT / "arm" / "misterplexd" / "media_player.cpp"


def fail(msg: str) -> None:
    print(f"FAIL audio_pts_bind_480i: {msg}", file=sys.stderr)
    sys.exit(1)


def frame_content_ms(frame_index: int, num: int, den: int) -> int:
    return (frame_index * 1000 * den) // num


def even_field_ms(frame_index: int, num: int, den: int) -> int:
    is_frame = (num == 30000 and den == 1001) or (num == 30 and den == 1)
    bind_num = (num * 2) if is_frame else num
    field_index = (frame_index * 2 - 1) if is_frame else frame_index
    return frame_content_ms(field_index, bind_num, den)


# Independent math: 30 fps frame 1 is 16 ms on the even-field tick, 33 ms on
# the progressive clock. Doubling both index and rate is a no-op.
if frame_content_ms(1, 30, 1) != 33:
    fail("progressive 30/1 frame 1 must be 33 ms")
if even_field_ms(1, 30, 1) != 16:
    fail("480i 30/1 frame 1 even-field tick must be 16 ms")
if even_field_ms(1, 30, 1) == frame_content_ms(1, 30, 1):
    fail("even-field tick must differ from progressive frame clock at 30/1")
if frame_content_ms(1 * 2, 30 * 2, 1) != frame_content_ms(1, 30, 1):
    fail("fieldIndex=2*F with bindNum=2*N is a no-op; do not use it as the bind")
if even_field_ms(1, 30000, 1001) != 16:
    fail("480i 30000/1001 frame 1 even-field tick must be 16 ms")
if frame_content_ms(1, 30000, 1001) != 33:
    fail("progressive 30000/1001 frame 1 must be 33 ms")
if even_field_ms(1, 60000, 1001) != frame_content_ms(1, 60000, 1001):
    fail("60000/1001 is already field rate; do not double")
if even_field_ms(1, 24, 1) != frame_content_ms(1, 24, 1):
    fail("24/1 film must not take the 480i field shift")
if even_field_ms(1, 24000, 1001) != frame_content_ms(1, 24000, 1001):
    fail("24000/1001 film must not take the 480i field shift")
if even_field_ms(1, 60, 1) != frame_content_ms(1, 60, 1):
    fail("60/1 is 480p60/720p60, not 480i")

p = MEDIA.read_text(encoding="utf-8")
need = [
    "is480iFrameRate ? (frameIndex * 2 - 1) : frameIndex",
    "is480iFrameRate ? (fpsNum * 2) : fpsNum",
    "fpsNum == 30000 && fpsDen == 1001",
    "fpsNum == 60000 && fpsDen == 1001",
    "fpsNum == 30 && fpsDen == 1",
    "frameIndex * 2 - 1",
    "frameContentMs(fieldIndex, bindNum, bindDen)",
    "bool audioBindLogged480i = false;",
    "media: 480i audio PTS bind field_clock=",
    "field_clock=",
    "fieldIndex=",
    "avOffsetMs=",
]
for tok in need:
    if tok not in p:
        fail(f"missing {tok!r}")

forbid = [
    "rawH == 480 && fpsNum == 60",
    "static std::atomic<bool> audioBindLogged480i",
    "fpsNum == 60 && fpsDen == 1",
    "is480iFrameRate ? (frameIndex * 2) : frameIndex",
    "seekMs(0)",
]
for tok in forbid:
    if tok in p:
        fail(f"forbidden {tok!r}")

print(
    "test_audio_pts_bind_480i: OK even_field_30fps_f1_ms=16 "
    "progressive_30fps_f1_ms=33 no_op_2F_2N=33"
)
