module p3_intra_mb0_tb (
	input  wire [3:0] mode,
	input  wire [7:0] above [0:7],
	input  wire [7:0] left [0:3],
	input  wire [7:0] top_left,
	input  wire       has_above,
	input  wire       has_left,
	input  wire signed [28:0] residual [0:15],
	output wire [3:0] used_mode,
	output wire [7:0] pred [0:15],
	output wire [7:0] recon [0:15]
);
	wire [7:0] pred_raw [0:15];
	genvar gi;

	h264_intra4x4_pred pred4 (
		.mode(mode),
		.above(above),
		.left(left),
		.top_left(top_left),
		.has_above(has_above),
		.has_left(has_left),
		.used_mode(used_mode),
		.pred(pred_raw)
	);

	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : gen_pred_out
`ifdef P3_INTRA_NEGATIVE_TEST
			if (gi == 0) assign pred[gi] = pred_raw[gi] ^ 8'h01;
			else assign pred[gi] = pred_raw[gi];
`else
			assign pred[gi] = pred_raw[gi];
`endif
		end
	endgenerate

	h264_recon4x4 recon4 (
		.pred(pred),
		.residual(residual),
		.recon(recon)
	);
endmodule
