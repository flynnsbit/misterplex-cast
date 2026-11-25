# sys/ascal.vhd -> Template_MiSTer baaeff6 (HDMI snow after a pll_hdmi reconfig)

Apply from the root of the eac1064 sys pin (the dir that contains `sys/`), after the SS1 patches:

    patch -p1 < 0001-sys-ascal-template-baaeff6.patch

What it is: the eac1064 `sys/ascal.vhd` (md5 1b17376e, git blob a735e6b4) is byte-identical to
Template_MiSTer d93bc2e ("ASCAL : Add 2304, 2560 resolutions", 2025-07-09). This patch is the
upstream diff d93bc2e..baaeff6, unchanged. The patched file is byte-identical to Template_MiSTer
baaeff609b69e3d4b424ae9b5f4fa6f2c6907aa4 sys/ascal.vhd (paulb-nl, 2026-08-22, "ascal: reset
o_state at vsync"): md5 efc9d8f6bf2c21abc89913c3d7f722a8, git blob 13a517160889130406a855801f04679bae42304a.
It is also the current upstream file: nothing touched sys/ascal.vhd after baaeff6 (checked at
upstream d6324f2, 2026-11-20). 0874acd/54ac838 (gray counters) were reverted by f38ab86, so
d93bc2e..baaeff6 is just these 16 lines:

- reset: also clear o_readack / o_readdataack and their two sync stages;
- every output VSync: o_copy <= sWAIT, o_state <= sDISP, o_readlev/o_copylev <= 0, o_hsp <= '0'.

Why (upstream comment): "Sometimes o_clk is paused during PLL reconfig and causes o_state to get
stuck in sREAD state." o_clk is clk_hdmi (pll_hdmi). Main reprograms pll_hdmi on core load and on
HDMI mode changes. If a read handshake is lost while o_clk is stopped, the output line fetcher stays
in sREAD/sWAITREAD for the rest of the session. HDMI then shows the stale line buffers and unwritten
scaler DDR, which is the snow on lab 042f8617 (fresh load, Video Output switches, mid-stream).
See /work/mfd/browse-review-fixes/PLAN-core-snow-042f8617.md for the evidence.

Ports and generics are unchanged, so no sys_top.v or qsf change is needed. The two
`ALLOW_SYNCH_CTRL_USAGE OFF -to "ascal:ascal|o_[vh]_poly_pix*"` lines still match.
Analog output does not go through ascal in this core (VGA_SCALER=0), so 15 kHz / 240p / 480i analog
is unaffected. Any sys/ change re-rolls the fit seed: launch_yc.sh applies this after the SS1 pair,
pins md5 efc9d8f6, and requires exactly ascal.vhd, hps_io.sv and yc_out.sv to differ from eac1064.
This file ships with the core as GPL source, like the SS1 pair.
