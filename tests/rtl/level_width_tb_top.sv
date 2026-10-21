// Red-before-green testbench for coefficient width truncation bug.
// Wraps the REAL product dequant/IDCT/recon RTL and exposes 16-bit
// coefficient inputs.  On the CURRENT product code (signed [8:0]),
// the g_narrow adapter truncates; after the fix it passes through.
`default_nettype none

module level_width_tb_top (
	input  wire signed [15:0] coeff    [0:15],
	input  wire        [4:0]  max_coeff,
	input  wire        [5:0]  qp,
	input  wire        [7:0]  pred     [0:15],
	output wire signed [28:0] dequant  [0:15],
	output wire signed [28:0] idct     [0:15],
	output wire        [7:0]  recon    [0:15]
);
	// Product RTL coefficient width — now matches the full 16-bit spec width.
	wire signed [15:0] coeff_prod [0:15];
	genvar i;
	generate
		for (i = 0; i < 16; i = i + 1) begin : g_pass
			assign coeff_prod[i] = coeff[i];
		end
	endgenerate

	h264_dequant4x4 u_dequant (
		.coeff(coeff_prod),
		.qp(qp),
		.max_coeff(max_coeff),
		.dequant(dequant)
	);

	h264_idct4x4 u_idct (
		.dequant(dequant),
		.residual(idct)
	);

	h264_recon4x4 u_recon (
		.pred(pred),
		.residual(idct),
		.recon(recon)
	);
endmodule

`default_nettype wire
