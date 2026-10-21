// Phase P3 syntax primitives: RBSP EPB removal and registered Exp-Golomb reader.
// Offsets produced/consumed here are RBSP offsets after emulation-prevention-byte removal.

module h264_rbsp_filter (
	input  wire       clk,
	input  wire       reset,
	input  wire       clear,

	input  wire       in_valid,
	input  wire [7:0] in_byte,
	input  wire       in_last,
	output wire       in_ready,

	output reg        out_valid,
	output reg  [7:0] out_byte,
	output reg        out_last,
	output reg [15:0] out_index,
	input  wire       out_ready,

	output reg [15:0] rbsp_len,
	output reg [15:0] epb_removed,
	output reg        done
);
	reg [1:0] zero_count;
	reg       inhibit_skip;

	wire can_accept = !out_valid || out_ready;
	wire skip_epb = in_valid && can_accept && !inhibit_skip && (zero_count == 2'd2) && (in_byte == 8'h03);
	assign in_ready = can_accept;

	always @(posedge clk) begin
		if (reset || clear) begin
			out_valid <= 1'b0;
			out_byte <= 8'd0;
			out_last <= 1'b0;
			out_index <= 16'd0;
			rbsp_len <= 16'd0;
			epb_removed <= 16'd0;
			done <= 1'b0;
			zero_count <= 2'd0;
			inhibit_skip <= 1'b0;
		end else begin
			if (out_valid && out_ready)
				out_valid <= 1'b0;
			if (in_valid && can_accept) begin
				if (skip_epb) begin
					epb_removed <= epb_removed + 16'd1;
					inhibit_skip <= 1'b1;
					if (in_last)
						done <= 1'b1;
				end else begin
					out_valid <= 1'b1;
					out_byte <= in_byte;
					out_last <= in_last;
					out_index <= rbsp_len;
					rbsp_len <= rbsp_len + 16'd1;
					if (in_byte == 8'h00)
						zero_count <= (zero_count == 2'd2) ? 2'd2 : (zero_count + 2'd1);
					else
						zero_count <= 2'd0;
					inhibit_skip <= 1'b0;
					if (in_last)
						done <= 1'b1;
				end
			end
		end
	end
endmodule

module h264_baseline_syntax_parser #(
	parameter int MAX_RBSP_BYTES = 8192
) (
	input  wire              clk,
	input  wire              reset,
	input  wire              clear,

	input  wire [1:0]        mode, // 0=PPS, 1=slice header, 2=macroblock layer
	input  wire [15:0]       start_bit,
	input  wire [4:0]        nal_unit_type,
	input  wire [1:0]        nal_ref_idc,
	input  wire [4:0]        log2_max_frame_num,
	input  wire [4:0]        log2_max_pic_order_cnt_lsb,
	input  wire [2:0]        poc_type_in,
	input  wire signed [7:0] pps_pic_init_qp,
	input  wire              pps_deblock_ctrl,
	input  wire [7:0]        active_slice_type,
	input  wire signed [7:0] qp_in,
	input  wire [2:0]        num_ref_idx_l0_active_minus1,

	input  wire              in_valid,
	input  wire [7:0]        in_byte,
	input  wire              in_last,
	output wire              in_ready,

	output reg               valid,
	output reg               error,
	output reg               unsupported,
	output reg               busy,
	output reg [15:0]        rbsp_bits_consumed,

	output reg [7:0]         pps_id,
	output reg [7:0]         sps_id,
	output reg               entropy_cabac,
	output reg [7:0]         num_ref_idx_l0_default_minus1,
	output reg signed [7:0]  pic_init_qp,
	output reg               deblock_ctrl,

	output reg [15:0]        first_mb_in_slice,
	output reg [7:0]         slice_type,
	output reg [15:0]        frame_num,
	output reg [15:0]        pic_order_cnt_lsb,
	output reg [15:0]        idr_pic_id,
	output reg signed [7:0]  slice_qp_delta,
	output reg signed [7:0]  slice_qp,
	output reg [1:0]         disable_deblocking_idc,
	output reg signed [7:0]  slice_alpha_c0_offset_div2,
	output reg signed [7:0]  slice_beta_offset_div2,
	output reg [15:0]        macroblock_bit_offset,

	output reg [15:0]        mb_skip_run,
	output reg               mb_skipped,
	output reg [7:0]         mb_type,
	output reg [3:0]         partition_mode,
	output reg [15:0]        intra4x4_pred_mode_flags,
	output reg [47:0]        intra4x4_rem_modes,
	output reg [2:0]         intra_chroma_pred_mode,
	output reg [5:0]         coded_block_pattern,
	output reg signed [7:0]  mb_qp_delta,
	output reg signed [7:0]  mb_qp,
	output reg [15:0]        residual_bit_offset
);
	localparam int MAX_BITS = MAX_RBSP_BYTES * 8;
	localparam [1:0] MODE_PPS = 2'd0, MODE_SLICE = 2'd1, MODE_MB = 2'd2;
	localparam [3:0]
		PART_UNKNOWN = 4'd0,
		PART_P_SKIP  = 4'd1,
		PART_P16X16  = 4'd2,
		PART_P16X8   = 4'd3,
		PART_P8X16   = 4'd4,
		PART_P8X8    = 4'd5,
		PART_I_NXN   = 4'd6,
		PART_I16X16  = 4'd7,
		PART_IPCM    = 4'd8;

	reg [MAX_BITS-1:0] rbsp_bits;
	reg [15:0] rbsp_len;
	reg [15:0] bit_pos;
	reg [7:0] st;
	reg [7:0] ret_st;
	reg [7:0] fixed_left;
	reg [31:0] fixed_acc;
	reg [7:0] ue_zero;
	reg [7:0] ue_suffix_left;
	reg [31:0] ue_suffix;
	reg [31:0] ue_value;
	reg [7:0] i_mb_type;
	reg [4:0] i4_idx;
	reg [7:0] mvd_pairs_left;
	reg [2:0] sub_idx;
	reg [5:0] cbp_mapped;

	assign in_ready = !busy && (rbsp_len < MAX_RBSP_BYTES[15:0]);

	function automatic bit rbsp_bit_at;
		input [15:0] idx;
		integer byte_i;
		integer bit_i;
		begin
			byte_i = $signed({16'd0, idx}) >>> 3;
			bit_i = 7 - ($signed({16'd0, idx}) & 32'd7);
			rbsp_bit_at = rbsp_bits[byte_i * 8 + bit_i];
		end
	endfunction

	function automatic signed [7:0] se8_from_ue;
		input [31:0] code;
		reg signed [31:0] tmp;
		begin
			if (code[0])
				tmp = $signed({1'b0, code[31:1]}) + 32'sd1;
			else
				tmp = -$signed({1'b0, code[31:1]});
			se8_from_ue = tmp[7:0];
		end
	endfunction

	function automatic [5:0] cbp_intra_map;
		input [5:0] code;
		begin
			case (code)
			6'd0: cbp_intra_map = 6'd47; 6'd1: cbp_intra_map = 6'd31; 6'd2: cbp_intra_map = 6'd15; 6'd3: cbp_intra_map = 6'd0;
			6'd4: cbp_intra_map = 6'd23; 6'd5: cbp_intra_map = 6'd27; 6'd6: cbp_intra_map = 6'd29; 6'd7: cbp_intra_map = 6'd30;
			6'd8: cbp_intra_map = 6'd7; 6'd9: cbp_intra_map = 6'd11; 6'd10: cbp_intra_map = 6'd13; 6'd11: cbp_intra_map = 6'd14;
			6'd12: cbp_intra_map = 6'd39; 6'd13: cbp_intra_map = 6'd43; 6'd14: cbp_intra_map = 6'd45; 6'd15: cbp_intra_map = 6'd46;
			6'd16: cbp_intra_map = 6'd16; 6'd17: cbp_intra_map = 6'd3; 6'd18: cbp_intra_map = 6'd5; 6'd19: cbp_intra_map = 6'd10;
			6'd20: cbp_intra_map = 6'd12; 6'd21: cbp_intra_map = 6'd19; 6'd22: cbp_intra_map = 6'd21; 6'd23: cbp_intra_map = 6'd26;
			6'd24: cbp_intra_map = 6'd28; 6'd25: cbp_intra_map = 6'd35; 6'd26: cbp_intra_map = 6'd37; 6'd27: cbp_intra_map = 6'd42;
			6'd28: cbp_intra_map = 6'd44; 6'd29: cbp_intra_map = 6'd1; 6'd30: cbp_intra_map = 6'd2; 6'd31: cbp_intra_map = 6'd4;
			6'd32: cbp_intra_map = 6'd8; 6'd33: cbp_intra_map = 6'd17; 6'd34: cbp_intra_map = 6'd18; 6'd35: cbp_intra_map = 6'd20;
			6'd36: cbp_intra_map = 6'd24; 6'd37: cbp_intra_map = 6'd6; 6'd38: cbp_intra_map = 6'd9; 6'd39: cbp_intra_map = 6'd22;
			6'd40: cbp_intra_map = 6'd25; 6'd41: cbp_intra_map = 6'd32; 6'd42: cbp_intra_map = 6'd33; 6'd43: cbp_intra_map = 6'd34;
			6'd44: cbp_intra_map = 6'd36; 6'd45: cbp_intra_map = 6'd40; 6'd46: cbp_intra_map = 6'd38; 6'd47: cbp_intra_map = 6'd41;
			default: cbp_intra_map = 6'd0;
			endcase
		end
	endfunction

	function automatic [5:0] cbp_inter_map;
		input [5:0] code;
		begin
			case (code)
			6'd0: cbp_inter_map = 6'd0; 6'd1: cbp_inter_map = 6'd16; 6'd2: cbp_inter_map = 6'd1; 6'd3: cbp_inter_map = 6'd2;
			6'd4: cbp_inter_map = 6'd4; 6'd5: cbp_inter_map = 6'd8; 6'd6: cbp_inter_map = 6'd32; 6'd7: cbp_inter_map = 6'd3;
			6'd8: cbp_inter_map = 6'd5; 6'd9: cbp_inter_map = 6'd10; 6'd10: cbp_inter_map = 6'd12; 6'd11: cbp_inter_map = 6'd15;
			6'd12: cbp_inter_map = 6'd47; 6'd13: cbp_inter_map = 6'd7; 6'd14: cbp_inter_map = 6'd11; 6'd15: cbp_inter_map = 6'd13;
			6'd16: cbp_inter_map = 6'd14; 6'd17: cbp_inter_map = 6'd6; 6'd18: cbp_inter_map = 6'd9; 6'd19: cbp_inter_map = 6'd31;
			6'd20: cbp_inter_map = 6'd35; 6'd21: cbp_inter_map = 6'd37; 6'd22: cbp_inter_map = 6'd42; 6'd23: cbp_inter_map = 6'd44;
			6'd24: cbp_inter_map = 6'd33; 6'd25: cbp_inter_map = 6'd34; 6'd26: cbp_inter_map = 6'd36; 6'd27: cbp_inter_map = 6'd40;
			6'd28: cbp_inter_map = 6'd39; 6'd29: cbp_inter_map = 6'd43; 6'd30: cbp_inter_map = 6'd45; 6'd31: cbp_inter_map = 6'd46;
			6'd32: cbp_inter_map = 6'd17; 6'd33: cbp_inter_map = 6'd18; 6'd34: cbp_inter_map = 6'd20; 6'd35: cbp_inter_map = 6'd24;
			6'd36: cbp_inter_map = 6'd19; 6'd37: cbp_inter_map = 6'd21; 6'd38: cbp_inter_map = 6'd26; 6'd39: cbp_inter_map = 6'd28;
			6'd40: cbp_inter_map = 6'd23; 6'd41: cbp_inter_map = 6'd27; 6'd42: cbp_inter_map = 6'd29; 6'd43: cbp_inter_map = 6'd30;
			6'd44: cbp_inter_map = 6'd22; 6'd45: cbp_inter_map = 6'd25; 6'd46: cbp_inter_map = 6'd38; 6'd47: cbp_inter_map = 6'd41;
			default: cbp_inter_map = 6'd0;
			endcase
		end
	endfunction

	function automatic is_i_slice;
		input [7:0] t;
		begin
			is_i_slice = (t == 8'd2) || (t == 8'd7);
		end
	endfunction

	function automatic is_p_slice;
		input [7:0] t;
		begin
			is_p_slice = (t == 8'd0) || (t == 8'd5);
		end
	endfunction

	function automatic [7:0] sub_mb_mvd_pairs;
		input [31:0] sub_type;
		begin
			case (sub_type[1:0])
			2'd0: sub_mb_mvd_pairs = 8'd1; // P_L0_8x8
			2'd1: sub_mb_mvd_pairs = 8'd2; // P_L0_8x4
			2'd2: sub_mb_mvd_pairs = 8'd2; // P_L0_4x8
			default: sub_mb_mvd_pairs = 8'd4; // P_L0_4x4
			endcase
		end
	endfunction

	function automatic [5:0] i16_cbp_from_type;
		input [7:0] t;
		reg [7:0] x;
		reg [7:0] c;
		reg [5:0] luma;
		reg [5:0] chroma;
		begin
			x = t - 8'd1;
			c = (x / 8'd4) % 8'd3;
			luma = (x >= 8'd12) ? 6'd15 : 6'd0;
			chroma = {4'd0, c[1:0]};
			i16_cbp_from_type = luma | (chroma << 4);
		end
	endfunction

	localparam [7:0]
		ST_IDLE             = 8'd0,
		ST_BITS             = 8'd1,
		ST_UE_ZERO          = 8'd2,
		ST_UE_SUFFIX        = 8'd3,
		ST_DISPATCH         = 8'd4,
		ST_FINISH           = 8'd5,
		ST_FAIL             = 8'd6,
		ST_PPS_ID           = 8'd10,
		ST_PPS_SPS          = 8'd11,
		ST_PPS_ENTROPY      = 8'd12,
		ST_PPS_BOTTOM       = 8'd13,
		ST_PPS_GROUPS       = 8'd14,
		ST_PPS_REF0         = 8'd15,
		ST_PPS_REF1         = 8'd16,
		ST_PPS_WEIGHTED     = 8'd17,
		ST_PPS_WEIGHTED_BI  = 8'd18,
		ST_PPS_QP           = 8'd19,
		ST_PPS_QS           = 8'd20,
		ST_PPS_CHROMA       = 8'd21,
		ST_PPS_DEBLOCK      = 8'd22,
		ST_PPS_CONSTRAINED  = 8'd23,
		ST_PPS_REDUNDANT    = 8'd24,
		ST_SL_FIRST         = 8'd40,
		ST_SL_TYPE          = 8'd41,
		ST_SL_PPS           = 8'd42,
		ST_SL_FRAME         = 8'd43,
		ST_SL_IDR           = 8'd44,
		ST_SL_POC           = 8'd45,
		ST_SL_IDR_MARK0     = 8'd46,
		ST_SL_IDR_MARK1     = 8'd47,
		ST_SL_REF_MARK      = 8'd48,
		ST_SL_QP_DELTA      = 8'd49,
		ST_SL_DEBLOCK_IDC   = 8'd50,
		ST_SL_ALPHA         = 8'd51,
		ST_SL_BETA          = 8'd52,
		ST_MB_START         = 8'd70,
		ST_MB_P_SKIP        = 8'd71,
		ST_MB_TYPE          = 8'd72,
		ST_MB_I4_FLAG       = 8'd73,
		ST_MB_I4_REM        = 8'd74,
		ST_MB_CHROMA        = 8'd75,
		ST_MB_CBP_INTRA     = 8'd76,
		ST_MB_CBP_INTER     = 8'd77,
		ST_MB_QP_DELTA      = 8'd78,
		ST_MB_SUB_TYPE      = 8'd79,
		ST_MB_MVD_X         = 8'd80,
		ST_MB_MVD_Y         = 8'd81,
		ST_MB_MVD_PAIR_DONE = 8'd82;

	task automatic start_bits;
		input [7:0] nbits;
		input [7:0] next_st;
		begin
			fixed_left <= nbits;
			fixed_acc <= 32'd0;
			ret_st <= next_st;
			st <= ST_BITS;
		end
	endtask

	task automatic start_ue;
		input [7:0] next_st;
		begin
			ue_zero <= 8'd0;
			ue_suffix_left <= 8'd0;
			ue_suffix <= 32'd0;
			ue_value <= 32'd0;
			ret_st <= next_st;
			st <= ST_UE_ZERO;
		end
	endtask

	task automatic fail;
		begin
			error <= 1'b1;
			valid <= 1'b0;
			busy <= 1'b0;
			st <= ST_FAIL;
		end
	endtask

	task automatic finish_ok;
		begin
			rbsp_bits_consumed <= bit_pos;
			valid <= 1'b1;
			busy <= 1'b0;
			st <= ST_IDLE;
		end
	endtask

	task automatic reset_outputs;
		begin
			valid <= 1'b0;
			error <= 1'b0;
			unsupported <= 1'b0;
			rbsp_bits_consumed <= 16'd0;
			pps_id <= 8'd0;
			sps_id <= 8'd0;
			entropy_cabac <= 1'b0;
			num_ref_idx_l0_default_minus1 <= 8'd0;
			pic_init_qp <= 8'sd26;
			deblock_ctrl <= 1'b0;
			first_mb_in_slice <= 16'd0;
			slice_type <= 8'd0;
			frame_num <= 16'd0;
			pic_order_cnt_lsb <= 16'd0;
			idr_pic_id <= 16'd0;
			slice_qp_delta <= 8'sd0;
			slice_qp <= 8'sd26;
			disable_deblocking_idc <= 2'd0;
			slice_alpha_c0_offset_div2 <= 8'sd0;
			slice_beta_offset_div2 <= 8'sd0;
			macroblock_bit_offset <= 16'd0;
			mb_skip_run <= 16'd0;
			mb_skipped <= 1'b0;
			mb_type <= 8'd0;
			partition_mode <= PART_UNKNOWN;
			intra4x4_pred_mode_flags <= 16'd0;
			intra4x4_rem_modes <= 48'd0;
			intra_chroma_pred_mode <= 3'd0;
			coded_block_pattern <= 6'd0;
			mb_qp_delta <= 8'sd0;
			mb_qp <= qp_in;
			residual_bit_offset <= 16'd0;
			i_mb_type <= 8'd0;
			i4_idx <= 5'd0;
			mvd_pairs_left <= 8'd0;
			sub_idx <= 3'd0;
			cbp_mapped <= 6'd0;
		end
	endtask

	always @(posedge clk) begin
		if (reset || clear) begin
			rbsp_len <= 16'd0;
			bit_pos <= 16'd0;
			busy <= 1'b0;
			st <= ST_IDLE;
			ret_st <= ST_IDLE;
			fixed_left <= 8'd0;
			fixed_acc <= 32'd0;
			ue_zero <= 8'd0;
			ue_suffix_left <= 8'd0;
			ue_suffix <= 32'd0;
			ue_value <= 32'd0;
			reset_outputs();
		end else begin
			if (!busy && in_valid && in_ready) begin
				rbsp_bits[rbsp_len * 8 +: 8] <= in_byte;
				rbsp_len <= rbsp_len + 16'd1;
				if (in_last) begin
					reset_outputs();
					busy <= 1'b1;
					bit_pos <= start_bit;
					st <= ST_DISPATCH;
				end
			end else if (busy) begin
				case (st)
				ST_BITS: begin
					if (fixed_left == 8'd0) begin
						st <= ret_st;
					end else if (bit_pos >= (rbsp_len * 16'd8)) begin
						fail();
					end else begin
						fixed_acc <= (fixed_acc << 1) | {31'd0, rbsp_bit_at(bit_pos)};
						bit_pos <= bit_pos + 16'd1;
						fixed_left <= fixed_left - 8'd1;
						if (fixed_left == 8'd1)
							st <= ret_st;
					end
				end
				ST_UE_ZERO: begin
					if (bit_pos >= (rbsp_len * 16'd8)) begin
						fail();
					end else if (!rbsp_bit_at(bit_pos)) begin
						bit_pos <= bit_pos + 16'd1;
						if (ue_zero >= 8'd24)
							fail();
						else
							ue_zero <= ue_zero + 8'd1;
					end else begin
						bit_pos <= bit_pos + 16'd1;
						if (ue_zero == 8'd0) begin
							ue_value <= 32'd0;
							st <= ret_st;
						end else begin
							ue_suffix_left <= ue_zero;
							ue_suffix <= 32'd0;
							st <= ST_UE_SUFFIX;
						end
					end
				end
				ST_UE_SUFFIX: begin
					if (bit_pos >= (rbsp_len * 16'd8)) begin
						fail();
					end else begin
						ue_suffix <= (ue_suffix << 1) | {31'd0, rbsp_bit_at(bit_pos)};
						bit_pos <= bit_pos + 16'd1;
						if (ue_suffix_left == 8'd1) begin
							ue_value <= ((32'd1 << ue_zero) - 32'd1) + ((ue_suffix << 1) | {31'd0, rbsp_bit_at(bit_pos)});
							st <= ret_st;
						end
						ue_suffix_left <= ue_suffix_left - 8'd1;
					end
				end
				ST_DISPATCH: begin
					if (mode == MODE_PPS)
						start_ue(ST_PPS_ID);
					else if (mode == MODE_SLICE)
						start_ue(ST_SL_FIRST);
					else if (mode == MODE_MB)
						st <= ST_MB_START;
					else
						fail();
				end
				ST_FINISH: finish_ok();

				ST_PPS_ID: begin pps_id <= ue_value[7:0]; start_ue(ST_PPS_SPS); end
				ST_PPS_SPS: begin sps_id <= ue_value[7:0]; start_bits(8'd1, ST_PPS_ENTROPY); end
				ST_PPS_ENTROPY: begin entropy_cabac <= fixed_acc[0]; start_bits(8'd1, ST_PPS_BOTTOM); end
				ST_PPS_BOTTOM: start_ue(ST_PPS_GROUPS);
				ST_PPS_GROUPS: begin
					if (ue_value != 32'd0) begin
						unsupported <= 1'b1;
						fail();
					end else begin
						start_ue(ST_PPS_REF0);
					end
				end
				ST_PPS_REF0: begin num_ref_idx_l0_default_minus1 <= ue_value[7:0]; start_ue(ST_PPS_REF1); end
				ST_PPS_REF1: start_bits(8'd1, ST_PPS_WEIGHTED);
				ST_PPS_WEIGHTED: start_bits(8'd2, ST_PPS_WEIGHTED_BI);
				ST_PPS_WEIGHTED_BI: start_ue(ST_PPS_QP);
				ST_PPS_QP: begin pic_init_qp <= 8'sd26 + se8_from_ue(ue_value); start_ue(ST_PPS_QS); end
				ST_PPS_QS: start_ue(ST_PPS_CHROMA);
				ST_PPS_CHROMA: start_bits(8'd1, ST_PPS_DEBLOCK);
				ST_PPS_DEBLOCK: begin deblock_ctrl <= fixed_acc[0]; start_bits(8'd1, ST_PPS_CONSTRAINED); end
				ST_PPS_CONSTRAINED: start_bits(8'd1, ST_PPS_REDUNDANT);
				ST_PPS_REDUNDANT: begin
					if (entropy_cabac) begin
						unsupported <= 1'b1;
						fail();
					end else begin
						st <= ST_FINISH;
					end
				end

				ST_SL_FIRST: begin first_mb_in_slice <= ue_value[15:0]; start_ue(ST_SL_TYPE); end
				ST_SL_TYPE: begin slice_type <= ue_value[7:0]; start_ue(ST_SL_PPS); end
				ST_SL_PPS: begin pps_id <= ue_value[7:0]; start_bits({3'd0, log2_max_frame_num}, ST_SL_FRAME); end
				ST_SL_FRAME: begin
					frame_num <= fixed_acc[15:0];
					if (nal_unit_type == 5'd5)
						start_ue(ST_SL_IDR);
					else if (poc_type_in == 3'd0)
						start_bits({3'd0, log2_max_pic_order_cnt_lsb}, ST_SL_POC);
					else if (nal_ref_idc != 2'd0)
						start_bits(8'd1, ST_SL_REF_MARK);
					else
						start_ue(ST_SL_QP_DELTA);
				end
				ST_SL_IDR: begin idr_pic_id <= ue_value[15:0]; start_bits(8'd1, ST_SL_IDR_MARK0); end
				ST_SL_POC: begin
					pic_order_cnt_lsb <= fixed_acc[15:0];
					if (nal_ref_idc != 2'd0)
						start_bits(8'd1, ST_SL_REF_MARK);
					else
						start_ue(ST_SL_QP_DELTA);
				end
				ST_SL_IDR_MARK0: start_bits(8'd1, ST_SL_IDR_MARK1);
				ST_SL_IDR_MARK1: start_ue(ST_SL_QP_DELTA);
				ST_SL_REF_MARK: begin
					if (fixed_acc[0]) begin
						unsupported <= 1'b1;
						fail();
					end else begin
						start_ue(ST_SL_QP_DELTA);
					end
				end
				ST_SL_QP_DELTA: begin
					slice_qp_delta <= se8_from_ue(ue_value);
					slice_qp <= pps_pic_init_qp + se8_from_ue(ue_value);
					if (pps_deblock_ctrl)
						start_ue(ST_SL_DEBLOCK_IDC);
					else begin
						macroblock_bit_offset <= bit_pos;
						st <= ST_FINISH;
					end
				end
				ST_SL_DEBLOCK_IDC: begin
					disable_deblocking_idc <= ue_value[1:0];
					if (ue_value != 32'd1)
						start_ue(ST_SL_ALPHA);
					else begin
						macroblock_bit_offset <= bit_pos;
						st <= ST_FINISH;
					end
				end
				ST_SL_ALPHA: begin slice_alpha_c0_offset_div2 <= se8_from_ue(ue_value); start_ue(ST_SL_BETA); end
				ST_SL_BETA: begin
					slice_beta_offset_div2 <= se8_from_ue(ue_value);
					macroblock_bit_offset <= bit_pos;
					st <= ST_FINISH;
				end

				ST_MB_START: begin
					if (is_p_slice(active_slice_type))
						start_ue(ST_MB_P_SKIP);
					else if (is_i_slice(active_slice_type))
						start_ue(ST_MB_TYPE);
					else begin
						unsupported <= 1'b1;
						fail();
					end
				end
				ST_MB_P_SKIP: begin
					mb_skip_run <= ue_value[15:0];
					if (ue_value != 32'd0) begin
						mb_skipped <= 1'b1;
						partition_mode <= PART_P_SKIP;
						residual_bit_offset <= bit_pos;
						mb_qp <= qp_in;
						st <= ST_FINISH;
					end else begin
						start_ue(ST_MB_TYPE);
					end
				end
				ST_MB_TYPE: begin
					mb_type <= ue_value[7:0];
					mb_qp <= qp_in;
					if (is_p_slice(active_slice_type) && ue_value <= 32'd4) begin
						if (num_ref_idx_l0_active_minus1 != 3'd0) begin
							unsupported <= 1'b1;
							fail();
						end else begin
							case (ue_value[2:0])
							3'd0: begin partition_mode <= PART_P16X16; mvd_pairs_left <= 8'd1; st <= ST_MB_MVD_X; end
							3'd1: begin partition_mode <= PART_P16X8;  mvd_pairs_left <= 8'd2; st <= ST_MB_MVD_X; end
							3'd2: begin partition_mode <= PART_P8X16;  mvd_pairs_left <= 8'd2; st <= ST_MB_MVD_X; end
							default: begin partition_mode <= PART_P8X8; sub_idx <= 3'd0; mvd_pairs_left <= 8'd0; start_ue(ST_MB_SUB_TYPE); end
							endcase
						end
					end else begin
						i_mb_type <= is_p_slice(active_slice_type) ? (ue_value[7:0] - 8'd5) : ue_value[7:0];
						if ((is_p_slice(active_slice_type) && ue_value > 32'd30) || (!is_p_slice(active_slice_type) && ue_value > 32'd25)) begin
							unsupported <= 1'b1;
							fail();
						end else if ((is_p_slice(active_slice_type) ? (ue_value[7:0] - 8'd5) : ue_value[7:0]) == 8'd25) begin
							partition_mode <= PART_IPCM;
							unsupported <= 1'b1;
							residual_bit_offset <= bit_pos;
							st <= ST_FINISH;
						end else if ((is_p_slice(active_slice_type) ? (ue_value[7:0] - 8'd5) : ue_value[7:0]) == 8'd0) begin
							partition_mode <= PART_I_NXN;
							i4_idx <= 5'd0;
							start_bits(8'd1, ST_MB_I4_FLAG);
						end else begin
							partition_mode <= PART_I16X16;
							coded_block_pattern <= i16_cbp_from_type(is_p_slice(active_slice_type) ? (ue_value[7:0] - 8'd5) : ue_value[7:0]);
							start_ue(ST_MB_CHROMA);
						end
					end
				end
				ST_MB_I4_FLAG: begin
					if (fixed_acc[0]) begin
						intra4x4_pred_mode_flags[i4_idx[3:0]] <= 1'b1;
						if (i4_idx == 5'd15)
							start_ue(ST_MB_CHROMA);
						else begin
							i4_idx <= i4_idx + 5'd1;
							start_bits(8'd1, ST_MB_I4_FLAG);
						end
					end else begin
						start_bits(8'd3, ST_MB_I4_REM);
					end
				end
				ST_MB_I4_REM: begin
					intra4x4_rem_modes[i4_idx * 3 +: 3] <= fixed_acc[2:0];
					if (i4_idx == 5'd15)
						start_ue(ST_MB_CHROMA);
					else begin
						i4_idx <= i4_idx + 5'd1;
						start_bits(8'd1, ST_MB_I4_FLAG);
					end
				end
				ST_MB_CHROMA: begin
					intra_chroma_pred_mode <= ue_value[2:0];
					if (partition_mode == PART_I16X16)
						start_ue(ST_MB_QP_DELTA);
					else
						start_ue(ST_MB_CBP_INTRA);
				end
				ST_MB_CBP_INTRA: begin
					if (ue_value >= 32'd48) begin
						unsupported <= 1'b1;
						fail();
					end else begin
						cbp_mapped <= cbp_intra_map(ue_value[5:0]);
						coded_block_pattern <= cbp_intra_map(ue_value[5:0]);
						if (cbp_intra_map(ue_value[5:0]) != 6'd0)
							start_ue(ST_MB_QP_DELTA);
						else begin
							residual_bit_offset <= bit_pos;
							st <= ST_FINISH;
						end
					end
				end
				ST_MB_SUB_TYPE: begin
					if (ue_value > 32'd3) begin
						unsupported <= 1'b1;
						fail();
					end else begin
						mvd_pairs_left <= mvd_pairs_left + sub_mb_mvd_pairs(ue_value);
						if (sub_idx == 3'd3)
							st <= ST_MB_MVD_X;
						else begin
							sub_idx <= sub_idx + 3'd1;
							start_ue(ST_MB_SUB_TYPE);
						end
					end
				end
				ST_MB_MVD_X: begin
					if (mvd_pairs_left == 8'd0)
						start_ue(ST_MB_CBP_INTER);
					else
						start_ue(ST_MB_MVD_Y);
				end
				ST_MB_MVD_Y: begin
					start_ue(ST_MB_MVD_PAIR_DONE);
				end
				ST_MB_MVD_PAIR_DONE: begin
					mvd_pairs_left <= mvd_pairs_left - 8'd1;
					st <= ST_MB_MVD_X;
				end
				ST_MB_CBP_INTER: begin
					if (ue_value >= 32'd48) begin
						unsupported <= 1'b1;
						fail();
					end else begin
						coded_block_pattern <= cbp_inter_map(ue_value[5:0]);
						if (cbp_inter_map(ue_value[5:0]) != 6'd0)
							start_ue(ST_MB_QP_DELTA);
						else begin
							residual_bit_offset <= bit_pos;
							st <= ST_FINISH;
						end
					end
				end
				ST_MB_QP_DELTA: begin
					mb_qp_delta <= se8_from_ue(ue_value);
					mb_qp <= qp_in + se8_from_ue(ue_value);
					residual_bit_offset <= bit_pos;
					st <= ST_FINISH;
				end
				ST_FAIL: busy <= 1'b0;
				default: fail();
				endcase
			end
		end
	end
endmodule

module h264_exp_golomb_reader #(
	parameter int MAX_LEADING_ZERO = 24
) (
	input  wire        clk,
	input  wire        reset,
	input  wire        start,
	input  wire        signed_mode,
	input  wire        bit_valid,
	input  wire        bit_value,
	output wire        bit_ready,

	output reg         busy,
	output reg         done,
	output reg         ok,
	output reg [31:0]  ue_value,
	output reg signed [31:0] se_value,
	output reg [7:0]   bits_consumed
);
	localparam [1:0] ST_IDLE = 2'd0;
	localparam [1:0] ST_ZERO = 2'd1;
	localparam [1:0] ST_SUFFIX = 2'd2;
	localparam [1:0] ST_DONE = 2'd3;

	reg [1:0] st;
	reg [7:0] zero_count;
	reg [7:0] suffix_left;
	reg [31:0] suffix_acc;
	reg [31:0] code_num;

	assign bit_ready = (st == ST_ZERO) || (st == ST_SUFFIX);

	function automatic signed [31:0] se_map;
		input [31:0] code;
		begin
			if (code[0])
				se_map = $signed({1'b0, code[31:1]}) + 32'sd1;
			else
				se_map = -$signed({1'b0, code[31:1]});
		end
	endfunction

	always @(posedge clk) begin
		if (reset) begin
			st <= ST_IDLE;
			busy <= 1'b0;
			done <= 1'b0;
			ok <= 1'b0;
			ue_value <= 32'd0;
			se_value <= 32'sd0;
			bits_consumed <= 8'd0;
			zero_count <= 8'd0;
			suffix_left <= 8'd0;
			suffix_acc <= 32'd0;
			code_num <= 32'd0;
		end else begin
			done <= 1'b0;
			case (st)
			ST_IDLE: begin
				busy <= 1'b0;
				if (start) begin
					busy <= 1'b1;
					ok <= 1'b0;
					ue_value <= 32'd0;
					se_value <= 32'sd0;
					bits_consumed <= 8'd0;
					zero_count <= 8'd0;
					suffix_left <= 8'd0;
					suffix_acc <= 32'd0;
					code_num <= 32'd0;
					st <= ST_ZERO;
				end
			end
			ST_ZERO: begin
				if (bit_valid) begin
					bits_consumed <= bits_consumed + 8'd1;
					if (!bit_value) begin
						if (zero_count >= MAX_LEADING_ZERO[7:0]) begin
							ok <= 1'b0;
							done <= 1'b1;
							busy <= 1'b0;
							st <= ST_DONE;
						end else begin
							zero_count <= zero_count + 8'd1;
						end
					end else if (zero_count == 8'd0) begin
						code_num <= 32'd0;
						ue_value <= 32'd0;
						se_value <= 32'd0;
						ok <= 1'b1;
						done <= 1'b1;
						busy <= 1'b0;
						st <= ST_DONE;
					end else begin
						suffix_left <= zero_count;
						suffix_acc <= 32'd0;
						st <= ST_SUFFIX;
					end
				end
			end
			ST_SUFFIX: begin
				if (bit_valid) begin
					bits_consumed <= bits_consumed + 8'd1;
					suffix_acc <= (suffix_acc << 1) | {31'd0, bit_value};
					if (suffix_left == 8'd1) begin
						code_num <= ((32'd1 << zero_count) - 32'd1) + ((suffix_acc << 1) | {31'd0, bit_value});
						ue_value <= ((32'd1 << zero_count) - 32'd1) + ((suffix_acc << 1) | {31'd0, bit_value});
						se_value <= signed_mode ? se_map(((32'd1 << zero_count) - 32'd1) + ((suffix_acc << 1) | {31'd0, bit_value})) : $signed(((32'd1 << zero_count) - 32'd1) + ((suffix_acc << 1) | {31'd0, bit_value}));
						ok <= 1'b1;
						done <= 1'b1;
						busy <= 1'b0;
						st <= ST_DONE;
					end else begin
						suffix_left <= suffix_left - 8'd1;
					end
				end
			end
			ST_DONE: begin
				if (!start)
					st <= ST_IDLE;
			end
			default: st <= ST_IDLE;
			endcase
		end
	end
endmodule

module h264_sps_geometry_parser #(
	parameter int MAX_RBSP_BYTES = 128
) (
	input  wire        clk,
	input  wire        reset,
	input  wire        clear,

	input  wire        in_valid,
	input  wire [7:0]  in_byte,
	input  wire        in_last,
	output wire        in_ready,

	output reg         valid,
	output reg         error,
	output reg [7:0]   profile_idc,
	output reg [7:0]   level_idc,
	output reg [4:0]   log2_max_frame_num,
	output reg [2:0]   poc_type,
	output reg [15:0]  coded_width,
	output reg [15:0]  coded_height,
	output reg [15:0]  display_width,
	output reg [15:0]  display_height,
	output reg [15:0]  crop_left,
	output reg [15:0]  crop_right,
	output reg [15:0]  crop_top,
	output reg [15:0]  crop_bottom,
	output reg [15:0]  rbsp_bits_consumed,
	output reg         busy
);
	localparam int MAX_BITS = MAX_RBSP_BYTES * 8;
	reg [MAX_BITS-1:0] rbsp_bits;
	reg [15:0] rbsp_len;
	reg [15:0] bit_pos;

	reg [5:0] st;
	reg [5:0] ret_st;
	reg [7:0] fixed_left;
	reg [31:0] fixed_acc;
	reg [7:0] ue_zero;
	reg [7:0] ue_suffix_left;
	reg [31:0] ue_suffix;
	reg [31:0] ue_value;
	reg [15:0] pic_width_mbs;
	reg [15:0] pic_height_map_units;
	reg frame_mbs_only;
	reg crop_flag;

	assign in_ready = !busy && (rbsp_len < MAX_RBSP_BYTES[15:0]);

	function automatic bit rbsp_bit_at;
		input [15:0] idx;
		integer byte_i;
		integer bit_i;
		begin
			byte_i = $signed({16'd0, idx}) >>> 3;
			bit_i = 7 - ($signed({16'd0, idx}) & 32'd7);
			rbsp_bit_at = rbsp_bits[byte_i * 8 + bit_i];
		end
	endfunction

	localparam [5:0]
		ST_IDLE       = 6'd0,
		ST_BITS       = 6'd1,
		ST_UE_ZERO    = 6'd2,
		ST_UE_SUFFIX  = 6'd3,
		ST_PROFILE    = 6'd4,
		ST_CONSTRAINT = 6'd5,
		ST_LEVEL      = 6'd6,
		ST_SPS_ID     = 6'd7,
		ST_LOG2_FN    = 6'd8,
		ST_POC_TYPE   = 6'd9,
		ST_POC_LSB    = 6'd10,
		ST_REF_FRAMES = 6'd11,
		ST_GAPS       = 6'd12,
		ST_WIDTH      = 6'd13,
		ST_HEIGHT     = 6'd14,
		ST_FRAME_ONLY = 6'd15,
		ST_MBAFF      = 6'd16,
		ST_DIRECT     = 6'd17,
		ST_CROP_FLAG  = 6'd18,
		ST_CROP_L     = 6'd19,
		ST_CROP_R     = 6'd20,
		ST_CROP_T     = 6'd21,
		ST_CROP_B     = 6'd22,
		ST_FINISH     = 6'd23,
		ST_FAIL       = 6'd24;

	task automatic start_bits;
		input [7:0] nbits;
		input [5:0] next_st;
		begin
			fixed_left <= nbits;
			fixed_acc <= 32'd0;
			ret_st <= next_st;
			st <= ST_BITS;
		end
	endtask

	task automatic start_ue;
		input [5:0] next_st;
		begin
			ue_zero <= 8'd0;
			ue_suffix_left <= 8'd0;
			ue_suffix <= 32'd0;
			ue_value <= 32'd0;
			ret_st <= next_st;
			st <= ST_UE_ZERO;
		end
	endtask

	task automatic fail;
		begin
			error <= 1'b1;
			valid <= 1'b0;
			busy <= 1'b0;
			st <= ST_FAIL;
		end
	endtask

	always @(posedge clk) begin
		if (reset || clear) begin
			rbsp_bits <= {MAX_BITS{1'b0}};
			rbsp_len <= 16'd0;
			bit_pos <= 16'd0;
			valid <= 1'b0;
			error <= 1'b0;
			profile_idc <= 8'd0;
			level_idc <= 8'd0;
			log2_max_frame_num <= 5'd0;
			poc_type <= 3'd0;
			coded_width <= 16'd0;
			coded_height <= 16'd0;
			display_width <= 16'd0;
			display_height <= 16'd0;
			crop_left <= 16'd0;
			crop_right <= 16'd0;
			crop_top <= 16'd0;
			crop_bottom <= 16'd0;
			rbsp_bits_consumed <= 16'd0;
			busy <= 1'b0;
			st <= ST_IDLE;
			ret_st <= ST_IDLE;
			fixed_left <= 8'd0;
			fixed_acc <= 32'd0;
			ue_zero <= 8'd0;
			ue_suffix_left <= 8'd0;
			ue_suffix <= 32'd0;
			ue_value <= 32'd0;
			pic_width_mbs <= 16'd0;
			pic_height_map_units <= 16'd0;
			frame_mbs_only <= 1'b1;
			crop_flag <= 1'b0;
		end else begin
			if (!busy && in_valid && in_ready) begin
				rbsp_bits[rbsp_len * 8 +: 8] <= in_byte;
				rbsp_len <= rbsp_len + 16'd1;
				if (in_last) begin
					busy <= 1'b1;
					valid <= 1'b0;
					error <= 1'b0;
					bit_pos <= 16'd0;
					crop_left <= 16'd0;
					crop_right <= 16'd0;
					crop_top <= 16'd0;
					crop_bottom <= 16'd0;
					start_bits(8'd8, ST_PROFILE);
				end
			end else if (busy) begin
				case (st)
				ST_BITS: begin
					if (fixed_left == 8'd0) begin
						st <= ret_st;
					end else if (bit_pos >= (rbsp_len * 16'd8)) begin
						fail();
					end else begin
						fixed_acc <= (fixed_acc << 1) | {31'd0, rbsp_bit_at(bit_pos)};
						bit_pos <= bit_pos + 16'd1;
						fixed_left <= fixed_left - 8'd1;
						if (fixed_left == 8'd1)
							st <= ret_st;
					end
				end
				ST_UE_ZERO: begin
					if (bit_pos >= (rbsp_len * 16'd8)) begin
						fail();
					end else if (!rbsp_bit_at(bit_pos)) begin
						bit_pos <= bit_pos + 16'd1;
						if (ue_zero >= 8'd24)
							fail();
						else
							ue_zero <= ue_zero + 8'd1;
					end else begin
						bit_pos <= bit_pos + 16'd1;
						if (ue_zero == 8'd0) begin
							ue_value <= 32'd0;
							st <= ret_st;
						end else begin
							ue_suffix_left <= ue_zero;
							ue_suffix <= 32'd0;
							st <= ST_UE_SUFFIX;
						end
					end
				end
				ST_UE_SUFFIX: begin
					if (bit_pos >= (rbsp_len * 16'd8)) begin
						fail();
					end else begin
						ue_suffix <= (ue_suffix << 1) | {31'd0, rbsp_bit_at(bit_pos)};
						bit_pos <= bit_pos + 16'd1;
						if (ue_suffix_left == 8'd1) begin
							ue_value <= ((32'd1 << ue_zero) - 32'd1) + ((ue_suffix << 1) | {31'd0, rbsp_bit_at(bit_pos)});
							st <= ret_st;
						end
						ue_suffix_left <= ue_suffix_left - 8'd1;
					end
				end
				ST_PROFILE: begin
					profile_idc <= fixed_acc[7:0];
					start_bits(8'd8, ST_CONSTRAINT);
				end
				ST_CONSTRAINT: start_bits(8'd8, ST_LEVEL);
				ST_LEVEL: begin
					level_idc <= fixed_acc[7:0];
					start_ue(ST_SPS_ID);
				end
				ST_SPS_ID: start_ue(ST_LOG2_FN);
				ST_LOG2_FN: begin
					if (ue_value > 32'd12)
						fail();
					else begin
						log2_max_frame_num <= ue_value[4:0] + 5'd4;
						start_ue(ST_POC_TYPE);
					end
				end
				ST_POC_TYPE: begin
					poc_type <= ue_value[2:0];
					if (ue_value == 32'd0)
						start_ue(ST_POC_LSB);
					else if (ue_value == 32'd2)
						start_ue(ST_REF_FRAMES);
					else
						fail();
				end
				ST_POC_LSB: start_ue(ST_REF_FRAMES);
				ST_REF_FRAMES: start_bits(8'd1, ST_GAPS);
				ST_GAPS: start_ue(ST_WIDTH);
				ST_WIDTH: begin
					pic_width_mbs <= ue_value[15:0] + 16'd1;
					start_ue(ST_HEIGHT);
				end
				ST_HEIGHT: begin
					pic_height_map_units <= ue_value[15:0] + 16'd1;
					start_bits(8'd1, ST_FRAME_ONLY);
				end
				ST_FRAME_ONLY: begin
					frame_mbs_only <= fixed_acc[0];
					if (!fixed_acc[0])
						start_bits(8'd1, ST_MBAFF);
					else
						start_bits(8'd1, ST_DIRECT);
				end
				ST_MBAFF: start_bits(8'd1, ST_DIRECT);
				ST_DIRECT: start_bits(8'd1, ST_CROP_FLAG);
				ST_CROP_FLAG: begin
					crop_flag <= fixed_acc[0];
					if (fixed_acc[0])
						start_ue(ST_CROP_L);
					else
						st <= ST_FINISH;
				end
				ST_CROP_L: begin crop_left <= ue_value[15:0]; start_ue(ST_CROP_R); end
				ST_CROP_R: begin crop_right <= ue_value[15:0]; start_ue(ST_CROP_T); end
				ST_CROP_T: begin crop_top <= ue_value[15:0]; start_ue(ST_CROP_B); end
				ST_CROP_B: begin crop_bottom <= ue_value[15:0]; st <= ST_FINISH; end
				ST_FINISH: begin
					coded_width <= pic_width_mbs << 4;
					coded_height <= pic_height_map_units << (frame_mbs_only ? 4 : 5);
					display_width <= (pic_width_mbs << 4) - ((crop_left + crop_right) << 1);
					display_height <= (pic_height_map_units << (frame_mbs_only ? 4 : 5)) - ((crop_top + crop_bottom) << 1);
					rbsp_bits_consumed <= bit_pos;
					valid <= 1'b1;
					busy <= 1'b0;
					st <= ST_IDLE;
				end
				ST_FAIL: busy <= 1'b0;
				default: fail();
				endcase
			end
		end
	end
endmodule
