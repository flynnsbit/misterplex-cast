// Two-master f2sdram arbiter for the single HPS DDR port.
//
// Master 0 is the video frame store — both master and arbiter share clk
// (the DDR bridge clock, general[2].gpll, 90 MHz).
//
// Master 1 is the compressed-bitstream ring reader on the system clock
// (general[0].gpll, 20 MHz).  clk_m1 carries the consumer's clock so
// that DDR read responses can be safely forwarded via an async FIFO.
// m1_want gets a 2-FF synchroniser; data/address signals are protocol-
// guarded (stable while m1_busy is deasserted).
//
// ⚠ This module previously ran on clk_sys (20 MHz) which placed its
// registered state (rsp_left, grant_m1) between the 90 MHz DDR bridge
// and the 90 MHz frame store, creating a 5.555 ns setup path that
// failed STA by −1.346 ns.  Moving to clk_ddr eliminates that crossing.
//
// ⚠ m1_dout_ready is a single clk_ddr pulse (11.1 ns).  The 20 MHz
// consumer misses ~70 % of pulses depending on DDR CAS alignment.
// An async_fifo on the m1 response path absorbs the rate difference:
// write on clk_ddr, auto-pop read on clk_m1.  Beat-conservation test
// confirmed 7/10 drops WITHOUT the FIFO, 0/10 WITH it.

