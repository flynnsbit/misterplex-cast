`default_nettype none

module h264_p_slice_modes_tb_top #(
	parameter bit FAULT_SWAP_16x8 = 1'b0
) (
	input  wire       skipped,
	input  wire [5:0] mb_type,
	input  wire [1:0] sub_mb_type,
	input  wire       sub_mb_valid,
	output wire       is_p_skip,
	output wire       is_inter,
	output wire       is_intra,
	output wire       uses_sub_mb,
	output wire       ref0_only,
	output wire       unsupported,
	output wire [2:0] part_mode,
	output wire [2:0] mb_part_count,
	output wire [4:0] mb_part_w,
	output wire [4:0] mb_part_h,
	output wire [2:0] sub_part_count,
	output wire [3:0] sub_part_w,
	output wire [3:0] sub_part_h
);
	wire [2:0] raw_part_mode;
	wire [4:0] raw_mb_part_w;
	wire [4:0] raw_mb_part_h;

	h264_p_mb_type_decode dut (
		.skipped(skipped),
		.mb_type(mb_type),
		.sub_mb_type(sub_mb_type),
		.sub_mb_valid(sub_mb_valid),
		.is_p_skip(is_p_skip),
		.is_inter(is_inter),
		.is_intra(is_intra),
		.uses_sub_mb(uses_sub_mb),
		.ref0_only(ref0_only),
		.unsupported(unsupported),
		.part_mode(raw_part_mode),
		.mb_part_count(mb_part_count),
		.mb_part_w(raw_mb_part_w),
		.mb_part_h(raw_mb_part_h),
		.sub_part_count(sub_part_count),
		.sub_part_w(sub_part_w),
		.sub_part_h(sub_part_h)
	);

	assign part_mode = (FAULT_SWAP_16x8 && mb_type == 6'd1 && !skipped) ? 3'd2 : raw_part_mode;
	assign mb_part_w = (FAULT_SWAP_16x8 && mb_type == 6'd1 && !skipped) ? 5'd8 : raw_mb_part_w;
	assign mb_part_h = (FAULT_SWAP_16x8 && mb_type == 6'd1 && !skipped) ? 5'd16 : raw_mb_part_h;
endmodule

`default_nettype wire
