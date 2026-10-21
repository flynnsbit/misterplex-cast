module h264_sps_geometry_tb_top #(
	parameter bit FAULT_CROP_RIGHT = 1'b0
) (
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,
	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,
	output wire        in_ready,
	output wire        valid,
	output wire        error,
	output wire [7:0]  profile_idc,
	output wire [7:0]  level_idc,
	output wire [4:0]  log2_max_frame_num,
	output wire [2:0]  poc_type,
	output wire [15:0] coded_width,
	output wire [15:0] coded_height,
	output wire [15:0] display_width,
	output wire [15:0] display_height,
	output wire [15:0] crop_left,
	output wire [15:0] crop_right,
	output wire [15:0] crop_top,
	output wire [15:0] crop_bottom,
	output wire [15:0] rbsp_bits_consumed,
	output wire        busy
);
	wire [15:0] crop_right_raw;
	wire [15:0] display_width_raw;

	h264_sps_geometry_parser parser (
		.clk(clk), .reset(reset), .clear(clear),
		.in_valid(in_valid), .in_byte(in_byte), .in_last(in_last), .in_ready(in_ready),
		.valid(valid), .error(error), .profile_idc(profile_idc), .level_idc(level_idc),
		.log2_max_frame_num(log2_max_frame_num), .poc_type(poc_type),
		.coded_width(coded_width), .coded_height(coded_height),
		.display_width(display_width_raw), .display_height(display_height),
		.crop_left(crop_left), .crop_right(crop_right_raw), .crop_top(crop_top), .crop_bottom(crop_bottom),
		.rbsp_bits_consumed(rbsp_bits_consumed), .busy(busy)
	);

	assign crop_right = FAULT_CROP_RIGHT ? (crop_right_raw ^ 16'd1) : crop_right_raw;
	assign display_width = FAULT_CROP_RIGHT ? (display_width_raw + 16'd2) : display_width_raw;
endmodule
