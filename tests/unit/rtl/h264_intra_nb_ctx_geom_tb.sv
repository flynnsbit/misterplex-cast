// Thin TB: exercise h264_intra_nb_ctx geometry cap + sticky geom_reject.
// Parameter MB_WIDTH_MAX is set by the harness (-GMB_WIDTH_MAX=... or default 80).
`default_nettype none

module h264_intra_nb_ctx_geom_tb #(
	parameter int MB_WIDTH_MAX = 80
) (
	input  wire       clk,
	input  wire       reset,
	input  wire [7:0] mb_x,
	input  wire [7:0] mb_y,
	input  wire [7:0] mb_width,
	input  wire       mb_start,
	input  wire [3:0] block_idx,
	input  wire       block_valid,
	output wire       geom_reject
);
	wire [7:0] recon_pixels [0:15];
	wire [7:0] above [0:7];
	wire [7:0] left [0:3];
	wire [7:0] top_left;
	wire       has_above, has_left;

	genvar gi;
	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : gen_recon
			assign recon_pixels[gi] = 8'hA0 + gi[7:0];
		end
	endgenerate

	h264_intra_nb_ctx #(
		.MB_WIDTH_MAX(MB_WIDTH_MAX)
	) u_nb (
		.clk(clk),
		.reset(reset),
		.mb_x(mb_x),
		.mb_y(mb_y),
		.mb_width(mb_width),
		.mb_start(mb_start),
		.block_idx(block_idx),
		.block_valid(block_valid),
		.recon_pixels(recon_pixels),
		.above(above),
		.left(left),
		.top_left(top_left),
		.has_above(has_above),
		.has_left(has_left),
		.geom_reject(geom_reject)
	);
endmodule

`default_nettype wire
