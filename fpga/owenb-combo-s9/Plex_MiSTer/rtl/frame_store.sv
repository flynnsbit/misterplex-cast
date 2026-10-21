// SDRAM-backed RGB565 frame store.
// Write path: HPS/decoder pixels cross into the 100 MHz SDRAM controller.
// Read path: scanout uses parameterized line buffers and prefetches ahead of
// the raster so the visible pixel path remains BRAM-like and edge timing stays
// aligned with present_core's measured DE_LAG.

module frame_store #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240,
	parameter int FRAME_STRIDE = FRAME_W,
	parameter int REFRESH_CYCLES = 780,
	parameter int CMD_FIFO_AW = 5,
	parameter int LINE_COUNT = 2
)(
	input  wire        clk,
	input  wire        clk_sdram,
	input  wire        reset,

	// ---- write (ioctl / HPS) ----
	input  wire        wr_en,
	input  wire [15:0] wr_pixel,
	input  wire        wr_reset_ptr,
	output wire        wr_ready,
	output reg  [31:0] wr_count,
	output wire        wr_frame_done,

	// ---- read (present) ----
	input  wire [$clog2(FRAME_W)-1:0] rd_x,
	input  wire [$clog2(FRAME_H)-1:0] rd_y,
	input  wire        rd_active,
	output reg  [7:0]  rd_r,
	output reg  [7:0]  rd_g,
	output reg  [7:0]  rd_b,

	// ---- SDRAM controller port (clk_sdram domain) ----
	input  wire [15:0] sdram_dout,
	input  wire        sdram_ready,
	output reg         sdram_sel,
	output reg  [26:1] sdram_addr,
	output reg  [15:0] sdram_din,
	output reg         sdram_wr,
	output reg         sdram_rd,
	output wire  [1:0] sdram_bs,
	output reg         sdram_refresh,

	// ---- control ----
	input  wire        swap_banks,
	input  wire        vsync_pulse,
	output reg         has_frame,
	output reg         swap_pending,
	output reg  [15:0] underrun_count,
	output wire  [7:0] debug_state
);

	localparam int PIXELS = FRAME_W * FRAME_H;
	localparam int FRAME_WORDS = FRAME_STRIDE * FRAME_H;
	localparam int ADDR_W = $clog2(FRAME_WORDS * 2);
	localparam int X_W = $clog2(FRAME_W);
	localparam int Y_W = $clog2(FRAME_H);
	localparam [ADDR_W-1:0] FRAME_WORDS_W = ADDR_W'(FRAME_WORDS);
	localparam [ADDR_W-1:0] BANK0_BASE = '0;
	localparam [ADDR_W-1:0] BANK1_BASE = FRAME_WORDS_W;
	localparam [31:0] PIXELS_COUNT = PIXELS;
	localparam [31:0] LAST_COUNT = PIXELS_COUNT - 32'd1;
	localparam [X_W-1:0] LAST_X = X_W'(FRAME_W - 1);
	localparam [Y_W-1:0] LAST_Y = Y_W'(FRAME_H - 1);
	localparam [15:0] REFRESH_LIMIT = REFRESH_CYCLES[15:0];
	localparam int SDRAM_ADDR_PAD = 26 - ADDR_W;
	localparam int LINE_AW = X_W;
	localparam int MAX_LINES = 8;
	localparam int LINE_SLOTS = MAX_LINES * 2;
	localparam [3:0] SECOND_SET_BASE = 4'd8;

	localparam [1:0] CMD_PIXEL = 2'd0;
	localparam [1:0] CMD_RESET = 2'd1;
	localparam [1:0] CMD_SWAP  = 2'd2;

	assign sdram_bs = 2'b11;

	reg  [LINE_SLOTS-1:0] line_wr;
	reg  [LINE_AW-1:0]  line_wr_addr;
	reg  [15:0]         line_wr_data;
	wire [15:0]         line_q [0:LINE_SLOTS-1];
	wire [X_W-1:0]     rd_x_clamped = (rd_x <= LAST_X) ? rd_x : LAST_X;
	wire [LINE_AW-1:0]  line_rd_addr = rd_x_clamped[LINE_AW-1:0];

	genvar li;
	generate
		for (li = 0; li < LINE_SLOTS; li = li + 1) begin : gen_line
			if ((li < LINE_COUNT) ||
			    ((li >= MAX_LINES) && (li < (MAX_LINES + LINE_COUNT)))) begin : used
				line_buf_ram #(
					.WIDTH(FRAME_W),
					.AW(LINE_AW)
				) ram (
					.wr_clk(clk_sdram),
					.wr_en(line_wr[li]),
					.wr_addr(line_wr_addr),
					.wr_data(line_wr_data),
					.rd_clk(clk),
					.rd_addr(line_rd_addr),
					.rd_data(line_q[li])
				);
			end else begin : unused
				assign line_q[li] = 16'd0;
			end
		end
	endgenerate

	reg disp_bank;
	reg disp_buf;
	reg swap_commit_wait;
	wire cmd_full, cmd_almost_full, cmd_empty;
	wire [17:0] cmd_rdata;
	reg  cmd_pop;
	wire accept_cmd = !cmd_almost_full && !swap_pending && !swap_commit_wait;
	wire push_reset = wr_reset_ptr && accept_cmd;
	wire push_pixel = wr_en && accept_cmd;
	wire push_swap  = swap_banks && accept_cmd;
	wire cmd_push   = push_reset || push_pixel || push_swap;
	wire [17:0] cmd_wdata = push_reset ? {CMD_RESET, 16'd0} :
	                       push_swap  ? {CMD_SWAP,  16'd0} :
	                                    {CMD_PIXEL, wr_pixel};
	assign wr_ready = accept_cmd;
	assign wr_frame_done = push_pixel && (wr_count == LAST_COUNT);

	async_fifo #(
		.WIDTH(18),
		.AW(CMD_FIFO_AW)
	) cmd_fifo (
		.wr_clk(clk),
		.wr_reset(reset),
		.wr_en(cmd_push),
		.wr_data(cmd_wdata),
		.wr_full(cmd_full),
		.wr_almost_full(cmd_almost_full),
		.rd_clk(clk_sdram),
		.rd_reset(reset),
		.rd_en(cmd_pop),
		.rd_data(cmd_rdata),
		.rd_empty(cmd_empty)
	);
	wire _cmd_full_unused = cmd_full;

	reg swap_done_t_sdram;
	reg swap_done_s1, swap_done_s2, swap_done_seen;
	reg pending_ready_s1, pending_ready_s2;
	reg pending_ready_sdram;

	always @(posedge clk) begin
		if (reset) begin
			disp_bank <= 1'b0;
			disp_buf <= 1'b0;
			has_frame <= 1'b0;
			swap_pending <= 1'b0;
			swap_commit_wait <= 1'b0;
			wr_count <= 32'd0;
			swap_done_s1 <= 1'b0;
			swap_done_s2 <= 1'b0;
			swap_done_seen <= 1'b0;
			pending_ready_s1 <= 1'b0;
			pending_ready_s2 <= 1'b0;
		end else begin
			swap_done_s1 <= swap_done_t_sdram;
			swap_done_s2 <= swap_done_s1;
			pending_ready_s1 <= pending_ready_sdram;
			pending_ready_s2 <= pending_ready_s1;

			if (push_reset)
				wr_count <= 32'd0;
			else if (push_pixel && wr_count < PIXELS_COUNT)
				wr_count <= wr_count + 32'd1;

			if (push_swap)
				swap_commit_wait <= 1'b1;
			if (swap_done_s2 != swap_done_seen) begin
				swap_done_seen <= swap_done_s2;
				swap_commit_wait <= 1'b0;
				swap_pending <= 1'b1;
			end
			if (vsync_pulse && swap_pending && pending_ready_s2) begin
				disp_bank <= ~disp_bank;
				disp_buf <= ~disp_buf;
				has_frame <= 1'b1;
				swap_pending <= 1'b0;
			end
		end
	end

	// Video-domain line-buffer read. The line RAM adds the same one-cycle
	// address->data latency the old BRAM store had, so present_core's DE_LAG=3
	// edge alignment is intentionally preserved.
	reg       rd_active_r;
	reg       hit_r;
	reg [3:0] hit_idx_r;
	reg [15:0] rd_q;
	reg        rd_active_d;
	reg        miss_d;
	reg [LINE_SLOTS-1:0] line_valid_v1, line_valid_v2;
	reg [LINE_SLOTS-1:0] line_bank_v1, line_bank_v2;
	reg [Y_W-1:0] line_y_v1 [0:LINE_SLOTS-1];
	reg [Y_W-1:0] line_y_v2 [0:LINE_SLOTS-1];
	reg [Y_W-1:0] want_y_sys;
	reg [Y_W-1:0] want_y_s1, want_y_s2;

	integer vi;
	reg hit_now;
	reg [3:0] hit_idx_now;
	reg [15:0] selected_line_q;
	reg [3:0] video_slot;
	always @* begin
		hit_now = 1'b0;
		hit_idx_now = 4'd0;
		selected_line_q = 16'd0;
		for (vi = 0; vi < MAX_LINES; vi = vi + 1) begin
			if (vi < LINE_COUNT) begin
				video_slot = (disp_buf ? SECOND_SET_BASE : 4'd0) + vi[3:0];
				if (line_valid_v2[video_slot] && (line_bank_v2[video_slot] == disp_bank)
				    && (line_y_v2[video_slot] == rd_y) && !hit_now) begin
					hit_now = 1'b1;
					hit_idx_now = video_slot;
				end
				if (hit_idx_r == video_slot)
					selected_line_q = line_q[video_slot];
			end
		end
	end
	wire rd_miss_now = rd_active && has_frame && !hit_now;

	always @(posedge clk) begin
		if (reset) begin
			rd_active_r <= 1'b0;
			hit_r <= 1'b0;
			hit_idx_r <= 4'd0;
			rd_q <= 16'd0;
			rd_active_d <= 1'b0;
			miss_d <= 1'b0;
			underrun_count <= 16'd0;
			want_y_sys <= '0;
			line_valid_v1 <= '0;
			line_valid_v2 <= '0;
			line_bank_v1 <= '0;
			line_bank_v2 <= '0;
		end else begin
			line_valid_v1 <= line_valid;
			line_valid_v2 <= line_valid_v1;
			line_bank_v1 <= line_bank;
			line_bank_v2 <= line_bank_v1;
			for (vi = 0; vi < LINE_SLOTS; vi = vi + 1) begin
				line_y_v1[vi] <= line_y[vi];
				line_y_v2[vi] <= line_y_v1[vi];
			end

			if (rd_y != want_y_sys)
				want_y_sys <= rd_y;

			rd_active_r <= rd_active;
			hit_r <= hit_now;
			hit_idx_r <= hit_idx_now;
			miss_d <= rd_miss_now;
			rd_q <= hit_r ? selected_line_q : 16'd0;
			rd_active_d <= rd_active_r;
			if (miss_d && underrun_count != 16'hFFFF)
				underrun_count <= underrun_count + 16'd1;

			if ((rd_active_d || !rd_active) && has_frame && !miss_d) begin
				rd_r <= {rd_q[15:11], rd_q[15:13]};
				rd_g <= {rd_q[10:5],  rd_q[10:9]};
				rd_b <= {rd_q[4:0],   rd_q[4:2]};
			end else if (!has_frame || miss_d) begin
				rd_r <= 8'd0;
				rd_g <= 8'd0;
				rd_b <= 8'd0;
			end
		end
	end

	// SDRAM-domain controller: line reads have priority; writes drain in the
	// slack and blanking windows. A 100 MHz controller at ~8 cycles/word fills
	// one FRAME_W-word line in proportion to source width; keep starvation fixes
	// structural rather than blindly adding prefetch depth.
	localparam [3:0] S_IDLE       = 4'd0;
	localparam [3:0] S_READ_ISSUE = 4'd1;
	localparam [3:0] S_READ_WAIT  = 4'd2;
	localparam [3:0] S_WRITE_ISSUE= 4'd3;
	localparam [3:0] S_WRITE_WAIT = 4'd4;
	localparam [3:0] S_SWAP       = 4'd5;
	localparam [3:0] S_DECIDE     = 4'd6;

	reg [3:0] state_sdram;
	reg [ADDR_W-1:0] wr_addr_sdram;
	reg [X_W-1:0]    wr_x_sdram;
	reg [Y_W-1:0]    wr_y_sdram;
	reg              wr_bank_sdram;
	reg              fill_bank;
	reg [X_W-1:0]    fill_x;
	reg [Y_W-1:0]    fill_y;
	reg [3:0]        fill_idx;
	reg [15:0]       refresh_ctr;
	reg [LINE_SLOTS-1:0] line_valid;
	reg [LINE_SLOTS-1:0] line_bank;
	reg [Y_W-1:0]    line_y [0:LINE_SLOTS-1];
	reg              disp_bank_s1, disp_bank_s2;
	reg              disp_buf_s1, disp_buf_s2;
	reg              swap_pending_s1, swap_pending_s2;
	reg [17:0]       cmd_hold;
	reg [Y_W-1:0]    desired_y_r [0:MAX_LINES-1];

	function automatic [Y_W-1:0] clamp_ahead(input [Y_W-1:0] base, input integer ahead);
		integer sum;
		begin
			sum = base;
			sum = sum + ahead;
			clamp_ahead = (sum >= FRAME_H) ? LAST_Y : sum[Y_W-1:0];
		end
	endfunction

	function automatic [ADDR_W-1:0] row_word_addr(input [Y_W-1:0] row);
		row_word_addr = ADDR_W'(row) * ADDR_W'(FRAME_STRIDE);
	endfunction

	integer ti, tj, tk;
	reg need_fill_cur_c, need_fill_prep_c, pending_ready_c;
	reg [Y_W-1:0] target_y_cur_c, target_y_prep_c;
	reg [3:0] target_idx_cur_c, target_idx_prep_c;
	reg need_fill_cur, need_fill_prep;
	reg [Y_W-1:0] target_y_cur, target_y_prep;
	reg [3:0] target_idx_cur, target_idx_prep;
	reg found_line;
	reg slot_keep;
	reg found_slot_cur, found_slot_prep;
	reg [Y_W-1:0] desired_y;
	reg [3:0] cur_base_idx, prep_base_idx;
	always @* begin
		cur_base_idx = disp_buf_s2 ? SECOND_SET_BASE : 4'd0;
		prep_base_idx = disp_buf_s2 ? 4'd0 : SECOND_SET_BASE;
		need_fill_cur_c = 1'b0;
		need_fill_prep_c = 1'b0;
		target_y_cur_c = desired_y_r[0];
		target_y_prep_c = '0;
		target_idx_cur_c = cur_base_idx;
		target_idx_prep_c = prep_base_idx;
		found_slot_cur = 1'b0;
		found_slot_prep = 1'b0;
		pending_ready_c = 1'b1;

		for (ti = 0; ti < MAX_LINES; ti = ti + 1) begin
			if (ti < LINE_COUNT) begin
				desired_y = desired_y_r[ti];
				found_line = 1'b0;
				for (tj = 0; tj < MAX_LINES; tj = tj + 1) begin
					if (tj < LINE_COUNT && line_valid[cur_base_idx + tj[3:0]]
					    && (line_bank[cur_base_idx + tj[3:0]] == disp_bank_s2)
					    && (line_y[cur_base_idx + tj[3:0]] == desired_y))
						found_line = 1'b1;
				end
				if (!found_line && !need_fill_cur_c) begin
					need_fill_cur_c = 1'b1;
					target_y_cur_c = desired_y;
				end

				found_line = 1'b0;
				for (tj = 0; tj < MAX_LINES; tj = tj + 1) begin
					if (tj < LINE_COUNT && line_valid[prep_base_idx + tj[3:0]]
					    && (line_bank[prep_base_idx + tj[3:0]] == ~disp_bank_s2)
					    && (line_y[prep_base_idx + tj[3:0]] == ti[Y_W-1:0]))
						found_line = 1'b1;
				end
				if (!found_line) begin
					pending_ready_c = 1'b0;
					if (!need_fill_prep_c) begin
						need_fill_prep_c = 1'b1;
						target_y_prep_c = ti[Y_W-1:0];
					end
				end
			end
		end

		for (tj = 0; tj < MAX_LINES; tj = tj + 1) begin
			if (tj < LINE_COUNT) begin
				slot_keep = 1'b0;
				for (tk = 0; tk < MAX_LINES; tk = tk + 1) begin
					if (tk < LINE_COUNT && line_valid[cur_base_idx + tj[3:0]]
					    && (line_bank[cur_base_idx + tj[3:0]] == disp_bank_s2)
					    && (line_y[cur_base_idx + tj[3:0]] == desired_y_r[tk]))
						slot_keep = 1'b1;
				end
				if ((!line_valid[cur_base_idx + tj[3:0]] || !slot_keep) && !found_slot_cur) begin
					found_slot_cur = 1'b1;
					target_idx_cur_c = cur_base_idx + tj[3:0];
				end

				slot_keep = 1'b0;
				for (tk = 0; tk < MAX_LINES; tk = tk + 1) begin
					if (tk < LINE_COUNT && line_valid[prep_base_idx + tj[3:0]]
					    && (line_bank[prep_base_idx + tj[3:0]] == ~disp_bank_s2)
					    && (line_y[prep_base_idx + tj[3:0]] == tk[Y_W-1:0]))
						slot_keep = 1'b1;
				end
				if ((!line_valid[prep_base_idx + tj[3:0]] || !slot_keep) && !found_slot_prep) begin
					found_slot_prep = 1'b1;
					target_idx_prep_c = prep_base_idx + tj[3:0];
				end
			end
		end
	end

	wire [ADDR_W-1:0] rd_base = fill_bank ? BANK1_BASE : BANK0_BASE;
	wire [ADDR_W-1:0] wr_base = wr_bank_sdram ? BANK1_BASE : BANK0_BASE;
	wire [ADDR_W-1:0] fill_x_addr = ADDR_W'(fill_x);
	wire [ADDR_W-1:0] read_word_addr = rd_base + row_word_addr(fill_y) + fill_x_addr;
	wire [2:0] line_count_code = LINE_COUNT[2:0];
	assign debug_state = {line_count_code, |line_valid, state_sdram};

	always @(posedge clk_sdram) begin
		if (reset) begin
			state_sdram <= S_IDLE;
			sdram_sel <= 1'b0;
			sdram_wr <= 1'b0;
			sdram_rd <= 1'b0;
			sdram_addr <= 26'd0;
			sdram_din <= 16'd0;
			line_wr <= '0;
			line_wr_addr <= '0;
			line_wr_data <= 16'd0;
			sdram_refresh <= 1'b0;
			refresh_ctr <= 16'd0;
			wr_addr_sdram <= BANK1_BASE;
			wr_x_sdram <= '0;
			wr_y_sdram <= '0;
			wr_bank_sdram <= 1'b1;
			fill_bank <= 1'b0;
			fill_x <= '0;
			fill_y <= '0;
			fill_idx <= 4'd0;
			line_valid <= '0;
			line_bank <= '0;
			for (ti = 0; ti < LINE_SLOTS; ti = ti + 1)
				line_y[ti] <= '0;
			disp_bank_s1 <= 1'b0;
			disp_bank_s2 <= 1'b0;
			disp_buf_s1 <= 1'b0;
			disp_buf_s2 <= 1'b0;
			swap_pending_s1 <= 1'b0;
			swap_pending_s2 <= 1'b0;
			want_y_s1 <= '0;
			want_y_s2 <= '0;
			pending_ready_sdram <= 1'b0;
			need_fill_cur <= 1'b0;
			need_fill_prep <= 1'b0;
			target_y_cur <= '0;
			target_y_prep <= '0;
			target_idx_cur <= 4'd0;
			target_idx_prep <= 4'd0;
			for (ti = 0; ti < MAX_LINES; ti = ti + 1)
				desired_y_r[ti] <= '0;
			swap_done_t_sdram <= 1'b0;
			cmd_hold <= 18'd0;
			cmd_pop <= 1'b0;
		end else begin
			sdram_sel <= 1'b0;
			sdram_wr <= 1'b0;
			sdram_rd <= 1'b0;
			line_wr <= '0;
			cmd_pop <= 1'b0;

			disp_bank_s1 <= disp_bank;
			disp_bank_s2 <= disp_bank_s1;
			disp_buf_s1 <= disp_buf;
			disp_buf_s2 <= disp_buf_s1;
			swap_pending_s1 <= swap_pending;
			swap_pending_s2 <= swap_pending_s1;
			want_y_s1 <= want_y_sys;
			want_y_s2 <= want_y_s1;
			for (ti = 0; ti < MAX_LINES; ti = ti + 1)
				desired_y_r[ti] <= clamp_ahead(want_y_s1, ti);

			if (refresh_ctr == REFRESH_LIMIT) begin
				refresh_ctr <= 16'd0;
				sdram_refresh <= ~sdram_refresh;
			end else begin
				refresh_ctr <= refresh_ctr + 16'd1;
			end

			case (state_sdram)
				S_IDLE: begin
					need_fill_cur <= need_fill_cur_c;
					need_fill_prep <= need_fill_prep_c;
					target_y_cur <= target_y_cur_c;
					target_y_prep <= target_y_prep_c;
					target_idx_cur <= target_idx_cur_c;
					target_idx_prep <= target_idx_prep_c;
					pending_ready_sdram <= pending_ready_c;
					state_sdram <= S_DECIDE;
				end

				S_DECIDE: begin
					if (need_fill_cur) begin
						fill_bank <= disp_bank_s2;
						fill_y <= target_y_cur;
						fill_x <= '0;
						fill_idx <= target_idx_cur;
						line_valid[target_idx_cur] <= 1'b0;
						line_bank[target_idx_cur] <= disp_bank_s2;
						state_sdram <= S_READ_ISSUE;
					end else if (swap_pending_s2 && need_fill_prep) begin
						fill_bank <= ~disp_bank_s2;
						fill_y <= target_y_prep;
						fill_x <= '0;
						fill_idx <= target_idx_prep;
						line_valid[target_idx_prep] <= 1'b0;
						line_bank[target_idx_prep] <= ~disp_bank_s2;
						state_sdram <= S_READ_ISSUE;
					end else if (!cmd_empty) begin
						cmd_hold <= cmd_rdata;
						cmd_pop <= 1'b1;
						case (cmd_rdata[17:16])
							CMD_RESET: begin
								wr_bank_sdram <= ~disp_bank_s2;
								wr_addr_sdram <= (~disp_bank_s2) ? BANK1_BASE : BANK0_BASE;
								wr_x_sdram <= '0;
								wr_y_sdram <= '0;
							end
							CMD_SWAP: state_sdram <= S_SWAP;
							default: state_sdram <= S_WRITE_ISSUE;
						endcase
					end else
						state_sdram <= S_IDLE;
				end

				S_READ_ISSUE: begin
					if (sdram_ready) begin
						sdram_sel <= 1'b1;
						sdram_addr <= {{SDRAM_ADDR_PAD{1'b0}}, read_word_addr};
						sdram_rd <= 1'b1;
						state_sdram <= S_READ_WAIT;
					end
				end

				S_READ_WAIT: begin
					if (sdram_ready) begin
						line_wr_addr <= fill_x[LINE_AW-1:0];
						line_wr_data <= sdram_dout;
						line_wr[fill_idx] <= 1'b1;
						if (fill_x == LAST_X) begin
							line_y[fill_idx] <= fill_y;
							line_bank[fill_idx] <= fill_bank;
							line_valid[fill_idx] <= 1'b1;
							state_sdram <= S_IDLE;
						end else begin
							fill_x <= fill_x + 1'b1;
							state_sdram <= S_READ_ISSUE;
						end
					end
				end

				S_WRITE_ISSUE: begin
					if (sdram_ready) begin
						sdram_sel <= 1'b1;
						sdram_addr <= {{SDRAM_ADDR_PAD{1'b0}}, wr_addr_sdram};
						sdram_din <= cmd_hold[15:0];
						sdram_wr <= 1'b1;
						state_sdram <= S_WRITE_WAIT;
					end
				end

				S_WRITE_WAIT: begin
					if (sdram_ready) begin
						if (wr_x_sdram == LAST_X) begin
							wr_x_sdram <= '0;
							if (wr_y_sdram == LAST_Y) begin
								wr_y_sdram <= '0;
								wr_addr_sdram <= wr_base;
							end else begin
								wr_y_sdram <= wr_y_sdram + 1'b1;
								wr_addr_sdram <= wr_base + row_word_addr(wr_y_sdram + 1'b1);
							end
						end else begin
							wr_x_sdram <= wr_x_sdram + 1'b1;
							wr_addr_sdram <= wr_addr_sdram + 1'b1;
						end
						state_sdram <= S_IDLE;
					end
				end

				S_SWAP: begin
					swap_done_t_sdram <= ~swap_done_t_sdram;
					state_sdram <= S_IDLE;
				end

				default: state_sdram <= S_IDLE;
			endcase
		end
	end

endmodule
