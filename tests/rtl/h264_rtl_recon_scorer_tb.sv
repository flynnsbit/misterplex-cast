// RTL-in-the-loop reconstruction scorer.
// Instantiates the actual dequant/IDCT/recon pipeline and exposes
// coefficient/prediction injection ports for the C++ testbench driver.
// This tests whether the FPGA arithmetic produces correct output
// for known golden inputs — independent of CAVLC parsing or prediction.
//
// Two modes:
//   inject_bypass_dequant=0: coeff → dequant → IDCT → recon (tests full pipeline)
//   inject_bypass_dequant=1: inject_dequant → IDCT → recon (tests IDCT+recon only)
//   The bypass mode handles I_16x16 MBs where DC comes from Hadamard, not per-block CAVLC.

module h264_rtl_recon_scorer_tb #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240
)(
	input  wire        clk,
	input  wire        reset,

	// Testbench injection: coefficients + QP + prediction for one 4x4 block.
	// Widths track product h264_iq_idct_4x4.sv (coeff 16b, dequant/residual 29b).
	input  wire signed [15:0] inject_coeff [0:15],
	input  wire [5:0]         inject_qp,
	input  wire [7:0]         inject_pred [0:15],
	input  wire               inject_valid,

	// Direct dequant injection (bypass dequant module for I_16x16 DC path)
	input  wire signed [28:0] inject_dequant [0:15],
	input  wire               inject_bypass_dequant,

	// RTL reconstruction output
	output wire [7:0]         recon_out [0:15],
	output wire               recon_ready,

	// Pipeline visibility for degeneracy checks
	output wire signed [28:0] dequant_out [0:15],
	output wire signed [28:0] idct_out [0:15]
);

	// Dequant stage
	wire signed [28:0] dq [0:15];
	h264_dequant4x4 u_dequant (
		.coeff(inject_coeff),
		.qp(inject_qp),
		.max_coeff(5'd16),
		.dequant(dq)
	);

	// Mux: use RTL dequant output or direct injection
	wire signed [28:0] idct_in [0:15];
	genvar gi;
	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : gen_dq_mux
			assign idct_in[gi] = inject_bypass_dequant ? inject_dequant[gi] : dq[gi];
		end
	endgenerate

	// IDCT stage
	wire signed [28:0] res [0:15];
	h264_idct4x4 u_idct (
		.dequant(idct_in),
		.residual(res)
	);

	// Reconstruction stage (pred + residual, clipped to [0,255])
	wire [7:0] rec [0:15];
	h264_recon4x4 u_recon (
		.pred(inject_pred),
		.residual(res),
		.recon(rec)
	);

	assign recon_out = rec;
	assign dequant_out = idct_in;
	assign idct_out = res;

	// Combinational: output ready same cycle as input valid
	assign recon_ready = inject_valid;

endmodule
