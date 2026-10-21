#!/usr/bin/env python3
"""L4 gate: dead modules become INSTANTIATED behind default-off PLEX_PRESENT_720P_L4.

Default product (macro off):
  - PLEX_PRESENT_720P_L4 must NOT be an active QSF macro
  - FRAME_W/H stay 640/480
  - present_core still has colorbars Template path text

When scanning sources (ifdef bodies count as instantiation intent):
  - present_geom_latch, plex_present_geom_mux appear in Plex.sv under L4
  - present_content_window appears in present_core under L4
  - Beam totals 1312×762 and DE 1280×720 are named in present_core L4 path
  - Dual-header 720p24 beam constants match

Negative: dropping u_content_window instance name fails the gate.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QSF = ROOT / "fpga/Plex_MiSTer/Plex.qsf"
PLEX = ROOT / "fpga/Plex_MiSTer/Plex.sv"
CORE = ROOT / "fpga/Plex_MiSTer/rtl/present_core.sv"
QIP = ROOT / "fpga/Plex_MiSTer/files.qip"
SVH = ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_layout_params.svh"
HPP = ROOT / "host/libmisterplex/ddr_frame_layout.hpp"


def active_macros(qsf: str) -> list[str]:
    return re.findall(
        r'^\s*set_global_assignment\s+-name\s+VERILOG_MACRO\s+"([^"]+)"',
        qsf,
        flags=re.M,
    )


def select_ifdefs(text: str, defines: set[str]) -> str:
    """Nested `ifdef/`ifndef/`elsif/`else/`endif resolved for `defines`; inactive lines dropped."""
    out, stack, active = [], [], True
    for line in text.splitlines(keepends=True):
        m = re.match(r"\s*`(ifdef|ifndef|elsif|else|endif)\b[ \t]*(\w*)", line)
        if not m:
            if active:
                out.append(line)
            continue
        k, name = m.groups()
        if k in ("ifdef", "ifndef"):
            cond = (name in defines) == (k == "ifdef")
            stack.append([active, cond])
            active = active and cond
        elif k == "elsif":
            parent, taken = stack[-1]
            cond = not taken and name in defines
            stack[-1][1] = taken or cond
            active = parent and cond
        elif k == "else":
            active = stack[-1][0] and not stack[-1][1]
            stack[-1][1] = True
        else:
            active = stack.pop()[0]
    return "".join(out)


# T-RTLINV-STALE A-1 (H264 e9176de5, review 19:34): anchored localparam/constexpr pins under the QSF's
# macros. T-720P24-CLK-DRIFT: the L4 build sets PLEX_CLK_SYS_24, so the L4 clk_sys is 24 MHz
# (pll_0002.v out0) and the beam's pixel clock is that clk_sys (PRESENT_CLK_PIX_PLL off). The
# 20 MHz clk_sys of 2affb815 is the non-L4 default, not this beam's clock.
L4_CLK_HZ = "24_000_000"
BEAM_SVH = (("DDR_FRAME_720P24_BEAM_H_TOTAL", "1312"), ("DDR_FRAME_720P24_BEAM_V_TOTAL", "762"),
            ("DDR_FRAME_720P24_BEAM_H_DE", "1280"), ("DDR_FRAME_720P24_BEAM_V_ACTIVE", "720"),
            ("DDR_FRAME_720P24_CLK_SYS_HZ", L4_CLK_HZ), ("DDR_FRAME_720P24_CLK_PIX_HZ", L4_CLK_HZ))
BEAM_HPP = (("kPlex720p24BeamHTotal", "1312"), ("kPlex720p24BeamVTotal", "762"),
            ("kPlex720p24ClkPixHz", "24000000"))
PLL = ROOT / "fpga/Plex_MiSTer/rtl/pll/pll_0002.v"


def pll_clk_sys24_hz(pll: str) -> list[int]:
    """out0 of every altera_pll under `ifdef PLEX_CLK_SYS_24, in Hz."""
    got = re.findall(r"`ifdef\s+PLEX_CLK_SYS_24\s*\n\s*\.output_clock_frequency0\(\"([0-9.]+) MHz\"\)", pll)
    return [round(float(mhz) * 1_000_000) for mhz in got]


