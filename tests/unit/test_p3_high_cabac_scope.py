#!/usr/bin/env python3
"""Scope constants for the measured PMS High/CABAC/B 480p stream.

This is not a decoder. It keeps the load-bearing geometry, DPB, memory, and
CABAC throughput numbers executable so later docs do not drift silently.
"""

from __future__ import annotations

import os
import sys


CODED_W = 624
DISPLAY_W = 618
CODED_H = 480
FPS = 25
MAX_REFS = 4
VIDEO_KBPS = 1344.3
CLK_SYS_HZ = 20_000_000

MB_W = CODED_W // 16
MB_H = CODED_H // 16
MBS_PER_FRAME = MB_W * MB_H
MBS_PER_SECOND = MBS_PER_FRAME * FPS
YUV420_FRAME_BYTES = CODED_W * CODED_H * 3 // 2
RGB565_FRAME_BYTES = CODED_W * CODED_H * 2
DPB_REF_BYTES = MAX_REFS * YUV420_FRAME_BYTES
REF_PLUS_CURRENT_BYTES = DPB_REF_BYTES + YUV420_FRAME_BYTES
REF_CURRENT_PRESENT_YUV_BYTES = REF_PLUS_CURRENT_BYTES + YUV420_FRAME_BYTES

# CABAC bin demand cannot be read from the compressed byte rate directly. The
# bitrate gives a hard lower bound; the planning/stress figures model low-
# bitrate inter content at ~6.5 and ~13 bins/coded-bit respectively.
LOWER_BOUND_BINS_PER_SEC = VIDEO_KBPS * 1000.0
PLANNING_BINS_PER_MB = 300
STRESS_BINS_PER_MB = 600
PLANNING_BINS_PER_SEC = MBS_PER_SECOND * PLANNING_BINS_PER_MB
STRESS_BINS_PER_SEC = MBS_PER_SECOND * STRESS_BINS_PER_MB


def fail(msg: str) -> int:
    print(f"FAIL p3_high_cabac_scope: {msg}")
    return 1


def main() -> int:
    refs = MAX_REFS
    if os.environ.get("MPLEX_P3_HIGH_SCOPE_PERTURB") == "refs":
        refs += 1
    if refs != 4:
        return fail(f"max_num_ref_frames drift refs={refs} want=4")
    if (MB_W, MB_H, MBS_PER_FRAME) != (39, 30, 1170):
        return fail(f"geometry coded={CODED_W}x{CODED_H} mb={MB_W}x{MB_H}={MBS_PER_FRAME}")
    if DPB_REF_BYTES != 1_797_120 or REF_PLUS_CURRENT_BYTES != 2_246_400:
        return fail(f"dpb bytes refs={DPB_REF_BYTES} ref_plus_current={REF_PLUS_CURRENT_BYTES}")
    if PLANNING_BINS_PER_SEC != 8_775_000 or STRESS_BINS_PER_SEC != 17_550_000:
        return fail(
            f"cabac bins planning={PLANNING_BINS_PER_SEC} stress={STRESS_BINS_PER_SEC}"
        )

    print(
        "test_p3_high_cabac_scope: OK "
        f"coded={CODED_W}x{CODED_H} display={DISPLAY_W}x{CODED_H} "
        f"mb={MBS_PER_FRAME} fps={FPS} mb_per_s={MBS_PER_SECOND} refs={MAX_REFS} "
        f"yuv420_frame={YUV420_FRAME_BYTES} rgb565_frame={RGB565_FRAME_BYTES} "
        f"dpb_refs={DPB_REF_BYTES} ref_plus_current={REF_PLUS_CURRENT_BYTES} "
        f"ref_current_present_yuv={REF_CURRENT_PRESENT_YUV_BYTES} "
        f"cabac_lower={LOWER_BOUND_BINS_PER_SEC / 1_000_000:.3f}Mbin/s "
        f"cabac_plan={PLANNING_BINS_PER_SEC / 1_000_000:.3f}Mbin/s "
        f"cabac_stress={STRESS_BINS_PER_SEC / 1_000_000:.3f}Mbin/s "
        f"clk20_plan_cycles_per_bin={CLK_SYS_HZ / PLANNING_BINS_PER_SEC:.2f} "
        f"clk20_stress_cycles_per_bin={CLK_SYS_HZ / STRESS_BINS_PER_SEC:.2f}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
