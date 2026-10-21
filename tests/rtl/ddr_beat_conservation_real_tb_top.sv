// Beat conservation test using the ACTUAL ddr_bus_arbiter module.
// Tests both clk_ddr and clk_sys arbiter configurations.
// Simulates a DDR bridge that issues 1-cycle DOUT_READY pulses on clk_ddr.
`default_nettype none

module ddr_beat_conservation_real_tb #(
	parameter int NUM_READS = 50,
	parameter bit ARBITER_ON_DDR = 1'b1
)(
	input  wire clk_ddr,
	input  wire clk_sys,
	input  wire reset,

	output reg [31:0] ddr_beats_issued,
	output reg [31:0] sys_beats_seen,
	output reg [31:0] arb_rsp_left_stuck_cycles,
	output reg        test_done,
	output reg        test_pass
);

	// --- DDR bridge model (always clk_ddr) ---
	wire        DDRAM_BUSY;
	reg         DDRAM_DOUT_READY;
	reg  [63:0] DDRAM_DOUT;
	wire  [7:0] DDRAM_BURSTCNT;
	wire [28:0] DDRAM_ADDR;
	wire        DDRAM_RD;
	wire [63:0] DDRAM_DIN;
	wire  [7:0] DDRAM_BE;
	wire        DDRAM_WE;
	assign DDRAM_BUSY = 1'b0;

	// Latency pipeline for DDR responses
	reg [2:0] rsp_pipe;
	reg [31:0] ddr_seq;

	always @(posedge clk_ddr) begin
		DDRAM_DOUT_READY <= 1'b0;
		if (reset) begin
			rsp_pipe <= 3'd0;
			ddr_seq <= 32'd0;
			DDRAM_DOUT <= 64'd0;
			ddr_beats_issued <= 32'd0;
		end else begin
			// Pipeline: shift right. Bit 0 fires response.
			if (DDRAM_RD) begin
				rsp_pipe <= {rsp_pipe[1:0], 1'b0} | 3'b100; // 3-cycle latency
			end else begin
				rsp_pipe <= {1'b0, rsp_pipe[2:1]};
			end
			if (rsp_pipe[0]) begin
				DDRAM_DOUT_READY <= 1'b1;
				DDRAM_DOUT <= {32'hCAFE0000 | ddr_seq, 32'hFACE0000 | ddr_seq};
				ddr_seq <= ddr_seq + 32'd1;
				ddr_beats_issued <= ddr_beats_issued + 32'd1;
			end
		end
	end

	// --- Arbiter (actual module) ---
	wire arb_clk = ARBITER_ON_DDR ? clk_ddr : clk_sys;

	wire        m1_busy;
	wire [63:0] m1_dout;
	wire        m1_dout_ready;
	reg         m1_rd;
	reg  [28:0] m1_addr;

	ddr_bus_arbiter arb (
		.clk(arb_clk),
		.reset(reset),
		.m1_want(1'b1),

		.m0_busy(),
		.m0_burstcnt(8'd1),
		.m0_addr(29'd0),
		.m0_dout(),
		.m0_dout_ready(),
		.m0_rd(1'b0),
		.m0_din(64'd0),
		.m0_be(8'd0),
		.m0_we(1'b0),

		.m1_busy(m1_busy),
		.m1_burstcnt(8'd1),
		.m1_addr(m1_addr),
		.m1_dout(m1_dout),
		.m1_dout_ready(m1_dout_ready),
		.m1_rd(m1_rd),
		.m1_din(64'd0),
		.m1_be(8'd0),
		.m1_we(1'b0),

		.DDRAM_BUSY(DDRAM_BUSY),
		.DDRAM_BURSTCNT(DDRAM_BURSTCNT),
		.DDRAM_ADDR(DDRAM_ADDR),
		.DDRAM_DOUT(DDRAM_DOUT),
		.DDRAM_DOUT_READY(DDRAM_DOUT_READY),
		.DDRAM_RD(DDRAM_RD),
		.DDRAM_DIN(DDRAM_DIN),
		.DDRAM_BE(DDRAM_BE),
		.DDRAM_WE(DDRAM_WE)
	);

	// --- Master 1 request generator (clk_sys, like ddr_bitstream_reader) ---
	reg [31:0] reads_sent;
	reg [2:0]  m1_state;
	localparam [2:0] M1_IDLE = 3'd0, M1_WAIT_GRANT = 3'd1, M1_ISSUE = 3'd2,
	                 M1_WAIT_RSP = 3'd3, M1_DONE = 3'd4;
	reg [3:0] m1_gap;

	always @(posedge clk_sys) begin
		m1_rd <= 1'b0;
		if (reset) begin
			reads_sent <= 32'd0;
			m1_state <= M1_IDLE;
			m1_addr <= 29'd0;
			m1_gap <= 4'd0;
		end else begin
			case (m1_state)
			M1_IDLE: m1_state <= M1_WAIT_GRANT;
			M1_WAIT_GRANT: begin
				if (!m1_busy && reads_sent < NUM_READS[31:0]) begin
					m1_rd <= 1'b1;
					m1_addr <= 29'(reads_sent);
					reads_sent <= reads_sent + 32'd1;
					m1_state <= M1_WAIT_RSP;
				end
			end
			M1_WAIT_RSP: begin
				if (m1_dout_ready) begin
					m1_gap <= reads_sent[3:0] % 4'd3;
					m1_state <= (reads_sent < NUM_READS[31:0]) ? M1_WAIT_GRANT : M1_DONE;
				end
			end
			M1_DONE: ;
			default: ;
			endcase
		end
	end

	// --- Consumer beat counter (clk_sys) ---
	reg [31:0] wait_timeout;
	always @(posedge clk_sys) begin
		if (reset) begin
			sys_beats_seen <= 32'd0;
			arb_rsp_left_stuck_cycles <= 32'd0;
			test_done <= 1'b0;
			test_pass <= 1'b0;
			wait_timeout <= 32'd0;
		end else begin
			if (m1_dout_ready)
				sys_beats_seen <= sys_beats_seen + 32'd1;

			// Detect arbiter stuck (rsp_left not clearing)
			if (m1_busy && m1_state == M1_WAIT_RSP)
				arb_rsp_left_stuck_cycles <= arb_rsp_left_stuck_cycles + 32'd1;

			if (m1_state == M1_DONE && !test_done) begin
				wait_timeout <= wait_timeout + 32'd1;
				if (wait_timeout > 32'd100) begin
					test_done <= 1'b1;
					test_pass <= (sys_beats_seen == ddr_beats_issued) &&
					             (sys_beats_seen == NUM_READS[31:0]);
				end
			end
			// Timeout if stuck waiting for response
			if (m1_state == M1_WAIT_RSP && arb_rsp_left_stuck_cycles > 32'd50000) begin
				test_done <= 1'b1;
				test_pass <= 1'b0;
			end
		end
	end
endmodule

`default_nettype wire