def beam_pin_fails(svh_sel: str, hpp: str, pll: str = "") -> list[str]:
    out = []
    for name, val in BEAM_SVH:
        found = re.findall(rf"^\s*localparam\s+int\s+{name}\s*=\s*([0-9_]+)\s*;", svh_sel, re.M)
        if found != [val]:
            out.append(f"svh {name}: want exactly one '= {val}', found {found}")
    for name, val in BEAM_HPP:
        found = re.findall(rf"^\s*constexpr\s+int\s+{name}\s*=\s*([0-9]+)\s*;", hpp, re.M)
        if found != [val]:
            out.append(f"hpp {name}: want exactly one '= {val}', found {found}")
    if pll:
        hz = pll_clk_sys24_hz(pll)
        if len(hz) < 2 or any(h != int(L4_CLK_HZ) for h in hz):
            out.append(f"pll PLEX_CLK_SYS_24 out0 (both altera_pll) want {L4_CLK_HZ} Hz, found {hz}")
    return out


def main() -> int:
    qsf = QSF.read_text(errors="ignore")
    plex = PLEX.read_text(errors="ignore")
    core = CORE.read_text(errors="ignore")
    qip = QIP.read_text(errors="ignore")
    svh = SVH.read_text(errors="ignore")
    hpp = HPP.read_text(errors="ignore")
    fails: list[str] = []

    act = active_macros(qsf)
    for m in act:
        if m.startswith("PLEX_PRESENT_720P_L4") or m.startswith("FABRIC_NATIVE_720P_GEOM"):
            fails.append(f"DEFAULT_OFF=no active QSF macro {m}")
    if not any("FRAME_W=640" in m for m in act):
        fails.append("DEFAULT product FRAME_W=640 missing from active QSF")
    if not any("FRAME_H=480" in m for m in act):
        fails.append("DEFAULT product FRAME_H=480 missing from active QSF")
    if not any("PLEX_PRESENT_720P_L4" in line and line.strip().startswith("#") for line in qsf.splitlines()):
        fails.append("QSF missing commented PLEX_PRESENT_720P_L4 enable recipe")
    if not any("FABRIC_NATIVE_720P_GEOM" in line and line.strip().startswith("#") for line in qsf.splitlines()):
        fails.append("QSF missing commented FABRIC_NATIVE_720P_GEOM recipe")
    if not any("FRAME_W=1280" in line and line.strip().startswith("#") for line in qsf.splitlines()):
        fails.append("QSF missing commented FRAME_W=1280 L4 recipe")

    # Instantiation intent (ifdef bodies)
    checks = [
        ("present_geom_latch", "u_plxg_latch" in plex and "present_geom_latch" in plex),
        ("plex_present_geom_mux", "u_present_geom_mux" in plex and "plex_present_geom_mux" in plex),
        ("present_content_window", "u_content_window" in core and "present_content_window" in core),
        (
            "beam_720p24",
            "u_beam_720p24" in core
            and (
                # Legacy localparams or parameterized defaults (same 1312×762)
                ("L4_H_TOTAL = 1312" in core and "L4_V_TOTAL = 762" in core)
                or (
                    "L4_H_TOTAL_P = 1312" in core
                    and "L4_V_TOTAL_P = 762" in core
                    and "L4_H_TOTAL = L4_H_TOTAL_P" in core
                    and "L4_V_TOTAL = L4_V_TOTAL_P" in core
                )
            ),
        ),
        ("PLEX_PRESENT_720P_L4_gate", "`ifdef PLEX_PRESENT_720P_L4" in core and "`ifdef PLEX_PRESENT_720P_L4" in plex),
        ("colorbars_default", "colorbars bars" in core),
    ]
    for name, ok in checks:
        if ok:
            print(f"OK INSTANTIATED_INTENT={name}")
        else:
            fails.append(f"INSTANTIATED_INTENT=no {name}")

    for mod in (
        "present_content_window.sv",
        "present_geom_latch.sv",
        "plex_present_geom_mux.sv",
        "present_beam_content_de.sv",
        "present_video_timing_720p.sv",
    ):
        if f"rtl/{mod}" not in qip:
            fails.append(f"IN_FILES_QIP=no {mod}")
        else:
            print(f"OK IN_FILES_QIP={mod}")

    # Dual-header beam (anchored, under this QSF's macros; see BEAM_SVH / BEAM_HPP)
    defs = {m.split("=", 1)[0] for m in act}
    pll = PLL.read_text(errors="ignore")
    beam = beam_pin_fails(select_ifdefs(svh, defs), hpp, pll)
    fails += beam
    if not beam:
        print("OK beam pins: svh H/V/DE/ACTIVE + CLK_SYS 24e6 + CLK_PIX 24e6; hpp H/V + ClkPix 24e6; "
              "pll PLEX_CLK_SYS_24 out0 24 MHz")
    sys_line = f"DDR_FRAME_720P24_CLK_SYS_HZ = {L4_CLK_HZ};"
    pix_line = f"DDR_FRAME_720P24_CLK_PIX_HZ = {L4_CLK_HZ};"
    pll24 = '`ifdef PLEX_CLK_SYS_24\n\t\t.output_clock_frequency0("24.000000 MHz")'
    # Each tooth: (name, svh, hpp, pll). A tooth whose mutation changes nothing is itself a FAIL.
    beam_teeth = (
        ("CLK_SYS_HZ 21_000_000", svh.replace(sys_line, sys_line.replace(L4_CLK_HZ, "21_000_000")), hpp, pll),
        ("CLK_SYS_HZ 20_000_000 (non-L4 default)", svh.replace(sys_line, sys_line.replace(L4_CLK_HZ, "20_000_000")),
         hpp, pll),
        ("CLK_PIX_HZ 20_000_000", svh.replace(pix_line, pix_line.replace(L4_CLK_HZ, "20_000_000")), hpp, pll),
        ("CLK_PIX_HZ removed", svh.replace(pix_line, ""), hpp, pll),
        ("hpp ClkPixHz 20000000", svh, hpp.replace("kPlex720p24ClkPixHz = 24000000;",
                                                   "kPlex720p24ClkPixHz = 20000000;"), pll),
        ("pll PLEX_CLK_SYS_24 out0 20 MHz", svh, hpp, pll.replace(pll24, pll24.replace("24.000000", "20.000000"))),
    )
    for name, m_svh, m_hpp, m_pll in beam_teeth:
        if (m_svh, m_hpp, m_pll) == (svh, hpp, pll):
            fails.append(f"beam tooth '{name}' is vacuous (mutation changed nothing)")
        elif not beam_pin_fails(select_ifdefs(m_svh, defs), m_hpp, m_pll):
            fails.append(f"beam tooth '{name}' stayed GREEN")
        else:
            print(f"OK beam tooth RED: {name}")

    # Negative twin: content_window instance must be load-bearing in the gate
    if "u_content_window" in core:
        twin = core.replace(") u_content_window (", ") u_GONE (")
        if "u_content_window" in twin:
            fails.append("negative twin could not rename u_content_window instance")
        else:
            print("OK negative twin: removing u_content_window would fail this gate")

    # fps arithmetic check (integer): 24000000 / (1312*762) milli
    pix = 1312 * 762
    fps_milli = (24_000_000 * 1000) // pix
    if fps_milli != 24006:
        fails.append(f"beam fps_milli want 24006 got {fps_milli} (24e6/(1312*762))")
    else:
        print("OK beam arithmetic 24e6/(1312*762) → 24.006 Hz")

    if fails:
        print("FAIL present_720p_l4_static:")
        for f in fails:
            print(" ", f)
        return 1
    print("PASS present_720p_l4_static: L4 instantiate-behind-flag, default-off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
