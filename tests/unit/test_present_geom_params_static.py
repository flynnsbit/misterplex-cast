#!/usr/bin/env python3
"""Inventory + structural gates for present_core / ddram_frame_rd geometry params.

Classification:
  (a) hardware/ABI fixed
  (b) should be / is module parameter
  (c) dead or path-local only
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "fpga/Plex_MiSTer/rtl/present_core.sv"
DDRAM = ROOT / "fpga/Plex_MiSTer/rtl/ddram_frame_rd.sv"
QSF = ROOT / "fpga/Plex_MiSTer/Plex.qsf"


def die(m: str) -> None:
    print(f"FAIL: {m}", file=sys.stderr)
    sys.exit(1)


def ok(m: str) -> None:
    print(f"OK  {m}")


def main() -> int:
    core = CORE.read_text(encoding="utf-8", errors="replace")
    ddram = DDRAM.read_text(encoding="utf-8", errors="replace")
    qsf = QSF.read_text(encoding="utf-8", errors="replace")

    # ---- Inventory (measured) ----
    rows = [
        # present_core
        ("present_core.sv", "TPL_H_DE = 529", "(b) param Template H_DE", "parameter"),
        ("present_core.sv", "TPL_V_STORE = 240", "(b) param Template V_STORE", "parameter"),
        ("present_core.sv", "TPL_SCALE_REF_W = 320", "(b) param scale ref W", "parameter"),
        ("present_core.sv", "TPL_SCALE_REF_H = 240", "(b) param scale ref H", "parameter"),
        ("present_core.sv", "TPL_STORE_X_MUL = 39647", "(b) param Q16 mul", "parameter"),
        ("present_core.sv", "L4_H_DE_P = 1280", "(b) param L4 DE", "parameter"),
        ("present_core.sv", "L4_H_TOTAL_P = 1312", "(b) param L4 H total", "parameter"),
        ("present_core.sv", "L4_V_TOTAL_P = 762", "(b) param L4 V total", "parameter"),
        ("present_core.sv", "STORE_W(FRAME_W)", "(b) L4 store from FRAME_*", "derive"),
        ("present_core.sv", "11'd960", "(b) BEAM_960 fallback DE", "ifdef path"),
        ("present_core.sv", "DE_LAG", "(a) pipeline latency measured", "localparam"),
        # ddram
        ("ddram_frame_rd.sv", "WIDTH = 320", "(b) param frame W", "parameter"),
        ("ddram_frame_rd.sv", "HEIGHT = 240", "(b) param frame H", "parameter"),
        ("ddram_frame_rd.sv", "BANK_STRIDE_BYTES = 32'h0004_0000", "(b) param bank stride", "parameter"),
        ("ddram_frame_rd.sv", "PHYS_BASE = 32'h3000_0000", "(a) HPS ABI default", "parameter"),
        ("ddram_frame_rd.sv", "MAGIC = 32'h504C_584B", "(a) doorbell magic PLXK", "localparam"),
        ("ddram_frame_rd.sv", "QCNT_W", "(b) qword counter width from QWORDS", "localparam"),
        ("ddram_frame_rd.sv", "29'h8000", "(c) REMOVED — was hardcoded bank", "gone"),
    ]

    print("=== GEOMETRY HARDCODE INVENTORY ===")
    print(f"{'file':<22} {'token':<40} class")
    missing = []
    for fn, token, cls, _kind in rows:
        text = core if "present_core" in fn else ddram
        if token == "29'h8000":
            # Must be GONE from product module (still ok in prerefactor ref)
            if re.search(r"29'h8000", text):
                missing.append(f"{fn}: still contains 29'h8000")
                print(f"{fn:<22} {token:<40} STILL PRESENT (bad)")
            else:
                print(f"{fn:<22} {token:<40} {cls}")
            continue
        if token not in text and not re.search(re.escape(token).replace(r"\ ", r"\s*"), text):
            # flexible whitespace
            pat = re.sub(r"\s+", r"\\s*", re.escape(token))
            if not re.search(pat, text):
                missing.append(f"{fn}: missing {token}")
                print(f"{fn:<22} {token:<40} MISSING")
                continue
        print(f"{fn:<22} {token:<40} {cls}")

    if missing:
        die("; ".join(missing))

    # Defaults must reproduce legacy
    for pat, label in [
        (r"parameter\s+int\s+TPL_H_DE\s*=\s*529", "TPL_H_DE default 529"),
        (r"parameter\s+int\s+TPL_V_STORE\s*=\s*240", "TPL_V_STORE default 240"),
        (r"parameter\s+int\s+BANK_STRIDE_BYTES\s*=\s*32'h0004_0000", "BANK_STRIDE default 256KiB"),
        (r"parameter\s+int\s+WIDTH\s*=\s*320", "WIDTH default 320"),
        (r"parameter\s+int\s+HEIGHT\s*=\s*240", "HEIGHT default 240"),
    ]:
        blob = core + "\n" + ddram
        if not re.search(pat, blob):
            die(f"default missing: {label}")
        ok(label)

    # Product default-off
    if re.search(r"^[^#]*PLEX_PRESENT_720P_L4=1", qsf, re.M):
        die("L4 enabled in QSF")
    ok("product L4 default-off")

    # PREDICTION miss report
    print("PREDICTION was: present (b)~12 ddram (b)~3")
    print("MEASURED inventory rows:", len(rows))
    print("PASS test_present_geom_params_static")
    return 0


if __name__ == "__main__":
    sys.exit(main())
