// Simulation-only harness for the real SDRAM controller + real memtest + a
// minimal external SDRAM read-path model.

module sdram_dq_turnaround_top #(
	parameter int unsigned SDRAM_CLK_HZ = 100_000_000,
	parameter bit DEVICE_DRIVES = 1'b1
)(
	input  wire clk,
	input  wire reset,
	input  wire pll_locked,

	output wire SDRAM_nCS,
	output wire SDRAM_nRAS,
	output wire SDRAM_nCAS,
	output wire SDRAM_nWE,
	output wire SDRAM_CKE,
	output wire [12:0] SDRAM_A,
	output wire [1:0]  SDRAM_BA,
	output wire SDRAM_DQML,
	output wire SDRAM_DQMH,
	output wire [15:0] dq_bus,

	output wire sdram_ready,
	output wire sdram_sel,
	output wire sdram_rd,
	output wire sdram_wr,
	output wire [15:0] sdram_dout,
	output wire [3:0] memtest_state,
	output wire [3:0] memtest_size,
	output wire [15:0] memtest_errors,
	output wire [15:0] memtest_read_sample,
	output wire        first_fail_valid,
	output wire [25:0] first_fail_addr,
	output wire [15:0] first_fail_expect,
	output wire        memtest_done,

	output wire        device_drive,
	output wire [15:0] device_drive_data,
	output wire [2:0]  device_cas_latency,
	output wire [3:0]  device_burst_len,
	output wire [31:0] device_read_count,
	output wire        ctl_dq_drive
);
	localparam longint unsigned REFRESH_CYCLES_CALC = ((longint'(SDRAM_CLK_HZ) * 64_000) / (8192 * 1_000_000)) - 1;
	localparam int SDRAM_REFRESH_CYCLES = REFRESH_CYCLES_CALC[31:0];

	wire [15:0] SDRAM_DQ;
	wire [26:1] sdram_addr;
	wire [15:0] sdram_din;
	wire [1:0]  sdram_bs;
	wire        sdram_refresh;
	wire        memtest_pass;
	wire        shared_reset = reset | ~pll_locked;
	wire        sdram_clk_unused;

	assign dq_bus = device_drive ? device_drive_data : (DEVICE_DRIVES ? SDRAM_DQ : 16'hffff);
	assign ctl_dq_drive = (!SDRAM_nCS && {SDRAM_nRAS, SDRAM_nCAS, SDRAM_nWE} == 3'b100);

	always @* begin
		if (device_drive) begin
			force ctl.SDRAM_DQ = device_drive_data;
		end else if (!DEVICE_DRIVES) begin
			force ctl.SDRAM_DQ = 16'hffff;
		end else begin
			release ctl.SDRAM_DQ;
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
		.read_sample(memtest_read_sample),
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
		.SDRAM_CLK(sdram_clk_unused),
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

	sdram_read_model #(
		.DEVICE_DRIVES(DEVICE_DRIVES)
	) dram (
		.clk(clk),
		.cke(SDRAM_CKE),
		.nCS(SDRAM_nCS),
		.nRAS(SDRAM_nRAS),
		.nCAS(SDRAM_nCAS),
		.nWE(SDRAM_nWE),
		.A(SDRAM_A),
		.BA(SDRAM_BA),
		.dq_drive(device_drive),
		.dq_value(device_drive_data),
		.cas_latency(device_cas_latency),
		.burst_len(device_burst_len),
		.read_count(device_read_count)
	);

endmodule
