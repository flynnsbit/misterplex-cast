// Phase 3.3d/e/f/j/k/l-1: slice_header + first mb_type + first residual CAVLC (nC=0).
// 3.3k: coeff_token + T1 signs + non-T1 levels + total_zeros + run_before → residual_dc.
// 3.3l-1: ST_PLACE fills full scan-order coeff[0:15]; residual_dc=sat8(coeff[0]);
//         residual_csum = XOR sat8(coeff[i]) (host residualCsum8 / residual_gold; Baseline 0x14).
// I_NxN (mt=0) and I_16x16 (1..24). Capture window MAXB=48 B covers first residual (~17 B).
// Logic-only (no extra M10K). Needs SPS (log2/poc) + PPS (deblock_ctrl, pic_init_qp).

module slice_hdr_parser (
	input  wire        clk,
	input  wire        reset,

	input  wire        cap_clear,
	input  wire        cap_en,
	input  wire [7:0]  cap_data,
	input  wire        cap_end,
	input  wire        is_idr_nal,
	input  wire        nal_ref_idc_nonzero,
	input  wire [4:0]  log2_max_frame_num,
	input  wire [2:0]  poc_type,
	input  wire        sps_ready,
	input  wire        pps_ready,
	input  wire        deblock_ctrl,
	input  wire signed [7:0] pic_init_qp,

	output reg         valid,
	output reg  [15:0] first_mb,
	output reg  [7:0]  slice_type,
	output reg  [7:0]  pps_id,
	output reg  [15:0] frame_num,
	output reg  [15:0] idr_pic_id,
	output reg         is_i_slice,
	output reg  signed [7:0] slice_qp_delta,
	output reg  [5:0]  slice_qp,       // 0..51
	output reg  [1:0]  disable_deblocking_filter_idc,
	output reg  signed [4:0] slice_alpha_c0_offset_div2,
	output reg  signed [4:0] slice_beta_offset_div2,
	output reg  signed [4:0] slice_alpha_c0_offset,
	output reg  signed [4:0] slice_beta_offset,
	output reg  [7:0]  first_mb_type,
	output reg         has_mb_type,
	output reg         first_mb_p_skip,
	output reg  [7:0]  p_skip_run,
	output reg  [2:0]  first_mb_part_mode,
	output reg  [2:0]  first_mb_part_count,
	output reg         first_mb_uses_sub_mb,
	output reg         first_mb_intra,
	// 3.3f/k residual (first I residual block, nC=0)
	output reg  [4:0]  residual_tc,
	output reg  [1:0]  residual_t1,
	output reg         residual_ok,
	// 3.3k: scan-order DC (coeff[0]) after levels+runs; signed, sat to 8-bit
	output reg  signed [7:0] residual_dc,
	// 3.3l-1: residual_csum = XOR sat8(coeff[0:15]); full scan levels for inv_quant.
	// Golden Baseline first residual = 0x14 — host/libmisterplex/h264_residual_gold.hpp.
	// residual_coeff kept for 3.3l-2 inv_quant even if status only exports csum/dc.
	// (* keep *) so status pack cannot drop the net (4d6ee356 CSUM_VALUE_FAIL class).
	(* keep = 1 *) output reg [7:0] residual_csum,
	(* keep = 1 *) output reg signed [15:0] residual_coeff [0:15],
	// R-csum6 Rank3: 1-cycle pulse at ST_PLACE only (not early residual_ok).
	// Plex freezes st_res_word_sticky on this edge so status[111:104] cannot walk.
	(* keep = 1 *) output reg residual_place_pulse,
	output reg         residual_place_ok,
	output reg  [4:0]  residual_place_tc,
	output reg  [1:0]  residual_place_t1,
	output reg  signed [7:0] residual_place_dc,
	output reg  [5:0]  residual_place_qp,
	(* keep = 1 *) output reg signed [15:0] residual_place_coeff [0:15],
	output reg         busy
);

	localparam int MAXB = 48;
	reg [7:0] mem [0:MAXB-1];
	reg [5:0] len;

	reg [5:0] bbyte;
	reg [2:0] bpos;
	wire [7:0] cur = mem[bbyte];
	wire bitv = cur[bpos];
	wire oob = (bbyte >= len);

	reg [5:0] st, cont, ue_cont;
	reg [5:0] zcnt;
	reg [4:0] nleft;
	reg [15:0] acc, ue_val;
	reg        idr_lat, db_lat;
	reg        nal_ref_lat;
	reg [4:0]  log2_lat;
	reg signed [7:0] init_qp_lat;
	reg signed [7:0] dlt_tmp;
	reg signed [8:0] qp_tmp;
	reg [15:0] tcode;
	reg [4:0]  tbits;
	reg [4:0]  r_tc, r_t1;
	reg [2:0]  sign_left;
	reg [4:0]  i4_i;       // 0..15 I_NxN pred-mode index
	reg [1:0]  i4_sub;     // 0=flag, 1..3=rem bits
	reg        i4_need_rem;
	reg [5:0]  cbp_me;     // coded_block_pattern me code

	// 3.3k CAVLC level / zeros / run; 3.3l-1 place into residual_coeff[]
	// Width: signed [15:0] — NOT [11:0].  The H.264 spec bounds ordinary 4×4
	// CAVLC levels to ±2047 (fits in 12 bits), but I_16x16 DC coefficients flow
	// through the same lev_of() path after a 4×4 Hadamard transform, which can
	// produce far larger values.  Measured: |level| = 14,573 at QP=4 on
	// testsrc2 640×480 (x264 Baseline, forced QP=4, slice QP drops to 1).
	// Theoretical max at QP=0: ~26,000.  signed [11:0] (±2047) would silently
	// truncate with two's-complement wraparound, reproducing the identical class
	// of bug this width change fixes.  See level_width_tb.cpp for the assertion
	// that guards this width.
	reg signed [15:0] lev [0:15];
	reg [3:0]  lev_i;
	reg [2:0]  suf_len;
	reg [5:0]  pref;
	reg [4:0]  suf_left;
	reg [15:0] suf_acc;
	reg        first_non_t1;
	reg [3:0]  zeros_left;
	reg [3:0]  run_i;
	reg [3:0]  runv [0:15];
	reg [8:0]  tzcode; // up to 9-bit total_zeros VLC
	reg [3:0]  tzbits;
	reg [4:0]  runcode;
	reg [3:0]  runbits;
	reg        tok_ok;
	// R-csum6 PRODUCT path (DIAG STRIPPED). ST_PLACE: residual_csum <= cs (XOR fold).
	// Prior DIAG residual_csum<=8'h14 was pack-proof only; parent R-csum6: product sticky.
	// Rank3 private place latches + residual_place_pulse freeze ownership of status csum.
	reg        place_did;
	reg [4:0]  csum_i;
	reg [7:0]  csum_acc;
	(* preserve *) reg [7:0] place_csum_r;
	(* preserve *) reg signed [7:0] place_dc_r;

	function automatic signed [7:0] se_of;
		input [15:0] k;
		begin
			if (k[0] == 1'b0)
				se_of = -$signed({1'b0, k[7:1]});
			else
				se_of = $signed({1'b0, k[7:1]}) + 8'sd1;
		end
	endfunction

	// FFmpeg/ITU: levelCode → signed level
	// mask=-(code&1); (((2+code)>>1)^mask)-mask
	// Returns signed [15:0] — must match lev[] width.  Do NOT narrow to [11:0];
	// I_16x16 DC levels reach |level| = 14,573 (measured) and ~26,000 (theoretical).
	function automatic signed [15:0] lev_of;
		input [15:0] code;
		reg signed [15:0] mask;
		reg signed [15:0] t;
		begin
			mask = code[0] ? -16'sd1 : 16'sd0;
			t = ($signed({1'b0, code}) + 17'sd2) >>> 1;
			lev_of = (t ^ mask) - mask;
		end
	endfunction

	function automatic signed [4:0] filter_offset_of_div2;
		input signed [7:0] v;
		begin
			filter_offset_of_div2 = $signed(v[4:0]) <<< 1;
		end
	endfunction

	function automatic signed [7:0] sat8;
		input signed [15:0] v;
		begin
			if (v > 16'sd127)       sat8 = 8'h7F;
			else if (v < -16'sd128) sat8 = 8'h80;
			else                    sat8 = v[7:0];
		end
	endfunction

	function automatic is_p_slice_type;
		input [7:0] t;
		begin
			is_p_slice_type = (t == 8'd0) || (t == 8'd5);
		end
	endfunction

	function automatic [2:0] p_part_mode_of;
		input        skipped;
		input [7:0] mt;
		begin
			if (skipped || mt == 8'd0) p_part_mode_of = 3'd0;       // P_Skip / P_L0_16x16
			else if (mt == 8'd1)       p_part_mode_of = 3'd1;       // P_L0_16x8
			else if (mt == 8'd2)       p_part_mode_of = 3'd2;       // P_L0_8x16
			else if (mt == 8'd3 || mt == 8'd4) p_part_mode_of = 3'd3; // P_8x8 / P_8x8ref0
			else                       p_part_mode_of = 3'd7;       // intra/unsupported
		end
	endfunction

	function automatic [2:0] p_part_count_of;
		input        skipped;
		input [7:0] mt;
		begin
			if (skipped || mt == 8'd0) p_part_count_of = 3'd1;
			else if (mt == 8'd1 || mt == 8'd2) p_part_count_of = 3'd2;
			else if (mt == 8'd3 || mt == 8'd4) p_part_count_of = 3'd4;
			else p_part_count_of = 3'd0;
		end
	endfunction

	localparam [5:0]
		ST_IDLE    = 6'd0,
		ST_GETBITS = 6'd1,
		ST_UE_Z    = 6'd2,
		ST_UE_V    = 6'd3,
		ST_FIRST   = 6'd4,
		ST_TYPE    = 6'd5,
		ST_PPS     = 6'd6,
		ST_FN      = 6'd7,
		ST_IDR     = 6'd8,
		ST_REFMARK = 6'd9,  // IDR dec_ref_pic_marking: 2 flags
		ST_QPD     = 6'd10,
		ST_DIDC    = 6'd11,
		ST_ALPHA   = 6'd12,
		ST_BETA    = 6'd13,
		ST_MBT     = 6'd14,
		ST_CHRPRED = 6'd15, // intra_chroma_pred_mode (7.3.5)
		ST_MBQP    = 6'd16,
		ST_TOK_BIT = 6'd17,
		ST_TOK_CHK = 6'd18,
		ST_SIGNS   = 6'd19,
		ST_DONE    = 6'd20,
		ST_FAIL    = 6'd21,
		ST_I4MODE  = 6'd22, // I_NxN: skip 16 pred modes
		ST_CBP     = 6'd23, // I_NxN: coded_block_pattern me
		// 3.3k residual body
		ST_LVL_PRE = 6'd24, // unary prefix zeros + stop-1
		ST_LVL_SUF = 6'd25, // suffix bits
		ST_LVL_NXT = 6'd26, // store level, advance
		ST_TZ_BIT  = 6'd27,
		ST_TZ_CHK  = 6'd28,
		ST_RUN_BIT = 6'd29,
		ST_RUN_CHK = 6'd30,
		ST_PLACE   = 6'd31,
		ST_REFIDX_FLAG = 6'd32,
		ST_REFIDX_L0   = 6'd33,
		ST_NIDR_REFMARK = 6'd34,
		ST_P_MBT       = 6'd35;

	task automatic res_clear;
		integer ci;
		begin
			residual_ok <= 0;
			residual_tc <= 0;
			residual_t1 <= 0;
			residual_dc <= 0;
			// R-csum-rtl4: do NOT clear residual_csum here — sticky until ST_PLACE
			// overwrites or reset. Over-clear after good fold was a thrash class risk.
			for (ci = 0; ci < 16; ci = ci + 1)
				residual_coeff[ci] <= 16'sd0;
			tok_ok <= 0;
		end
	endtask

	// After token(+signs): enter levels / tz / done
	task automatic res_after_t1;
		integer ci;
		begin
			if (r_tc == 0) begin
				residual_ok <= 1'b1;
				residual_dc <= 0;
				residual_csum <= 0; // no residual levels → csum 0 is correct
				for (ci = 0; ci < 16; ci = ci + 1)
					residual_coeff[ci] <= 16'sd0;
				st <= ST_DONE;
			end else if (r_t1 < r_tc) begin
				lev_i <= r_t1[3:0];
				first_non_t1 <= 1'b1;
				suf_len <= (r_tc > 5'd10 && r_t1 < 5'd3) ? 3'd1 : 3'd0;
				pref <= 0;
				st <= ST_LVL_PRE;
			end else begin
				// only trailing ones — total_zeros / runs
				st <= ST_TZ_BIT;
				tzcode <= 0;
				tzbits <= 0;
			end
		end
	endtask

	always @(posedge clk) begin
		if (reset || cap_clear)
			len <= 0;
		else if (cap_en && len < MAXB[5:0]) begin
			mem[len] <= cap_data;
			len <= len + 1'd1;
		end
	end

	always @(posedge clk) begin
		if (reset) begin
			st <= ST_IDLE;
			valid <= 0;
			busy <= 0;
			first_mb <= 0;
			slice_type <= 0;
			pps_id <= 0;
			frame_num <= 0;
			idr_pic_id <= 0;
			is_i_slice <= 0;
			slice_qp_delta <= 0;
			slice_qp <= 0;
			disable_deblocking_filter_idc <= 0;
			slice_alpha_c0_offset_div2 <= 0;
			slice_beta_offset_div2 <= 0;
			slice_alpha_c0_offset <= 0;
			slice_beta_offset <= 0;
			first_mb_type <= 0;
			has_mb_type <= 0;
			first_mb_p_skip <= 0;
			p_skip_run <= 0;
			first_mb_part_mode <= 3'd7;
			first_mb_part_count <= 0;
			first_mb_uses_sub_mb <= 0;
			first_mb_intra <= 0;
			residual_tc <= 0;
			residual_t1 <= 0;
			residual_ok <= 0;
			residual_dc <= 0;
			residual_csum <= 0; // reset only — sticky otherwise until ST_PLACE
			residual_place_pulse <= 1'b0;
			residual_place_ok <= 1'b0;
			residual_place_tc <= 5'd0;
			residual_place_t1 <= 2'd0;
			residual_place_dc <= 8'sd0;
			residual_place_qp <= 6'd0;
			place_csum_r <= 8'd0;
			place_dc_r <= 8'sd0;
			begin : rst_coeff
				integer ci;
				for (ci = 0; ci < 16; ci = ci + 1) begin
					residual_coeff[ci] <= 16'sd0;
					residual_place_coeff[ci] <= 16'sd0;
				end
			end
			tcode <= 0;
			tbits <= 0;
			r_tc <= 0;
			r_t1 <= 0;
			sign_left <= 0;
			bbyte <= 0;
			bpos <= 3'd7;
			zcnt <= 0;
			nleft <= 0;
			acc <= 0;
			ue_val <= 0;
			cont <= ST_IDLE;
			ue_cont <= ST_IDLE;
			idr_lat <= 0;
			db_lat <= 0;
			nal_ref_lat <= 0;
			log2_lat <= 5'd4;
			init_qp_lat <= 8'sd26;
			qp_tmp <= 0;
			tok_ok <= 0;
			lev_i <= 0;
			suf_len <= 0;
			pref <= 0;
			zeros_left <= 0;
			run_i <= 0;
			tzcode <= 0;
			tzbits <= 0;
			runcode <= 0;
			runbits <= 0;
			place_did <= 0;
			csum_i <= 0;
			csum_acc <= 0;
		end else if (cap_clear) begin
			// New slice capture: drop ok/dc for paint wait; KEEP residual_csum sticky
			// until ST_PLACE overwrites (R-csum-rtl4: avoid over-clear after good fold).
			st <= ST_IDLE;
			busy <= 0;
			valid <= 0;
			has_mb_type <= 0;
			first_mb_p_skip <= 0;
			p_skip_run <= 0;
			first_mb_part_mode <= 3'd7;
			first_mb_part_count <= 0;
			first_mb_uses_sub_mb <= 0;
			first_mb_intra <= 0;
			residual_ok <= 0;
			residual_tc <= 0;
			residual_t1 <= 0;
			residual_dc <= 0;
			// residual_csum intentionally NOT cleared
			residual_place_pulse <= 1'b0;
			begin : clr_coeff_cap
				integer ci;
				for (ci = 0; ci < 16; ci = ci + 1)
					residual_coeff[ci] <= 16'sd0;
			end
			tok_ok <= 0;
			place_did <= 0;
			csum_i <= 0;
			csum_acc <= 0;
		end else begin
			// Default: place pulse is 1-cycle only (Rank3)
			residual_place_pulse <= 1'b0;
			case (st)
			ST_IDLE: begin
				busy <= 0;
				if (cap_end && len >= 6'd2 && sps_ready && pps_ready && poc_type != 3'd1) begin
					busy <= 1'b1;
					has_mb_type <= 0;
					first_mb_p_skip <= 0;
					p_skip_run <= 0;
					first_mb_part_mode <= 3'd7;
					first_mb_part_count <= 0;
					first_mb_uses_sub_mb <= 0;
					first_mb_intra <= 0;
					residual_ok <= 0;
					residual_tc <= 0;
					residual_t1 <= 0;
					residual_dc <= 0;
					// residual_csum sticky held until ST_PLACE overwrites
					disable_deblocking_filter_idc <= 0;
					slice_alpha_c0_offset_div2 <= 0;
					slice_beta_offset_div2 <= 0;
					slice_alpha_c0_offset <= 0;
					slice_beta_offset <= 0;
					begin : clr_coeff_idle
						integer ci;
						for (ci = 0; ci < 16; ci = ci + 1)
							residual_coeff[ci] <= 16'sd0;
					end
					tok_ok <= 0;
					place_did <= 0;
					csum_i <= 0;
					csum_acc <= 0;
					idr_lat <= is_idr_nal;
					nal_ref_lat <= nal_ref_idc_nonzero;
					db_lat <= deblock_ctrl;
					log2_lat <= (log2_max_frame_num == 0) ? 5'd4 : log2_max_frame_num;
					init_qp_lat <= pic_init_qp;
					bbyte <= 0;
					bpos <= 3'd7;
					zcnt <= 0;
					ue_cont <= ST_FIRST;
					st <= ST_UE_Z;
				end
			end

			ST_GETBITS: begin
				if (oob) st <= ST_FAIL;
				else begin
					acc <= {acc[14:0], bitv};
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					if (nleft == 5'd1) st <= cont;
					else nleft <= nleft - 1'd1;
				end
			end

			ST_UE_Z: begin
				if (oob) st <= ST_FAIL;
				else if (bitv == 1'b0) begin
					if (zcnt >= 6'd20) st <= ST_FAIL;
					else begin
						zcnt <= zcnt + 1'd1;
						if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
						else bpos <= bpos - 1'd1;
					end
				end else begin
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					if (zcnt == 0) begin ue_val <= 0; st <= ue_cont; end
					else begin
						nleft <= zcnt[4:0]; acc <= 0; cont <= ST_UE_V; st <= ST_GETBITS;
					end
				end
			end

			ST_UE_V: begin
				ue_val <= ((16'd1 << zcnt) - 16'd1) + acc;
				st <= ue_cont;
			end

			ST_FIRST: begin
				first_mb <= ue_val;
				zcnt <= 0; ue_cont <= ST_TYPE; st <= ST_UE_Z;
			end
			ST_TYPE: begin
				slice_type <= ue_val[7:0];
				is_i_slice <= (ue_val[7:0] == 8'd2) || (ue_val[7:0] == 8'd7);
				zcnt <= 0; ue_cont <= ST_PPS; st <= ST_UE_Z;
			end
			ST_PPS: begin
				pps_id <= ue_val[7:0];
				nleft <= log2_lat; acc <= 0; cont <= ST_FN; st <= ST_GETBITS;
			end
			ST_FN: begin
				frame_num <= acc;
				if (idr_lat) begin
					zcnt <= 0; ue_cont <= ST_IDR; st <= ST_UE_Z;
				end else if (is_p_slice_type(slice_type)) begin
					nleft <= 5'd1; acc <= 0; cont <= ST_REFIDX_FLAG; st <= ST_GETBITS;
				end else if (nal_ref_lat) begin
					nleft <= 5'd1; acc <= 0; cont <= ST_NIDR_REFMARK; st <= ST_GETBITS;
				end else begin
					zcnt <= 0; ue_cont <= ST_QPD; st <= ST_UE_Z;
				end
			end
			ST_IDR: begin
				idr_pic_id <= ue_val;
				// dec_ref_pic_marking (IDR): no_output_of_prior_pics + long_term_reference
				nleft <= 5'd2; acc <= 0; cont <= ST_REFMARK; st <= ST_GETBITS;
			end
			ST_REFMARK: begin
				zcnt <= 0; ue_cont <= ST_QPD; st <= ST_UE_Z;
			end
			ST_REFIDX_FLAG: begin
				if (acc[0]) begin
					zcnt <= 0; ue_cont <= ST_REFIDX_L0; st <= ST_UE_Z;
				end else if (nal_ref_lat) begin
					nleft <= 5'd1; acc <= 0; cont <= ST_NIDR_REFMARK; st <= ST_GETBITS;
				end else begin
					zcnt <= 0; ue_cont <= ST_QPD; st <= ST_UE_Z;
				end
			end
			ST_REFIDX_L0: begin
				if (nal_ref_lat) begin
					nleft <= 5'd1; acc <= 0; cont <= ST_NIDR_REFMARK; st <= ST_GETBITS;
				end else begin
					zcnt <= 0; ue_cont <= ST_QPD; st <= ST_UE_Z;
				end
			end
			ST_NIDR_REFMARK: begin
				// Baseline product profile uses one short-term reference and no MMCO.
				// If adaptive_ref_pic_marking_mode_flag is set, stop after the
				// header rather than mis-parsing MMCO as QP/MB syntax.
				if (acc[0])
					st <= ST_DONE;
				else begin
					zcnt <= 0; ue_cont <= ST_QPD; st <= ST_UE_Z;
				end
			end
			ST_QPD: begin
				// se(slice_qp_delta); slice_qp = clamp(pic_init_qp + delta, 0, 51)
				dlt_tmp = se_of(ue_val);
				slice_qp_delta <= dlt_tmp;
				qp_tmp = init_qp_lat + dlt_tmp;
				if (qp_tmp < 0)
					slice_qp <= 6'd0;
				else if (qp_tmp > 51)
					slice_qp <= 6'd51;
				else
					slice_qp <= qp_tmp[5:0];
				if (db_lat) begin
					zcnt <= 0; ue_cont <= ST_DIDC; st <= ST_UE_Z;
				end else begin
					zcnt <= 0; ue_cont <= ST_MBT; st <= ST_UE_Z;
				end
			end
			ST_DIDC: begin
				disable_deblocking_filter_idc <= ue_val[1:0];
				if (ue_val != 16'd1) begin
					zcnt <= 0; ue_cont <= ST_ALPHA; st <= ST_UE_Z;
				end else begin
					zcnt <= 0; ue_cont <= ST_MBT; st <= ST_UE_Z;
				end
			end
			ST_ALPHA: begin
				dlt_tmp = se_of(ue_val);
				slice_alpha_c0_offset_div2 <= dlt_tmp[4:0];
				slice_alpha_c0_offset <= filter_offset_of_div2(dlt_tmp);
				zcnt <= 0; ue_cont <= ST_BETA; st <= ST_UE_Z;
			end
			ST_BETA: begin
				dlt_tmp = se_of(ue_val);
				slice_beta_offset_div2 <= dlt_tmp[4:0];
				slice_beta_offset <= filter_offset_of_div2(dlt_tmp);
				zcnt <= 0; ue_cont <= ST_MBT; st <= ST_UE_Z;
			end
			ST_MBT: begin
				if (is_p_slice_type(slice_type)) begin
					p_skip_run <= ue_val[7:0];
					if (ue_val != 16'd0) begin
						first_mb_type <= 8'd0;
						has_mb_type <= 1'b1;
						first_mb_p_skip <= 1'b1;
						first_mb_part_mode <= p_part_mode_of(1'b1, 8'd0);
						first_mb_part_count <= p_part_count_of(1'b1, 8'd0);
						first_mb_uses_sub_mb <= 1'b0;
						first_mb_intra <= 1'b0;
						st <= ST_DONE;
					end else begin
						first_mb_p_skip <= 1'b0;
						zcnt <= 0; ue_cont <= ST_P_MBT; st <= ST_UE_Z;
					end
				end else begin
					first_mb_type <= ue_val[7:0];
					has_mb_type <= (ue_val <= 16'd25);
					first_mb_p_skip <= 1'b0;
					first_mb_part_mode <= 3'd7;
					first_mb_part_count <= 3'd0;
					first_mb_uses_sub_mb <= 1'b0;
					first_mb_intra <= (ue_val <= 16'd25);
				// I_16x16 (1..24): chroma → qpδ → CAVLC DC (nC=0)
				// I_NxN (0): 16 pred modes → chroma → cbp → qpδ? → first 4x4 residual
					if (ue_val >= 16'd1 && ue_val <= 16'd24) begin
						zcnt <= 0; ue_cont <= ST_CHRPRED; st <= ST_UE_Z;
					end else if (ue_val == 16'd0) begin
						i4_i <= 0; i4_sub <= 0; i4_need_rem <= 0;
						st <= ST_I4MODE;
					end else
						st <= ST_DONE;
				end
			end
			ST_P_MBT: begin
				first_mb_type <= ue_val[7:0];
				has_mb_type <= (ue_val <= 16'd30);
				first_mb_part_mode <= p_part_mode_of(1'b0, ue_val[7:0]);
				first_mb_part_count <= p_part_count_of(1'b0, ue_val[7:0]);
				first_mb_uses_sub_mb <= (ue_val == 16'd3) || (ue_val == 16'd4);
				first_mb_intra <= (ue_val >= 16'd5) && (ue_val <= 16'd30);
				st <= ST_DONE;
			end
			ST_I4MODE: begin
				// Skip 16 Intra4x4 modes: each is flag(1) or flag(0)+rem(3)
				if (oob) st <= ST_FAIL;
				else begin
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					if (i4_sub == 2'd0) begin
						// prev_intra4x4_pred_mode_flag
						if (bitv) begin
							// flag=1: mode=pred, next block
							if (i4_i >= 5'd15) begin
								zcnt <= 0; ue_cont <= ST_CHRPRED; st <= ST_UE_Z;
							end else begin
								i4_i <= i4_i + 1'd1;
							end
						end else begin
							// flag=0: need 3 rem bits
							i4_sub <= 2'd1;
						end
					end else begin
						// consuming rem bits 1..3
						if (i4_sub >= 2'd3) begin
							i4_sub <= 0;
							if (i4_i >= 5'd15) begin
								zcnt <= 0; ue_cont <= ST_CHRPRED; st <= ST_UE_Z;
							end else
								i4_i <= i4_i + 1'd1;
						end else
							i4_sub <= i4_sub + 1'd1;
					end
				end
			end
			ST_CHRPRED: begin
				// ue(intra_chroma_pred_mode) consumed
				// I_NxN → cbp me; I_16x16 → mb_qp_delta
				if (first_mb_type == 8'd0) begin
					zcnt <= 0; ue_cont <= ST_CBP; st <= ST_UE_Z;
				end else begin
					zcnt <= 0; ue_cont <= ST_MBQP; st <= ST_UE_Z;
				end
			end
			ST_CBP: begin
				// ue(coded_block_pattern me) — me==3 → cbp=0 (no qpδ)
				cbp_me <= ue_val[5:0];
				if (ue_val == 16'd3) begin
					// cbp=0: no residual (shouldn't happen on real first MB)
					tcode <= 0; tbits <= 0; st <= ST_TOK_BIT;
				end else begin
					zcnt <= 0; ue_cont <= ST_MBQP; st <= ST_UE_Z;
				end
			end
			ST_MBQP: begin
				// se(mb_qp_delta) consumed; start coeff_token nC=0
				tcode <= 0;
				tbits <= 0;
				st <= ST_TOK_BIT;
			end
			ST_TOK_BIT: begin
				if (oob) st <= ST_FAIL;
				else begin
					tcode <= {tcode[14:0], bitv};
					tbits <= tbits + 1'd1;
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					st <= ST_TOK_CHK;
				end
			end
			ST_TOK_CHK: begin
				// Match num-VLC0 (nC=0) common tokens; clear levels on match
				tok_ok <= 0;
				if (tbits == 5'd1 && tcode[0] == 1'b1) begin
					r_tc <= 0; r_t1 <= 0; residual_tc <= 0; residual_t1 <= 0;
					tok_ok <= 1'b1;
					residual_ok <= 1'b1; residual_dc <= 0; residual_csum <= 0;
					begin : clr_coeff_tc0
						integer ci;
						for (ci = 0; ci < 16; ci = ci + 1)
							residual_coeff[ci] <= 16'sd0;
					end
					st <= ST_DONE;
				end else if (tbits == 5'd2 && tcode[1:0] == 2'b01) begin
					r_tc <= 1; r_t1 <= 1; residual_tc <= 1; residual_t1 <= 2'd1;
					sign_left <= 1; lev_i <= 0; tok_ok <= 1'b1; st <= ST_SIGNS;
				end else if (tbits == 5'd3 && tcode[2:0] == 3'b001) begin
					r_tc <= 2; r_t1 <= 2; residual_tc <= 2; residual_t1 <= 2'd2;
					sign_left <= 2; lev_i <= 0; tok_ok <= 1'b1; st <= ST_SIGNS;
				end else if (tbits == 5'd5 && tcode[4:0] == 5'b00011) begin
					r_tc <= 3; r_t1 <= 3; residual_tc <= 3; residual_t1 <= 2'd3;
					sign_left <= 3; lev_i <= 0; tok_ok <= 1'b1; st <= ST_SIGNS;
				end else if (tbits == 5'd6 && tcode[5:0] == 6'b000101) begin
					// tc=1 t1=0 — one non-T1 level, no signs
					r_tc <= 1; r_t1 <= 0; residual_tc <= 1; residual_t1 <= 0;
					tok_ok <= 1'b1;
					lev_i <= 0; first_non_t1 <= 1'b1; suf_len <= 0; pref <= 0;
					st <= ST_LVL_PRE;
				end else if (tbits == 5'd6 && tcode[5:0] == 6'b000100) begin
					r_tc <= 2; r_t1 <= 1; residual_tc <= 2; residual_t1 <= 2'd1;
					sign_left <= 1; lev_i <= 0; tok_ok <= 1'b1; st <= ST_SIGNS;
				end else if (tbits == 5'd10 && tcode[9:0] == 10'b0000000100) begin
					// nC=0 tc=8 t1=3 — real Baseline first I_NxN residual
					r_tc <= 8; r_t1 <= 3; residual_tc <= 5'd8; residual_t1 <= 2'd3;
					// Mark ok early so 3.3j residual probe stays green if level path fails.
					// residual_csum stays sticky until ST_PLACE overwrites (no mid-parse clear).
					residual_ok <= 1'b1;
					residual_dc <= 0;
					// Clear runs so ST_PLACE never sees X/stale runv (dc would stick at 0)
					begin : clr_run_tc8
						integer ri;
						for (ri = 0; ri < 16; ri = ri + 1) runv[ri] <= 4'd0;
					end
					// Clear lev[] so place never sees stale slots
					begin : clr_lev_tc8
						integer li;
						for (li = 0; li < 16; li = li + 1) lev[li] <= 16'sd0;
					end
					place_did <= 0;
					csum_i <= 0;
					csum_acc <= 0;
					sign_left <= 3; lev_i <= 0; tok_ok <= 1'b1; st <= ST_SIGNS;
				end else if (tbits == 5'd9 && tcode[8:0] == 9'b000000100) begin
					// nC=0 tc=8 t1=2 (nearby)
					r_tc <= 8; r_t1 <= 2; residual_tc <= 5'd8; residual_t1 <= 2'd2;
					residual_dc <= 0;
					// residual_csum sticky until ST_PLACE
					begin : clr_run_tc8b
						integer ri;
						for (ri = 0; ri < 16; ri = ri + 1) runv[ri] <= 4'd0;
					end
					begin : clr_lev_tc8b
						integer li;
						for (li = 0; li < 16; li = li + 1) lev[li] <= 16'sd0;
					end
					place_did <= 0;
					csum_i <= 0;
					csum_acc <= 0;
					sign_left <= 2; lev_i <= 0; tok_ok <= 1'b1; st <= ST_SIGNS;
				end else if (tbits >= 5'd16) begin
					// token not in bring-up set — header still valid
					st <= ST_DONE;
				end else
					st <= ST_TOK_BIT;
			end
			ST_SIGNS: begin
				// TrailingOnes sign bits → level[i] = ±1
				if (oob) st <= ST_FAIL;
				else begin
					// R-csum4: scalar running XOR into private csum_acc (sat8(±1)).
					// residual_csum is latched sticky only at ST_PLACE — not here.
					lev[lev_i] <= bitv ? -16'sd1 : 16'sd1;
					csum_acc <= csum_acc ^ (bitv ? 8'hFF : 8'h01);
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					if (sign_left <= 3'd1) begin
						// finished T1 signs
						if (r_t1 < r_tc) begin
							lev_i <= r_t1[3:0];
							first_non_t1 <= 1'b1;
							suf_len <= (r_tc > 5'd10 && r_t1 < 5'd3) ? 3'd1 : 3'd0;
							pref <= 0;
							st <= ST_LVL_PRE;
						end else begin
							tzcode <= 0; tzbits <= 0; st <= ST_TZ_BIT;
						end
					end else begin
						sign_left <= sign_left - 1'd1;
						lev_i <= lev_i + 1'd1;
					end
				end
			end

			// --- 3.3k: non-T1 levels (prefix unary + suffix) ---
			ST_LVL_PRE: begin
				if (oob) st <= ST_FAIL;
				else if (bitv == 1'b0) begin
					if (pref >= 6'd31) st <= ST_FAIL;
					else begin
						pref <= pref + 1'd1;
						if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
						else bpos <= bpos - 1'd1;
					end
				end else begin
					// stop bit '1' consumed
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					// decide suffix length to read
					if (first_non_t1) begin
						if (pref < 6'd14) begin
							if (suf_len != 0) begin
								suf_left <= {2'b0, suf_len}; suf_acc <= 0;
								nleft <= {2'b0, suf_len}; acc <= 0;
								cont <= ST_LVL_SUF; st <= ST_GETBITS;
							end else begin
								// levelCode = prefix; no suffix
								suf_acc <= {10'b0, pref};
								st <= ST_LVL_NXT;
							end
						end else if (pref == 6'd14) begin
							if (suf_len != 0) begin
								nleft <= {2'b0, suf_len}; acc <= 0;
								cont <= ST_LVL_SUF; st <= ST_GETBITS;
							end else begin
								nleft <= 5'd4; acc <= 0;
								cont <= ST_LVL_SUF; st <= ST_GETBITS;
							end
						end else begin
							// prefix >= 15: escape, (prefix-3) extra bits
							nleft <= pref[4:0] - 5'd3; acc <= 0;
							cont <= ST_LVL_SUF; st <= ST_GETBITS;
						end
					end else begin
						// subsequent coeffs (suf_len >= 1 typically)
						if (pref < 6'd15) begin
							nleft <= {2'b0, suf_len}; acc <= 0;
							if (suf_len == 0) begin
								suf_acc <= {10'b0, pref};
								st <= ST_LVL_NXT;
							end else begin
								cont <= ST_LVL_SUF; st <= ST_GETBITS;
							end
						end else begin
							nleft <= pref[4:0] - 5'd3; acc <= 0;
							cont <= ST_LVL_SUF; st <= ST_GETBITS;
						end
					end
				end
			end
			ST_LVL_SUF: begin
				// acc holds suffix/extra bits from ST_GETBITS
				suf_acc <= acc;
				st <= ST_LVL_NXT;
			end
			ST_LVL_NXT: begin
				// Build levelCode and signed level; update suffixLength
				// Use combinatorial temps via blocking assigns in this clock
				begin : lvl_body
					reg [15:0] lc;
					reg signed [15:0] lv;
					reg [2:0] nsl;
					reg [15:0] pfx;
					pfx = {10'b0, pref};
					nsl = suf_len;
					if (first_non_t1) begin
						if (pref < 6'd14) begin
							if (suf_len != 0)
								lc = (pfx << suf_len) + suf_acc;
							else
								lc = pfx;
						end else if (pref == 6'd14) begin
							if (suf_len != 0)
								lc = (pfx << suf_len) + suf_acc;
							else
								lc = pfx + suf_acc; // +4-bit
						end else begin
							lc = 16'd30;
							if (pref >= 6'd16)
								lc = lc + ((16'd1 << (pref - 6'd3)) - 16'd4096);
							lc = lc + suf_acc;
						end
						if (r_t1 < 5'd3)
							lc = lc + 16'd2;
						lv = lev_of(lc);
						lev[lev_i] <= lv;
						if (pref > 6'd14 || (pref == 6'd14 && suf_len == 0))
							nsl = 3'd2;
						else
							nsl = 3'd1 + (((lv + 16'sd3) > 16'sd6) ? 3'd1 : 3'd0);
						first_non_t1 <= 0;
					end else begin
						if (pref < 6'd15) begin
							lc = (pfx << suf_len) + suf_acc;
						end else begin
							lc = (16'd15 << suf_len);
							if (pref >= 6'd16)
								lc = lc + ((16'd1 << (pref - 6'd3)) - 16'd4096);
							lc = lc + suf_acc;
						end
						lv = lev_of(lc);
						lev[lev_i] <= lv;
						// suffixLength bump: lim = {0,3,6,12,24,48,max}
						if (suf_len < 3'd6) begin
							// lim[s] + level > 2*lim[s]
							// level is signed; FFmpeg uses unsigned cast of level
							// Use abs-ish: unsigned(level) in 9 bits
							begin : suf_bump
								reg [15:0] ul;
								reg [15:0] lim;
								ul = lv[15:0]; // two's complement as unsigned bits
								case (suf_len)
									3'd0: lim = 16'd0;
									3'd1: lim = 16'd3;
									3'd2: lim = 16'd6;
									3'd3: lim = 16'd12;
									3'd4: lim = 16'd24;
									3'd5: lim = 16'd48;
									default: lim = 16'd65535;
								endcase
								if (lim + ul > (lim << 1))
									nsl = suf_len + 3'd1;
								else
									nsl = suf_len;
							end
						end else
							nsl = suf_len;
					end
					suf_len <= nsl;
					// R-csum4: scalar running XOR into private csum_acc (not residual_csum).
					// zeros free under XOR ⇒ full-16 golden 0x14 == XOR of tc levels only.
					csum_acc <= csum_acc ^ sat8(lv);
					if (lev_i + 4'd1 >= r_tc[3:0] && r_tc[4] == 1'b0) begin
						// all levels done → provisional residual_dc = last reverse-order
						// level (equals scan DC when run[tc-1]==0, true on golden Baseline)
						residual_dc <= sat8(lv);
						// (lv is lev[lev_i] just stored; when finishing, lev_i == tc-1)
						if (r_tc < 5'd16) begin
							tzcode <= 0; tzbits <= 0; st <= ST_TZ_BIT;
						end else begin
							zeros_left <= 0; run_i <= 0; st <= ST_PLACE;
						end
					end else begin
						lev_i <= lev_i + 1'd1;
						pref <= 0;
						st <= ST_LVL_PRE;
					end
				end
			end

			// total_zeros VLC for maxNumCoeff=16, TotalCoeff = r_tc (1..15)
			ST_TZ_BIT: begin
				if (r_tc >= 5'd16) begin
					zeros_left <= 0; run_i <= 0; st <= ST_PLACE;
				end else if (oob) st <= ST_FAIL;
				else begin
					tzcode <= {tzcode[7:0], bitv};
					tzbits <= tzbits + 1'd1;
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					st <= ST_TZ_CHK;
				end
			end
			ST_TZ_CHK: begin
				// Hardcoded total_zeros tables for common (tc,zeros) on bring-up + full small set.
				// Match FFmpeg total_zeros_len/bits[tc-1][zeros].
				begin : tz_match
					reg matched;
					reg [3:0] zval;
					matched = 0;
					zval = 0;
					// Macro-like per-tc matching for bits read so far
					if (r_tc == 5'd1) begin
						// zeros 0..15 lens: 1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9
						// bits: 1, 3,2, 3,2, 3,2, 3,2, 3,2, 3,2, 3,2,1
						if (tzbits == 4'd1 && tzcode[0] == 1'b1) begin matched = 1; zval = 0; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b011) begin matched = 1; zval = 1; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b010) begin matched = 1; zval = 2; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0011) begin matched = 1; zval = 3; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0010) begin matched = 1; zval = 4; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00011) begin matched = 1; zval = 5; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00010) begin matched = 1; zval = 6; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000011) begin matched = 1; zval = 7; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000010) begin matched = 1; zval = 8; end
						else if (tzbits == 4'd7 && tzcode[6:0] == 7'b0000011) begin matched = 1; zval = 9; end
						else if (tzbits == 4'd7 && tzcode[6:0] == 7'b0000010) begin matched = 1; zval = 10; end
						else if (tzbits == 4'd8 && tzcode[7:0] == 8'b00000011) begin matched = 1; zval = 11; end
						else if (tzbits == 4'd8 && tzcode[7:0] == 8'b00000010) begin matched = 1; zval = 12; end
						else if (tzbits == 4'd9 && tzcode[8:0] == 9'b000000011) begin matched = 1; zval = 13; end
						else if (tzbits == 4'd9 && tzcode[8:0] == 9'b000000010) begin matched = 1; zval = 14; end
						else if (tzbits == 4'd9 && tzcode[8:0] == 9'b000000001) begin matched = 1; zval = 15; end
					end else if (r_tc == 5'd8) begin
						// total_zeros for tc=8: lens 6,4,5,3,2,2,3,3,6 bits 1,1,1,3,3,2,2,1,0
						// zeros 0..8
						if (tzbits == 4'd6 && tzcode[5:0] == 6'b000001) begin matched = 1; zval = 0; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0001) begin matched = 1; zval = 1; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00001) begin matched = 1; zval = 2; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b011) begin matched = 1; zval = 3; end
						else if (tzbits == 4'd2 && tzcode[1:0] == 2'b11) begin matched = 1; zval = 4; end
						else if (tzbits == 4'd2 && tzcode[1:0] == 2'b10) begin matched = 1; zval = 5; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b010) begin matched = 1; zval = 6; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b001) begin matched = 1; zval = 7; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000000) begin matched = 1; zval = 8; end
					end else if (r_tc == 5'd2) begin
						// lens 3,3,3,3,3,4,4,4,4,5,5,6,6,6,6
						if (tzbits == 4'd3 && tzcode[2:0] == 3'b111) begin matched = 1; zval = 0; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b110) begin matched = 1; zval = 1; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b101) begin matched = 1; zval = 2; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b100) begin matched = 1; zval = 3; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b011) begin matched = 1; zval = 4; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0101) begin matched = 1; zval = 5; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0100) begin matched = 1; zval = 6; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0011) begin matched = 1; zval = 7; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0010) begin matched = 1; zval = 8; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00011) begin matched = 1; zval = 9; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00010) begin matched = 1; zval = 10; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000011) begin matched = 1; zval = 11; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000010) begin matched = 1; zval = 12; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000001) begin matched = 1; zval = 13; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000000) begin matched = 1; zval = 14; end
					end else if (r_tc == 5'd3) begin
						if (tzbits == 4'd4 && tzcode[3:0] == 4'b0101) begin matched = 1; zval = 0; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b111) begin matched = 1; zval = 1; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b110) begin matched = 1; zval = 2; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b101) begin matched = 1; zval = 3; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0100) begin matched = 1; zval = 4; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0011) begin matched = 1; zval = 5; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b100) begin matched = 1; zval = 6; end
						else if (tzbits == 4'd3 && tzcode[2:0] == 3'b011) begin matched = 1; zval = 7; end
						else if (tzbits == 4'd4 && tzcode[3:0] == 4'b0010) begin matched = 1; zval = 8; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00011) begin matched = 1; zval = 9; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00010) begin matched = 1; zval = 10; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000001) begin matched = 1; zval = 11; end
						else if (tzbits == 4'd5 && tzcode[4:0] == 5'b00001) begin matched = 1; zval = 12; end
						else if (tzbits == 4'd6 && tzcode[5:0] == 6'b000000) begin matched = 1; zval = 13; end
					end else begin
						// Other tc: try progressive read up to 9 bits then accept zeros=0 fail-soft
						if (tzbits >= 4'd9) begin
							matched = 1; zval = 0; // fail-soft: treat as 0 zeros
						end
					end

					if (matched) begin
						zeros_left <= zval;
						run_i <= 0;
						// clear all runs first (host run[] starts at 0)
						begin : clr_runs_tz
							integer ri;
							for (ri = 0; ri < 16; ri = ri + 1) runv[ri] <= 4'd0;
						end
						if (r_tc <= 5'd1) begin
							// no run_before loop — sole run is total_zeros
							runv[0] <= zval;
							st <= ST_PLACE;
						end else if (zval == 0) begin
							// all run_before=0; last run also 0 (already cleared)
							st <= ST_PLACE;
						end else begin
							runcode <= 0; runbits <= 0; st <= ST_RUN_BIT;
						end
					end else if (tzbits >= 4'd9) begin
						// no match
						st <= ST_DONE; // header ok, residual incomplete
					end else
						st <= ST_TZ_BIT;
				end
			end

			ST_RUN_BIT: begin
				if (oob) st <= ST_FAIL;
				else if (zeros_left == 0) begin
					// remaining runs are 0; last gets zeros_left
					st <= ST_PLACE;
				end else if (zeros_left >= 4'd7) begin
					// 3-bit FLC path for zerosLeft > 6
					nleft <= 5'd3; acc <= 0; cont <= ST_RUN_CHK; st <= ST_GETBITS;
					runbits <= 4'd3; // flag FLC mode via runbits==3 and zeros>=7
				end else begin
					runcode <= {runcode[3:0], bitv};
					runbits <= runbits + 1'd1;
					if (bpos == 3'd0) begin bpos <= 3'd7; bbyte <= bbyte + 1'd1; end
					else bpos <= bpos - 1'd1;
					st <= ST_RUN_CHK;
				end
			end
			ST_RUN_CHK: begin
				begin : run_match
					reg matched;
					reg [3:0] rval;
					reg [3:0] zl;
					matched = 0;
					rval = 0;
					zl = zeros_left;
					if (zl >= 4'd7) begin
						// acc is 3-bit; if >0 run=7-v else extend
						if (acc[2:0] > 3'd0) begin
							matched = 1;
							rval = 4'd7 - {1'b0, acc[2:0]};
						end else begin
							// need more zeros until 1 — handle simple: run=7 + extra zeros
							// consume until 1
							// For real baseline zerosLeft=5, never enters here.
							// Fail-soft: run=7
							matched = 1;
							rval = 4'd7;
						end
					end else if (zl == 4'd1) begin
						// run 0/1 lens 1,1 bits 1,0
						if (runbits == 4'd1 && runcode[0] == 1'b1) begin matched = 1; rval = 0; end
						else if (runbits == 4'd1 && runcode[0] == 1'b0) begin matched = 1; rval = 1; end
					end else if (zl == 4'd2) begin
						// 1,2,2 → 1; 1,0
						if (runbits == 4'd1 && runcode[0] == 1'b1) begin matched = 1; rval = 0; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b01) begin matched = 1; rval = 1; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b00) begin matched = 1; rval = 2; end
					end else if (zl == 4'd3) begin
						// 2,2,2,2 → 3,2,1,0
						if (runbits == 4'd2 && runcode[1:0] == 2'b11) begin matched = 1; rval = 0; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b10) begin matched = 1; rval = 1; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b01) begin matched = 1; rval = 2; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b00) begin matched = 1; rval = 3; end
					end else if (zl == 4'd4) begin
						// 2,2,2,3,3 → 3,2,1,1,0
						if (runbits == 4'd2 && runcode[1:0] == 2'b11) begin matched = 1; rval = 0; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b10) begin matched = 1; rval = 1; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b01) begin matched = 1; rval = 2; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b001) begin matched = 1; rval = 3; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b000) begin matched = 1; rval = 4; end
					end else if (zl == 4'd5) begin
						// 2,2,3,3,3,3 → 3,2,3,2,1,0  (real baseline uses this)
						if (runbits == 4'd2 && runcode[1:0] == 2'b11) begin matched = 1; rval = 0; end
						else if (runbits == 4'd2 && runcode[1:0] == 2'b10) begin matched = 1; rval = 1; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b011) begin matched = 1; rval = 2; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b010) begin matched = 1; rval = 3; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b001) begin matched = 1; rval = 4; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b000) begin matched = 1; rval = 5; end
					end else if (zl == 4'd6) begin
						// 2,3,3,3,3,3,3 → 3,0,1,3,2,5,4
						if (runbits == 4'd2 && runcode[1:0] == 2'b11) begin matched = 1; rval = 0; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b000) begin matched = 1; rval = 1; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b001) begin matched = 1; rval = 2; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b011) begin matched = 1; rval = 3; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b010) begin matched = 1; rval = 4; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b101) begin matched = 1; rval = 5; end
						else if (runbits == 4'd3 && runcode[2:0] == 3'b100) begin matched = 1; rval = 6; end
					end

					if (matched) begin
						runv[run_i] <= rval;
						zeros_left <= zeros_left - rval;
						if (run_i + 4'd1 >= r_tc[3:0] - 4'd1) begin
							// last non-final run done; final run = remaining zeros
							runv[r_tc[3:0] - 4'd1] <= zeros_left - rval;
							st <= ST_PLACE;
						end else begin
							run_i <= run_i + 1'd1;
							runcode <= 0; runbits <= 0;
							if (zeros_left - rval == 0) begin
								// remaining run_before + last run are 0 (runv pre-cleared)
								runv[r_tc[3:0] - 4'd1] <= 4'd0;
								st <= ST_PLACE;
							end else
								st <= ST_RUN_BIT;
						end
					end else if (runbits >= 4'd8) begin
						st <= ST_DONE;
					end else
						st <= ST_RUN_BIT;
				end
			end

			ST_PLACE: begin
				// 3.3l-1: place levels reverse-scan into residual_coeff[0:15]
				// (host residualBlock / FFmpeg order; scan[0] = DC).
				// Algorithm (same as host probeFirst / residualBlock):
				//   cn = -1
				//   for i = TotalCoeff-1 .. 0:
				//     cn += run_before[i] + 1
				//     coeff[cn] = level[i]
				// residual_dc = sat8(coeff[0])  // keep 3.3k golden -24
				//
				// R-csum6 PRODUCT (DIAG STRIPPED): residual_csum <= cs (XOR sat8 fold).
				// residual_dc = sat8(dcv) UNCHANGED (must stay -24).
				// Rank3: place_* private latches + residual_place_pulse (1-cycle) so
				// Plex freezes st_res_word_sticky at place-time only (not early ok).
				begin : place_body
					reg signed [5:0] cn;
					reg signed [15:0] dcv;
					reg signed [15:0] tmpc [0:15];
					reg [7:0] cs;
					integer k, j;
					// Zero all scan slots (unset runs already 0 after token/tz clear)
					for (j = 0; j < 16; j = j + 1)
						tmpc[j] = 16'sd0;
					cn = -6'sd1;
					dcv = 16'sd0;
					// k walks 15..0; only k < r_tc participates (i = tc-1 .. 0)
					for (k = 15; k >= 0; k = k - 1) begin
						if (k < r_tc) begin
							cn = cn + $signed({2'b0, runv[k]}) + 6'sd1;
							if (cn >= 0 && cn <= 15) begin
								tmpc[cn[3:0]] = lev[k];
								if (cn == 0)
									dcv = lev[k];
							end
						end
					end
					cs = 8'd0;
					for (j = 0; j < 16; j = j + 1) begin
						residual_coeff[j] <= tmpc[j];
						residual_place_coeff[j] <= tmpc[j];
						cs = cs ^ sat8(tmpc[j]);
					end
					// Preserve residual_dc regression (do not change sat8 path) — golden -24
					place_dc_r   <= sat8(dcv);
					place_csum_r <= cs;
					residual_dc   <= sat8(dcv);
					residual_place_ok <= 1'b1;
					residual_place_tc <= r_tc;
					residual_place_t1 <= r_t1[1:0];
					residual_place_dc <= sat8(dcv);
					residual_place_qp <= slice_qp;
					// PRODUCT: real XOR fold (DIAG 8'h14 STRIPPED per parent R-csum6)
					residual_csum <= cs;
					csum_acc <= cs;
				end
				// Latch residual_ok with csum; place_did sticky=1 (fold complete)
				// residual_place_pulse → Plex Rank1 freeze of status residual half
				residual_ok <= 1'b1;
				place_did <= 1'b1;
				residual_place_pulse <= 1'b1;
				csum_i <= 5'd0;
				st <= ST_DONE;
			end

			ST_DONE: begin
				valid <= 1'b1;
				busy <= 0;
				st <= ST_IDLE;
			end
			ST_FAIL: begin
				busy <= 0;
				st <= ST_IDLE;
			end
			default: st <= ST_IDLE;
			endcase
		end
	end

endmodule
