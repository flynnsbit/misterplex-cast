// Sweep DDR response latency 1-9 cycles to find fragile beat-conservation cases.
// This proves that whether beats are seen depends on WHICH clk_ddr edge the
// response lands on — making the design position-dependent and fragile.
`default_nettype none

module ddr_beat_sweep_tb #(
	parameter int DDR_LATENCY = 3,
	parameter int NUM_READS = 20
)(
	input  wire clk_ddr,
	input  wire clk_sys,
	input  wire reset,

	output reg [31:0] ddr_beats_issued,
	output reg [31:0] sys_beats_seen,
	output reg        test_done,
	output reg        test_pass,
	output reg        test_deadlocked
);

	wire        DDRAM_BUSY = 1'b0;
	reg         DDRAM_DOUT_READY;
	reg  [63:0] DDRAM_DOUT;
	wire  [7:0] DDRAM_BURSTCNT;
	wire [28:0] DDRAM_ADDR;
	wire        DDRAM_RD;
	wire [63:0] DDRAM_DIN;
	wire  [7:0] DDRAM_BE;
	wire        DDRAM_WE;

	// DDR response model: configurable latency pipeline
	reg [15:0] rsp_pipe;
	reg [31:0] ddr_seq;
	wire [15:0] rsp_pipe_shifted = {1'b0, rsp_pipe[15:1]};

	always @(posedge clk_ddr) begin
		DDRAM_DOUT_READY <= 1'b0;
		if (reset) begin
			rsp_pipe <= 16'd0;
			ddr_seq <= 32'd0;
			DDRAM_DOUT <= 64'd0;
			ddr_beats_issued <= 32'd0;
		end else begin
			if (DDRAM_RD)
				rsp_pipe <= rsp_pipe_shifted | (16'd1 << (DDR_LATENCY[3:0] - 4'd1));
			else
				rsp_pipe <= rsp_pipe_shifted;
			if (rsp_pipe[0]) begin
				DDRAM_DOUT_READY <= 1'b1;
				DDRAM_DOUT <= {32'hCAFE0000 | ddr_seq, 32'hFACE0000 | ddr_seq};
				ddr_seq <= ddr_seq + 32'd1;
				ddr_beats_issued <= ddr_beats_issued + 32'd1;
			end
		end
	end

	// Arbiter on clk_ddr (w-a3's fix)
	wire        m1_busy;
	wire [63:0] m1_dout;
	wire        m1_dout_ready;
	reg         m1_rd;
	reg  [28:0] m1_addr;

	ddr_bus_arbiter arb (
		.clk(clk_ddr),
		.reset(reset),
		.m1_want(1'b1),
		.m0_busy(), .m0_burstcnt(8'd1), .m0_addr(29'd0), .m0_dout(),
		.m0_dout_ready(), .m0_rd(1'b0), .m0_din(64'd0), .m0_be(8'd0), .m0_we(1'b0),
		.m1_busy(m1_busy), .m1_burstcnt(8'd1), .m1_addr(m1_addr),
		.m1_dout(m1_dout), .m1_dout_ready(m1_dout_ready),
		.m1_rd(m1_rd), .m1_din(64'd0), .m1_be(8'd0), .m1_we(1'b0),
		.DDRAM_BUSY(DDRAM_BUSY), .DDRAM_BURSTCNT(DDRAM_BURSTCNT),
		.DDRAM_ADDR(DDRAM_ADDR), .DDRAM_DOUT(DDRAM_DOUT),
		.DDRAM_DOUT_READY(DDRAM_DOUT_READY), .DDRAM_RD(DDRAM_RD),
		.DDRAM_DIN(DDRAM_DIN), .DDRAM_BE(DDRAM_BE), .DDRAM_WE(DDRAM_WE)
	);

	// Consumer on clk_sys
	reg [31:0] reads_sent;
	reg [1:0]  m1_state;
	localparam [1:0] S_IDLE = 2'd0, S_ISSUE = 2'd1, S_WAIT = 2'd2, S_DONE = 2'd3;
	reg [31:0] wait_cnt;
	reg [31:0] done_wait;

	always @(posedge clk_sys) begin
		m1_rd <= 1'b0;
		if (reset) begin
			reads_sent <= 32'd0;
			m1_state <= S_IDLE;
			m1_addr <= 29'd0;
			sys_beats_seen <= 32'd0;
			test_done <= 1'b0;
			test_pass <= 1'b0;
			test_deadlocked <= 1'b0;
			wait_cnt <= 32'd0;
			done_wait <= 32'd0;
		end else begin
			case (m1_state)
			S_IDLE: m1_state <= S_ISSUE;
			S_ISSUE: begin
				if (!m1_busy && reads_sent < NUM_READS[31:0]) begin
					m1_rd <= 1'b1;
					m1_addr <= 29'(reads_sent);
					reads_sent <= reads_sent + 32'd1;
					m1_state <= S_WAIT;
					wait_cnt <= 32'd0;
				end
			end
			S_WAIT: begin
				wait_cnt <= wait_cnt + 32'd1;
				if (m1_dout_ready) begin
					sys_beats_seen <= sys_beats_seen + 32'd1;
					m1_state <= (reads_sent < NUM_READS[31:0]) ? S_ISSUE : S_DONE;
				end
				// Deadlock detection: 500 clk_sys cycles = 25 us, way longer than any DDR response
				if (wait_cnt > 32'd500) begin
					test_done <= 1'b1;
					test_pass <= 1'b0;
					test_deadlocked <= 1'b1;
				end
			end
			S_DONE: begin
				done_wait <= done_wait + 32'd1;
				if (done_wait > 32'd10) begin
					test_done <= 1'b1;
					test_pass <= (sys_beats_seen == NUM_READS[31:0]);
				end
			end
			endcase
		end
	end
endmodule

`default_nettype wire
