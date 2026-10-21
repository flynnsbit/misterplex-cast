#!/usr/bin/env python3
"""Gate: 720p present path is ON main tree + in files.qip, default-off in QSF.

Red-before-green: missing QIP entry or enabled PRESENT_* product macro fails.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QIP = ROOT / "fpga/Plex_MiSTer/files.qip"
QSF = ROOT / "fpga/Plex_MiSTer/Plex.qsf"
CORE = ROOT / "fpga/Plex_MiSTer/rtl/present_core.sv"
RTL = ROOT / "fpga/Plex_MiSTer/rtl"

REQUIRED = [
    "present_video_timing_720p.sv",
    "present_video_timing_960.sv",
    "present_beam_content_de.sv",
    "present_beam_ppc.sv",
    "present_npx_path.sv",
    "present_pix_rate_match.sv",
    "present_content_window.sv",
    "present_geom_latch.sv",
    "present_vtotal_bresenham.sv",
    "yuv_bt601_npx.sv",
    "plex_present_geom_mux.sv",
]


def main() -> int:
    qip = QIP.read_text(errors="ignore")
    qsf = QSF.read_text(errors="ignore")
    core = CORE.read_text(errors="ignore")
    fails: list[str] = []

    for name in REQUIRED:
        path = RTL / name
        if not path.is_file():
            fails.append(f"ON_MAIN=no missing file rtl/{name}")
            continue
        if f"rtl/{name}" not in qip:
            fails.append(f"IN_FILES_QIP=no rtl/{name} not listed in files.qip")
        else:
            print(f"OK {name}: ON_MAIN=yes IN_FILES_QIP=yes")

    # Product macros must stay commented / absent as active assignments.
    active = re.findall(
        r'^\s*set_global_assignment\s+-name\s+VERILOG_MACRO\s+"([^"]+)"',
        qsf,
        flags=re.M,
    )
    for m in active:
        if m.startswith("PRESENT_BEAM_960") or m.startswith("PRESENT_MULTI_PIXEL"):
            fails.append(f"DEFAULT_OFF=no active QSF macro {m}")
    if not any("PRESENT_BEAM_960" in line and line.strip().startswith("#") for line in qsf.splitlines()):
        fails.append("QSF missing commented PRESENT_BEAM_960 enable recipe")
    if not any("PRESENT_MULTI_PIXEL" in line and line.strip().startswith("#") for line in qsf.splitlines()):
        fails.append("QSF missing commented PRESENT_MULTI_PIXEL enable recipe")

    if "u_keep_timing_720p" not in core:
        fails.append("present_core missing always-on u_keep_timing_720p hierarchy keep")
    if "u_keep_timing_960" not in core:
        fails.append("present_core missing always-on u_keep_timing_960 hierarchy keep")
    if "`ifdef PRESENT_BEAM_960" not in core:
        fails.append("present_core missing PRESENT_BEAM_960 gate")
    if "`ifdef PRESENT_MULTI_PIXEL" not in core:
        fails.append("present_core missing PRESENT_MULTI_PIXEL gate")
    # Default path still colorbars Template
    if "colorbars bars" not in core:
        fails.append("present_core lost default colorbars bars instance")

    # Negative: pretend QIP drop
    if "present_video_timing_720p.sv" in qip:
        twin = qip.replace(
            "set_global_assignment -name SYSTEMVERILOG_FILE rtl/present_video_timing_720p.sv\n",
            "",
        )
        if "rtl/present_video_timing_720p.sv" in twin:
            fails.append("negative twin could not drop timing_720p line")
        else:
            print("OK negative twin: dropping timing_720p from QIP would fail this gate")

    if fails:
        print("FAIL present_720p_land_static:")
        for f in fails:
            print(" ", f)
        return 1
    print("PASS present_720p_land_static: default-off 720p present path landed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
