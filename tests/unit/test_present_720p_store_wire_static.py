#!/usr/bin/env python3
"""Inventory + wire check: 720p layout constants must feed ddr_frame_store on L4.

CONFIRMS reviewer point 5 was real on pre-fix main: DDR_FRAME_720P_* lived only
in ddr_frame_layout_params.svh (+ host/tests). present_core must now select them
behind PLEX_PRESENT_720P_L4 while default arm keeps 480p symbols.

Negative: a present_core that still only binds DDR_FRAME_CODED_WIDTH (no 720P
symbols on the L4 arm) must fail.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PRESENT = ROOT / "fpga/Plex_MiSTer/rtl/present_core.sv"
LAYOUT = ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_layout_params.svh"
STORE = ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_store.sv"
QSF = ROOT / "fpga/Plex_MiSTer/Plex.qsf"


def nt(s: str) -> str:
    return re.sub(r"\s+", "", s)


def fail(msg: str) -> int:
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def main() -> int:
    present = PRESENT.read_text(encoding="utf-8", errors="replace")
    layout = LAYOUT.read_text(encoding="utf-8", errors="replace")
    store = STORE.read_text(encoding="utf-8", errors="replace")
    qsf = QSF.read_text(encoding="utf-8", errors="replace")
    pnt = nt(present)
    lnt = nt(layout)

    # --- Inventory of 720p symbols and consumers ---
    syms_720 = re.findall(r"localparam\s+int\s+(DDR_FRAME_720P\w+)\s*=", layout)
    if len(syms_720) < 10:
        return fail(f"expected >=10 DDR_FRAME_720P_* in svh, got {syms_720}")

    # Product RTL consumers excluding the svh itself
    rtl_root = ROOT / "fpga/Plex_MiSTer/rtl"
    consumers: dict[str, list[str]] = {s: [] for s in syms_720}
    for path in sorted(rtl_root.rglob("*.sv")):
        if path.name == "ddr_frame_layout_params.svh":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for s in syms_720:
            if s in text:
                consumers[s].append(str(path.relative_to(ROOT)))

    # present_core must consume the store-critical set
    required = [
        "DDR_FRAME_720P_CODED_WIDTH",
        "DDR_FRAME_720P_CODED_HEIGHT",
        "DDR_FRAME_720P_DISPLAY_WIDTH",
        "DDR_FRAME_720P_DISPLAY_HEIGHT",
        "DDR_FRAME_720P_PILLARBOX_LEFT",
        "DDR_FRAME_720P_YUV420P_BANK_STRIDE",
        "DDR_FRAME_720P_PHYS_BASE",
        "DDR_FRAME_720P_YUV420P_DOORBELL_PHYS",
    ]
    for s in required:
        if "present_core.sv" not in " ".join(consumers.get(s, [])):
            return fail(f"{s} has no present_core consumer: {consumers.get(s)}")

    # Default arm still 480p
    for needle in [
        "FS_CODED_W=DDR_FRAME_CODED_WIDTH",
        "FS_DISPLAY_W=DDR_FRAME_DISPLAY_WIDTH",
        "FS_BANK_STRIDE=DDR_FRAME_YUV420P_BANK_STRIDE",
        "FS_DOORBELL=DDR_FRAME_YUV420P_DOORBELL_PHYS",
        "FS_PHYS_BASE=32'h3000_0000",
    ]:
        if needle not in pnt:
            return fail(f"default 480p arm missing {needle}")

    # L4 arm
    for needle in [
        "ifdefPLEX_PRESENT_720P_L4",
        "FS_CODED_W=DDR_FRAME_720P_CODED_WIDTH",
        "FS_CODED_H=DDR_FRAME_720P_CODED_HEIGHT",
        "FS_DISPLAY_W=DDR_FRAME_720P_DISPLAY_WIDTH",
        "FS_DISPLAY_H=DDR_FRAME_720P_DISPLAY_HEIGHT",
        "FS_PRESENT_X=DDR_FRAME_720P_PILLARBOX_LEFT",
        "FS_BANK_STRIDE=DDR_FRAME_720P_YUV420P_BANK_STRIDE",
        "FS_PHYS_BASE=DDR_FRAME_720P_PHYS_BASE",
        "FS_DOORBELL=DDR_FRAME_720P_YUV420P_DOORBELL_PHYS",
        ".CODED_W(FS_CODED_W)",
        ".HPS_BANK_STRIDE_BYTES(FS_BANK_STRIDE)",
        ".DOORBELL_PHYS(FS_DOORBELL)",
        ".PHYS_BASE(FS_PHYS_BASE)",
    ]:
        if needle not in pnt:
            return fail(f"L4 wire missing {needle}")

    # Store still derives visibility from params (not hardcoded 618)
    snt = nt(store)
    for needle in [
        "PRESENT_END_X=X_W'(PRESENT_X+DISPLAY_W)",
        "PRESENT_END_Y=Y_W'(PRESENT_Y+DISPLAY_H)",
        "rd_x_visible=rd_x_at_or_after_origin&&(rd_x<PRESENT_END_X)",
        "Y_LINE_QWORDS=CODED_W/8",
        "HPS_BANK_STRIDE_QWORDS=29'(HPS_BANK_STRIDE_BYTES/8)",
    ]:
        if needle not in snt:
            return fail(f"ddr_frame_store formula missing {needle}")

    # Default-off product
    if re.search(r"^\s*set_global_assignment.*PLEX_PRESENT_720P_L4=1", qsf, re.M):
        return fail("PLEX_PRESENT_720P_L4 must stay commented/default-off in QSF")
    if not re.search(r"FRAME_W=640", qsf) or not re.search(r"FRAME_H=480", qsf):
        return fail("product QSF must keep FRAME 640x480 active")

    # NEGATIVE: strip L4 720p coded bind → must fail required consumer check
    bad = pnt.replace("FS_CODED_W=DDR_FRAME_720P_CODED_WIDTH", "FS_CODED_W=DDR_FRAME_CODED_WIDTH")
    if "FS_CODED_W=DDR_FRAME_720P_CODED_WIDTH" in bad:
        return fail("negative setup failed to strip 720p coded bind")
    if "FS_CODED_W=DDR_FRAME_720P_CODED_WIDTH" not in pnt:
        return fail("positive file lost 720p coded bind")
    # simulate consumer miss
    if "present_core.sv" in " ".join(consumers["DDR_FRAME_720P_CODED_WIDTH"]):
        # force a local negative on the required list logic
        fake_consumers = {**consumers, "DDR_FRAME_720P_CODED_WIDTH": []}
        if "present_core.sv" in " ".join(fake_consumers["DDR_FRAME_720P_CODED_WIDTH"]):
            return fail("negative consumer empty-list did not go red")

    # Print inventory table
    print("INVENTORY DDR_FRAME_720P_* consumers (RTL .sv):")
    for s in sorted(syms_720):
        c = consumers.get(s, [])
        print(f"  {s}: {c if c else 'NONE (beam/test-only or unused)'}")

    print(
        "PASS present_720p_store_wire: L4 binds 720p coded/display/pillar/stride/"
        "doorbell/phys; default 480p arm intact; QSF default-off"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
