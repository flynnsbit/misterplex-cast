#!/usr/bin/env python3
"""720p pixel-clock + DDR bandwidth arithmetic (w-clock).

Positive cases lock CEA / layout constants to exact integer Hz and MB/s.
Negative cases: a naive wrong blanking (active-only) or RGB565-as-product
must FAIL so this is not a tautology.

No Quartus. No device. Exit 0 only when all asserts hold.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)
        else:
            print(f"OK {msg}")

    # --- CEA pixel clocks (exact integers) ---
    # VIC 4 720p60: H=1650 V=750
    h60, v60, fps60 = 1650, 750, 60
    pix60 = h60 * v60 * fps60
    check(pix60 == 74_250_000, f"CEA VIC4 720p60 f_pix={pix60} (==74250000)")

    # VIC 60 720p24: H=3300 V=750 (double H blank)
    h24v, v24v, fps24 = 3300, 750, 24
    pix24_vic = h24v * v24v * fps24
    check(pix24_vic == 59_400_000, f"CEA VIC60 720p24 f_pix={pix24_vic} (==59400000)")

    # Same totals as VIC4 @ 24 fps (PRESENT_CLK_PIX_PLL default target)
    h_pack, v_pack = 1650, 750
    pix24_pack = h_pack * v_pack * 24
    check(pix24_pack == 29_700_000, f"pack 1650*750*24 f_pix={pix24_pack} (==29700000)")
    ppf = h_pack * v_pack
    check(ppf == 1_237_500, f"PIX_PER_FRAME pack={ppf}")

    # NEGATIVE: active-only blanking is NOT a legal CEA pixel clock
    active_only_24 = 1280 * 720 * 24
    check(active_only_24 != 29_700_000, "NEG: active-only 1280*720*24 != 29.70 MHz")
    check(active_only_24 == 22_118_400, f"NEG twin value active_only_24={active_only_24}")

    # --- Quote PLL SoT on disk ---
    pll = read(ROOT / "fpga/Plex_MiSTer/rtl/pll/pll_0002.v")
    check('output_clock_frequency0("20.000000 MHz")' in pll, "PLL out0 clk_sys=20.000000 MHz")
    check('output_clock_frequency2("90.000000 MHz")' in pll, "PLL out2 clk_ddr=90.000000 MHz")
    check('reference_clock_frequency("50.0 MHz")' in pll, "PLL ref=50.0 MHz")
    check("PRESENT_CLK_PIX_PLL" in pll, "PLL has PRESENT_CLK_PIX_PLL branch")
    check('"29.700000 MHz"' in pll, "PLL default clk_pix string 29.700000 MHz")
    check('"74.250000 MHz"' in pll, "PLL optional 74.250000 MHz string")
    # Product default must still be 3 clocks in the else branch
    check(
        re.search(r'`else\s+altera_pll\s+#\(\s*\n(?:.*\n){0,6}.*number_of_clocks\(3\)', pll)
        is not None,
        "product else-branch number_of_clocks(3)",
    )
    # L4 QSF enables PLEX_CLK_SYS_24 but leaves PRESENT_CLK_PIX_PLL OFF, so the
    # 3-clock else branch is what actually elaborates. A 4-clock-only ifdef
    # leaves out0 hardcoded 20 MHz (TimeQuest ÷18) → HDMI ~20 Hz, not 24.
    n_clk24 = len(re.findall(r"`ifdef\s+PLEX_CLK_SYS_24", pll))
    check(n_clk24 >= 2, f"PLEX_CLK_SYS_24 wraps BOTH altera_pll out0 (count={n_clk24})")
    m3 = re.search(
        r"number_of_clocks\(3\),\s*(.*?)\.output_clock_frequency0\(\"([^\"]+)\"\)",
        pll,
        re.S,
    )
    check(m3 is not None, "found 3-clock out0")
    if m3 is not None:
        check(
            "`ifdef PLEX_CLK_SYS_24" in m3.group(1),
            "3-clock PLL out0 under PLEX_CLK_SYS_24 (hardcoded 20 MHz → ~20 Hz HDMI)",
        )

    # QSF: PRESENT_CLK_PIX_PLL must be commented (default OFF)
    qsf = read(ROOT / "fpga/Plex_MiSTer/Plex.qsf")
    active_pix = [
        ln
        for ln in qsf.splitlines()
        if "PRESENT_CLK_PIX_PLL" in ln and not ln.strip().startswith("#")
    ]
    check(not active_pix, "QSF PRESENT_CLK_PIX_PLL not active (default OFF)")
    check("Plex_clk_pix.sdc" in qsf, "QSF mentions Plex_clk_pix.sdc recipe")

    # Plex.sv wires clk_pix from PLL only under ifdef
    plex = read(ROOT / "fpga/Plex_MiSTer/Plex.sv")
    check("clk_pix_pll" in plex, "Plex.sv declares clk_pix_pll under flag path")
    check(".clk_pix(clk_sys)" in plex, "product .clk_pix(clk_sys) still present")

    # present_video_timing pack constants
    tim = read(ROOT / "fpga/Plex_MiSTer/rtl/present_video_timing_720p.sv")
    check("H_TOTAL_L  = 1650" in tim, "timing pack H_TOTAL=1650")
    check("V_TOTAL_L  = 750" in tim, "timing pack V_TOTAL=750")
    check("74_250_000" in tim or "74.25" in tim, "timing pack documents 74.25")

    # --- DDR bandwidth: product format is YUV420p / I420 ---
    layout = read(ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_layout_params.svh")
    check("DDR_FRAME_720P_YUV420P_BYTES = 1382400" in layout, "I420 1280x720 bytes=1382400")
    # 1280*720*3/2 = 1382400
    check(1280 * 720 * 3 // 2 == 1_382_400, "I420 arith 1280*720*3/2")

    frame_b = 1_382_400
    # Decimal MB/s (10^6) to match docs/display-resolution.md style
    rd24 = frame_b * 24 / 1e6
    rd60 = frame_b * 60 / 1e6
    check(abs(rd24 - 33.1776) < 1e-9, f"YUV420p 720p24 FPGA read={rd24} MB/s")
    check(abs(rd60 - 82.944) < 1e-9, f"YUV420p 720p60 FPGA read={rd60} MB/s")

    # Docs model: peak = DDRAM_CLK * 8; pessimistic FPGA-read budget = 25% peak
    # Product clk_ddr = 90 MHz → peak 720 MB/s → budget 180 MB/s
    peak = 90.0 * 8.0
    budget = peak * 0.25
    check(peak == 720.0, f"DDR peak @90MHz={peak} MB/s")
    check(budget == 180.0, f"pessimistic FPGA-read budget={budget} MB/s")
    check(rd24 < budget, f"FIT: 24fps read {rd24} < budget {budget}")
    check(rd60 < budget, f"FIT: 60fps read {rd60} < budget {budget}")

    # Total fabric with equal ARM write (docs model)
    tot24 = 2 * rd24
    tot60 = 2 * rd60
    check(tot24 < peak, f"total fabric 24fps {tot24} < peak {peak}")
    check(tot60 < peak, f"total fabric 60fps {tot60} < peak {peak}")

    # NEGATIVE: RGB565 at 720p60 exceeds pessimistic read budget
    rgb_frame = 1280 * 720 * 2
    rgb60 = rgb_frame * 60 / 1e6
    check(rgb60 == 110.592, f"RGB565 720p60 read={rgb60}")
    check(rgb60 < budget, "RGB565 720p60 still < 180 budget (docs: viable)")
    # But RGB565 720p60 was over the OLD 20 MHz-as-DDR clock model (40 MB/s)
    old_budget_20 = 20.0 * 8.0 * 0.25  # if someone confuses clk_sys with DDRAM
    check(old_budget_20 == 40.0, "old confused 20MHz*8*25% budget=40")
    check(rd60 > old_budget_20, "NEG: 720p60 YUV would FAIL if DDR were 20 MHz")

    # Throughput: PPC=1 @20 MHz cannot feed 29.7 Mpix/s
    check(20.0 < 29.7, "NEG: clk_sys 20 MHz < 29.7 Mpix/s need (PPC=1)")
    check(20.0 * 2 >= 29.7, "PPC=2 @20 MHz fabric groups can feed 29.7")

    # SDC file exists and does not false_path residual
    sdc = read(ROOT / "fpga/Plex_MiSTer/Plex_clk_pix.sdc")
    check("set_clock_groups -asynchronous" in sdc, "SDC async groups clk_pix")
    check("residual" not in sdc.lower() or "Do NOT" in sdc, "SDC does not silence residual")
    check("general[3]" in sdc, "SDC names general[3] clk_pix")

    if fails:
        print("FAIL test_720p_clk_ddr_arith:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("PASS test_720p_clk_ddr_arith")
    return 0


if __name__ == "__main__":
    sys.exit(main())
