module p3_intra_frame_tb (
	input  wire        clk,

	input  wire [3:0] i4_mode,
	input  wire [7:0] i4_above [0:7],
	input  wire [7:0] i4_left [0:3],
	input  wire [7:0] i4_top_left,
	input  wire       i4_has_above,
	input  wire       i4_has_left,
	input  wire signed [28:0] i4_residual [0:15],
	output wire [3:0] i4_used_mode,
	output wire [7:0] i4_pred [0:15],
	output wire [7:0] i4_recon [0:15],

	input  wire        i16_start,
	input  wire [1:0] i16_mode,
	input  wire [7:0] i16_above [0:15],
	input  wire [7:0] i16_left [0:15],
	input  wire [7:0] i16_top_left,
	input  wire       i16_has_above,
	input  wire       i16_has_left,
	output wire       i16_unsupported,
	output wire       i16_valid,
	output wire [7:0] i16_pred [0:255],

	input  wire        chroma_start,
	input  wire [1:0] chroma_mode,
	input  wire [7:0] chroma_above [0:7],
	input  wire [7:0] chroma_left [0:7],
	input  wire [7:0] chroma_top_left,
	input  wire       chroma_has_above,
	input  wire       chroma_has_left,
	output wire       chroma_valid,
	output wire [7:0] chroma_pred [0:63],

	input  wire [7:0] recon_pred [0:15],
	input  wire signed [28:0] recon_residual [0:15],
	output wire [7:0] recon_out [0:15]
);
	wire [7:0] i4_pred_raw [0:15];
	wire [7:0] i4_pred_checked [0:15];
	genvar gi;

	h264_intra4x4_pred pred4 (
		.mode(i4_mode),
		.above(i4_above),
		.left(i4_left),
		.top_left(i4_top_left),
		.has_above(i4_has_above),
		.has_left(i4_has_left),
		.used_mode(i4_used_mode),
		.pred(i4_pred_raw)
	);

	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : gen_i4_pred_out
`ifdef P3_INTRA_FRAME_RARE_NEGATIVE_TEST
			if (gi == 0) assign i4_pred_checked[gi] = (i4_used_mode == 4'd4) ? (i4_pred_raw[gi] ^ 8'h01) : i4_pred_raw[gi];
			else assign i4_pred_checked[gi] = i4_pred_raw[gi];
`else
			assign i4_pred_checked[gi] = i4_pred_raw[gi];
`endif
			assign i4_pred[gi] = i4_pred_checked[gi];
		end
	endgenerate

	h264_recon4x4 recon4_i4 (
		.pred(i4_pred_checked),
		.residual(i4_residual),
		.recon(i4_recon)
	);

	h264_intra16x16_pred pred16 (
		.clk(clk),
		.start(i16_start),
		.mode(i16_mode),
		.above(i16_above),
		.left(i16_left),
		.top_left(i16_top_left),
		.has_above(i16_has_above),
		.has_left(i16_has_left),
		.unsupported(i16_unsupported),
		.valid(i16_valid),
		.pred(i16_pred)
	);

	h264_chroma8x8_pred pred_chroma (
		.clk(clk),
		.start(chroma_start),
		.mode(chroma_mode),
		.above(chroma_above),
		.left(chroma_left),
		.top_left(chroma_top_left),
		.has_above(chroma_has_above),
		.has_left(chroma_has_left),
		.valid(chroma_valid),
		.pred(chroma_pred)
	);

	h264_recon4x4 recon4_generic (
		.pred(recon_pred),
		.residual(recon_residual),
		.recon(recon_out)
	);
endmodule
