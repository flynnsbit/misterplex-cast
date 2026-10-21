// Beat conservation test for the m1 response FIFO in ddr_bus_arbiter.
// Varies CAS latency to exercise all clk_ddr phase alignments.
// MUST report 0 dropped beats to pass.
`timescale 1ns/1ps
module tb_arb_beat_conservation;
	// Same PLL phase: both clocks start high, first posedge at half-period
	reg clk_sys = 1, clk_ddr = 1, reset = 1;
	always #25    clk_sys = ~clk_sys;  // 20 MHz
	always #5.556 clk_ddr = ~clk_ddr;  // ~90 MHz

	wire        m1_busy;
	reg   [7:0] m1_burstcnt;
	reg  [28:0] m1_addr;
	wire [63:0] m1_dout;
	wire        m1_dout_ready;
	reg         m1_rd = 0;
	reg  [63:0] m1_din = 0;
	reg   [7:0] m1_be = 8'hFF;
	reg         m1_we = 0;

	wire m0_busy;
	wire [63:0] m0_dout;
	wire m0_dout_ready;

	reg         ddr_busy = 0;
	reg  [63:0] ddr_dout = 0;
	reg         ddr_dout_ready = 0;
	wire  [7:0] ddr_burstcnt;
	wire [28:0] ddr_addr;
	wire        ddr_rd;
	wire [63:0] ddr_din;
	wire  [7:0] ddr_be;
	wire        ddr_we;

	ddr_bus_arbiter arb (
		.clk(clk_ddr), .clk_m1(clk_sys), .reset(reset),
		.m1_want(1'b1),
		.m0_busy(m0_busy),
		.m0_burstcnt(8'd1), .m0_addr(29'd0),
		.m0_dout(m0_dout), .m0_dout_ready(m0_dout_ready),
		.m0_rd(1'b0), .m0_din(64'd0), .m0_be(8'hFF), .m0_we(1'b0),
		.m1_busy(m1_busy),
		.m1_burstcnt(m1_burstcnt), .m1_addr(m1_addr),
		.m1_dout(m1_dout), .m1_dout_ready(m1_dout_ready),
		.m1_rd(m1_rd), .m1_din(m1_din), .m1_be(m1_be), .m1_we(m1_we),
		.DDRAM_BUSY(ddr_busy),
		.DDRAM_BURSTCNT(ddr_burstcnt),
		.DDRAM_ADDR(ddr_addr),
		.DDRAM_DOUT(ddr_dout),
		.DDRAM_DOUT_READY(ddr_dout_ready),
		.DDRAM_RD(ddr_rd),
		.DDRAM_DIN(ddr_din),
		.DDRAM_BE(ddr_be),
		.DDRAM_WE(ddr_we)
	);

	// --- Beat counters ---
	// Write-side counter: beats pushed into FIFO (clk_ddr domain)
	integer wr_beats = 0;
	always @(posedge clk_ddr) begin
		if (arb.m1_rsp_wr_en)
			wr_beats = wr_beats + 1;
	end

	// Read-side counter: beats delivered to consumer (clk_sys domain)
	integer rd_beats = 0;
	always @(posedge clk_sys) begin
		if (!reset && m1_dout_ready)
			rd_beats = rd_beats + 1;
	end

	// Data integrity: check each delivered beat matches expected
	integer data_errors = 0;
	reg [63:0] expected_data;
	always @(posedge clk_sys) begin
		if (!reset && m1_dout_ready) begin
			if (m1_dout !== expected_data) begin
				$display("DATA ERROR at %0t: got %h, expected %h",
					$time, m1_dout, expected_data);
				data_errors = data_errors + 1;
			end
		end
	end

	// DDR CAS model with configurable latency
	reg [3:0] cas_latency = 4'd6;
	reg [3:0] cas_pipe = 0;
	reg [63:0] cas_data;
	always @(posedge clk_ddr) begin
		ddr_dout_ready <= 1'b0;
		if (!reset) begin
			if (ddr_rd && !ddr_busy) begin
				cas_pipe <= cas_latency;
				cas_data <= {ddr_addr[28:0], 3'b000, 32'hCAFE_0000};
			end
			if (cas_pipe != 0) begin
				cas_pipe <= cas_pipe - 4'd1;
				if (cas_pipe == 4'd1) begin
					ddr_dout_ready <= 1'b1;
					ddr_dout <= cas_data;
				end
			end
		end
	end

	integer ri, wr_before, rd_before, any_drop, total_drops;
	initial begin
		any_drop = 0;
		total_drops = 0;
		m1_burstcnt = 8'd1;
		m1_addr = 29'h100;

		repeat (20) @(posedge clk_sys);
		reset = 0;
		repeat (10) @(posedge clk_sys);

		$display("=== BEAT CONSERVATION TEST (WITH RESPONSE FIFO) ===");
		$display("CAS  WrBeats  RdBeats  Status");

		for (ri = 3; ri <= 12; ri = ri + 1) begin
			cas_latency = ri[3:0];
			wr_before = wr_beats;
			rd_before = rd_beats;

			// Set expected data to match CAS model output
			expected_data = {29'(ri * 8), 3'b000, 32'hCAFE_0000};

			while (m1_busy) @(posedge clk_sys);
			@(posedge clk_sys);
			m1_rd = 1;
			m1_addr = 29'(ri * 8);
			@(posedge clk_sys);
			m1_rd = 0;

			// Wait for FIFO propagation: CAS + 2-FF sync + margin
			repeat (40) @(posedge clk_sys);

			if ((wr_beats - wr_before) != (rd_beats - rd_before)) begin
				$display("  %2d     %0d        %0d       DROPPED", ri,
					wr_beats - wr_before, rd_beats - rd_before);
				any_drop = 1;
				total_drops = total_drops + 1;
			end else begin
				$display("  %2d     %0d        %0d       ok", ri,
					wr_beats - wr_before, rd_beats - rd_before);
			end
		end

		$display("");
		$display("Total: wr_beats=%0d rd_beats=%0d drops=%0d data_errors=%0d",
			wr_beats, rd_beats, total_drops, data_errors);
		if (any_drop || data_errors)
			$display("FAIL: beats dropped or data corrupt");
		else
			$display("PASS: all beats conserved, data intact");
		$finish;
	end

	initial begin
		#2000000;
		$display("TIMEOUT");
		$finish;
	end
endmodule