module ddr_bus_arbiter (
	input  wire        clk,      // DDR bridge clock (90 MHz)
	input  wire        clk_m1,   // m1 consumer clock (20 MHz / clk_sys)
	input  wire        reset,    // synchronous to clk_m1 domain

	input  wire        m1_want,

	output wire        m0_busy,
	input  wire  [7:0] m0_burstcnt,
	input  wire [28:0] m0_addr,
	output wire [63:0] m0_dout,
	output wire        m0_dout_ready,
	input  wire        m0_rd,
	input  wire [63:0] m0_din,
	input  wire  [7:0] m0_be,
	input  wire        m0_we,

	output wire        m1_busy,
	input  wire  [7:0] m1_burstcnt,
	input  wire [28:0] m1_addr,
	output wire [63:0] m1_dout,
	output wire        m1_dout_ready,
	input  wire        m1_rd,
	input  wire [63:0] m1_din,
	input  wire  [7:0] m1_be,
	input  wire        m1_we,

	input  wire        DDRAM_BUSY,
	output wire  [7:0] DDRAM_BURSTCNT,
	output wire [28:0] DDRAM_ADDR,
	input  wire [63:0] DDRAM_DOUT,
	input  wire        DDRAM_DOUT_READY,
	output wire        DDRAM_RD,
	output wire [63:0] DDRAM_DIN,
	output wire  [7:0] DDRAM_BE,
	output wire        DDRAM_WE
);
	// Reset synchroniser (reset originates in clk_sys, we run on clk_ddr)
	reg reset_s1, reset_s2;
	always @(posedge clk or posedge reset) begin
		if (reset) begin
			reset_s1 <= 1'b1;
			reset_s2 <= 1'b1;
		end else begin
			reset_s1 <= 1'b0;
			reset_s2 <= reset_s1;
		end
	end
	wire rst = reset_s2;

	// 2-FF synchroniser for m1_want (clk_sys → clk_ddr)
	reg m1_want_s1, m1_want_s2;
	always @(posedge clk) begin
		if (rst) begin
			m1_want_s1 <= 1'b0;
			m1_want_s2 <= 1'b0;
		end else begin
			m1_want_s1 <= m1_want;
			m1_want_s2 <= m1_want_s1;
		end
	end

	reg grant_m1;
	reg rsp_owner_m1;
	reg [8:0] rsp_left;

	wire rsp_active = rsp_left != 9'd0;
	wire m0_cmd = m0_rd | m0_we;
	wire m1_cmd = m1_rd | m1_we;
	wire use_m1 = grant_m1;
	wire [7:0] selected_burst = use_m1 ? m1_burstcnt : m0_burstcnt;

	assign m0_busy = DDRAM_BUSY | grant_m1 | (rsp_active & rsp_owner_m1);

	// m1_busy: register on clk_ddr to eliminate combinational glitches,
	// then 2-FF sync to clk_m1 for proper CDC.  The consumer uses this
	// only as a level gate (!busy before issuing commands), so the ~100ns
	// sync latency just delays the next command — no protocol breakage.
	wire m1_busy_comb = DDRAM_BUSY | !grant_m1 | (rsp_active & !rsp_owner_m1);
	reg  m1_busy_r;
	always @(posedge clk) begin
		if (rst)
			m1_busy_r <= 1'b1;
		else
			m1_busy_r <= m1_busy_comb;
	end
	reg m1_busy_s1, m1_busy_s2;
	always @(posedge clk_m1) begin
		if (reset) begin
			m1_busy_s1 <= 1'b1;
			m1_busy_s2 <= 1'b1;
		end else begin
			m1_busy_s1 <= m1_busy_r;
			m1_busy_s2 <= m1_busy_s1;
		end
	end
	assign m1_busy = m1_busy_s2;

	assign DDRAM_BURSTCNT = use_m1 ? m1_burstcnt : m0_burstcnt;
	assign DDRAM_ADDR     = use_m1 ? m1_addr      : m0_addr;
	assign DDRAM_RD       = use_m1 ? m1_rd        : m0_rd;
	assign DDRAM_DIN      = use_m1 ? m1_din       : m0_din;
	assign DDRAM_BE       = use_m1 ? m1_be        : m0_be;
	assign DDRAM_WE       = use_m1 ? m1_we        : m0_we;

	assign m0_dout = DDRAM_DOUT;
	assign m0_dout_ready = DDRAM_DOUT_READY & rsp_active & !rsp_owner_m1;

	// ── m1 response FIFO (clk_ddr → clk_m1) ──
	// DDRAM_DOUT_READY is a single clk_ddr pulse per beat.  The clk_m1
	// (20 MHz) consumer would miss ~70 % of those pulses if sampled
	// directly.  The FIFO absorbs beats on the fast side and auto-pops
	// them one per clk_m1 cycle on the slow side.
	wire        m1_rsp_fifo_full;
	wire        m1_rsp_fifo_empty;
	wire [63:0] m1_rsp_fifo_rdata;
	wire        m1_rsp_wr_en = DDRAM_DOUT_READY & rsp_active & rsp_owner_m1;

	async_fifo #(.WIDTH(64), .AW(3)) m1_rsp_fifo (
		.wr_clk   (clk),
		.wr_reset (rst),
		.wr_en    (m1_rsp_wr_en),
		.wr_data  (DDRAM_DOUT),
		.wr_full  (m1_rsp_fifo_full),
		.wr_almost_full (),

		.rd_clk   (clk_m1),
		.rd_reset (reset),       // reset is synchronous to clk_m1
		.rd_en    (!m1_rsp_fifo_empty),  // auto-pop
		.rd_data  (m1_rsp_fifo_rdata),
		.rd_empty (m1_rsp_fifo_empty)
	);

	assign m1_dout       = m1_rsp_fifo_rdata;
	assign m1_dout_ready = !m1_rsp_fifo_empty;

	always @(posedge clk) begin
		if (rst) begin
			grant_m1 <= 1'b0;
			rsp_owner_m1 <= 1'b0;
			rsp_left <= 9'd0;
		end else begin
			if (DDRAM_DOUT_READY && rsp_active)
				rsp_left <= rsp_left - 9'd1;

			if (!DDRAM_BUSY && !rsp_active) begin
				if (grant_m1) begin
					if (m1_rd) begin
						rsp_owner_m1 <= 1'b1;
						rsp_left <= {1'b0, selected_burst};
						grant_m1 <= 1'b0;
					end else if (m1_we || !m1_want_s2) begin
						grant_m1 <= 1'b0;
					end
				end else begin
					if (m0_rd) begin
						rsp_owner_m1 <= 1'b0;
						rsp_left <= {1'b0, selected_burst};
					end else if (!m0_cmd && m1_want_s2) begin
						grant_m1 <= 1'b1;
					end
				end
			end

			if (grant_m1 && !DDRAM_BUSY && m1_cmd && !m1_rd)
				grant_m1 <= 1'b0;
		end
	end
endmodule
