`default_nettype none

module h264_p_mb_type_decode (
	input  wire       skipped,
	input  wire [5:0] mb_type,
	input  wire [1:0] sub_mb_type,
	input  wire       sub_mb_valid,
	output reg        is_p_skip,
	output reg        is_inter,
	output reg        is_intra,
	output reg        uses_sub_mb,
	output reg        ref0_only,
	output reg        unsupported,
	output reg  [2:0] part_mode,
	output reg  [2:0] mb_part_count,
	output reg  [4:0] mb_part_w,
	output reg  [4:0] mb_part_h,
	output reg  [2:0] sub_part_count,
	output reg  [3:0] sub_part_w,
	output reg  [3:0] sub_part_h
);
	localparam [2:0] PART_P16x16 = 3'd0;
	localparam [2:0] PART_P16x8  = 3'd1;
	localparam [2:0] PART_P8x16  = 3'd2;
	localparam [2:0] PART_P8x8   = 3'd3;
	localparam [2:0] PART_SUB    = 3'd4;
	localparam [2:0] PART_INTRA  = 3'd7;

	always @* begin
		is_p_skip = skipped;
		is_inter = skipped;
		is_intra = 1'b0;
		uses_sub_mb = 1'b0;
		ref0_only = 1'b0;
		unsupported = 1'b0;
		part_mode = skipped ? PART_P16x16 : PART_INTRA;
		mb_part_count = skipped ? 3'd1 : 3'd0;
		mb_part_w = skipped ? 5'd16 : 5'd0;
		mb_part_h = skipped ? 5'd16 : 5'd0;
		sub_part_count = 3'd0;
		sub_part_w = 4'd0;
		sub_part_h = 4'd0;

		if (!skipped) begin
			if (mb_type == 6'd0) begin
				is_inter = 1'b1;
				part_mode = PART_P16x16;
				mb_part_count = 3'd1;
				mb_part_w = 5'd16;
				mb_part_h = 5'd16;
			end else if (mb_type == 6'd1) begin
				is_inter = 1'b1;
				part_mode = PART_P16x8;
				mb_part_count = 3'd2;
				mb_part_w = 5'd16;
				mb_part_h = 5'd8;
			end else if (mb_type == 6'd2) begin
				is_inter = 1'b1;
				part_mode = PART_P8x16;
				mb_part_count = 3'd2;
				mb_part_w = 5'd8;
				mb_part_h = 5'd16;
			end else if (mb_type == 6'd3 || mb_type == 6'd4) begin
				is_inter = 1'b1;
				uses_sub_mb = 1'b1;
				ref0_only = (mb_type == 6'd4);
				part_mode = PART_P8x8;
				mb_part_count = 3'd4;
				mb_part_w = 5'd8;
				mb_part_h = 5'd8;
			end else if (mb_type >= 6'd5 && mb_type <= 6'd30) begin
				is_intra = 1'b1;
				part_mode = PART_INTRA;
			end else begin
				unsupported = 1'b1;
				part_mode = PART_INTRA;
			end
		end

		if (uses_sub_mb) begin
			part_mode = PART_SUB;
			if (!sub_mb_valid) begin
				sub_part_count = 3'd0;
				sub_part_w = 4'd0;
				sub_part_h = 4'd0;
			end else if (sub_mb_type == 2'd0) begin
				sub_part_count = 3'd1;
				sub_part_w = 4'd8;
				sub_part_h = 4'd8;
			end else if (sub_mb_type == 2'd1) begin
				sub_part_count = 3'd2;
				sub_part_w = 4'd8;
				sub_part_h = 4'd4;
			end else if (sub_mb_type == 2'd2) begin
				sub_part_count = 3'd2;
				sub_part_w = 4'd4;
				sub_part_h = 4'd8;
			end else begin
				sub_part_count = 3'd4;
				sub_part_w = 4'd4;
				sub_part_h = 4'd4;
			end
		end
	end
endmodule

`default_nettype wire
