// Wrapper only. Frequency SoT is rtl/pll/pll_0002.v (edit that file).
// PRESENT_CLK_PIX_PLL adds outclk_3 = clk_pix (default OFF).

`timescale 1 ps / 1 ps
module pll (
		input  wire  refclk,   //  refclk.clk
		input  wire  rst,      //   reset.reset
		output wire  outclk_0, // outclk0.clk — clk_sys 20 MHz
		output wire  outclk_1, // outclk1.clk — SDRAM controller clock
		output wire  outclk_2, // outclk2.clk — DDRAM bridge clock 90 MHz
`ifdef PRESENT_CLK_PIX_PLL
		output wire  outclk_3, // outclk3.clk — clk_pix (CEA 720p present)
`endif
		output wire  locked    //  locked.export
	);

	pll_0002 pll_inst (
		.refclk   (refclk),
		.rst      (rst),
		.outclk_0 (outclk_0),
		.outclk_1 (outclk_1),
		.outclk_2 (outclk_2),
`ifdef PRESENT_CLK_PIX_PLL
		.outclk_3 (outclk_3),
`endif
		.locked   (locked)
	);

endmodule
