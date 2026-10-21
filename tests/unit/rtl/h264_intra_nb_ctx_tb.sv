// Testbench wrapper: drives h264_intra4x4_pred through h264_intra_nb_ctx
// for multi-MB neighbour context verification.
// This test goes RED until h264_intra_nb_ctx is implemented.
module h264_intra_nb_ctx_tb (
	input  wire        clk,
	input  wire        reset,
	// MB-level control
	input  wire [7:0]  mb_x,
	input  wire [7:0]  mb_y,
	input  wire [7:0]  mb_width,
	input  wire        mb_start,
	// Block-level control within MB
	input  wire [3:0]  block_idx,    // 0..15 raster within MB
	input  wire [3:0]  pred_mode,
	input  wire        block_valid,
	// Residual input
	input  wire signed [28:0] residual [0:15],
	// Outputs
	output wire [3:0]  used_mode,
	output wire [7:0]  pred [0:15],
	output wire [7:0]  recon [0:15],
	// Degeneracy assertion outputs
	output reg  [15:0] nb_used_count,  // MBs where has_above && has_left && pred != all-128
	output reg  [15:0] mb_count,
	output wire        geom_reject
);
	// Neighbour context — default MAX=80 (1280/16)
	wire [7:0] ctx_above [0:7];
	wire [7:0] ctx_left [0:3];
	wire [7:0] ctx_top_left;
	wire       ctx_has_above;
	wire       ctx_has_left;

	h264_intra_nb_ctx #(
		.MB_WIDTH_MAX(80)   // 1280/16
	) u_nb_ctx (
		.clk(clk),
		.reset(reset),
		.mb_x(mb_x),
		.mb_y(mb_y),
		.mb_width(mb_width),
		.mb_start(mb_start),
		.block_idx(block_idx),
		.block_valid(block_valid),
		.recon_pixels(recon),   // feedback: reconstructed pixels stored for neighbours
		.above(ctx_above),
		.left(ctx_left),
		.top_left(ctx_top_left),
		.has_above(ctx_has_above),
		.has_left(ctx_has_left),
		.geom_reject(geom_reject)
	);

	// Intra 4x4 prediction
	wire [7:0] pred_raw [0:15];
	h264_intra4x4_pred u_pred (
		.mode(pred_mode),
		.above(ctx_above),
		.left(ctx_left),
		.top_left(ctx_top_left),
		.has_above(ctx_has_above),
		.has_left(ctx_has_left),
		.used_mode(used_mode),
		.pred(pred_raw)
	);

	genvar gi;
	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : gen_pred
			assign pred[gi] = pred_raw[gi];
		end
	endgenerate

	// Reconstruction
	h264_recon4x4 u_recon (
		.pred(pred),
		.residual(residual),
		.recon(recon)
	);

	// Degeneracy counter: count MBs where neighbours were available AND used
	reg any_non_128;
	integer pi;
	always @* begin
		any_non_128 = 1'b0;
		for (pi = 0; pi < 16; pi = pi + 1)
			if (pred_raw[pi] != 8'd128) any_non_128 = 1'b1;
	end

	always @(posedge clk or posedge reset) begin
		if (reset) begin
			nb_used_count <= 16'd0;
			mb_count <= 16'd0;
		end else if (mb_start) begin
			mb_count <= mb_count + 16'd1;
			// Count if this MB has both neighbours AND prediction is not all-128
			if (ctx_has_above && ctx_has_left && any_non_128)
				nb_used_count <= nb_used_count + 16'd1;
		end
	end
endmodule
