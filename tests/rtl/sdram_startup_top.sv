// Simulation-only SDRAM startup harness.
// Instantiates the real controller and real bring-up memtest with the same
// reset/clock/refresh wiring used by Plex.sv.

module sdram_startup_top #(
	parameter int unsigned SDRAM_CLK_HZ = 100_000_000
)(
	input  wire clk,
	input  wire reset,
	input  wire pll_locked,
	input  wire force_ready_init_high,

	output wire SDRAM_nCS,
	output wire SDRAM_nRAS,
	output wire SDRAM_nCAS,
	output wire SDRAM_nWE,
	output wire SDRAM_CKE,
	output wire SDRAM_CLK,
	output wire [12:0] SDRAM_A,
	output wire [1:0]  SDRAM_BA,
	output wire SDRAM_DQML,
	output wire SDRAM_DQMH,

	output wire sdram_ready,
	output wire sdram_sel,
	output wire sdram_rd,
	output wire sdram_wr,
	output wire [3:0] memtest_state,
	output wire [3:0] memtest_size,
	output wire [15:0] memtest_errors,
	output wire [31:0] startup_cycles,
	output wire [31:0] refresh_cycles,
	output wire [2:0] configured_cas_latency
);
	localparam longint unsigned STARTUP_CYCLES_CALC = ((longint'(SDRAM_CLK_HZ) * 121) + 999_999) / 1_000_000;
	localparam longint unsigned REFRESH_CYCLES_CALC = ((longint'(SDRAM_CLK_HZ) * 64_000) / (8192 * 1_000_000)) - 1;
	localparam int SDRAM_REFRESH_CYCLES = REFRESH_CYCLES_CALC[31:0];

	wire [15:0] SDRAM_DQ;
	wire [26:1] sdram_addr;
	wire [15:0] sdram_din;
	wire [15:0] sdram_dout;
	wire [1:0]  sdram_bs;
	wire        sdram_refresh;
	wire        memtest_done;
	wire        memtest_pass;
	wire [15:0] read_sample;
	wire        first_fail_valid;
	wire [25:0] first_fail_addr;
	wire [15:0] first_fail_expect;
	wire        shared_reset = reset | ~pll_locked;
	assign startup_cycles = STARTUP_CYCLES_CALC[31:0];
	assign refresh_cycles = REFRESH_CYCLES_CALC[31:0];
`ifdef SDRAM_CL3
	assign configured_cas_latency = 3'd3;
`else
	assign configured_cas_latency = 3'd2;
`endif

	reg force_released;
	always @(posedge clk) begin
		if (shared_reset) begin
			force_released <= 1'b0;
			if (force_ready_init_high)
				force ctl.ready = 1'b1;
		end else if (force_ready_init_high && !force_released) begin
			release ctl.ready;
			force_released <= 1'b1;
		end
	end

	sdram_memtest #(
		.REFRESH_CYCLES(SDRAM_REFRESH_CYCLES)
	) memtest (
		.clk(clk),
		.reset(shared_reset),
		.sdram_dout(sdram_dout),
		.sdram_ready(sdram_ready),
		.sdram_sel(sdram_sel),
		.sdram_addr(sdram_addr),
		.sdram_din(sdram_din),
		.sdram_wr(sdram_wr),
		.sdram_rd(sdram_rd),
		.sdram_bs(sdram_bs),
		.sdram_refresh(sdram_refresh),
		.state_code(memtest_state),
		.size_code(memtest_size),
		.error_count(memtest_errors),
		.read_sample(read_sample),
		.first_fail_valid(first_fail_valid),
		.first_fail_addr(first_fail_addr),
		.first_fail_expect(first_fail_expect),
		.done(memtest_done),
		.pass(memtest_pass)
	);

	sdram #(
		.SDRAM_CLK_HZ(SDRAM_CLK_HZ)
	) ctl (
		.init(shared_reset),
		.clk(clk),
		.SDRAM_DQ(SDRAM_DQ),
		.SDRAM_A(SDRAM_A),
		.SDRAM_DQML(SDRAM_DQML),
		.SDRAM_DQMH(SDRAM_DQMH),
		.SDRAM_BA(SDRAM_BA),
		.SDRAM_nCS(SDRAM_nCS),
		.SDRAM_nWE(SDRAM_nWE),
		.SDRAM_nRAS(SDRAM_nRAS),
		.SDRAM_nCAS(SDRAM_nCAS),
		.SDRAM_CKE(SDRAM_CKE),
		.SDRAM_CLK(SDRAM_CLK),
		.SDRAM_EN(1'b1),
		.sel(sdram_sel),
		.addr(sdram_addr),
		.dout(sdram_dout),
		.din(sdram_din),
		.wr(sdram_wr),
		.bs(sdram_bs),
		.rd(sdram_rd),
		.ready(sdram_ready),
		.refresh(sdram_refresh),
		.cpsel(1'b0),
		.cpaddr(26'd0),
		.cpdin(16'd0),
		.cprd(),
		.cpreq(1'b0),
		.cpbusy()
	);

endmodule
