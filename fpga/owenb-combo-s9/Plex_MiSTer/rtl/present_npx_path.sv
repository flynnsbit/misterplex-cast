// Multi-pixel → single-pixel present bridge (P1 architecture).
//
// THROUGHPUT (rd-duck / w-clock confirmed):
//   Old design serialised PPC lanes on clk_sys *before* the FIFO (1 + PPC sys
//   cycles/group) → peak Mpix/s = F_sys * PPC/(1+PPC).
//   At F_sys=20 MHz, PPC=2: 20*2/3 = **13.33 Mpix/s** << 29.7 needed for
//   CEA 720p24.
//
//   This revision crosses **whole groups** on the async_fifo (1 write/group).
//   Peak Mpix/s = F_sys*PPC (40 @20/PPC2 ≥ 29.7). Unpack on clk_pix.
//
// PREFILL + SKID (rd-duck audit):
//   Equal long-term Bresenham rates still have local gaps (e.g. 100 ns emit
//   spacing vs 67 ns to unpack PPC=2). Starting drain on the first FIFO word
//   is phase-dependent. Read side waits for PREFILL_GROUPS async writes
//   (2FF-synced) before the first pop.
//   Sys-side SKID: beam backpressure via in_ready leaves headroom so delayed
//   ddr_frame_store rd_n_valid (multi-cycle) is not dropped when the async
//   FIFO is almost full. See IN_READY_UPSTREAM.md.
//
// INCLUDE_SYNC=1 packs {fs,vs,hs,vb,hb,lane_valid,r,g,b}.
//   fs = frame_start; must ride FIFO with RGB so bank-swap/cadence match DE
//   (same-clock with INCLUDE_SYNC=0 + direct beam H/V is a known trap — H/V lead).
// rd_underrun: real mid-stream empty after stream_seen. Never hard-tied 0.
// Product default: NOT instantiated (PRESENT_MULTI_PIXEL undefined).
// CDC: async_fifo gray + 2FF prefill_go only.

module present_npx_path #(
	parameter int PX_PER_CLK     = 4,
	parameter int FIFO_AW        = 6,  // async depth 64 groups
	parameter bit INCLUDE_SYNC   = 1'b0,
	parameter int PREFILL_GROUPS = 8,  // 0 = start on first word (unit tests)
	parameter int SKID_AW        = 4   // depth 16; headroom for store pipe
)(
	input  wire                 clk_sys,
	input  wire                 reset_sys,
	input  wire                 clk_pix,
	input  wire                 reset_pix,

	input  wire                 in_valid,
	input  wire [PX_PER_CLK*8-1:0] in_r,
	input  wire [PX_PER_CLK*8-1:0] in_g,
	input  wire [PX_PER_CLK*8-1:0] in_b,
	input  wire [PX_PER_CLK-1:0]   in_lane_valid,
	// Quartus forbids default values on inputs — parent must drive these.
	input  wire                 in_hblank,
	input  wire                 in_hsync,
	input  wire                 in_vblank,
	input  wire                 in_vsync,
	input  wire                 in_fstart,
	output wire                 in_ready,

	output reg                  out_ce,
	output reg  [7:0]           out_r,
	output reg  [7:0]           out_g,
	output reg  [7:0]           out_b,
	output reg                  out_hblank,
	output reg                  out_hsync,
	output reg                  out_vblank,
	output reg                  out_vsync,
	output reg                  out_fstart,

	output wire                 wr_full,
	output wire                 wr_almost_full,
	output reg                  rd_underrun,
	output wire                 rd_empty
);
	localparam int LANE_W = (PX_PER_CLK <= 1) ? 1 : $clog2(PX_PER_CLK);
	localparam int RGB_N  = PX_PER_CLK * 8;
	localparam int SYNC_W = 5; // fs,vs,hs,vb,hb
	localparam int W = (INCLUDE_SYNC ? SYNC_W : 0) + PX_PER_CLK + 3 * RGB_N;
	localparam int SKID_D = 1 << SKID_AW;
	// Leave ≥4 slots when deasserting in_ready (store pipe ~3: core reg +
	// rd_active_r/_d + commit). Hard full still refuses.
	localparam int SKID_AF_LEVEL = SKID_D - 4;
	localparam int PRE_W = 8;

	function automatic [W-1:0] pack_group(
		input fs, vs, hs, vb, hb,
		input [PX_PER_CLK-1:0] lv,
		input [RGB_N-1:0] r, g, b
	);
