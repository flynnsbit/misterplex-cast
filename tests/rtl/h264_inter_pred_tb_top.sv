// Testbench-only top for real product inter-prediction RTL.
`default_nettype none

module h264_inter_pred_tb #(
	parameter FAULT_BAD_ROUND = 0,
	parameter FAULT_BAD_PART_MV = 0
) (
	input  wire               avail_a,
	input  wire               avail_b,
	input  wire               avail_c,
	input  wire               avail_d,
	input  wire signed [15:0] mv_a_x,
	input  wire signed [15:0] mv_a_y,
	input  wire signed [15:0] mv_b_x,
	input  wire signed [15:0] mv_b_y,
	input  wire signed [15:0] mv_c_x,
	input  wire signed [15:0] mv_c_y,
	input  wire signed [15:0] mv_d_x,
	input  wire signed [15:0] mv_d_y,
	input  wire signed [15:0] mvd_x,
	input  wire signed [15:0] mvd_y,
	input  wire               p_skip,
	output wire signed [15:0] pred_x,
	output wire signed [15:0] pred_y,
	output wire signed [15:0] mv_x,
	output wire signed [15:0] mv_y,
	output wire               skip_zero,

	input  wire [2:0]         part_mode,
	input  wire [1:0]         part_idx,
	output wire signed [15:0] part_pred_x,
	output wire signed [15:0] part_pred_y,
	output wire signed [15:0] part_mv_x,
	output wire signed [15:0] part_mv_y,
	output wire               part_skip_zero,

	input  wire [7:0]         luma_ref [0:80],
	input  wire [1:0]         luma_frac_x,
	input  wire [1:0]         luma_frac_y,
	output wire [7:0]         luma_sample,

	input  wire [7:0]         c_p00,
	input  wire [7:0]         c_p10,
	input  wire [7:0]         c_p01,
	input  wire [7:0]         c_p11,
	input  wire [2:0]         chroma_frac_x,
	input  wire [2:0]         chroma_frac_y,
	output wire [7:0]         chroma_sample,

	input  wire signed [15:0] clamp_x,
	input  wire signed [15:0] clamp_y,
	input  wire [15:0]        clamp_w,
	input  wire [15:0]        clamp_h,
	output wire [15:0]        clamped_x,
	output wire [15:0]        clamped_y,

	input  wire signed [15:0] fetch_base_x,
	input  wire signed [15:0] fetch_base_y,
	input  wire [6:0]         fetch_tap_idx,
	input  wire [15:0]        fetch_w,
	input  wire [15:0]        fetch_h,
	output wire [15:0]        fetch_x,
	output wire [15:0]        fetch_y
);
	wire [7:0] luma_good;
	wire [7:0] chroma_good;
	wire signed [15:0] part_pred_x_good, part_pred_y_good;
	wire signed [15:0] part_mv_x_good, part_mv_y_good;

	h264_mv_pred_16x16 u_mv (
		.avail_a(avail_a), .avail_b(avail_b), .avail_c(avail_c), .avail_d(avail_d),
		.mv_a_x(mv_a_x), .mv_a_y(mv_a_y), .mv_b_x(mv_b_x), .mv_b_y(mv_b_y),
		.mv_c_x(mv_c_x), .mv_c_y(mv_c_y), .mv_d_x(mv_d_x), .mv_d_y(mv_d_y),
		.mvd_x(mvd_x), .mvd_y(mvd_y), .p_skip(p_skip),
		.pred_x(pred_x), .pred_y(pred_y), .mv_x(mv_x), .mv_y(mv_y), .skip_zero(skip_zero)
	);

	h264_mv_pred_part u_part (
		.part_mode(part_mode), .part_idx(part_idx),
		.avail_a(avail_a), .avail_b(avail_b), .avail_c(avail_c), .avail_d(avail_d),
		.mv_a_x(mv_a_x), .mv_a_y(mv_a_y), .mv_b_x(mv_b_x), .mv_b_y(mv_b_y),
		.mv_c_x(mv_c_x), .mv_c_y(mv_c_y), .mv_d_x(mv_d_x), .mv_d_y(mv_d_y),
		.mvd_x(mvd_x), .mvd_y(mvd_y), .p_skip(p_skip),
		.pred_x(part_pred_x_good), .pred_y(part_pred_y_good),
		.mv_x(part_mv_x_good), .mv_y(part_mv_y_good), .skip_zero(part_skip_zero)
	);

	h264_luma_qpel_sample u_luma (
		.ref_pix(luma_ref), .frac_x(luma_frac_x), .frac_y(luma_frac_y), .sample(luma_good)
	);

	h264_chroma_epel_sample u_chroma (
		.p00(c_p00), .p10(c_p10), .p01(c_p01), .p11(c_p11),
		.frac_x(chroma_frac_x), .frac_y(chroma_frac_y), .sample(chroma_good)
	);

	h264_ref_clamp u_clamp (
		.x(clamp_x), .y(clamp_y), .width(clamp_w), .height(clamp_h),
		.clamped_x(clamped_x), .clamped_y(clamped_y)
	);

	h264_luma_ref_tap_addr u_fetch (
		.base_x(fetch_base_x), .base_y(fetch_base_y), .tap_idx(fetch_tap_idx),
		.width(fetch_w), .height(fetch_h), .tap_x(fetch_x), .tap_y(fetch_y)
	);

	assign part_pred_x = FAULT_BAD_PART_MV ? (part_pred_x_good + 16'sd1) : part_pred_x_good;
	assign part_pred_y = part_pred_y_good;
	assign part_mv_x = FAULT_BAD_PART_MV ? (part_mv_x_good + 16'sd1) : part_mv_x_good;
	assign part_mv_y = part_mv_y_good;
	assign luma_sample = FAULT_BAD_ROUND ? (luma_good + 8'd1) : luma_good;
	assign chroma_sample = FAULT_BAD_ROUND ? (chroma_good + 8'd1) : chroma_good;
endmodule

`default_nettype wire
