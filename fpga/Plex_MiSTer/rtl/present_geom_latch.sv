// Present geometry latch — DDR PLXG q0..q5 at FIXED +0x800.
//
// B4: epoch/session in q0[47:34]; zero epoch rejected (restart ABA).
// B16: commit STAGES only. Live outputs update on frame_boundary (atomic
//      promote at frame start). Mid-active commit must NOT change pitch/base
//      inside a raster — that mixed-frame class is silent to counting oracles.
// Parent ABI #2: q5 @ +0x828 (wr_idx==5):
//   [11:0] dar_x, [23:12] dar_y, [31:24] content_fps,
//   [32] dar_valid, [33] fps_valid, [63:34] RESERVED must be 0 (else reject).
//
// FAULT_IMMEDIATE_COMMIT: restore pre-B16 promote-on-commit (red twin REPRO).
// FAULT_NO_EPOCH: seq-only accept (B4 red twin).
//
// Wire ABI: host/libmisterplex/plxg_record.hpp + mailbox_abi_spec.hpp (0x800).

`timescale 1ns/1ps
`default_nettype none

module present_geom_latch (
	input  wire        clk,
	input  wire        reset,
	// DDR consumer write port (6 beats q0..q5, then commit).
	input  wire        wr_en,
	input  wire [2:0]  wr_idx,   // 0..5 (5 = DAR+fps qword)
	input  wire [63:0] wr_data,
	input  wire        commit,   // pulse: stage shadow → pending (not live)
	// B16: pulse at frame start (e.g. VSync rising on clk_sys). Promotes pending→live.
	input  wire        frame_boundary,
	// Live window + geometry (stable after promote until next promote).
	output reg         win_enable,
	output reg         geom_enable,
	output reg  [10:0] content_w,
	output reg  [10:0] content_h,
	output reg  [10:0] content_x0,
	output reg  [10:0] content_y0,
	output reg  [11:0] h_de,
	output reg  [11:0] v_de,
	output reg  [10:0] coded_w,
	output reg  [10:0] coded_h,
	output reg  [11:0] y_stride,
	output reg  [10:0] chroma_stride,
	output reg  [10:0] display_w,
	output reg  [10:0] display_h,
	output reg  [10:0] present_x,
	output reg  [10:0] present_y,
	output reg  [10:0] crop_left,
	output reg  [10:0] crop_top,
	// q5 host DAR + cadence (PMS) — parent arbitration #2.
	output reg         dar_valid,
	output reg  [11:0] dar_x,
	output reg  [11:0] dar_y,
	output reg         fps_valid,
	output reg  [7:0]  content_fps,
	output reg         live_valid,
	output reg  [15:0] live_seq,
	output reg  [13:0] live_epoch,
	// B16: sticky — pending staged, not yet promoted (debug / blackout).
	output reg         pending_valid,
	// Pulse 1 clk when pending → live (store geom-epoch bump).
	output reg         promote_pulse
);

	localparam [31:0] MAGIC_G = 32'h4758_4C50; // 'PLXG' LE

	reg [63:0] sh0, sh1, sh2, sh3, sh4, sh5;

	// Pending (staged at commit; promoted at frame_boundary).
	reg         pend_win_en, pend_geom_en;
	reg  [10:0] pend_cw, pend_ch, pend_cx0, pend_cy0;
	reg  [11:0] pend_hde, pend_vde;
	reg  [10:0] pend_coded_w, pend_coded_h;
	reg  [11:0] pend_y_stride;
	reg  [10:0] pend_c_stride;
	reg  [10:0] pend_dw, pend_dh, pend_px, pend_py, pend_cl, pend_ct;
	reg         pend_dar_valid, pend_fps_valid;
	reg  [11:0] pend_dar_x, pend_dar_y;
	reg  [7:0]  pend_content_fps;
	reg  [15:0] pend_seq;
	reg  [13:0] pend_epoch;

	wire [31:0] sh_magic = sh0[31:0];
	wire [15:0] sh_seq   = sh0[63:48];
	wire [13:0] sh_epoch = sh0[47:34];
	wire        sh_win   = sh0[32];
	wire        sh_geom  = sh0[33];
	wire        magic_ok = (sh_magic == MAGIC_G);
	// [63:34] reserved on q5 — 30 bits must be zero or promotion is rejected.
	wire        q5_rsvd_ok = (sh5[63:34] == 30'd0);

	// B4: require epoch != 0 so retained DDR after FPGA reset cannot ABA.
`ifdef PRESENT_GEOM_LATCH_FAULT_NO_EPOCH
	wire epoch_ok = 1'b1;
	wire seq_new =
		magic_ok && q5_rsvd_ok &&
		( (sh_seq != live_seq) || (pending_valid && (sh_seq != pend_seq)) || !live_valid );
`else
	wire epoch_ok = (sh_epoch != 14'd0);
	wire seq_new =
		magic_ok && epoch_ok && q5_rsvd_ok &&
		( (!live_valid && !pending_valid) ||
		  ({sh_epoch, sh_seq} != {live_epoch, live_seq}) ||
		  (pending_valid && ({sh_epoch, sh_seq} != {pend_epoch, pend_seq})) );
`endif

	always_ff @(posedge clk) begin
		if (reset) begin
			sh0 <= 64'd0; sh1 <= 64'd0; sh2 <= 64'd0; sh3 <= 64'd0; sh4 <= 64'd0; sh5 <= 64'd0;
			win_enable <= 1'b0;
			geom_enable <= 1'b0;
			content_w <= 11'd0; content_h <= 11'd0;
			content_x0 <= 11'd0; content_y0 <= 11'd0;
			h_de <= 12'd0; v_de <= 12'd0;
			coded_w <= 11'd0; coded_h <= 11'd0;
			y_stride <= 12'd0; chroma_stride <= 11'd0;
			display_w <= 11'd0; display_h <= 11'd0;
			present_x <= 11'd0; present_y <= 11'd0;
			crop_left <= 11'd0; crop_top <= 11'd0;
			dar_valid <= 1'b0; dar_x <= 12'd0; dar_y <= 12'd0;
			fps_valid <= 1'b0; content_fps <= 8'd0;
			live_valid <= 1'b0;
			live_seq <= 16'd0;
			live_epoch <= 14'd0;
			pending_valid <= 1'b0;
			promote_pulse <= 1'b0;
			pend_win_en <= 1'b0; pend_geom_en <= 1'b0;
			pend_cw <= 11'd0; pend_ch <= 11'd0; pend_cx0 <= 11'd0; pend_cy0 <= 11'd0;
			pend_hde <= 12'd0; pend_vde <= 12'd0;
			pend_coded_w <= 11'd0; pend_coded_h <= 11'd0;
			pend_y_stride <= 12'd0; pend_c_stride <= 11'd0;
			pend_dw <= 11'd0; pend_dh <= 11'd0; pend_px <= 11'd0; pend_py <= 11'd0;
			pend_cl <= 11'd0; pend_ct <= 11'd0;
			pend_dar_valid <= 1'b0; pend_fps_valid <= 1'b0;
			pend_dar_x <= 12'd0; pend_dar_y <= 12'd0; pend_content_fps <= 8'd0;
			pend_seq <= 16'd0; pend_epoch <= 14'd0;
		end else begin
			promote_pulse <= 1'b0;

			if (wr_en) begin
				unique case (wr_idx)
					3'd0: sh0 <= wr_data;
					3'd1: sh1 <= wr_data;
					3'd2: sh2 <= wr_data;
					3'd3: sh3 <= wr_data;
					3'd4: sh4 <= wr_data;
					3'd5: sh5 <= wr_data;
					default: ;
				endcase
			end

			// Stage on commit (or immediate promote under fault).
			if (commit && seq_new) begin
`ifdef PRESENT_GEOM_LATCH_FAULT_IMMEDIATE_COMMIT
				// RED twin: mid-raster live update (pre-B16 defect).
				win_enable     <= sh_win;
				geom_enable    <= sh_geom;
				content_w      <= sh1[10:0];
				content_h      <= sh1[26:16];
				content_x0     <= sh1[42:32];
				content_y0     <= sh1[58:48];
				h_de           <= sh2[11:0];
				v_de           <= sh2[27:16];
				coded_w        <= sh2[42:32];
				coded_h        <= sh2[58:48];
				y_stride       <= sh3[11:0];
				chroma_stride  <= sh3[26:16];
				display_w      <= sh3[42:32];
				display_h      <= sh3[58:48];
				present_x      <= sh4[10:0];
				present_y      <= sh4[26:16];
				crop_left      <= sh4[42:32];
				crop_top       <= sh4[58:48];
				dar_x          <= sh5[11:0];
				dar_y          <= sh5[23:12];
				content_fps    <= sh5[31:24];
				dar_valid      <= sh5[32];
				fps_valid      <= sh5[33];
				live_seq       <= sh_seq;
				live_epoch     <= sh_epoch;
				live_valid     <= 1'b1;
				pending_valid  <= 1'b0;
				promote_pulse  <= 1'b1;
`else
				pend_win_en    <= sh_win;
				pend_geom_en   <= sh_geom;
				pend_cw        <= sh1[10:0];
				pend_ch        <= sh1[26:16];
				pend_cx0       <= sh1[42:32];
				pend_cy0       <= sh1[58:48];
				pend_hde       <= sh2[11:0];
				pend_vde       <= sh2[27:16];
				pend_coded_w   <= sh2[42:32];
				pend_coded_h   <= sh2[58:48];
				pend_y_stride  <= sh3[11:0];
				pend_c_stride  <= sh3[26:16];
				pend_dw        <= sh3[42:32];
				pend_dh        <= sh3[58:48];
				pend_px        <= sh4[10:0];
				pend_py        <= sh4[26:16];
				pend_cl        <= sh4[42:32];
				pend_ct        <= sh4[58:48];
				pend_dar_x     <= sh5[11:0];
				pend_dar_y     <= sh5[23:12];
				pend_content_fps <= sh5[31:24];
				pend_dar_valid <= sh5[32];
				pend_fps_valid <= sh5[33];
				pend_seq       <= sh_seq;
				pend_epoch     <= sh_epoch;
				pending_valid  <= 1'b1;
`endif
			end

			// Atomic promote at frame boundary only (product path).
`ifndef PRESENT_GEOM_LATCH_FAULT_IMMEDIATE_COMMIT
			if (frame_boundary && pending_valid) begin
				win_enable     <= pend_win_en;
				geom_enable    <= pend_geom_en;
				content_w      <= pend_cw;
				content_h      <= pend_ch;
				content_x0     <= pend_cx0;
				content_y0     <= pend_cy0;
				h_de           <= pend_hde;
				v_de           <= pend_vde;
				coded_w        <= pend_coded_w;
				coded_h        <= pend_coded_h;
				y_stride       <= pend_y_stride;
				chroma_stride  <= pend_c_stride;
				display_w      <= pend_dw;
				display_h      <= pend_dh;
				present_x      <= pend_px;
				present_y      <= pend_py;
				crop_left      <= pend_cl;
				crop_top       <= pend_ct;
				dar_x          <= pend_dar_x;
				dar_y          <= pend_dar_y;
				content_fps    <= pend_content_fps;
				dar_valid      <= pend_dar_valid;
				fps_valid      <= pend_fps_valid;
				live_seq       <= pend_seq;
				live_epoch     <= pend_epoch;
				live_valid     <= 1'b1;
				pending_valid  <= 1'b0;
				promote_pulse  <= 1'b1;
			end
`endif
		end
	end

endmodule
`default_nettype wire