// Width-stable pack: always size to W (INCLUDE_SYNC folds into W at elab).
		begin
			if (INCLUDE_SYNC)
				pack_group = W'({fs, vs, hs, vb, hb, lv, r, g, b});
			else
				pack_group = W'({lv, r, g, b});
		end
	endfunction

	reg [W-1:0]     skid_mem [0:SKID_D-1];
	reg [SKID_AW:0] skid_wp, skid_rp;
	wire [SKID_AW:0] skid_level = skid_wp - skid_rp;
	wire skid_empty = (skid_wp == skid_rp);
	wire skid_full  = (skid_level == SKID_D[SKID_AW:0]);
	wire skid_af    = (skid_level >= SKID_AF_LEVEL[SKID_AW:0]);

	wire         fifo_full, fifo_almost, fifo_empty;
	wire [W-1:0] rd_data;
	reg          rd_en;

	// Combo write into async_fifo (rd-duck): never advance skid_rp on a
	// registered wr_en that can later be rejected at full. Old path set
	// wr_en<=1 and skid_rp++ together; FIFO saw wr_en one cycle later and
	// could drop the beat. Fire and data are combinatorial with skid head.
	wire        fifo_wr_fire = !reset_sys && !skid_empty && !fifo_full;
	wire [W-1:0] fifo_wr_data = skid_mem[skid_rp[SKID_AW-1:0]];

	async_fifo #(
		.WIDTH(W),
		.AW(FIFO_AW),
		.AF_HEADROOM(4)
	) u_pix_fifo (
		.wr_clk(clk_sys),
		.wr_reset(reset_sys),
		.wr_en(fifo_wr_fire),
		.wr_data(fifo_wr_data),
		.wr_full(fifo_full),
		.wr_almost_full(fifo_almost),
		.rd_clk(clk_pix),
		.rd_reset(reset_pix),
		.rd_en(rd_en),
		.rd_data(rd_data),
		.rd_empty(fifo_empty)
	);

	assign wr_full = fifo_full;
	// Soft AF for beam: async AF band OR skid AF. Hard full still blocks fire.
	assign wr_almost_full = fifo_almost || skid_af;
	assign rd_empty = fifo_empty;
	assign in_ready = !reset_sys && !skid_af;

	reg [PRE_W-1:0] wr_group_count;
	reg             prefill_go_sys;
	reg             prefill_go_p1, prefill_go_pix;

	wire [W-1:0] in_packed = pack_group(
		in_fstart, in_vsync, in_hsync, in_vblank, in_hblank,
		in_lane_valid, in_r, in_g, in_b);

	wire skid_accept = in_valid && !skid_full;

	always @(posedge clk_sys) begin
		if (reset_sys) begin
			skid_wp <= '0;
			skid_rp <= '0;
			wr_group_count <= '0;
			prefill_go_sys <= (PREFILL_GROUPS == 0);
		end else begin
			if (skid_accept) begin
				skid_mem[skid_wp[SKID_AW-1:0]] <= in_packed;
				skid_wp <= skid_wp + 1'b1;
			end

			// Pop skid only when FIFO accepts this cycle (combo fire).
			if (fifo_wr_fire) begin
				skid_rp <= skid_rp + 1'b1;
				if (wr_group_count != {PRE_W{1'b1}})
					wr_group_count <= wr_group_count + 1'b1;
				if (PREFILL_GROUPS == 0)
					prefill_go_sys <= 1'b1;
				else if ((wr_group_count + 1'b1) >= PRE_W'(PREFILL_GROUPS))
					prefill_go_sys <= 1'b1;
			end
		end
	end

	always @(posedge clk_pix) begin
		if (reset_pix) begin
			prefill_go_p1  <= 1'b0;
			prefill_go_pix <= 1'b0;
		end else begin
			prefill_go_p1  <= prefill_go_sys;
			prefill_go_pix <= prefill_go_p1;
		end
	end

	reg                   hold_busy;
	reg [LANE_W-1:0]      lane_idx;
	reg [RGB_N-1:0]       hold_r, hold_g, hold_b;
	reg [PX_PER_CLK-1:0]  hold_lv;
	reg                   hold_hb, hold_hs, hold_vb, hold_vs, hold_fs;
	reg                   stream_seen;

	wire [7:0] lane_r = hold_r[lane_idx*8 +: 8];
	wire [7:0] lane_g = hold_g[lane_idx*8 +: 8];
	wire [7:0] lane_b = hold_b[lane_idx*8 +: 8];
	wire       lane_v = hold_lv[lane_idx];

	wire [RGB_N-1:0] pop_b  = rd_data[RGB_N-1:0];
	wire [RGB_N-1:0] pop_g  = rd_data[2*RGB_N-1:RGB_N];
	wire [RGB_N-1:0] pop_r  = rd_data[3*RGB_N-1:2*RGB_N];
	wire [PX_PER_CLK-1:0] pop_lv = rd_data[3*RGB_N+PX_PER_CLK-1:3*RGB_N];
	wire pop_hb = INCLUDE_SYNC ? rd_data[3*RGB_N+PX_PER_CLK+0] : 1'b0;
	wire pop_vb = INCLUDE_SYNC ? rd_data[3*RGB_N+PX_PER_CLK+1] : 1'b0;
	wire pop_hs = INCLUDE_SYNC ? rd_data[3*RGB_N+PX_PER_CLK+2] : 1'b0;
	wire pop_vs = INCLUDE_SYNC ? rd_data[3*RGB_N+PX_PER_CLK+3] : 1'b0;
	wire pop_fs = INCLUDE_SYNC ? rd_data[3*RGB_N+PX_PER_CLK+4] : 1'b0;

	localparam logic [LANE_W-1:0] LANE_LAST = LANE_W'(PX_PER_CLK - 1);

	wire allow_pop = stream_seen || prefill_go_pix || (PREFILL_GROUPS == 0);

	always @(posedge clk_pix) begin
		if (reset_pix) begin
			out_ce      <= 1'b0;
			out_r       <= 8'd0;
			out_g       <= 8'd0;
			out_b       <= 8'd0;
			out_hblank  <= 1'b1;
			out_hsync   <= 1'b0;
			out_vblank  <= 1'b1;
			out_vsync   <= 1'b0;
			out_fstart  <= 1'b0;
			rd_underrun <= 1'b0;
			hold_busy   <= 1'b0;
			lane_idx    <= '0;
			hold_r      <= '0;
			hold_g      <= '0;
			hold_b      <= '0;
			hold_lv     <= '0;
			hold_hb     <= 1'b1;
			hold_hs     <= 1'b0;
			hold_vb     <= 1'b1;
			hold_vs     <= 1'b0;
			hold_fs     <= 1'b0;
			stream_seen <= 1'b0;
			rd_en       <= 1'b0;
		end else begin
			rd_en       <= 1'b0;
			out_ce      <= 1'b0;
			rd_underrun <= 1'b0;

			if (hold_busy) begin
				out_ce     <= 1'b1;
				out_hblank <= hold_hb;
				out_vblank <= hold_vb;
				out_hsync  <= hold_hs;
				out_vsync  <= hold_vs;
				// fstart is a group-level pulse: assert only on first unpacked lane
				out_fstart <= 1'b0;
				stream_seen <= 1'b1;
				if (lane_v) begin
					out_r <= lane_r;
					out_g <= lane_g;
					out_b <= lane_b;
				end else begin
					out_r <= 8'd0;
					out_g <= 8'd0;
					out_b <= 8'd0;
				end
				if (lane_idx == LANE_LAST) begin
					hold_busy <= 1'b0;
					lane_idx  <= '0;
				end else begin
					lane_idx <= lane_idx + 1'b1;
				end
			end else if (allow_pop && !fifo_empty) begin
				rd_en    <= 1'b1;
				hold_r   <= pop_r;
				hold_g   <= pop_g;
				hold_b   <= pop_b;
				hold_lv  <= pop_lv;
				hold_hb  <= pop_hb;
				hold_hs  <= pop_hs;
				hold_vb  <= pop_vb;
				hold_vs  <= pop_vs;
				hold_fs  <= pop_fs;
				if (PX_PER_CLK > 1) begin
					lane_idx  <= LANE_W'(1);
					hold_busy <= 1'b1;
				end else begin
					lane_idx  <= '0;
					hold_busy <= 1'b0;
				end
				out_ce     <= 1'b1;
				out_hblank <= pop_hb;
				out_vblank <= pop_vb;
				out_hsync  <= pop_hs;
				out_vsync  <= pop_vs;
				out_fstart <= pop_fs;
				stream_seen <= 1'b1;
				if (pop_lv[0]) begin
					out_r <= pop_r[7:0];
					out_g <= pop_g[7:0];
					out_b <= pop_b[7:0];
				end else begin
					out_r <= 8'd0;
					out_g <= 8'd0;
					out_b <= 8'd0;
				end
			end else begin
				out_r <= 8'd0;
				out_g <= 8'd0;
				out_b <= 8'd0;
				out_fstart <= 1'b0;
				if (stream_seen && fifo_empty)
					rd_underrun <= 1'b1;
			end
		end
	end
endmodule
