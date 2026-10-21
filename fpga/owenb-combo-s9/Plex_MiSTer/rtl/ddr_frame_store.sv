// HPS-DDR-backed YUV420p frame store.
//
// The ARM writes planar YUV420 frames into two HPS DDR banks using the layout
// from host/libmisterplex/ddr_frame_layout.hpp. The FPGA reads Y, U, and V
// source lines directly from HPS DDR into bank-tagged M10K line buffers.

module ddr_frame_store #(
	parameter int FRAME_W = 640,
	parameter int FRAME_H = 480,
	parameter int FRAME_STRIDE = FRAME_W,
	parameter int CODED_W = FRAME_W,
	parameter int CODED_H = FRAME_H,
	parameter int DISPLAY_W = FRAME_W,
	parameter int DISPLAY_H = FRAME_H,
	parameter int CROP_LEFT = 0,
	parameter int CROP_TOP = 0,
	parameter int PRESENT_X = 0,
	parameter int PRESENT_Y = 0,
	parameter int LINE_COUNT = 8,
	// Prefetch window step in coded lines. Product Template path maps
	// store_y = (py * FRAME_H / TPL_SCALE_REF_H) so 480 FOAR only scans even
	// coded lines (stride 2). Consecutive fill wastes ~half of LINE_COUNT on
	// never-hit odds → underrun class (prefetch_sim LC=8@60). Stride-matched
	// fill keeps LINE_COUNT=8 but stocks scanned lines only (STA-safer than L16).
	parameter int Y_FILL_STRIDE = 1,
	parameter [31:0] PHYS_BASE = 32'h3000_0000,
	parameter int HPS_BANK_STRIDE_BYTES = 524288,
	parameter [31:0] DOORBELL_PHYS = PHYS_BASE + (2 * HPS_BANK_STRIDE_BYTES) - 32'h1000,
	parameter [31:0] MAILBOX_PHYS  = 32'h3007_F100,
	parameter [31:0] INPUT_MAILBOX_PHYS = 32'h3007_F108,
	parameter [31:0] SDRAM_MAILBOX_PHYS = 32'h3007_F110,
	parameter [31:0] FRAME_MAILBOX_PHYS = 32'h3007_F118,
	parameter [31:0] BANK_MAILBOX_PHYS  = 32'h3007_F128,
	parameter [31:0] ASPECT_ACK_PHYS    = 32'h3007_F130,
	parameter int DDR_BURST_MAX = 128,
	parameter bit IGNORE_STALE_DOORBELL_AFTER_RESET = 1'b1,
	parameter int STALE_DOORBELL_FALLBACK_POLLS = 4096,
	parameter bit PIPELINE_REFILL_SCHEDULER = 1'b1,
	parameter bit STRICT_YUV_DOORBELL = 1'b1,
	// 0: fill_bank_base = fill_bank ? BASE_W1 : BASE_W0 (product, bit-identical).
	parameter bit DYN_BASE_EN = 1'b0,
	// 720p L4: LINE_COUNT=8 never covers the beam, so need_y_beam_c stays 1
	// and SOFT_C_Y_BEFORE_C never fetches C → HDMI grey chevron (Y hit, UV=128)
	// while DDR U=53 V=169. Interleave a C line after two Y fills.
	parameter bit C_INTERLEAVE_ON_Y_BEAM = 1'b0
)(
	input  wire        clk,
	input  wire        clk_ddr,
	input  wire        reset,

	input  wire [$clog2(FRAME_W)-1:0] rd_x,
	input  wire [$clog2(FRAME_H)-1:0] rd_y,
	input  wire        rd_active,
	output reg  [7:0]  rd_r,
	output reg  [7:0]  rd_g,
	output reg  [7:0]  rd_b,

	input  wire        start_req,
	input  wire        bank_sel,
	input  wire [15:0] status_osd,
	input  wire        input_cmd_valid,
	input  wire  [7:0] input_cmd,
	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire [15:0] ioctl_index,
	input  wire  [3:0] sdram_test_state,
	input  wire  [3:0] sdram_size_code,
	input  wire [15:0] sdram_error_count,

	output wire        DDRAM_CLK,
	input  wire        DDRAM_BUSY,
	output reg   [7:0] DDRAM_BURSTCNT,
	output reg  [28:0] DDRAM_ADDR,
	input  wire [63:0] DDRAM_DOUT,
	input  wire        DDRAM_DOUT_READY,
	output reg         DDRAM_RD,
	output reg  [63:0] DDRAM_DIN,
	output wire  [7:0] DDRAM_BE,
	output reg         DDRAM_WE,

	input  wire        vsync_pulse,
	output reg         has_frame,
	output reg         swap_pending,
	output reg  [15:0] underrun_count,
	output reg  [15:0] frames_done,
	output reg         doorbell_ok,
	output wire  [7:0] debug_state
);
	localparam int X_W = $clog2(FRAME_W);
	localparam int Y_W = $clog2(FRAME_H);
	localparam int CODED_X_W = $clog2(CODED_W);
	localparam int CODED_Y_W = $clog2(CODED_H);
	localparam int Y_LINE_QWORDS = CODED_W / 8;
	localparam int C_LINE_QWORDS = CODED_W / 16;
	localparam int Y_QW_AW = $clog2(Y_LINE_QWORDS);
	localparam int C_QW_AW = $clog2(C_LINE_QWORDS);
	localparam int LINE_SLOTS = LINE_COUNT * 2;
	localparam int SLOT_W = $clog2(LINE_SLOTS);
	localparam [SLOT_W-1:0] SECOND_SET_BASE = SLOT_W'(LINE_COUNT);
	localparam [X_W-1:0] LAST_X = X_W'(FRAME_W - 1);
	localparam [Y_W-1:0] LAST_Y = Y_W'(FRAME_H - 1);
	localparam [X_W-1:0] PRESENT_X_L = X_W'(PRESENT_X);
	localparam [Y_W-1:0] PRESENT_Y_L = Y_W'(PRESENT_Y);
	localparam [X_W-1:0] PRESENT_END_X = X_W'(PRESENT_X + DISPLAY_W);
	localparam [Y_W-1:0] PRESENT_END_Y = Y_W'(PRESENT_Y + DISPLAY_H);
	localparam [CODED_X_W-1:0] CROP_LEFT_L = CODED_X_W'(CROP_LEFT);
	localparam [CODED_Y_W-1:0] CROP_TOP_L = CODED_Y_W'(CROP_TOP);
	localparam [28:0] BASE_W0 = PHYS_BASE[31:3];
	localparam [28:0] HPS_BANK_STRIDE_QWORDS = 29'(HPS_BANK_STRIDE_BYTES / 8);
	localparam [28:0] BASE_W1 = PHYS_BASE[31:3] + HPS_BANK_STRIDE_QWORDS;
	localparam [28:0] DOORBELL_W = DOORBELL_PHYS[31:3];
	localparam [28:0] MAILBOX_W  = MAILBOX_PHYS[31:3];
	localparam [28:0] INPUT_MAILBOX_W = INPUT_MAILBOX_PHYS[31:3];
	localparam [28:0] SDRAM_MAILBOX_W = SDRAM_MAILBOX_PHYS[31:3];
	localparam [28:0] FRAME_MAILBOX_W = FRAME_MAILBOX_PHYS[31:3];
	localparam [28:0] BANK_MAILBOX_W  = BANK_MAILBOX_PHYS[31:3];
	localparam [28:0] ASPECT_ACK_W    = ASPECT_ACK_PHYS[31:3];
	localparam [28:0] Y_PLANE_QWORDS = 29'((CODED_W * CODED_H) / 8);
	localparam [28:0] C_PLANE_QWORDS = 29'((CODED_W * CODED_H) / 32);
	localparam [28:0] U_PLANE_BASE = Y_PLANE_QWORDS;
	localparam [28:0] V_PLANE_BASE = Y_PLANE_QWORDS + C_PLANE_QWORDS;
	localparam [28:0] Y_LINE_QWORDS_W = 29'(Y_LINE_QWORDS);
	localparam [28:0] C_LINE_QWORDS_W = 29'(C_LINE_QWORDS);
	localparam [Y_QW_AW:0] DDR_BURST_MAX_QWORDS = (Y_QW_AW+1)'(DDR_BURST_MAX);
	localparam [31:0] MAGIC = 32'h504C_584B;
	localparam [31:0] MAGIC_S = 32'h504C_5853;
	localparam [31:0] MAGIC_I = 32'h504C_5849;
	localparam [31:0] MAGIC_M = 32'h504C_584D;
	localparam [31:0] MAGIC_F = 32'h504C_5846;
	localparam [31:0] MAGIC_D = 32'h504C_5844; // PLXD bank-release (Display-bank)
	localparam [31:0] MAGIC_J = 32'h504C_584A; // PLXJ source-aspect ACK
	localparam [1:0] DOORBELL_FORMAT_YUV420P = 2'd1;
	localparam [7:0] DEBUG_FORMAT_ERROR = 8'hE1; // PLXF frame-debug: rejected non-YUV doorbell

	// Gray-code conversions for safe multi-bit CDC (want_y crossing)
	function automatic [Y_W-1:0] y_bin2gray(input [Y_W-1:0] b);
		y_bin2gray = b ^ (b >> 1);
	endfunction

	function automatic [Y_W-1:0] y_gray2bin(input [Y_W-1:0] g);
		integer gi;
		begin
			y_gray2bin[Y_W-1] = g[Y_W-1];
			for (gi = Y_W-2; gi >= 0; gi = gi - 1)
				y_gray2bin[gi] = y_gray2bin[gi+1] ^ g[gi];
		end
	endfunction

	assign DDRAM_CLK = clk_ddr;
	assign DDRAM_BE = 8'hFF;

	reg [LINE_SLOTS-1:0] y_wr, u_wr, v_wr;
	reg [Y_QW_AW-1:0] y_wr_addr;
	reg [C_QW_AW-1:0] c_wr_addr;
	reg [63:0] y_wr_data, u_wr_data, v_wr_data;
	wire [63:0] y_q [0:LINE_SLOTS-1];
	wire [63:0] u_q [0:LINE_SLOTS-1];
	wire [63:0] v_q [0:LINE_SLOTS-1];
	wire rd_x_at_or_after_origin;
	wire rd_y_at_or_after_origin;
	generate
		if (PRESENT_X == 0) begin : gen_present_x_zero
			assign rd_x_at_or_after_origin = 1'b1;
		end else begin : gen_present_x_nonzero
			assign rd_x_at_or_after_origin = (rd_x >= PRESENT_X_L);
		end
		if (PRESENT_Y == 0) begin : gen_present_y_zero
			assign rd_y_at_or_after_origin = 1'b1;
		end else begin : gen_present_y_nonzero
			assign rd_y_at_or_after_origin = (rd_y >= PRESENT_Y_L);
		end
	endgenerate
	wire rd_x_visible = rd_x_at_or_after_origin && (rd_x < PRESENT_END_X);
	wire rd_y_visible = rd_y_at_or_after_origin && (rd_y < PRESENT_END_Y);
	wire rd_visible = rd_x_visible && rd_y_visible;
	wire [X_W-1:0] display_x = rd_x - PRESENT_X_L;
	wire [Y_W-1:0] display_y = rd_y - PRESENT_Y_L;
	wire [CODED_X_W-1:0] src_x = rd_visible ? (display_x + CROP_LEFT_L) : '0;
	wire [CODED_Y_W-1:0] src_y = rd_visible ? (display_y + CROP_TOP_L) : '0;
	wire [Y_QW_AW-1:0] y_rd_addr = src_x[CODED_X_W-1:3];
	wire [C_QW_AW-1:0] c_rd_addr = src_x[CODED_X_W-1:4];

	genvar li;
	generate
		for (li = 0; li < LINE_SLOTS; li = li + 1) begin : gen_line
			line_buf_ram #(.WIDTH(Y_LINE_QWORDS), .AW(Y_QW_AW), .DATA_W(64)) yram (
				.wr_clk(clk_ddr), .wr_en(y_wr[li]), .wr_addr(y_wr_addr), .wr_data(y_wr_data),
				.rd_clk(clk), .rd_addr(y_rd_addr), .rd_data(y_q[li])
			);
			line_buf_ram #(.WIDTH(C_LINE_QWORDS), .AW(C_QW_AW), .DATA_W(64)) uram (
				.wr_clk(clk_ddr), .wr_en(u_wr[li]), .wr_addr(c_wr_addr), .wr_data(u_wr_data),
				.rd_clk(clk), .rd_addr(c_rd_addr), .rd_data(u_q[li])
			);
			line_buf_ram #(.WIDTH(C_LINE_QWORDS), .AW(C_QW_AW), .DATA_W(64)) vram (
				.wr_clk(clk_ddr), .wr_en(v_wr[li]), .wr_addr(c_wr_addr), .wr_data(v_wr_data),
				.rd_clk(clk), .rd_addr(c_rd_addr), .rd_data(v_q[li])
			);
		end
	endgenerate

	reg disp_bank;
	reg pending_bank;
	reg pending_bank_ddr;
	reg disp_buf;
	reg swap_req_s1, swap_req_s2, swap_req_seen;
	reg pending_bank_s1, pending_bank_s2;
	reg pending_ready_s1, pending_ready_s2;
	reg pending_ready_ddr;
	reg swap_req_t_ddr;
	reg vsync_toggle;
	reg reset_ddr_s1, reset_ddr_s2;
	wire reset_ddr = reset_ddr_s2;

	always @(posedge clk_ddr) begin
		if (reset) begin
			reset_ddr_s1 <= 1'b1;
			reset_ddr_s2 <= 1'b1;
		end else begin
			reset_ddr_s1 <= 1'b0;
			reset_ddr_s2 <= reset_ddr_s1;
		end
	end

	always @(posedge clk) begin
		if (reset) begin
			disp_bank <= 1'b0;
			pending_bank <= 1'b0;
			disp_buf <= 1'b0;
			has_frame <= 1'b0;
			swap_pending <= 1'b0;
			frames_done <= 16'd0;
			vsync_toggle <= 1'b0;
			swap_req_s1 <= 1'b0;
			swap_req_s2 <= 1'b0;
			swap_req_seen <= 1'b0;
			pending_bank_s1 <= 1'b0;
			pending_bank_s2 <= 1'b0;
			pending_ready_s1 <= 1'b0;
			pending_ready_s2 <= 1'b0;
		end else begin
			swap_req_s1 <= swap_req_t_ddr;
			swap_req_s2 <= swap_req_s1;
			pending_bank_s1 <= pending_bank_ddr;
			pending_bank_s2 <= pending_bank_s1;
			pending_ready_s1 <= pending_ready_ddr;
			pending_ready_s2 <= pending_ready_s1;

			if (swap_req_s2 != swap_req_seen) begin
				swap_req_seen <= swap_req_s2;
				pending_bank <= pending_bank_s2;
				swap_pending <= 1'b1;
			end

			if (vsync_pulse && swap_pending && pending_ready_s2) begin
				disp_bank <= pending_bank;
				disp_buf <= ~disp_buf;
				has_frame <= 1'b1;
				swap_pending <= 1'b0;
				frames_done <= frames_done + 16'd1;
				vsync_toggle <= ~vsync_toggle;
			end else if (vsync_pulse) begin
				vsync_toggle <= ~vsync_toggle;
			end
		end
	end

	function automatic [7:0] pick_byte(input [63:0] q, input [2:0] sel);
		case (sel)
			3'd0: pick_byte = q[7:0];
			3'd1: pick_byte = q[15:8];
			3'd2: pick_byte = q[23:16];
			3'd3: pick_byte = q[31:24];
			3'd4: pick_byte = q[39:32];
			3'd5: pick_byte = q[47:40];
			3'd6: pick_byte = q[55:48];
			default: pick_byte = q[63:56];
		endcase
	endfunction

	function automatic [7:0] sat8(input signed [11:0] v);
		begin
			if (v < 0)
				sat8 = 8'd0;
			else if (v > 12'sd255)
				sat8 = 8'd255;
			else
				sat8 = v[7:0];
		end
	endfunction

	reg [LINE_SLOTS-1:0] y_valid_v1, y_valid_v2, c_valid_v1, c_valid_v2;
	reg [LINE_SLOTS-1:0] y_bank_v1, y_bank_v2, c_bank_v1, c_bank_v2;
	reg [Y_W-1:0] y_line_v1 [0:LINE_SLOTS-1];
	reg [Y_W-1:0] y_line_v2 [0:LINE_SLOTS-1];
	reg [Y_W-2:0] c_line_v1 [0:LINE_SLOTS-1];
	reg [Y_W-2:0] c_line_v2 [0:LINE_SLOTS-1];
	reg [Y_W-1:0] want_y_sys;
	reg [Y_W-1:0] want_y_gray;  // Gray-encoded want_y for safe CDC

	// Toggle-snapshot registers for multi-bit CDC (clk → clk_ddr)
	reg [15:0] status_osd_hold;
	reg        status_osd_toggle;
	reg [23:0] sdram_status_hold;
	reg        sdram_status_toggle;

	reg rd_active_r, rd_active_d, rd_visible_r, rd_visible_d, miss_d;
	reg y_hit_r, c_hit_r;
	reg [SLOT_W-1:0] y_hit_idx_r, c_hit_idx_r;
	reg [2:0] y_sel_r, c_sel_r;

	integer vi;
	reg y_hit_now, c_hit_now;
	reg [SLOT_W-1:0] y_hit_idx_now, c_hit_idx_now;
	reg [63:0] selected_y_q, selected_u_q, selected_v_q;
	reg [SLOT_W-1:0] video_slot;
`ifdef DDR_FRAME_STORE_FAULT_CHROMA_VERTICAL_FULLRES
	wire [CODED_Y_W-2:0] rd_cy = src_y[CODED_Y_W-2:0];
`else
	wire [CODED_Y_W-2:0] rd_cy = src_y[CODED_Y_W-1:1];
`endif
	// product480-keepv: Y/C home probe first (matches y_home_off/c_home_off), then
	// scan half. Hard miss = Y only; C lag is soft (neutral UV) — keepv 720.
	always @* begin
		y_hit_now = 1'b0;
		c_hit_now = 1'b0;
		y_hit_idx_now = '0;
		c_hit_idx_now = '0;
		selected_y_q = 64'd0;
		selected_u_q = 64'd0;
		selected_v_q = 64'd0;
		video_slot = (disp_buf ? SECOND_SET_BASE : '0)
		    + y_home_off(Y_W'(src_y));
		if (y_valid_v2[video_slot] && (y_bank_v2[video_slot] == disp_bank)
		    && (y_line_v2[video_slot] == Y_W'(src_y))) begin
			y_hit_now = 1'b1;
			y_hit_idx_now = video_slot;
		end
		video_slot = (disp_buf ? SECOND_SET_BASE : '0)
		    + c_home_off((Y_W-1)'(rd_cy));
		if (c_valid_v2[video_slot] && (c_bank_v2[video_slot] == disp_bank)
		    && (c_line_v2[video_slot] == (Y_W-1)'(rd_cy))) begin
			c_hit_now = 1'b1;
			c_hit_idx_now = video_slot;
		end
		for (vi = 0; vi < LINE_COUNT; vi = vi + 1) begin
			video_slot = (disp_buf ? SECOND_SET_BASE : '0) + vi[SLOT_W-1:0];
			if (y_valid_v2[video_slot] && (y_bank_v2[video_slot] == disp_bank)
			    && (y_line_v2[video_slot] == Y_W'(src_y)) && !y_hit_now) begin
				y_hit_now = 1'b1;
				y_hit_idx_now = video_slot;
			end
			if (c_valid_v2[video_slot] && (c_bank_v2[video_slot] == disp_bank)
			    && (c_line_v2[video_slot] == (Y_W-1)'(rd_cy)) && !c_hit_now) begin
				c_hit_now = 1'b1;
				c_hit_idx_now = video_slot;
			end
			if (y_hit_idx_r == video_slot)
				selected_y_q = y_q[video_slot];
			if (c_hit_idx_r == video_slot) begin
				selected_u_q = u_q[video_slot];
				selected_v_q = v_q[video_slot];
			end
		end
	end
	// Hard miss only when Y under the beam is missing (keepv SOFT_C).
	wire rd_miss_now = rd_active && rd_visible && has_frame && !y_hit_now;

	wire [7:0] y_pix = pick_byte(selected_y_q, y_sel_r);
	wire [7:0] u_raw = pick_byte(selected_u_q, c_sel_r);
	wire [7:0] v_raw = pick_byte(selected_v_q, c_sel_r);
	// C lag: keep luma, neutral chroma (no full-pixel black / hold streaks).
	wire [7:0] u_pix = c_hit_r ? u_raw : 8'd128;
	wire [7:0] v_pix = c_hit_r ? v_raw : 8'd128;
	wire signed [11:0] y_s = {4'd0, y_pix};
	wire signed [11:0] u_s = {4'd0, u_pix} - 12'sd128;
	wire signed [11:0] v_s = {4'd0, v_pix} - 12'sd128;
	wire signed [20:0] y_ext = {{9{y_s[11]}}, y_s};
	wire signed [20:0] r_calc_w = (y_ext <<< 8) + (21'sd359 * v_s);
	wire signed [20:0] g_calc_w = (y_ext <<< 8) - (21'sd88 * u_s) - (21'sd183 * v_s);
	wire signed [20:0] b_calc_w = (y_ext <<< 8) + (21'sd454 * u_s);
	wire signed [11:0] r_calc = r_calc_w[19:8];
	wire signed [11:0] g_calc = g_calc_w[19:8];
	wire signed [11:0] b_calc = b_calc_w[19:8];

	always @(posedge clk) begin
		if (reset) begin
			rd_active_r <= 1'b0;
			rd_active_d <= 1'b0;
			rd_visible_r <= 1'b0;
			rd_visible_d <= 1'b0;
			miss_d <= 1'b0;
			underrun_count <= 16'd0;
			want_y_sys <= '0;
			want_y_gray <= '0;
			status_osd_hold <= 16'd0;
			status_osd_toggle <= 1'b0;
			sdram_status_hold <= 24'd0;
			sdram_status_toggle <= 1'b0;
			y_valid_v1 <= '0;
			y_valid_v2 <= '0;
			c_valid_v1 <= '0;
			c_valid_v2 <= '0;
			y_bank_v1 <= '0;
			y_bank_v2 <= '0;
			c_bank_v1 <= '0;
			c_bank_v2 <= '0;
			y_hit_r <= 1'b0;
			c_hit_r <= 1'b0;
			y_hit_idx_r <= '0;
			c_hit_idx_r <= '0;
			y_sel_r <= 3'd0;
			c_sel_r <= 3'd0;
		end else begin
			y_valid_v1 <= y_valid;
			y_valid_v2 <= y_valid_v1;
			c_valid_v1 <= c_valid;
			c_valid_v2 <= c_valid_v1;
			y_bank_v1 <= y_bank;
			y_bank_v2 <= y_bank_v1;
			c_bank_v1 <= c_bank;
			c_bank_v2 <= c_bank_v1;
			for (vi = 0; vi < LINE_SLOTS; vi = vi + 1) begin
				y_line_v1[vi] <= y_line[vi];
				y_line_v2[vi] <= y_line_v1[vi];
				c_line_v1[vi] <= c_line[vi];
				c_line_v2[vi] <= c_line_v1[vi];
			end

			if (Y_W'(src_y) != want_y_sys)
				want_y_sys <= Y_W'(src_y);
			want_y_gray <= y_bin2gray(want_y_sys);

			// Toggle-snapshot: status_osd (clk → clk_ddr)
			if (status_osd != status_osd_hold) begin
				status_osd_hold <= status_osd;
				status_osd_toggle <= ~status_osd_toggle;
			end

			// Toggle-snapshot: sdram_status (clk → clk_ddr)
			if ({sdram_error_count, sdram_size_code, sdram_test_state} != sdram_status_hold) begin
				sdram_status_hold <= {sdram_error_count, sdram_size_code, sdram_test_state};
				sdram_status_toggle <= ~sdram_status_toggle;
			end

			rd_active_r <= rd_active;
			rd_active_d <= rd_active_r;
			rd_visible_r <= rd_visible;
			rd_visible_d <= rd_visible_r;
			y_hit_r <= y_hit_now;
			c_hit_r <= c_hit_now;
			y_hit_idx_r <= y_hit_idx_now;
			c_hit_idx_r <= c_hit_idx_now;
			y_sel_r <= src_x[2:0];
			c_sel_r <= src_x[3:1];
			miss_d <= rd_miss_now;
			if (miss_d && underrun_count != 16'hFFFF)
				underrun_count <= underrun_count + 16'd1;

			// keepv: paint when Y hits (C soft via neutral UV). Black only on Y miss.
			if ((rd_active_d || !rd_active) && rd_visible_d && has_frame && !miss_d && y_hit_r) begin
				rd_r <= sat8(r_calc);
				rd_g <= sat8(g_calc);
				rd_b <= sat8(b_calc);
			end else if (!has_frame || !rd_visible_d || miss_d) begin
				rd_r <= 8'd0;
				rd_g <= 8'd0;
				rd_b <= 8'd0;
			end
		end
	end

	localparam [3:0] S_IDLE       = 4'd0;
	localparam [3:0] S_LINE_ISSUE = 4'd1;
	localparam [3:0] S_LINE_WAIT  = 4'd2;
	localparam [3:0] S_POLL_WAIT  = 4'd3;
	localparam [3:0] S_WRITE_WAIT = 4'd4;
	localparam [3:0] S_LINE_PREP  = 4'd5; // keepv21: sample line_addr → S_LINE_ISSUE

	reg [3:0] state_ddr;
	reg [LINE_SLOTS-1:0] y_valid, c_valid;
	reg [LINE_SLOTS-1:0] y_bank, c_bank;
	reg [Y_W-1:0] y_line [0:LINE_SLOTS-1];
	reg [Y_W-2:0] c_line [0:LINE_SLOTS-1];
	reg disp_bank_d1, disp_bank_d2;
	reg disp_buf_d1, disp_buf_d2;
	reg has_frame_d1, has_frame_d2;
	reg swap_pending_d1, swap_pending_d2;
	reg pending_bank_d1, pending_bank_d2;
	reg [Y_W-1:0] want_y_gray_s1, want_y_gray_s2;  // Gray-coded 2-FF sync
	reg [Y_W-1:0] desired_y_r [0:LINE_COUNT-1];
	reg [15:0] poll_div;
	reg poll_pending;
	reg poll_beat;
	reg [1:0] c_rr_r;
	reg [28:0] dyn_base0_r, dyn_base1_r;
	reg dyn_valid0_r, dyn_valid1_r;
	reg kick_bank_r;
	localparam int STALE_DB_POLL_MAX = (STALE_DOORBELL_FALLBACK_POLLS < 1) ? 1 : STALE_DOORBELL_FALLBACK_POLLS;
	localparam int STALE_DB_POLL_W = $clog2(STALE_DB_POLL_MAX + 1);
	reg [31:0] last_seq;
	reg have_seq;
	reg doorbell_primed;
	reg format_error;
	reg [STALE_DB_POLL_W-1:0] stale_db_polls;
	reg [15:0] mbox_seq, mbox_last;
	reg mbox_req, mbox_valid;
	reg [17:0] mbox_hb;
	reg [7:0] sdram_mbox_seq, frame_mbox_seq;
	reg [23:0] sdram_mbox_last, frame_mbox_last;
	reg sdram_mbox_req, sdram_mbox_valid;
	reg frame_mbox_req, frame_mbox_valid;
	reg [17:0] sdram_mbox_hb, frame_mbox_hb;
	reg bank_mbox_req, bank_mbox_valid;
	reg [17:0] bank_mbox_hb;
	reg [7:0] bank_mbox_seq;
	reg [15:0] bank_vsync_count;
	reg vsync_t_d1, vsync_t_d2, vsync_t_seen;
	reg start_d1, start_d2, start_seen;
	reg bank_sel_d1, bank_sel_d2;

	// Toggle-snapshot CDC receivers (clk → clk_ddr)
	reg        status_osd_tog_s1, status_osd_tog_s2, status_osd_tog_seen;
	reg [15:0] status_osd_safe;
	reg        sdram_status_tog_s1, sdram_status_tog_s2, sdram_status_tog_seen;
	reg [23:0] sdram_status_safe;

	wire cmd_empty;
	wire [7:0] cmd_rdata;
	reg cmd_pop;
	wire        plxa_toggle;
	wire [11:0] plxa_x;
	wire [11:0] plxa_y;
	wire [7:0]  plxa_token;
	source_aspect_ack u_plxa (
		.clk(clk),
		.reset(reset),
		.ioctl_download(ioctl_download),
		.ioctl_wr(ioctl_wr),
		.ioctl_dout(ioctl_dout),
		.ioctl_index(ioctl_index),
		.ack_toggle(plxa_toggle),
		.ack_x(plxa_x),
		.ack_y(plxa_y),
		.ack_token(plxa_token)
	);
	reg        plxa_tog_s1, plxa_tog_s2, plxa_tog_seen;
	reg        plxj_req;
	reg [11:0] plxj_x, plxj_y;
	reg [7:0]  plxj_token;
	async_fifo #(.WIDTH(8), .AW(2)) input_fifo (
		.wr_clk(clk), .wr_reset(reset),
		.wr_en(input_cmd_valid && (input_cmd != 8'd0)), .wr_data(input_cmd),
		.wr_full(), .wr_almost_full(),
		.rd_clk(clk_ddr), .rd_reset(reset_ddr), .rd_en(cmd_pop), .rd_data(cmd_rdata), .rd_empty(cmd_empty)
	);

	function automatic [Y_W-1:0] clamp_ahead(input [Y_W-1:0] base, input integer ahead);
		integer sum;
		begin
			sum = {{(32-Y_W){1'b0}}, base};
			sum = sum + ahead;
			clamp_ahead = (sum >= FRAME_H) ? LAST_Y : sum[Y_W-1:0];
		end
	endfunction

	// Y_HOME_SLOT / C_HOME_SLOT (keepv Track A2): stable home idx =
	// half_base + home_off(line). Free-list put line L in arbitrary free
	// slot → pitch-LINE_COUNT missblack.
	//
	// FOAR Y_FILL_STRIDE=2: only even coded Y lines are scanned. Classic
	// line%8 maps them only to even homes → collisions (0 vs 8) → beam Y miss
	// sticky → soft-C starves chroma → gray glass (keepv22/25/26 STA0).
	// keepv27: pure wire home for LC=8 stride=2: (line>>1)&7 = line[3:1]
	// (no divide / modulo — keepv23/24 hold-red on arithmetic home).
	function automatic [SLOT_W-1:0] y_home_off(input [Y_W-1:0] line_y);
		if ((Y_FILL_STRIDE == 2) && (LINE_COUNT == 8))
			y_home_off = SLOT_W'(line_y[3:1]);
		else
			y_home_off = SLOT_W'(line_y % LINE_COUNT);
	endfunction
	// C: FOAR cy already = y/2 for even Y; cy%8 fills all homes 0..7.
	function automatic [SLOT_W-1:0] c_home_off(input [Y_W-2:0] line_cy);
		c_home_off = SLOT_W'(line_cy % LINE_COUNT);
	endfunction
	// Quartus will not part-select a function result (Error 10170 near "[").
	wire [Y_W-1:0] beam_y_bin = y_gray2bin(want_y_gray_s2);
	wire [Y_W-2:0] beam_cy = beam_y_bin[Y_W-1:1];

	// CUR_TAG_WINDOW_R: registered flat LINE_COUNT CUR tags so O(n²) schedule
	// scan does not mux through disp_buf_d2→array address (H-R1e Path#1 family).
	// Sample on clk_ddr; combo need/target use only *_cur_r / base_r.
	reg [SLOT_W-1:0] cur_base_idx_r, prep_base_idx_r;
	reg              disp_bank_cur_r, pending_bank_prep_r;
	reg [LINE_COUNT-1:0] y_valid_cur_r, c_valid_cur_r;
	reg [LINE_COUNT-1:0] y_bank_cur_r, c_bank_cur_r;
	reg [Y_W-1:0]        y_line_cur_r [0:LINE_COUNT-1];
	reg [Y_W-2:0]        c_line_cur_r [0:LINE_COUNT-1];


	integer ti, tj, tk;
	reg need_y_cur_c, need_c_cur_c, need_y_prep_c, need_c_prep_c, pending_ready_c;
	// keepv25/26: beam-only Y miss for soft-C starve (not whole fill window).
	reg need_y_beam_c;
	reg [Y_W-1:0] target_y_cur_c, target_y_prep_c;
	reg [Y_W-2:0] target_c_cur_c, target_c_prep_c;
	reg [SLOT_W-1:0] target_y_idx_cur_c, target_y_idx_prep_c, target_c_idx_cur_c, target_c_idx_prep_c;
	reg found_line, slot_keep, found_slot_c_cur, found_slot_c_prep;
	reg [Y_W-1:0] desired_y;
	reg [Y_W-2:0] desired_c;
	reg [SLOT_W-1:0] cur_base_idx, prep_base_idx, yh;
	reg sched_valid, sched_is_y, sched_for_pending;
	reg sched_bank, sched_pending_ready;
	reg [Y_W-1:0] sched_y;
	reg [Y_W-2:0] sched_cy;
	reg [SLOT_W-1:0] sched_idx;
	always @* begin
		// Absolute bases still needed for absolute fill indices / prep live scan.
		cur_base_idx = cur_base_idx_r;
		prep_base_idx = prep_base_idx_r;
		need_y_cur_c = 1'b0;
		need_c_cur_c = 1'b0;
		need_y_prep_c = 1'b0;
		need_c_prep_c = 1'b0;
		need_y_beam_c = 1'b0;
		target_y_cur_c = desired_y_r[0];
		target_y_prep_c = '0;
		target_c_cur_c = desired_y_r[0][Y_W-1:1];
		target_c_prep_c = '0;
		// Y: home slot. C: free-list (keepv26) — C_HOME fill+hit left glass gray
		// on STA0 keepv22/25 (center R=G=B → permanent soft neutral UV).
		target_y_idx_cur_c = cur_base_idx_r + y_home_off(desired_y_r[0]);
		target_y_idx_prep_c = prep_base_idx_r + y_home_off(Y_W'(0));
		target_c_idx_cur_c = cur_base_idx_r;
		target_c_idx_prep_c = prep_base_idx_r;
		found_slot_c_cur = 1'b0;
		found_slot_c_prep = 1'b0;
		pending_ready_c = 1'b1;

		// CUR Y: O(1) home. CUR C: linear scan of flat window for need detect.
		for (ti = 0; ti < LINE_COUNT; ti = ti + 1) begin
			desired_y = desired_y_r[ti];
			desired_c = desired_y_r[ti][Y_W-1:1];
			yh = y_home_off(desired_y);
			found_line = y_valid_cur_r[yh] && (y_bank_cur_r[yh] == disp_bank_cur_r)
			    && (y_line_cur_r[yh] == desired_y);
			if (!found_line && !need_y_cur_c) begin
				need_y_cur_c = 1'b1;
				target_y_cur_c = desired_y;
				target_y_idx_cur_c = cur_base_idx_r + yh;
			end
			if (ti == 0 && !found_line)
				need_y_beam_c = 1'b1;

			found_line = 1'b0;
			for (tj = 0; tj < LINE_COUNT; tj = tj + 1) begin
				if (c_valid_cur_r[tj] && (c_bank_cur_r[tj] == disp_bank_cur_r)
				    && (c_line_cur_r[tj] == desired_c))
					found_line = 1'b1;
			end
			if (!found_line && !need_c_cur_c) begin
				need_c_cur_c = 1'b1;
				target_c_cur_c = desired_c;
			end

			// PREP: Y home + C free-list hole (swap-only).
			if (swap_pending_d2) begin
				yh = y_home_off(ti[Y_W-1:0]);
				found_line = y_valid[prep_base_idx_r + yh]
				    && (y_bank[prep_base_idx_r + yh] == pending_bank_prep_r)
				    && (y_line[prep_base_idx_r + yh] == ti[Y_W-1:0]);
				if (!found_line) begin
					pending_ready_c = 1'b0;
					if (!need_y_prep_c) begin
						need_y_prep_c = 1'b1;
						target_y_prep_c = ti[Y_W-1:0];
						target_y_idx_prep_c = prep_base_idx_r + yh;
					end
				end

				found_line = 1'b0;
				for (tj = 0; tj < LINE_COUNT; tj = tj + 1) begin
					if (c_valid[prep_base_idx_r + tj[SLOT_W-1:0]]
					    && (c_bank[prep_base_idx_r + tj[SLOT_W-1:0]] == pending_bank_prep_r)
					    && (c_line[prep_base_idx_r + tj[SLOT_W-1:0]] == ti[Y_W-1:1]))
						found_line = 1'b1;
				end
				if (!found_line) begin
					pending_ready_c = 1'b0;
					if (!need_c_prep_c) begin
						need_c_prep_c = 1'b1;
						target_c_prep_c = ti[Y_W-1:1];
					end
				end
			end
		end

		// C free-list on flat CUR window (and prep holes).
		for (tj = 0; tj < LINE_COUNT; tj = tj + 1) begin
			slot_keep = 1'b0;
			for (tk = 0; tk < LINE_COUNT; tk = tk + 1) begin
				if (c_valid_cur_r[tj] && (c_bank_cur_r[tj] == disp_bank_cur_r)
				    && (c_line_cur_r[tj] == desired_y_r[tk][Y_W-1:1]))
					slot_keep = 1'b1;
			end
			if ((!c_valid_cur_r[tj] || !slot_keep) && !found_slot_c_cur) begin
				found_slot_c_cur = 1'b1;
				target_c_idx_cur_c = cur_base_idx_r + tj[SLOT_W-1:0];
			end
			if (swap_pending_d2) begin
				if ((!c_valid[prep_base_idx_r + tj[SLOT_W-1:0]]) && !found_slot_c_prep) begin
					found_slot_c_prep = 1'b1;
					target_c_idx_prep_c = prep_base_idx_r + tj[SLOT_W-1:0];
				end
			end
		end
	end


	reg fill_bank, fill_is_chroma, fill_plane_v;
	reg [Y_W-1:0] fill_y;
	reg [Y_W-2:0] fill_cy;
	reg [SLOT_W-1:0] fill_idx;
	reg [Y_QW_AW:0] fill_qword;
	reg [Y_QW_AW:0] burst_left;
	reg [Y_QW_AW:0] qwords_remaining;
	reg [7:0] imbox_cmd_seq;
	reg [15:0] imbox_seq;
	wire [28:0] fill_bank_base;
	wire        fill_base_using_dyn;
	ddr_frame_base_mux #(
		.DYN_BASE_EN(DYN_BASE_EN)
	) u_fill_base_mux (
		.bank(fill_bank),
		.base_w0(BASE_W0),
		.base_w1(BASE_W1),
		.dyn_base0(dyn_base0_r),
		.dyn_base1(dyn_base1_r),
		.dyn_valid0(dyn_valid0_r),
		.dyn_valid1(dyn_valid1_r),
		.fill_bank_base(fill_bank_base),
		.using_dyn(fill_base_using_dyn)
	);
	// Strength-reduce line_idx * LINE_QWORDS (keepv20). keepv19 fit packed
	// fill_y into Mult4~mac on the general[2] cone; FOAR 624→78 / 39 are
	// shift-add friendly so the DSP MAC leaves the path.
	wire [28:0] fill_y_ext = {{(29-Y_W){1'b0}}, fill_y};
	wire [28:0] fill_cy_ext = {{(30-Y_W){1'b0}}, fill_cy};
	wire [28:0] fill_y_qword;
	wire [28:0] fill_cy_qword;
	generate
		if (Y_LINE_QWORDS == 78) begin : g_yq_foar624
			// 78 = 64+8+4+2
			assign fill_y_qword = (fill_y_ext << 6) + (fill_y_ext << 3)
			                   + (fill_y_ext << 2) + (fill_y_ext << 1);
		end else if (Y_LINE_QWORDS == 80) begin : g_yq_640
			// 80 = 64+16
			assign fill_y_qword = (fill_y_ext << 6) + (fill_y_ext << 4);
		end else if (Y_LINE_QWORDS == 160) begin : g_yq_1280
			// 160 = 128+32
			assign fill_y_qword = (fill_y_ext << 7) + (fill_y_ext << 5);
		end else begin : g_yq_generic
			assign fill_y_qword = fill_y_ext * Y_LINE_QWORDS_W;
		end
`ifdef DDR_FRAME_STORE_FAULT_CHROMA_LUMA_STRIDE
		if (Y_LINE_QWORDS == 78) begin : g_cyq_fault_foar
			assign fill_cy_qword = (fill_cy_ext << 6) + (fill_cy_ext << 3)
			                    + (fill_cy_ext << 2) + (fill_cy_ext << 1);
		end else if (Y_LINE_QWORDS == 80) begin : g_cyq_fault_640
			assign fill_cy_qword = (fill_cy_ext << 6) + (fill_cy_ext << 4);
		end else if (Y_LINE_QWORDS == 160) begin : g_cyq_fault_1280
			assign fill_cy_qword = (fill_cy_ext << 7) + (fill_cy_ext << 5);
		end else begin : g_cyq_fault_generic
			assign fill_cy_qword = fill_cy_ext * Y_LINE_QWORDS_W;
		end
`else
		if (C_LINE_QWORDS == 39) begin : g_cyq_foar624
			// 39 = 32+4+2+1
			assign fill_cy_qword = (fill_cy_ext << 5) + (fill_cy_ext << 2)
			                    + (fill_cy_ext << 1) + fill_cy_ext;
		end else if (C_LINE_QWORDS == 40) begin : g_cyq_640
			// 40 = 32+8
			assign fill_cy_qword = (fill_cy_ext << 5) + (fill_cy_ext << 3);
		end else if (C_LINE_QWORDS == 80) begin : g_cyq_1280
			// 80 = 64+16
			assign fill_cy_qword = (fill_cy_ext << 6) + (fill_cy_ext << 4);
		end else begin : g_cyq_generic
			assign fill_cy_qword = fill_cy_ext * C_LINE_QWORDS_W;
		end
`endif
	endgenerate
	wire [28:0] fill_qword_y = {{(29-Y_QW_AW){1'b0}}, fill_qword[Y_QW_AW-1:0]};
	wire [28:0] fill_qword_c = {{(29-C_QW_AW){1'b0}}, fill_qword[C_QW_AW-1:0]};
	wire [28:0] y_addr = fill_bank_base + fill_y_qword + fill_qword_y;
	wire [28:0] u_addr = fill_bank_base + U_PLANE_BASE + fill_cy_qword + fill_qword_c;
	wire [28:0] v_addr = fill_bank_base + V_PLANE_BASE + fill_cy_qword + fill_qword_c;
`ifdef DDR_FRAME_STORE_FAULT_SWAP_UV_READ
	wire [28:0] chroma_addr = fill_plane_v ? u_addr : v_addr;
`else
	wire [28:0] chroma_addr = fill_plane_v ? v_addr : u_addr;
`endif
	wire [28:0] line_addr = fill_is_chroma ? chroma_addr : y_addr;
	wire [Y_QW_AW:0] burst_cap = (qwords_remaining > DDR_BURST_MAX_QWORDS) ? DDR_BURST_MAX_QWORDS : qwords_remaining;
	wire [7:0] burst_this = burst_cap[7:0];
	// keepv21: register address/burst for S_LINE_ISSUE D-side (+1 clk_ddr).
	// Breaks fill_y/shift-add/base → DDRAM_ADDR setup cone on general[2].
	reg [28:0] line_addr_r;
	reg [7:0]  burst_this_r;
	reg [Y_QW_AW:0] burst_cap_r;
	wire db_magic_ok = poll_pending && DDRAM_DOUT_READY && (poll_beat == 1'b0) &&
	                   (DDRAM_DOUT[31:0] == MAGIC);
	wire [31:0] db_token = DDRAM_DOUT[63:32];
	wire [1:0] db_format = db_token[30:29];
	wire db_format_ok = (db_format == DOORBELL_FORMAT_YUV420P) || !STRICT_YUV_DOORBELL;
	wire db_bad_format = db_magic_ok && !db_format_ok;
	wire db_valid_token = db_magic_ok && db_format_ok;
	wire db_token_new = db_valid_token && (!have_seq || (db_token != last_seq));
	wire db_token_same = db_valid_token && have_seq && (db_token == last_seq);
	wire db_stale_fallback = db_token_same && IGNORE_STALE_DOORBELL_AFTER_RESET &&
	                         doorbell_primed &&
	                         (stale_db_polls == STALE_DB_POLL_W'(STALE_DB_POLL_MAX));
	wire db_new_seq = (db_token_new && (!IGNORE_STALE_DOORBELL_AFTER_RESET || doorbell_primed)) ||
	                  db_stale_fallback;
	wire spi_edge_ddr = start_d2 != start_seen;

	assign debug_state = format_error ? DEBUG_FORMAT_ERROR : {LINE_COUNT[2:0], |y_valid, state_ddr};

	always @(posedge clk_ddr) begin
		if (reset_ddr) begin
			state_ddr <= S_IDLE;
			DDRAM_RD <= 1'b0;
			DDRAM_WE <= 1'b0;
			DDRAM_ADDR <= 29'd0;
			DDRAM_BURSTCNT <= 8'd1;
			DDRAM_DIN <= 64'd0;
			line_addr_r <= 29'd0;
			burst_this_r <= 8'd1;
			burst_cap_r <= '0;
			y_wr <= '0;
			u_wr <= '0;
			v_wr <= '0;
			y_wr_addr <= '0;
			c_wr_addr <= '0;
			y_wr_data <= 64'd0;
			u_wr_data <= 64'd0;
			v_wr_data <= 64'd0;
			y_valid <= '0;
			c_valid <= '0;
			y_bank <= '0;
			c_bank <= '0;
			for (ti = 0; ti < LINE_SLOTS; ti = ti + 1) begin
				y_line[ti] <= '0;
				c_line[ti] <= '0;
			end
			disp_bank_d1 <= 1'b0;
			disp_bank_d2 <= 1'b0;
			disp_buf_d1 <= 1'b0;
			disp_buf_d2 <= 1'b0;
			has_frame_d1 <= 1'b0;
			has_frame_d2 <= 1'b0;
			swap_pending_d1 <= 1'b0;
			swap_pending_d2 <= 1'b0;
			pending_bank_d1 <= 1'b0;
			pending_bank_d2 <= 1'b0;
			cur_base_idx_r <= '0;
			prep_base_idx_r <= '0;
			disp_bank_cur_r <= 1'b0;
			pending_bank_prep_r <= 1'b0;
			y_valid_cur_r <= '0;
			c_valid_cur_r <= '0;
			y_bank_cur_r <= '0;
			c_bank_cur_r <= '0;
			for (ti = 0; ti < LINE_COUNT; ti = ti + 1) begin
				y_line_cur_r[ti] <= '0;
				c_line_cur_r[ti] <= '0;
			end
			want_y_gray_s1 <= '0;
			want_y_gray_s2 <= '0;
			for (ti = 0; ti < LINE_COUNT; ti = ti + 1)
				desired_y_r[ti] <= '0;
			pending_ready_ddr <= 1'b0;
			pending_bank_ddr <= 1'b0;
			swap_req_t_ddr <= 1'b0;
			poll_div <= 16'd0;
			poll_pending <= 1'b0;
			poll_beat <= 1'b0;
			c_rr_r <= 2'd0;
			dyn_base0_r <= 29'd0;
			dyn_base1_r <= 29'd0;
			dyn_valid0_r <= 1'b0;
			dyn_valid1_r <= 1'b0;
			kick_bank_r <= 1'b0;
			last_seq <= 32'd0;
			have_seq <= 1'b0;
			doorbell_primed <= 1'b0;
			format_error <= 1'b0;
			stale_db_polls <= '0;
			doorbell_ok <= 1'b0;
			start_d1 <= 1'b0;
			start_d2 <= 1'b0;
			start_seen <= 1'b0;
			bank_sel_d1 <= 1'b0;
			bank_sel_d2 <= 1'b0;
			status_osd_tog_s1 <= 1'b0;
			status_osd_tog_s2 <= 1'b0;
			status_osd_tog_seen <= 1'b0;
			status_osd_safe <= 16'd0;
			sdram_status_tog_s1 <= 1'b0;
			sdram_status_tog_s2 <= 1'b0;
			sdram_status_tog_seen <= 1'b0;
			sdram_status_safe <= 24'd0;
			plxa_tog_s1 <= 1'b0;
			plxa_tog_s2 <= 1'b0;
			plxa_tog_seen <= 1'b0;
			plxj_req <= 1'b0;
			plxj_x <= 12'd0;
			plxj_y <= 12'd0;
			plxj_token <= 8'd0;
			mbox_seq <= 16'd0;
			mbox_last <= 16'd0;
			mbox_req <= 1'b1;
			mbox_valid <= 1'b0;
			mbox_hb <= 18'd0;
			sdram_mbox_seq <= 8'd0;
			sdram_mbox_last <= 24'd0;
			sdram_mbox_req <= 1'b1;
			sdram_mbox_valid <= 1'b0;
			sdram_mbox_hb <= 18'd0;
			frame_mbox_seq <= 8'd0;
			frame_mbox_last <= 24'd0;
			frame_mbox_req <= 1'b1;
			frame_mbox_valid <= 1'b0;
			frame_mbox_hb <= 18'd0;
			bank_mbox_req <= 1'b1;
			bank_mbox_valid <= 1'b0;
			bank_mbox_hb <= 18'd0;
			bank_mbox_seq <= 8'd0;
			bank_vsync_count <= 16'd0;
			vsync_t_d1 <= 1'b0;
			vsync_t_d2 <= 1'b0;
			vsync_t_seen <= 1'b0;
			cmd_pop <= 1'b0;
			sched_valid <= 1'b0;
			sched_is_y <= 1'b0;
			sched_for_pending <= 1'b0;
			sched_bank <= 1'b0;
			sched_pending_ready <= 1'b0;
			sched_y <= '0;
			sched_cy <= '0;
			sched_idx <= '0;
			imbox_seq <= 16'd0;
			imbox_cmd_seq <= 8'd0;
			fill_qword <= '0;
			burst_left <= '0;
			qwords_remaining <= '0;
			fill_is_chroma <= 1'b0;
			fill_plane_v <= 1'b0;
		end else begin
			if (!DDRAM_BUSY) begin
				DDRAM_RD <= 1'b0;
				DDRAM_WE <= 1'b0;
			end
			y_wr <= '0;
			u_wr <= '0;
			v_wr <= '0;
			cmd_pop <= 1'b0;

			disp_bank_d1 <= disp_bank;
			disp_bank_d2 <= disp_bank_d1;
			disp_buf_d1 <= disp_buf;
			disp_buf_d2 <= disp_buf_d1;
			has_frame_d1 <= has_frame;
			has_frame_d2 <= has_frame_d1;
			swap_pending_d1 <= swap_pending;
			swap_pending_d2 <= swap_pending_d1;
			pending_bank_d1 <= pending_bank;
			pending_bank_d2 <= pending_bank_d1;

			// CUR_TAG_WINDOW sample: base/bank mux only into these Ds.
			cur_base_idx_r <= disp_buf_d2 ? SECOND_SET_BASE : '0;
			prep_base_idx_r <= disp_buf_d2 ? '0 : SECOND_SET_BASE;
			disp_bank_cur_r <= disp_bank_d2;
			pending_bank_prep_r <= pending_bank_d2;
			for (ti = 0; ti < LINE_COUNT; ti = ti + 1) begin
				y_valid_cur_r[ti] <= y_valid[(disp_buf_d2 ? SECOND_SET_BASE : '0) + ti[SLOT_W-1:0]];
				c_valid_cur_r[ti] <= c_valid[(disp_buf_d2 ? SECOND_SET_BASE : '0) + ti[SLOT_W-1:0]];
				y_bank_cur_r[ti]  <= y_bank[(disp_buf_d2 ? SECOND_SET_BASE : '0) + ti[SLOT_W-1:0]];
				c_bank_cur_r[ti]  <= c_bank[(disp_buf_d2 ? SECOND_SET_BASE : '0) + ti[SLOT_W-1:0]];
				y_line_cur_r[ti]  <= y_line[(disp_buf_d2 ? SECOND_SET_BASE : '0) + ti[SLOT_W-1:0]];
				c_line_cur_r[ti]  <= c_line[(disp_buf_d2 ? SECOND_SET_BASE : '0) + ti[SLOT_W-1:0]];
			end

			// want_y: Gray-coded 2-FF sync (crossing #13)
			want_y_gray_s1 <= want_y_gray;
			want_y_gray_s2 <= want_y_gray_s1;
			// Stride-aware fill: ahead = ti * Y_FILL_STRIDE (see module param).
			// Y_FILL_STRIDE==1 is bit-identical to prior consecutive window.
			for (ti = 0; ti < LINE_COUNT; ti = ti + 1)
				desired_y_r[ti] <= clamp_ahead(y_gray2bin(want_y_gray_s2), ti * Y_FILL_STRIDE);

			start_d1 <= start_req;
			start_d2 <= start_d1;
			bank_sel_d1 <= bank_sel;
			bank_sel_d2 <= bank_sel_d1;

			// status_osd: toggle-snapshot CDC (crossing #14)
			status_osd_tog_s1 <= status_osd_toggle;
			status_osd_tog_s2 <= status_osd_tog_s1;
			if (status_osd_tog_s2 != status_osd_tog_seen) begin
				status_osd_safe <= status_osd_hold;
				status_osd_tog_seen <= status_osd_tog_s2;
			end

			// sdram_status: toggle-snapshot CDC (crossing #15)
			sdram_status_tog_s1 <= sdram_status_toggle;
			sdram_status_tog_s2 <= sdram_status_tog_s1;
			if (sdram_status_tog_s2 != sdram_status_tog_seen) begin
				sdram_status_safe <= sdram_status_hold;
				sdram_status_tog_seen <= sdram_status_tog_s2;
			end

			plxa_tog_s1 <= plxa_toggle;
			plxa_tog_s2 <= plxa_tog_s1;
			if (plxa_tog_s2 != plxa_tog_seen) begin
				plxj_x <= plxa_x;
				plxj_y <= plxa_y;
				plxj_token <= plxa_token;
				plxj_req <= 1'b1;
				plxa_tog_seen <= plxa_tog_s2;
			end

			mbox_hb <= mbox_hb + 18'd1;
			if (!mbox_valid || (status_osd_safe != mbox_last) || (mbox_hb == 18'd0))
				mbox_req <= 1'b1;
			sdram_mbox_hb <= sdram_mbox_hb + 18'd1;
			if (!sdram_mbox_valid || (sdram_status_safe != sdram_mbox_last) || (sdram_mbox_hb == 18'd0))
				sdram_mbox_req <= 1'b1;
			frame_mbox_hb <= frame_mbox_hb + 18'd1;
			if (!frame_mbox_valid || ({underrun_count, debug_state} != frame_mbox_last) || (frame_mbox_hb == 18'd0))
				frame_mbox_req <= 1'b1;

			// PLXD bank-release: vsync toggle sync and heartbeat
			vsync_t_d1 <= vsync_toggle;
			vsync_t_d2 <= vsync_t_d1;
			if (vsync_t_d2 != vsync_t_seen) begin
				vsync_t_seen <= vsync_t_d2;
				bank_vsync_count <= bank_vsync_count + 16'd1;
				bank_mbox_req <= 1'b1;
			end
			bank_mbox_hb <= bank_mbox_hb + 18'd1;
			if (!bank_mbox_valid || (bank_mbox_hb == 18'd0))
				bank_mbox_req <= 1'b1;

			if (db_bad_format) begin
				format_error <= 1'b1;
				doorbell_ok <= 1'b0;
				frame_mbox_req <= 1'b1;
			end
			if (db_token_new) begin
				last_seq <= db_token;
				have_seq <= 1'b1;
				format_error <= 1'b0;
			end
			if (db_magic_ok && doorbell_primed && !db_token_new && !db_stale_fallback) begin
				if (stale_db_polls != STALE_DB_POLL_W'(STALE_DB_POLL_MAX))
					stale_db_polls <= stale_db_polls + 1'b1;
			end
			if (db_token_new)
				stale_db_polls <= '0;
			if (db_new_seq) begin
				pending_bank_ddr <= DDRAM_DOUT[63];
				swap_req_t_ddr <= ~swap_req_t_ddr;
				doorbell_ok <= 1'b1;
				stale_db_polls <= '0;
			end
			if (spi_edge_ddr) begin
				start_seen <= start_d2;
				if (!STRICT_YUV_DOORBELL || (have_seq && !format_error)) begin
					pending_bank_ddr <= bank_sel_d2;
					swap_req_t_ddr <= ~swap_req_t_ddr;
				end
			end

			case (state_ddr)
				S_IDLE: begin
					pending_ready_ddr <= swap_pending_d2 &&
					                     (sched_valid ? (sched_for_pending && sched_pending_ready) : pending_ready_c);
					poll_div <= poll_div + 16'd1;
					if (frame_mbox_req && (!frame_mbox_valid || poll_div[7:0] == 8'd224)
					    && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= FRAME_MAILBOX_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_DIN <= {underrun_count, debug_state, frame_mbox_seq + 8'd1, MAGIC_F};
						DDRAM_WE <= 1'b1;
						frame_mbox_seq <= frame_mbox_seq + 8'd1;
						frame_mbox_last <= {underrun_count, debug_state};
						frame_mbox_valid <= 1'b1;
						frame_mbox_req <= 1'b0;
						state_ddr <= S_WRITE_WAIT;
					end else if (bank_mbox_req && (!bank_mbox_valid || poll_div[7:0] == 8'd160)
					    && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						// PLXD bank-release: tell ARM which bank is safe to write
						// Layout: [63:48] frames_done, [35] swap_pending,
						//   [34] disp_bank, [33:32] free_bank_mask, [31:0] magic
						DDRAM_ADDR <= BANK_MAILBOX_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_DIN <= {bank_vsync_count,                    // [63:48] frames_done
						              12'd0,                                // [47:36] reserved
						              swap_pending_d2,                      // [35]
						              disp_bank_d2,                         // [34]
						              swap_pending_d2 ? 2'b00 :             // [33:32] free_bank_mask
						                (disp_bank_d2 ? 2'b01 : 2'b10),
						              MAGIC_D};                             // [31:0]
						DDRAM_WE <= 1'b1;
						bank_mbox_seq <= bank_mbox_seq + 8'd1;
						bank_mbox_valid <= 1'b1;
						bank_mbox_req <= 1'b0;
						bank_mbox_hb <= 18'd0;
						state_ddr <= S_WRITE_WAIT;
					// KEEP_VALID_UNTIL_FILL_DONE: do NOT clear y_valid/c_valid or
					// retag bank at arm/issue. Tag+valid commit only when full
					// line lands (S_LINE_WAIT done) — anti-shear keepv.
					end else if (PIPELINE_REFILL_SCHEDULER && sched_valid) begin
						fill_bank <= sched_bank;
						fill_idx <= sched_idx;
						fill_plane_v <= 1'b0;
						fill_qword <= '0;
						sched_valid <= 1'b0;
						if (sched_is_y) begin
							fill_y <= sched_y;
							qwords_remaining <= Y_LINE_QWORDS[Y_QW_AW:0];
							fill_is_chroma <= 1'b0;
						end else begin
							fill_cy <= sched_cy;
							qwords_remaining <= C_LINE_QWORDS[Y_QW_AW:0];
							fill_is_chroma <= 1'b1;
						end
						state_ddr <= S_LINE_PREP;
					// keepv2 tip: S_IDLE from combo need_*_c (lite sample STA-regressed).
					// L4: take C *before* Y once two Y fills have run. Putting
					// this after the Y arm left c_rr stuck at 2 with C never
					// scheduled (HDMI grey chevron, DDR U=53 V=169).
					end else if (C_INTERLEAVE_ON_Y_BEAM && c_rr_r >= 2'd2) begin
						c_rr_r <= 2'd0;
						if (PIPELINE_REFILL_SCHEDULER) begin
							sched_valid <= 1'b1;
							sched_is_y <= 1'b0;
							sched_for_pending <= swap_pending_d2 && need_c_prep_c;
							sched_bank <= (swap_pending_d2 && need_c_prep_c) ? pending_bank_d2 : disp_bank_d2;
							sched_cy <= (swap_pending_d2 && need_c_prep_c) ? target_c_prep_c : target_c_cur_c;
							sched_idx <= (swap_pending_d2 && need_c_prep_c) ? target_c_idx_prep_c : target_c_idx_cur_c;
							sched_pending_ready <= pending_ready_c;
						end else begin
							fill_bank <= (swap_pending_d2 && need_c_prep_c) ? pending_bank_d2 : disp_bank_d2;
							fill_cy <= (swap_pending_d2 && need_c_prep_c) ? target_c_prep_c : target_c_cur_c;
							fill_idx <= (swap_pending_d2 && need_c_prep_c) ? target_c_idx_prep_c : target_c_idx_cur_c;
							fill_is_chroma <= 1'b1;
							fill_plane_v <= 1'b0;
							fill_qword <= '0;
							qwords_remaining <= C_LINE_QWORDS[Y_QW_AW:0];
							state_ddr <= S_LINE_PREP;
						end
					end else if ((swap_pending_d2 && need_y_prep_c) || (has_frame_d2 && need_y_cur_c)) begin
						if (C_INTERLEAVE_ON_Y_BEAM && c_rr_r < 2'd2)
							c_rr_r <= c_rr_r + 2'd1;
						if (PIPELINE_REFILL_SCHEDULER) begin
							sched_valid <= 1'b1;
							sched_is_y <= 1'b1;
							sched_for_pending <= swap_pending_d2 && need_y_prep_c;
							sched_bank <= (swap_pending_d2 && need_y_prep_c) ? pending_bank_d2 : disp_bank_d2;
							sched_y <= (swap_pending_d2 && need_y_prep_c) ? target_y_prep_c : target_y_cur_c;
							sched_idx <= (swap_pending_d2 && need_y_prep_c) ? target_y_idx_prep_c : target_y_idx_cur_c;
							sched_pending_ready <= pending_ready_c;
						end else begin
							fill_bank <= (swap_pending_d2 && need_y_prep_c) ? pending_bank_d2 : disp_bank_d2;
							fill_y <= (swap_pending_d2 && need_y_prep_c) ? target_y_prep_c : target_y_cur_c;
							fill_idx <= (swap_pending_d2 && need_y_prep_c) ? target_y_idx_prep_c : target_y_idx_cur_c;
							fill_is_chroma <= 1'b0;
							fill_plane_v <= 1'b0;
							fill_qword <= '0;
							qwords_remaining <= Y_LINE_QWORDS[Y_QW_AW:0];
							state_ddr <= S_LINE_PREP;
						end
					// SOFT_C_Y_BEFORE_C (keepv25): starve C only while *beam* Y
					// is missing. Ahead-window Y need must not block C forever
					// under FOAR Y_FILL_STRIDE=2 (keepv22 gray RCA).
					// L4 C_INTERLEAVE_ON_Y_BEAM: after two Y fills, fetch C even
					// if the beam still wants Y (720p LC=8 otherwise stays grey).
					end else if (((swap_pending_d2 && need_c_prep_c) || (has_frame_d2 && need_c_cur_c))
					            && (!(has_frame_d2 && need_y_beam_c)
					                || (C_INTERLEAVE_ON_Y_BEAM && c_rr_r >= 2'd2))) begin
						c_rr_r <= 2'd0;
						if (PIPELINE_REFILL_SCHEDULER) begin
							sched_valid <= 1'b1;
							sched_is_y <= 1'b0;
							sched_for_pending <= swap_pending_d2 && need_c_prep_c;
							sched_bank <= (swap_pending_d2 && need_c_prep_c) ? pending_bank_d2 : disp_bank_d2;
							sched_cy <= (swap_pending_d2 && need_c_prep_c) ? target_c_prep_c : target_c_cur_c;
							sched_idx <= (swap_pending_d2 && need_c_prep_c) ? target_c_idx_prep_c : target_c_idx_cur_c;
							sched_pending_ready <= pending_ready_c;
						end else begin
							fill_bank <= (swap_pending_d2 && need_c_prep_c) ? pending_bank_d2 : disp_bank_d2;
							fill_cy <= (swap_pending_d2 && need_c_prep_c) ? target_c_prep_c : target_c_cur_c;
							fill_idx <= (swap_pending_d2 && need_c_prep_c) ? target_c_idx_prep_c : target_c_idx_cur_c;
							fill_is_chroma <= 1'b1;
							fill_plane_v <= 1'b0;
							fill_qword <= '0;
							qwords_remaining <= C_LINE_QWORDS[Y_QW_AW:0];
							state_ddr <= S_LINE_PREP;
						end
					end else if (!poll_pending && poll_div[7:0] == 8'd0 && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= DOORBELL_W;
						DDRAM_BURSTCNT <= DYN_BASE_EN ? 8'd2 : 8'd1;
						DDRAM_RD <= 1'b1;
						poll_pending <= 1'b1;
						poll_beat <= 1'b0;
						state_ddr <= S_POLL_WAIT;
					end else if (!cmd_empty && poll_div[7:0] == 8'd64 && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= INPUT_MAILBOX_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_DIN <= {imbox_seq + 16'd1, imbox_cmd_seq + 8'd1, cmd_rdata, MAGIC_I};
						DDRAM_WE <= 1'b1;
						cmd_pop <= 1'b1;
						imbox_seq <= imbox_seq + 16'd1;
						imbox_cmd_seq <= imbox_cmd_seq + 8'd1;
						state_ddr <= S_WRITE_WAIT;
					end else if (mbox_req && poll_div[7:0] == 8'd128 && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= MAILBOX_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_DIN <= {mbox_seq + 16'd1, status_osd_safe, MAGIC_S};
						DDRAM_WE <= 1'b1;
						mbox_seq <= mbox_seq + 16'd1;
						mbox_last <= status_osd_safe;
						mbox_valid <= 1'b1;
						mbox_req <= 1'b0;
						state_ddr <= S_WRITE_WAIT;
					end else if (sdram_mbox_req && poll_div[7:0] == 8'd192 && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= SDRAM_MAILBOX_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_DIN <= {sdram_status_safe, sdram_mbox_seq + 8'd1, MAGIC_M};
						DDRAM_WE <= 1'b1;
						sdram_mbox_seq <= sdram_mbox_seq + 8'd1;
						sdram_mbox_last <= sdram_status_safe;
						sdram_mbox_valid <= 1'b1;
						sdram_mbox_req <= 1'b0;
						state_ddr <= S_WRITE_WAIT;
					end else if (plxj_req && poll_div[7:0] == 8'd32 && !DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= ASPECT_ACK_W;
						DDRAM_BURSTCNT <= 8'd1;
						DDRAM_DIN <= {plxj_token, plxj_y, plxj_x, MAGIC_J};
						DDRAM_WE <= 1'b1;
						plxj_req <= 1'b0;
						state_ddr <= S_WRITE_WAIT;
					end
				end

				// keepv21: sample combo line_addr/burst, then issue (STA cut).
				S_LINE_PREP: begin
					line_addr_r <= line_addr;
					burst_this_r <= burst_this;
					burst_cap_r <= burst_cap;
					state_ddr <= S_LINE_ISSUE;
				end

				S_LINE_ISSUE: begin
					if (!DDRAM_BUSY && !DDRAM_RD && !DDRAM_WE) begin
						DDRAM_ADDR <= line_addr_r;
						DDRAM_BURSTCNT <= burst_this_r;
						DDRAM_RD <= 1'b1;
						burst_left <= burst_cap_r;
						state_ddr <= S_LINE_WAIT;
					end
				end

				S_LINE_WAIT: begin
					if (DDRAM_DOUT_READY) begin
						if (fill_is_chroma) begin
							c_wr_addr <= fill_qword[C_QW_AW-1:0];
							if (fill_plane_v) begin
								v_wr_data <= DDRAM_DOUT;
								v_wr[fill_idx] <= 1'b1;
							end else begin
								u_wr_data <= DDRAM_DOUT;
								u_wr[fill_idx] <= 1'b1;
							end
						end else begin
							y_wr_addr <= fill_qword[Y_QW_AW-1:0];
							y_wr_data <= DDRAM_DOUT;
							y_wr[fill_idx] <= 1'b1;
						end
						fill_qword <= fill_qword + 1'b1;
						qwords_remaining <= qwords_remaining - 1'b1;
						burst_left <= burst_left - 1'b1;
						if (qwords_remaining == 1) begin
							if (fill_is_chroma && !fill_plane_v) begin
								fill_plane_v <= 1'b1;
								fill_qword <= '0;
								qwords_remaining <= C_LINE_QWORDS[Y_QW_AW:0];
								state_ddr <= S_LINE_PREP;
							end else begin
								if (fill_is_chroma) begin
									c_line[fill_idx] <= fill_cy;
									c_bank[fill_idx] <= fill_bank;
									c_valid[fill_idx] <= 1'b1;
								end else begin
									y_line[fill_idx] <= fill_y;
									y_bank[fill_idx] <= fill_bank;
									y_valid[fill_idx] <= 1'b1;
								end
								state_ddr <= S_IDLE;
							end
						end else if (burst_left == 1) begin
							state_ddr <= S_LINE_PREP;
						end
					end
				end

				S_POLL_WAIT: begin
					if (DDRAM_DOUT_READY) begin
						if (!DYN_BASE_EN || poll_beat == 1'b0) begin
							doorbell_primed <= 1'b1;
							kick_bank_r <= DDRAM_DOUT[63];
							if (DYN_BASE_EN) begin
								poll_beat <= 1'b1;
							end else begin
								poll_pending <= 1'b0;
								state_ddr <= S_IDLE;
							end
						end else begin
							// Always sample valid (including 0). Sticky 1 from
							// leftover doorbell+8 made L4 read a bogus phys
							// (grey chevron on e7097c6c / ca7aff28).
							if (!kick_bank_r) begin
								dyn_base0_r <= DDRAM_DOUT[28:0];
								dyn_valid0_r <= DDRAM_DOUT[31];
							end else begin
								dyn_base1_r <= DDRAM_DOUT[28:0];
								dyn_valid1_r <= DDRAM_DOUT[31];
							end
							poll_beat <= 1'b0;
							poll_pending <= 1'b0;
							state_ddr <= S_IDLE;
						end
					end
				end

				S_WRITE_WAIT: begin
					if (!DDRAM_BUSY && !DDRAM_WE)
						state_ddr <= S_IDLE;
				end

				default: state_ddr <= S_IDLE;
			endcase
		end
	end

endmodule
