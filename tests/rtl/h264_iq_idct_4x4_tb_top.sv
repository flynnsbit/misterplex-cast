// Testbench-only top level. NOT part of the Quartus project and never synthesised.
//
// It exists so the Verilator testbench drives the REAL product modules in
// fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv rather than a private copy. An
// earlier version of this test simulated its own fork of the RTL, which meant a
// green run said nothing about the bitstream. Keep scaffolding here, not in
// synthesised source.
`default_nettype none

module h264_iq_idct_4x4 (
	input  wire signed [15:0] coeff    [0:15],
	input  wire        [4:0]  max_coeff,
	input  wire        [5:0]  qp,
	input  wire        [7:0]  pred     [0:15],
	output wire signed [28:0] dequant  [0:15],
	output wire signed [28:0] idct     [0:15],
	output wire        [7:0]  recon    [0:15]
);
	// Product RTL now takes full 16-bit signed coefficients.
	wire signed [15:0] coeff16 [0:15];
	genvar i;
	generate
		for (i = 0; i < 16; i = i + 1) begin : g_pass
			assign coeff16[i] = coeff[i];
		end
	endgenerate

	h264_dequant4x4 u_dequant (
		.coeff(coeff16),
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
