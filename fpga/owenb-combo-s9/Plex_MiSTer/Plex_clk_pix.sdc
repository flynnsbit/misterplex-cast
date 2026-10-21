# Plex_clk_pix.sdc — constraints for PRESENT_CLK_PIX_PLL (default OFF).
#
# Product QSF does NOT source this file. Enable only with the fit recipe:
#   set_global_assignment -name VERILOG_MACRO "PRESENT_CLK_PIX_PLL=1"
#   set_global_assignment -name SDC_FILE Plex_clk_pix.sdc
#
# Base Plex.sdc already runs derive_pll_clocks. This file names the new domain,
# documents CDC policy, and adds exclusive groups so STA does not invent
# related-edge paths across async FIFO crossings.
#
# Frequencies (from pll_0002.v SoT):
#   clk_sys  = general[0] = 20.000 MHz
#   clk_ddr  = general[2] = 90.000 MHz
#   clk_pix  = general[3] = 29.700 MHz  (or 74.250 with PRESENT_CLK_PIX_74_25)
#
# Do NOT add false_path on residual_csum / decode sameclk paths.
# Do NOT weaken setup to hide real violations.

# Named PLL output pins (Cyclone V altera_pll counter path — same family as STA dumps).
set plex_clk_sys {emu|pll|pll_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}
set plex_clk_ddr {emu|pll|pll_inst|altera_pll_i|general[2].gpll~PLL_OUTPUT_COUNTER|divclk}
set plex_clk_pix {emu|pll|pll_inst|altera_pll_i|general[3].gpll~PLL_OUTPUT_COUNTER|divclk}

# Exclusive: clk_pix is crossed only via async_fifo gray pointers + 2FF levels
# (present_npx_path). Treat as asynchronous to clk_sys / clk_ddr.
# If a future design intentionally uses related-edge multicycle between these,
# remove this group and add proven multicycle — never a silent false_path.
set_clock_groups -asynchronous \
	-group [get_clocks $plex_clk_sys] \
	-group [get_clocks $plex_clk_pix]

set_clock_groups -asynchronous \
	-group [get_clocks $plex_clk_ddr] \
	-group [get_clocks $plex_clk_pix]

# clk_sys <-> clk_ddr remain related (existing product CDC inventory). Do not
# put them in exclusive groups here.

# Period sanity (derive_pll_clocks should already create these; explicit
# create_generated_clock is NOT used — PLL is the generator).
# Parent STA one-pass after fit:
#   scripts/sta_onepass_interrogation.sh OUT/Plex.sta.rpt OUT/clk_sys_sameclk_setup.txt
# Interrogate ALSO:
#   - Fmax of general[3] (clk_pix) >= 29.7 (or 74.25)
#   - No setup fail inside present_npx_path on clk_pix
#   - No decode_stub as Fmax owner (parent miss #18)
