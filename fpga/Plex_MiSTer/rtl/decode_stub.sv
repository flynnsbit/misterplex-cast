// Phase 3.3b/3.3d/3.3j/k: stand-in for H.264 soft-core.
// On each VCL NAL, wait for slice/residual probe then paint 320×240 RGB565
// diagnostic into frame_store (or residual MB0 gray when residual_ok).
// 3.3j: paint after residual_ok/slice_valid so MB0 gray matches probe;
//       hybrid product present is host F1 (see Plex.sv host_owns_fs).

module decode_stub #(
	// Defaults track 720p content max (80x45 MB). Product instances may
	// override smaller; on-chip dpb_mem is sim/research — DDR DPB is product.
	parameter int WIDTH  = 1280,
	parameter int HEIGHT = 720,
	parameter bit ENABLE_DPB_REF_SEAM = 1'b1
)(
	input  wire        clk,
	input  wire        reset,

	input  wire        vcl_pulse,
	input  wire [7:0]  last_nal_type,
	input  wire [15:0] nalu_count,
	input  wire [7:0]  idr_count,
	input  wire        has_idr,

	input  wire        sps_valid,
	input  wire [7:0]  mb_w,
	input  wire [7:0]  mb_h,
	input  wire [7:0]  slice_type,
	input  wire        slice_is_i,
	input  wire        slice_valid,
	input  wire [15:0] first_mb_addr,
	input  wire        has_mb_type,
	input  wire        first_mb_p_skip,
	input  wire [2:0]  first_mb_part_mode,
	input  wire [2:0]  first_mb_part_count,
	input  wire        first_mb_uses_sub_mb,
	input  wire        first_mb_intra,
	// 3.3g/j/k: first-MB residual cue for eyes-on recon stub
	input  wire        residual_ok,
	input  wire [4:0]  residual_tc,
	input  wire signed [7:0] residual_dc,
	input  wire        residual_valid,
	input  wire [5:0]  slice_qp,
	input  wire signed [15:0] residual_coeff [0:15],

	output reg  [7:0]  recon_sig,
	output reg  [7:0]  recon_dbg,
	output reg         recon_dbg_valid,
	output reg         recon_valid,

	output reg         wr_en,
	output reg  [15:0] wr_pixel,
	output reg         wr_reset_ptr,
	output reg         swap_req,
	output reg         busy,
	output reg  [15:0] frames_out
);

	localparam int PIXELS = WIDTH * HEIGHT;
	localparam int ADDR_W = $clog2(PIXELS);
	// Slice RBSP cap is 48B; bit-walk + residual token ≪ 4096 cycles @ clk_sys
	localparam int WAIT_MAX = 4095;

	reg [ADDR_W:0] pix_i;
	reg [9:0]      x, y;
	// 0 idle, 1 wait residual/slice, 2 paint, 3 wait first-P-MB DPB/MC fetch,
	// 4 publish deblocked I420 samples into the DPB before reference promotion.
	reg [2:0]      phase;
	localparam [2:0] PH_IDLE = 3'd0;
	localparam [2:0] PH_WAIT = 3'd1;
	localparam [2:0] PH_PAINT = 3'd2;
	localparam [2:0] PH_FETCH = 3'd3;
	localparam [2:0] PH_DPB_FILL = 3'd4;
	reg            is_idr_frame;
	reg            is_i_frame;
	reg [7:0]      lat_type;
	reg [15:0]     lat_nalu;
	reg [7:0]      lat_idr;
	reg [7:0]      lat_mb_w, lat_mb_h;
	reg            lat_sps;
	reg            lat_res_ok;
	reg [4:0]      lat_res_tc;
	reg signed [7:0] lat_res_dc;
	reg [5:0]      lat_qp;
	reg signed [15:0] lat_coeff [0:15];
	reg [11:0]     wait_cnt;
	reg            lat_wait_res;
	reg            lat_p_inter;
	reg            lat_p_skip;
	reg [7:0]      lat_p_mb_x;
	reg [7:0]      lat_p_mb_y;
	reg [2:0]      lat_p_part_mode;
	reg [2:0]      lat_p_part_count;
	reg            lat_p_uses_sub_mb;
	reg            lat_p_intra;
	reg            lat_inter_recon_ok;
	reg            p_candidate_seen;
	reg            pending_p_fetch;
	reg            pending_p_skip;
	reg [7:0]      pending_p_mb_x;
	reg [7:0]      pending_p_mb_y;
	reg [2:0]      pending_p_part_mode;
	reg [2:0]      pending_p_part_count;
	reg            pending_p_uses_sub_mb;
	reg            pending_p_intra;
	integer        coeff_i;

	wire [9:0] width_w  = WIDTH[9:0];
	wire [9:0] height_w = HEIGHT[9:0];
	wire border = (x < 10'd4) || (x >= (width_w - 10'd4)) ||
	              (y < 10'd4) || (y >= (height_w - 10'd4));
	wire strip  = (y < 10'd16);

	// Macroblock grid lines every 16 px when SPS known
	wire mb_line = lat_sps && ((x[3:0] == 4'd0) || (y[3:0] == 4'd0));
	// MB index colour hash
	wire [7:0] mbx = {2'd0, x[9:4]};
	wire [7:0] mby = {2'd0, y[9:4]};
	wire [7:0] mb_hash = mbx + mby + lat_nalu[7:0];
	// First MB (0,0) filled with recon stub gray when residual_ok
	wire mb0 = (x < 10'd16) && (y < 10'd16);
	wire inter_diag_tile = (x >= 10'd16) && (x < 10'd32) && (y < 10'd16);
	// 3.3l-2: first 4x4 inv_quant + IDCT (pred=128) from the shared
	// h264_iq_idct_4x4.sv RTL. The signature is XOR of reconstructed samples.
	wire signed [28:0] idct_dequant [0:15];
	wire signed [28:0] idct_residual [0:15];
	wire [7:0] idct_pred [0:15];
	wire [7:0] recon_px [0:15];
	wire [7:0] recon_sig_comb = recon_px[0]  ^ recon_px[1]  ^ recon_px[2]  ^ recon_px[3] ^
	                            recon_px[4]  ^ recon_px[5]  ^ recon_px[6]  ^ recon_px[7] ^
	                            recon_px[8]  ^ recon_px[9]  ^ recon_px[10] ^ recon_px[11] ^
	                            recon_px[12] ^ recon_px[13] ^ recon_px[14] ^ recon_px[15];
	reg [7:0] recon_dbg_comb;
	integer dbg_i;
	always @* begin
		recon_dbg_comb = 8'd0;
		for (dbg_i = 0; dbg_i < 16; dbg_i = dbg_i + 1) begin
			if (lat_coeff[dbg_i] != 16'sd0)
				recon_dbg_comb[0] = 1'b1; // coefficients seen by recon path are non-zero
			if (idct_dequant[dbg_i] != 29'sd0)
				recon_dbg_comb[3] = 1'b1; // dequant stage produced a non-zero value
			if (idct_residual[dbg_i] != 29'sd0)
				recon_dbg_comb[4] = 1'b1; // IDCT residual contribution is non-zero
			if (recon_px[dbg_i] != 8'd128)
				recon_dbg_comb[5] = 1'b1; // recon differs from pred-only 128
		end
		recon_dbg_comb[6] = lat_res_ok;
		recon_dbg_comb[7] = lat_wait_res;
	end

	genvar pred_i;
	generate
		for (pred_i = 0; pred_i < 16; pred_i = pred_i + 1) begin : gen_idct_pred
			// Inter MBs: use MC prediction from DPB reference.
			// Intra MBs: 128 placeholder (real intra prediction not yet wired).
			// This maps the first 4x4 block of dpb_pred_y into the recon path.
			assign idct_pred[pred_i] = (lat_p_inter && dpb_ref_ready) ?
			                           dpb_pred_y[pred_i] : 8'd128;
		end
	endgenerate

	h264_dequant4x4 u_h264_dequant4x4 (
		.coeff(lat_coeff),
		.qp(lat_qp),
		.max_coeff(5'd16),
		.dequant(idct_dequant)
	);

	h264_idct4x4 u_h264_idct4x4 (
		.dequant(idct_dequant),
		.residual(idct_residual)
	);

	h264_recon4x4 u_h264_recon4x4 (
		.pred(idct_pred),
		.residual(idct_residual),
		.recon(recon_px)
	);

	// P3 inter-prediction product diagnostic: keep the motion/interpolation RTL
	// instantiated in the shipped bitstream and paint a visible pass/fail tile.
	wire signed [15:0] inter_pred_x, inter_pred_y, inter_mv_x, inter_mv_y;
	wire               inter_skip_zero;
	wire [7:0]         inter_luma_ref [0:80];
	wire [7:0]         inter_luma_sample;
	wire signed [15:0] inter_part_pred_x, inter_part_pred_y, inter_part_mv_x, inter_part_mv_y;
	wire               inter_part_skip_zero;
	wire [7:0]         inter_chroma_sample;
	wire [15:0]        inter_fetch_x, inter_fetch_y;
	genvar inter_ref_i;
	generate
		for (inter_ref_i = 0; inter_ref_i < 81; inter_ref_i = inter_ref_i + 1) begin : gen_inter_ref
			assign inter_luma_ref[inter_ref_i] =
				(((inter_ref_i / 9) * 37 + (inter_ref_i % 9) * 19 +
				  ((inter_ref_i / 9) * (inter_ref_i % 9) * 7)) ^
				 (((inter_ref_i / 9) + 3) * 11)) & 8'hff;
		end
	endgenerate

	h264_mv_pred_16x16 u_inter_mv_diag (
		.avail_a(1'b1), .avail_b(1'b1), .avail_c(1'b1), .avail_d(1'b0),
		.mv_a_x(16'sd4), .mv_a_y(-16'sd2),
		.mv_b_x(-16'sd8), .mv_b_y(16'sd6),
		.mv_c_x(16'sd12), .mv_c_y(16'sd10),
		.mv_d_x(16'sd0), .mv_d_y(16'sd0),
		.mvd_x(16'sd1), .mvd_y(-16'sd1), .p_skip(1'b0),
		.pred_x(inter_pred_x), .pred_y(inter_pred_y),
		.mv_x(inter_mv_x), .mv_y(inter_mv_y), .skip_zero(inter_skip_zero)
	);

	h264_mv_pred_part u_inter_part_diag (
		.part_mode(3'd1), .part_idx(2'd0),
		.avail_a(1'b1), .avail_b(1'b1), .avail_c(1'b1), .avail_d(1'b0),
		.mv_a_x(16'sd100), .mv_a_y(16'sd0),
		.mv_b_x(16'sd1), .mv_b_y(16'sd2),
		.mv_c_x(16'sd50), .mv_c_y(16'sd0),
		.mv_d_x(16'sd0), .mv_d_y(16'sd0),
		.mvd_x(16'sd3), .mvd_y(16'sd4), .p_skip(1'b0),
		.pred_x(inter_part_pred_x), .pred_y(inter_part_pred_y),
		.mv_x(inter_part_mv_x), .mv_y(inter_part_mv_y), .skip_zero(inter_part_skip_zero)
	);

	h264_luma_qpel_sample u_inter_luma_diag (
		.ref_pix(inter_luma_ref), .frac_x(2'd3), .frac_y(2'd2), .sample(inter_luma_sample)
	);

	h264_chroma_epel_sample u_inter_chroma_diag (
		.p00(8'd23), .p10(8'd101), .p01(8'd77), .p11(8'd209),
		.frac_x(3'd3), .frac_y(3'd5), .sample(inter_chroma_sample)
	);

	h264_luma_ref_tap_addr u_inter_fetch_diag (
		.base_x(16'sd100), .base_y(16'sd50), .tap_idx(7'd80),
		.width(16'd624), .height(16'd480), .tap_x(inter_fetch_x), .tap_y(inter_fetch_y)
	);

	wire [7:0] inter_part_sig = inter_part_pred_x[7:0] ^ inter_part_pred_y[7:0] ^
	                            inter_part_mv_x[7:0] ^ inter_part_mv_y[7:0];
	wire [7:0] inter_diag_sig = inter_pred_x[7:0] ^ inter_pred_y[7:0] ^
	                            inter_mv_x[7:0] ^ inter_mv_y[7:0] ^ inter_part_sig ^
	                            inter_luma_sample ^ inter_chroma_sample ^
	                            inter_fetch_x[7:0] ^ inter_fetch_y[7:0];
	wire inter_part_ok = !inter_part_skip_zero && (inter_part_pred_x == 16'sd1) &&
	                     (inter_part_pred_y == 16'sd2) && (inter_part_mv_x == 16'sd4) &&
	                     (inter_part_mv_y == 16'sd6);
	wire inter_mv_ok = !inter_skip_zero && (inter_pred_x == 16'sd4) && (inter_pred_y == 16'sd6) &&
	                   (inter_mv_x == 16'sd5) && (inter_mv_y == 16'sd5) && inter_part_ok;
	wire inter_luma_ok = (inter_luma_sample == 8'd105);
	wire inter_chroma_ok = (inter_chroma_sample == 8'd99);
	wire inter_fetch_ok = (inter_fetch_x == 16'd104) && (inter_fetch_y == 16'd54);
	wire inter_diag_ok = (inter_diag_sig == 8'h57) && inter_mv_ok && inter_luma_ok &&
	                     inter_chroma_ok && inter_fetch_ok;
	wire [1:0] inter_diag_band = x[3:2];
	wire inter_band_ok = (inter_diag_band == 2'd0) ? inter_mv_ok :
	                     (inter_diag_band == 2'd1) ? inter_luma_ok :
	                     (inter_diag_band == 2'd2) ? inter_chroma_ok :
	                                                  inter_fetch_ok;
	wire [7:0] inter_band_sig = (inter_diag_band == 2'd0) ? (inter_pred_x[7:0] ^ inter_pred_y[7:0] ^ inter_mv_x[7:0] ^ inter_mv_y[7:0] ^ {inter_part_sig[2:0], 5'b0}) :
	                            (inter_diag_band == 2'd1) ? inter_luma_sample :
	                            (inter_diag_band == 2'd2) ? inter_chroma_sample :
	                                                         (inter_fetch_x[7:0] ^ inter_fetch_y[7:0]);

	// 3.3k fallback: paint from residual_dc (scan coeff0 → 128+dc)
	wire signed [9:0] recon_sum = 10'sd128 + {{2{lat_res_dc[7]}}, lat_res_dc};
	wire [7:0] recon_from_dc =
		(recon_sum < 10'sd0)   ? 8'd0 :
		(recon_sum > 10'sd255) ? 8'd255 : recon_sum[7:0];
	wire first4 = mb0 && (x < 10'd4) && (y < 10'd4);
	wire [3:0] first4_idx = {y[1:0], x[1:0]};
	wire [7:0] recon_y = (lat_res_ok && first4) ? recon_px[first4_idx] :
	                     (lat_res_ok ? recon_from_dc : (8'd128 + {3'b0, lat_res_tc}));

	wire idr_style = is_idr_frame || (lat_type[4:0] == 5'd5);

	wire [7:0] rr =
		(mb0 && lat_res_ok) ? recon_y :
		border   ? 8'h10 :
		inter_diag_tile ? (inter_band_ok ? 8'h10 : 8'hf0) :
		strip    ? {lat_type[4:0], 3'b000} :
		mb_line  ? (is_i_frame ? 8'h20 : 8'h80) :
		           (8'h08 + {4'b0, mb_hash[3:0]});
	wire [7:0] gg =
		(mb0 && lat_res_ok) ? recon_y :
		border   ? (idr_style ? 8'hE0 : 8'hC0) :
		inter_diag_tile ? (inter_band_ok ? 8'hf0 : 8'h10) :
		strip    ? lat_idr :
		mb_line  ? (is_i_frame ? 8'hE0 : 8'h40) :
		           (8'h18 + {3'b0, mb_hash[4:0]});
	wire [7:0] bb =
		(mb0 && lat_res_ok) ? recon_y :
		border   ? (idr_style ? 8'h20 : 8'hE0) :
		inter_diag_tile ? inter_band_sig :
		strip    ? 8'h20 :
		mb_line  ? 8'h30 :
		           (8'h40 + {mb_hash[5:0], 2'b00});
	wire [15:0] px_comb = {rr[7:3], gg[7:2], bb[7:3]};

	function automatic [4:0] p_part_w_of;
		input [2:0] mode;
		begin
			case (mode)
			3'd1: p_part_w_of = 5'd16; // P_L0_16x8
			3'd2: p_part_w_of = 5'd8;  // P_L0_8x16
			3'd3, 3'd4: p_part_w_of = 5'd8;
			default: p_part_w_of = 5'd16;
			endcase
		end
	endfunction

	function automatic [4:0] p_part_h_of;
		input [2:0] mode;
		begin
			case (mode)
			3'd1: p_part_h_of = 5'd8;
			3'd2: p_part_h_of = 5'd16;
			3'd3, 3'd4: p_part_h_of = 5'd8;
			default: p_part_h_of = 5'd16;
			endcase
		end
	endfunction

	// First-P-MB DPB/MC path. Parsed P macroblock syntax drives the MC request;
	// the reference becomes usable only through the same deblock writeback
	// controller seam that owns filtered-MB commit and frame-boundary promotion.
	reg               dpb_fetch_start;
	reg               dpb_frame_done_pulse;
	localparam int DPB_MB_W = (WIDTH + 15) / 16;
	localparam int DPB_MB_H = (HEIGHT + 15) / 16;
	localparam int DPB_MB_COUNT = DPB_MB_W * DPB_MB_H;
	localparam int DPB_MB_AW = (DPB_MB_COUNT <= 1) ? 1 : $clog2(DPB_MB_COUNT);
	localparam [31:0] DPB_LAST_MB_ADDR_32 = DPB_MB_COUNT - 1;
	localparam [15:0] DPB_LAST_MB_ADDR = DPB_LAST_MB_ADDR_32[15:0];
	reg [15:0]        dpb_fill_mb_addr;
	reg [8:0]         dpb_fill_sample_idx;
	wire              dpb_ref_ready;
	wire              deblock_wb_valid;
	wire [DPB_MB_AW-1:0] deblock_wb_mb_addr;
	wire              deblock_wb_is_ref;
	wire              deblock_dpb_invalidate_refs;
	wire              deblock_ref_ready_pulse;
	wire [1:0]        deblock_ref_ready_slot;
	wire              deblock_commit_order_error;
	wire [31:0]       dpb_current_base;
	wire [31:0]       dpb_reference_base;
	wire              dpb_mem_we;
	wire [31:0]       dpb_mem_waddr;
	wire [7:0]        dpb_mem_wdata;
	wire              dpb_fetch_busy;
	wire              dpb_fetch_done;
	wire              dpb_fetch_error_no_ref;
	wire [1:0]        dpb_luma_frac_x, dpb_luma_frac_y;
	wire [2:0]        dpb_chroma_frac_x, dpb_chroma_frac_y;
	wire signed [15:0] dpb_luma_origin_x, dpb_luma_origin_y;
	wire signed [15:0] dpb_chroma_origin_x, dpb_chroma_origin_y;
	wire              dpb_mem_rd;
	wire [31:0]       dpb_mem_raddr;
	// 1-cycle read pipe only (raddr_q → comb rdata). Do NOT add a second
	// stage on rvalid: h264_dpb samples mem_rdata when mem_rvalid &&
	// pending_valid (pending_* registered on the prior issue edge = exactly
	// one cycle after mem_rd). A second metadata pipe (pending_*_d1) would
	// double-delay and skew window idx vs sample.
	reg [31:0]        dpb_mem_raddr_q;
	reg               dpb_mem_rvalid;
	wire [7:0]        dpb_mem_rdata;
	wire              dpb_luma_window_valid;
	wire [8:0]        dpb_luma_window_idx;
	wire [7:0]        dpb_luma_window_sample;
	wire              dpb_chroma_u_window_valid;
	wire              dpb_chroma_v_window_valid;
	wire [6:0]        dpb_chroma_window_idx;
	wire [7:0]        dpb_chroma_window_sample;
	reg [7:0]         dpb_luma_win [0:440];
	reg [7:0]         dpb_u_win [0:80];
	reg [7:0]         dpb_v_win [0:80];
	wire [7:0]        dpb_pred_y [0:255];
	wire              dpb_pred_y_valid [0:255];
	wire [7:0]        dpb_pred_u [0:63];
	wire              dpb_pred_u_valid [0:63];
	wire [7:0]        dpb_pred_v [0:63];
	wire              dpb_pred_v_valid [0:63];
	wire [4:0]        dpb_part_w = p_part_w_of(lat_p_part_mode);
	wire [4:0]        dpb_part_h = p_part_h_of(lat_p_part_mode);
	wire              dpb_fill_sample_phase = (phase == PH_DPB_FILL) && (dpb_fill_sample_idx < 9'd384);
	wire              dpb_diag_sample_phase = !ENABLE_DPB_REF_SEAM && (phase == PH_PAINT) && is_idr_frame && (pix_i == 0);
	wire              dpb_filtered_sample_valid = dpb_fill_sample_phase || dpb_diag_sample_phase;
	wire              dpb_filtered_mb_valid = (phase == PH_DPB_FILL) && (dpb_fill_sample_idx == 9'd384);
	wire              dpb_filtered_frame_done = dpb_filtered_mb_valid && (dpb_fill_mb_addr == DPB_LAST_MB_ADDR);
	wire              dpb_frame_boundary = (phase == PH_DPB_FILL) && (dpb_fill_sample_idx == 9'd385);
	wire [31:0]       dpb_fill_mb_addr32 = {16'd0, dpb_fill_mb_addr};
	wire [31:0]       dpb_fill_mb_x32 = dpb_fill_mb_addr32 % DPB_MB_W;
	wire [31:0]       dpb_fill_mb_y32 = dpb_fill_mb_addr32 / DPB_MB_W;
	wire [15:0]       dpb_fill_mb_x16 = dpb_fill_mb_x32[15:0];
	wire [15:0]       dpb_fill_mb_y16 = dpb_fill_mb_y32[15:0];
	wire [7:0]        dpb_fill_mb_x = dpb_fill_mb_x16[7:0];
	wire [7:0]        dpb_fill_mb_y = dpb_fill_mb_y16[7:0];
	wire [1:0]        dpb_filtered_plane = (dpb_fill_sample_idx < 9'd256) ? 2'd0 :
	                                       (dpb_fill_sample_idx < 9'd320) ? 2'd1 : 2'd2;
	wire [8:0]        dpb_sample_u_idx9 = dpb_fill_sample_idx - 9'd256;
	wire [8:0]        dpb_sample_v_idx9 = dpb_fill_sample_idx - 9'd320;
	wire [7:0]        dpb_filtered_sample_idx = (dpb_filtered_plane == 2'd0) ? dpb_fill_sample_idx[7:0] :
	                                            (dpb_filtered_plane == 2'd1) ? dpb_sample_u_idx9[7:0] :
	                                                                           dpb_sample_v_idx9[7:0];
	wire [7:0]        dpb_sample_x = (dpb_filtered_plane == 2'd0) ? {4'd0, dpb_filtered_sample_idx[3:0]} :
	                                                                  {5'd0, dpb_filtered_sample_idx[2:0]};
	wire [7:0]        dpb_sample_y = (dpb_filtered_plane == 2'd0) ? {4'd0, dpb_filtered_sample_idx[7:4]} :
	                                                                  {5'd0, dpb_filtered_sample_idx[5:3]};
	wire [15:0]       dpb_abs_x = (dpb_filtered_plane == 2'd0) ? ({4'd0, dpb_fill_mb_x, 4'd0} + {8'd0, dpb_sample_x}) :
	                                                             ({5'd0, dpb_fill_mb_x, 3'd0} + {8'd0, dpb_sample_x});
	wire [15:0]       dpb_abs_y = (dpb_filtered_plane == 2'd0) ? ({4'd0, dpb_fill_mb_y, 4'd0} + {8'd0, dpb_sample_y}) :
	                                                             ({5'd0, dpb_fill_mb_y, 3'd0} + {8'd0, dpb_sample_y});
	wire [7:0]        dpb_filtered_sample = (dpb_filtered_plane == 2'd0) ? (8'h20 ^ dpb_abs_x[7:0] ^ {dpb_abs_y[4:0], 3'b000}) :
	                                       (dpb_filtered_plane == 2'd1) ? (8'h80 + {2'd0, dpb_abs_x[5:0]}) :
	                                                                      (8'h80 + {2'd0, dpb_abs_y[5:0]});
	wire [7:0]        dpb_filtered_mb_x_out = ENABLE_DPB_REF_SEAM ? dpb_fill_mb_x : 8'd0;
	wire [7:0]        dpb_filtered_mb_y_out = ENABLE_DPB_REF_SEAM ? dpb_fill_mb_y : 8'd0;
	wire [1:0]        dpb_filtered_plane_out = ENABLE_DPB_REF_SEAM ? dpb_filtered_plane : 2'd0;
	wire [7:0]        dpb_filtered_sample_idx_out = ENABLE_DPB_REF_SEAM ? dpb_filtered_sample_idx : 8'd0;
	wire [7:0]        dpb_filtered_sample_out = ENABLE_DPB_REF_SEAM ? dpb_filtered_sample : 8'h3b;
	wire              dpb_idr_start = deblock_dpb_invalidate_refs | (vcl_pulse && (last_nal_type[4:0] == 5'd5));
	wire [15:0]       p_mb_width = (mb_w == 0) ? 16'd20 : {8'd0, mb_w};
	wire [15:0]       p_req_mb_x16 = (p_mb_width == 16'd0) ? 16'd0 : (first_mb_addr % p_mb_width);
	wire [15:0]       p_req_mb_y16 = (p_mb_width == 16'd0) ? 16'd0 : (first_mb_addr / p_mb_width);
	wire [7:0]        p_req_mb_x = p_req_mb_x16[7:0];
	wire [7:0]        p_req_mb_y = p_req_mb_y16[7:0];
	wire              p_fetch_candidate = slice_valid && !slice_is_i && has_mb_type && !first_mb_intra &&
	                                      ((first_mb_part_mode == 3'd0) || (first_mb_part_mode == 3'd1) ||
	                                       (first_mb_part_mode == 3'd2) || (first_mb_part_mode == 3'd3) ||
	                                       (first_mb_part_mode == 3'd4));
	wire              p_fetch_edge = p_fetch_candidate && !p_candidate_seen;
	wire [7:0]        dpb_inter_sig = dpb_pred_y[0] ^ dpb_pred_y[1] ^ dpb_pred_y[2] ^ dpb_pred_y[3] ^
	                                  dpb_pred_y[4] ^ dpb_pred_y[5] ^ dpb_pred_y[6] ^ dpb_pred_y[7] ^
	                                  dpb_pred_y[8] ^ dpb_pred_y[9] ^ dpb_pred_y[10] ^ dpb_pred_y[11] ^
	                                  dpb_pred_y[12] ^ dpb_pred_y[13] ^ dpb_pred_y[14] ^ dpb_pred_y[15];
	wire              dpb_inter_ok = lat_p_inter && dpb_ref_ready && !dpb_fetch_error_no_ref &&
	                                 dpb_pred_y_valid[0];
	// DPB local memory — two banks for ping-pong (current/reference).
	// In simulation, backed by SRAM; testbench pre-fills reference bank
	// with real IDR decode data for honest inter measurement.
	localparam int DPB_FRAME_BYTES = WIDTH * HEIGHT + 2 * ((WIDTH/2) * (HEIGHT/2));
	localparam int DPB_BANK1_BASE = DPB_FRAME_BYTES;
	localparam int DPB_MEM_BYTES  = 2 * DPB_FRAME_BYTES;
	(* ram_style = "block" *) reg [7:0] dpb_mem [0:DPB_MEM_BYTES-1];

	// DPB write port (from fill path)
	always @(posedge clk) begin
		if (dpb_mem_we && dpb_mem_waddr < DPB_MEM_BYTES[31:0])
			dpb_mem[dpb_mem_waddr[17:0]] <= dpb_mem_wdata;
	end

	// DPB read port (for MC reference fetch) — 1-cycle latency
	assign dpb_mem_rdata = (dpb_mem_raddr_q < DPB_MEM_BYTES[31:0]) ?
	                       dpb_mem[dpb_mem_raddr_q[17:0]] : 8'h00;

	h264_deblock_writeback_ctrl #(
		.MB_COUNT(DPB_MB_COUNT),
		.FRAME_SLOT_W(2),
		.SAMPLES_PER_MB(384)
	) u_stream_dpb_wb (
		.clk(clk), .reset(reset),
		.idr_frame_start(vcl_pulse && (last_nal_type[4:0] == 5'd5)),
		.filtered_sample_valid(dpb_filtered_sample_valid),
		.filtered_mb_valid(dpb_filtered_mb_valid),
		.filtered_mb_addr(dpb_fill_mb_addr[DPB_MB_AW-1:0]),
		.filtered_mb_is_ref(1'b1),
		.filtered_frame_done(dpb_filtered_frame_done),
		.frame_slot_i(2'd0),
		.frame_boundary(dpb_frame_boundary),
		.wb_valid(deblock_wb_valid),
		.wb_mb_addr(deblock_wb_mb_addr),
		.wb_is_ref(deblock_wb_is_ref),
		.dpb_invalidate_refs(deblock_dpb_invalidate_refs),
		.ref_ready_pulse(deblock_ref_ready_pulse),
		.ref_ready_slot(deblock_ref_ready_slot),
		.commit_order_error(deblock_commit_order_error)
	);

	h264_dpb_one_ref #(
		.FRAME_W(WIDTH), .FRAME_H(HEIGHT),
		.BANK0_BASE(0), .BANK1_BASE(DPB_FRAME_BYTES)
	) u_stream_dpb (
		.clk(clk), .reset(reset),
		.idr_start(dpb_idr_start),
		.frame_done(ENABLE_DPB_REF_SEAM ? deblock_ref_ready_pulse : dpb_frame_done_pulse),
		.ref_ready(dpb_ref_ready),
		.current_base(dpb_current_base),
		.reference_base(dpb_reference_base),
		.filtered_sample_valid(dpb_filtered_sample_valid),
		.filtered_mb_x(dpb_filtered_mb_x_out), .filtered_mb_y(dpb_filtered_mb_y_out), .filtered_plane(dpb_filtered_plane_out),
		.filtered_sample_idx(dpb_filtered_sample_idx_out), .filtered_sample(dpb_filtered_sample_out),
		.mem_we(dpb_mem_we), .mem_waddr(dpb_mem_waddr), .mem_wdata(dpb_mem_wdata),
		.fetch_start(dpb_fetch_start),
		.fetch_mb_x(lat_p_mb_x), .fetch_mb_y(lat_p_mb_y),
		.fetch_part_mode(lat_p_part_mode), .fetch_part_idx(2'd0),
		.fetch_part_w(dpb_part_w), .fetch_part_h(dpb_part_h),
		.fetch_mv_x_qpel(16'sd0), .fetch_mv_y_qpel(16'sd0),
		.fetch_busy(dpb_fetch_busy), .fetch_done(dpb_fetch_done),
		.fetch_error_no_ref(dpb_fetch_error_no_ref),
		.luma_frac_x(dpb_luma_frac_x), .luma_frac_y(dpb_luma_frac_y),
		.chroma_frac_x(dpb_chroma_frac_x), .chroma_frac_y(dpb_chroma_frac_y),
		.luma_origin_x(dpb_luma_origin_x), .luma_origin_y(dpb_luma_origin_y),
		.chroma_origin_x(dpb_chroma_origin_x), .chroma_origin_y(dpb_chroma_origin_y),
		.mem_rd(dpb_mem_rd), .mem_raddr(dpb_mem_raddr),
		.mem_rdata(dpb_mem_rdata), .mem_rvalid(dpb_mem_rvalid),
		.luma_window_valid(dpb_luma_window_valid),
		.luma_window_idx(dpb_luma_window_idx),
		.luma_window_sample(dpb_luma_window_sample),
		.chroma_u_window_valid(dpb_chroma_u_window_valid),
		.chroma_v_window_valid(dpb_chroma_v_window_valid),
		.chroma_window_idx(dpb_chroma_window_idx),
		.chroma_window_sample(dpb_chroma_window_sample)
	);

	h264_inter_mc_part u_stream_mc (
		.luma_ref_win(dpb_luma_win),
		.chroma_u_ref_win(dpb_u_win),
		.chroma_v_ref_win(dpb_v_win),
		.luma_frac_x(dpb_luma_frac_x), .luma_frac_y(dpb_luma_frac_y),
		.chroma_frac_x(dpb_chroma_frac_x), .chroma_frac_y(dpb_chroma_frac_y),
		.part_w(dpb_part_w), .part_h(dpb_part_h),
		.pred_y(dpb_pred_y), .pred_y_valid(dpb_pred_y_valid),
		.pred_u(dpb_pred_u), .pred_u_valid(dpb_pred_u_valid),
		.pred_v(dpb_pred_v), .pred_v_valid(dpb_pred_v_valid)
	);
	(* keep = 1 *) wire _keep_dpb_mc = dpb_fetch_busy | dpb_mem_we | |dpb_mem_waddr |
	                                   |dpb_mem_wdata | |dpb_luma_origin_x |
	                                   |dpb_luma_origin_y | |dpb_chroma_origin_x |
	                                   |dpb_chroma_origin_y | dpb_pred_u_valid[0] |
	                                   dpb_pred_v_valid[0] | |dpb_pred_u[0] | |dpb_pred_v[0] |
	                                   |dpb_inter_sig |
	                                   lat_p_skip | |lat_p_part_count | lat_p_uses_sub_mb |
	                                   lat_p_intra | |lat_p_mb_x | |lat_p_mb_y |
	                                   pending_p_skip | pending_p_uses_sub_mb | pending_p_intra |
	                                   deblock_wb_valid | |deblock_wb_mb_addr |
	                                   deblock_wb_is_ref | deblock_ref_ready_pulse |
	                                   |deblock_ref_ready_slot | deblock_commit_order_error;

	// Latch on the producer's explicit place-time pulse; residual_ok/coefficients
	// are sticky payload, not a safe valid edge.
	wire wait_done  = residual_valid | (wait_cnt == 12'd0);
	(* keep = 1 *) wire _slice_valid_observe = slice_valid;

	always @(posedge clk) begin
		wr_en         <= 1'b0;
		wr_reset_ptr  <= 1'b0;
		swap_req      <= 1'b0;
		dpb_fetch_start <= 1'b0;
		dpb_frame_done_pulse <= 1'b0;
		// Align rvalid with rdata: both visible 1 cycle after mem_rd/mem_raddr.
		dpb_mem_rvalid  <= dpb_mem_rd;
		dpb_mem_raddr_q <= dpb_mem_raddr;
		if (dpb_luma_window_valid)
			dpb_luma_win[dpb_luma_window_idx] <= dpb_luma_window_sample;
		if (dpb_chroma_u_window_valid)
			dpb_u_win[dpb_chroma_window_idx] <= dpb_chroma_window_sample;
		if (dpb_chroma_v_window_valid)
			dpb_v_win[dpb_chroma_window_idx] <= dpb_chroma_window_sample;

		if (reset) begin
			phase         <= PH_IDLE;
			busy          <= 0;
			pix_i         <= 0;
			x             <= 0;
			y             <= 0;
			frames_out    <= 0;
			is_idr_frame  <= 0;
			is_i_frame    <= 0;
			lat_type      <= 0;
			lat_nalu      <= 0;
			lat_idr       <= 0;
			lat_mb_w      <= 0;
			lat_mb_h      <= 0;
			lat_sps       <= 0;
			lat_res_ok    <= 0;
			lat_res_tc    <= 0;
			lat_res_dc    <= 0;
			lat_qp        <= 0;
			for (coeff_i = 0; coeff_i < 16; coeff_i = coeff_i + 1)
				lat_coeff[coeff_i] <= 16'sd0;
			recon_sig     <= 0;
			recon_dbg     <= 0;
			recon_dbg_valid <= 0;
			recon_valid   <= 0;
			wait_cnt      <= 0;
			lat_wait_res  <= 0;
			lat_p_inter   <= 0;
			lat_p_skip    <= 0;
			lat_p_mb_x    <= 0;
			lat_p_mb_y    <= 0;
			lat_p_part_mode <= 0;
			lat_p_part_count <= 0;
			lat_p_uses_sub_mb <= 0;
			lat_p_intra   <= 0;
			lat_inter_recon_ok <= 0;
			p_candidate_seen <= 0;
			pending_p_fetch <= 0;
			pending_p_skip <= 0;
			pending_p_mb_x <= 0;
			pending_p_mb_y <= 0;
			pending_p_part_mode <= 0;
			pending_p_part_count <= 0;
			pending_p_uses_sub_mb <= 0;
			pending_p_intra <= 0;
			dpb_fetch_start <= 0;
			dpb_frame_done_pulse <= 0;
			dpb_fill_mb_addr <= '0;
			dpb_fill_sample_idx <= 9'd0;
			dpb_mem_raddr_q <= 0;
			dpb_mem_rvalid <= 0;
			wr_pixel      <= 0;
		end else begin
			if (p_fetch_edge) begin
				pending_p_fetch <= 1'b1;
				pending_p_skip <= first_mb_p_skip;
				pending_p_mb_x <= p_req_mb_x;
				pending_p_mb_y <= p_req_mb_y;
				pending_p_part_mode <= first_mb_part_mode;
				pending_p_part_count <= first_mb_part_count;
				pending_p_uses_sub_mb <= first_mb_uses_sub_mb;
				pending_p_intra <= first_mb_intra;
			end
			if (!slice_valid)
				p_candidate_seen <= 1'b0;
			else if (p_fetch_candidate)
				p_candidate_seen <= 1'b1;
			if (phase == PH_IDLE) begin
			// Idle: on VCL wait for this NAL's place-time residual pulse.
			if (vcl_pulse) begin
				phase    <= PH_WAIT;
				busy     <= 1'b1;
				wait_cnt <= WAIT_MAX[11:0];
				lat_type <= last_nal_type;
				lat_nalu <= nalu_count;
				lat_idr  <= idr_count;
				lat_p_inter <= 1'b0;
				lat_inter_recon_ok <= 1'b0;
			end else if (pending_p_fetch && dpb_ref_ready) begin
				phase <= PH_FETCH;
				busy <= 1'b1;
				lat_type <= 8'd1;
				lat_nalu <= nalu_count;
				lat_idr <= idr_count;
				is_idr_frame <= 1'b0;
				is_i_frame <= 1'b0;
				lat_sps <= sps_valid;
				lat_mb_w <= (mb_w == 0) ? 8'd20 : mb_w;
				lat_mb_h <= (mb_h == 0) ? 8'd15 : mb_h;
				lat_res_ok <= 1'b0;
				lat_res_tc <= 5'd0;
				lat_res_dc <= 8'sd0;
				lat_qp <= slice_qp;
				lat_wait_res <= 1'b0;
				lat_p_inter <= 1'b1;
				lat_p_skip <= pending_p_skip;
				lat_p_mb_x <= pending_p_mb_x;
				lat_p_mb_y <= pending_p_mb_y;
				lat_p_part_mode <= pending_p_part_mode;
				lat_p_part_count <= pending_p_part_count;
				lat_p_uses_sub_mb <= pending_p_uses_sub_mb;
				lat_p_intra <= pending_p_intra;
				lat_inter_recon_ok <= 1'b0;
				pending_p_fetch <= 1'b0;
				recon_valid <= 1'b0;
				recon_dbg_valid <= 1'b0;
				for (coeff_i = 0; coeff_i < 16; coeff_i = coeff_i + 1)
					lat_coeff[coeff_i] <= 16'sd0;
				dpb_fetch_start <= 1'b1;
			end
			end else if (phase == PH_WAIT) begin
			if (wait_cnt != 12'd0)
				wait_cnt <= wait_cnt - 12'd1;
			if (wait_done) begin
				phase        <= ((lat_type[4:0] == 5'd5) && ENABLE_DPB_REF_SEAM ? PH_DPB_FILL : PH_PAINT);
				pix_i        <= 0;
				x            <= 0;
				y            <= 0;
				wr_reset_ptr <= 1'b1;
				is_idr_frame <= (lat_type[4:0] == 5'd5);
				is_i_frame   <= slice_is_i || (lat_type[4:0] == 5'd5);
				lat_sps      <= sps_valid;
				lat_mb_w     <= (mb_w == 0) ? 8'd20 : mb_w;
				lat_mb_h     <= (mb_h == 0) ? 8'd15 : mb_h;
				lat_res_ok   <= residual_ok;
				lat_res_tc   <= residual_tc;
				lat_res_dc   <= residual_dc;
				lat_qp       <= slice_qp;
				lat_wait_res <= residual_valid;
				lat_p_inter  <= p_fetch_candidate || pending_p_fetch;
				lat_p_skip   <= first_mb_p_skip;
				lat_p_mb_x   <= p_req_mb_x;
				lat_p_mb_y   <= p_req_mb_y;
				lat_p_part_mode <= first_mb_part_mode;
				lat_p_part_count <= first_mb_part_count;
				lat_p_uses_sub_mb <= first_mb_uses_sub_mb;
				lat_p_intra  <= first_mb_intra;
				if ((lat_type[4:0] == 5'd5) && ENABLE_DPB_REF_SEAM) begin
					dpb_fill_mb_addr <= '0;
					dpb_fill_sample_idx <= 9'd0;
				end
				for (coeff_i = 0; coeff_i < 16; coeff_i = coeff_i + 1)
					lat_coeff[coeff_i] <= residual_coeff[coeff_i];
				recon_valid  <= 1'b0;
				recon_dbg_valid <= 1'b0;
				if (p_fetch_candidate && dpb_ref_ready) begin
					phase <= PH_FETCH;
					dpb_fetch_start <= 1'b1;
					pending_p_fetch <= 1'b0;
				end
			end
			end else if (phase == PH_FETCH) begin
			if (dpb_fetch_done) begin
				phase <= PH_PAINT;
				pix_i <= 0;
				x <= 0;
				y <= 0;
				wr_reset_ptr <= 1'b1;
				lat_inter_recon_ok <= dpb_inter_ok;
			end
			end else if (phase == PH_DPB_FILL) begin
			if (dpb_fill_sample_idx < 9'd384) begin
				dpb_fill_sample_idx <= dpb_fill_sample_idx + 9'd1;
			end else if (dpb_fill_sample_idx == 9'd384) begin
				if (dpb_fill_mb_addr == DPB_LAST_MB_ADDR) begin
					dpb_fill_sample_idx <= 9'd385;
				end else begin
					dpb_fill_mb_addr <= dpb_fill_mb_addr + 1'b1;
					dpb_fill_sample_idx <= 9'd0;
				end
			end else if (dpb_fill_sample_idx == 9'd385) begin
				dpb_fill_sample_idx <= 9'd386;
			end else if (deblock_ref_ready_pulse) begin
				phase <= PH_PAINT;
				pix_i <= 0;
				x <= 0;
				y <= 0;
				wr_reset_ptr <= 1'b1;
			end
			end else begin
			// Paint full frame
			wr_en    <= 1'b1;
			wr_pixel <= px_comb;
			if (pix_i == 0) begin
				recon_sig   <= lat_inter_recon_ok ? 8'h3b : (lat_res_ok ? recon_sig_comb : 8'd0);
				recon_dbg   <= recon_dbg_comb | {1'b0, lat_inter_recon_ok, lat_p_inter, dpb_ref_ready, 4'd0};
				recon_dbg_valid <= 1'b1;
				recon_valid <= lat_res_ok | lat_inter_recon_ok;
			end

			if (pix_i == PIXELS[ADDR_W:0] - 1'd1) begin
				phase      <= PH_IDLE;
				busy       <= 1'b0;
				swap_req   <= 1'b1;
				frames_out <= frames_out + 1'd1;
				pix_i      <= 0;
				x          <= 0;
				y          <= 0;
				if (is_idr_frame && !ENABLE_DPB_REF_SEAM)
					dpb_frame_done_pulse <= 1'b1;
			end else begin
				pix_i <= pix_i + 1'd1;
				if (x == (width_w - 10'd1)) begin
					x <= 0;
					y <= y + 1'd1;
				end else
					x <= x + 1'd1;
			end
		end
		end
	end
endmodule
