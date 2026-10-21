// Beat conservation test: verify m1_dout_ready pulses from a clk_ddr-domain
// arbiter are reliably seen by a clk_sys-domain consumer.
//
// clk_ddr = 90 MHz (11.1 ns period), clk_sys = 20 MHz (50 ns period).
// Same PLL, 0 ps phase offset — synchronous but 4.5:1 ratio.
`default_nettype none

module ddr_beat_conservation_tb #(
	parameter int NUM_READS = 100
)(
	input  wire clk_ddr,
	input  wire clk_sys,
	input  wire reset,

	// Producer-side counters (clk_ddr domain)
	output reg [31:0] ddr_beats_issued,
	output reg [31:0] ddr_reads_sent,

	// Consumer-side counters (clk_sys domain)
	output reg [31:0] sys_beats_seen,
	output reg [31:0] sys_reads_done,

	// Arbiter m1 interface (clk_ddr domain output, clk_sys domain consumer)
	output wire        m1_dout_ready,
	output wire [63:0] m1_dout,

	output reg         test_done,
	output reg         test_pass
);

	// --- Simulated DDR bridge on clk_ddr ---
	// Mimics sysmem_lite: DDRAM_DOUT_READY is 1-cycle pulse per beat
	reg        DDRAM_DOUT_READY;
	reg [63:0] DDRAM_DOUT;
	reg        DDRAM_BUSY;
	reg        DDRAM_RD;
	reg [7:0]  DDRAM_BURSTCNT;

	// Simulated arbiter state (clk_ddr domain, matching w-a3's fix)
	reg        rsp_owner_m1;
	reg [8:0]  rsp_left;
	wire       rsp_active = rsp_left != 9'd0;

	// m1_dout_ready is COMBINATIONAL — exactly as in ddr_bus_arbiter.sv
	assign m1_dout_ready = DDRAM_DOUT_READY & rsp_active & rsp_owner_m1;
	assign m1_dout = DDRAM_DOUT;

	// DDR bridge model: after a read, respond with 1-cycle DOUT_READY after 3-cycle latency
	reg [3:0] rsp_delay;
	reg [31:0] rsp_data_seq;
	reg pending_rsp;

	// Request generation state machine
	reg [2:0] req_state;
	localparam [2:0] REQ_IDLE = 3'd0;
	localparam [2:0] REQ_ISSUE = 3'd1;
	localparam [2:0] REQ_WAIT_RSP = 3'd2;
	localparam [2:0] REQ_GAP = 3'd3;
	localparam [2:0] REQ_DONE = 3'd4;
	reg [3:0] gap_cnt;

	always @(posedge clk_ddr) begin
		DDRAM_DOUT_READY <= 1'b0;

		if (reset) begin
			DDRAM_BUSY <= 1'b0;
			DDRAM_DOUT_READY <= 1'b0;
			DDRAM_DOUT <= 64'd0;
			DDRAM_RD <= 1'b0;
			DDRAM_BURSTCNT <= 8'd1;
			rsp_owner_m1 <= 1'b0;
			rsp_left <= 9'd0;
			rsp_delay <= 4'd0;
			rsp_data_seq <= 32'd0;
			pending_rsp <= 1'b0;
			ddr_beats_issued <= 32'd0;
			ddr_reads_sent <= 32'd0;
			req_state <= REQ_IDLE;
			gap_cnt <= 4'd0;
		end else begin
			DDRAM_RD <= 1'b0;

			// Response pipeline: after read, 3-cycle latency then 1-cycle pulse
			if (pending_rsp) begin
				if (rsp_delay == 4'd0) begin
					DDRAM_DOUT_READY <= 1'b1;
					DDRAM_DOUT <= {32'hDEAD0000 | rsp_data_seq, 32'hBEEF0000 | rsp_data_seq};
					rsp_data_seq <= rsp_data_seq + 32'd1;
					ddr_beats_issued <= ddr_beats_issued + 32'd1;
					pending_rsp <= 1'b0;
				end else begin
					rsp_delay <= rsp_delay - 4'd1;
				end
			end

			// Arbiter rsp_left tracking (clk_ddr domain)
			if (DDRAM_DOUT_READY && rsp_active)
				rsp_left <= rsp_left - 9'd1;

			// Request state machine — issue reads with varying gaps
			case (req_state)
			REQ_IDLE: begin
				if (!reset)
					req_state <= REQ_ISSUE;
			end
			REQ_ISSUE: begin
				if (ddr_reads_sent < NUM_READS[31:0] && !DDRAM_BUSY && !rsp_active) begin
					DDRAM_RD <= 1'b1;
					rsp_owner_m1 <= 1'b1;
					rsp_left <= 9'd1; // burstcnt=1
					pending_rsp <= 1'b1;
					rsp_delay <= 4'd2; // 3-cycle latency
					ddr_reads_sent <= ddr_reads_sent + 32'd1;
					req_state <= REQ_WAIT_RSP;
				end
			end
			REQ_WAIT_RSP: begin
				if (!rsp_active) begin
					// Vary inter-read gap: 0, 1, 2, ... cycles (mod 5)
					gap_cnt <= ddr_reads_sent[3:0] % 4'd5;
					req_state <= REQ_GAP;
				end
			end
			REQ_GAP: begin
				if (gap_cnt == 4'd0) begin
					if (ddr_reads_sent < NUM_READS[31:0])
						req_state <= REQ_ISSUE;
					else
						req_state <= REQ_DONE;
				end else begin
					gap_cnt <= gap_cnt - 4'd1;
				end
			end
			REQ_DONE: begin
				// Stay here
			end
			endcase
		end
	end

	// --- Consumer on clk_sys (mimics ddr_bitstream_reader) ---
	// Samples m1_dout_ready on clk_sys posedge — exactly as stream_path does
	always @(posedge clk_sys) begin
		if (reset) begin
			sys_beats_seen <= 32'd0;
			sys_reads_done <= 32'd0;
			test_done <= 1'b0;
			test_pass <= 1'b0;
		end else begin
			if (m1_dout_ready) begin
				sys_beats_seen <= sys_beats_seen + 32'd1;
			end
			// Check for completion
			if (ddr_reads_sent == NUM_READS[31:0] && !rsp_active && !pending_rsp && !test_done) begin
				// Allow a few more clk_sys cycles for any in-flight samples
				sys_reads_done <= sys_reads_done + 32'd1;
				if (sys_reads_done >= 32'd10) begin
					test_done <= 1'b1;
					test_pass <= (sys_beats_seen == ddr_beats_issued);
				end
			end
		end
	end

endmodule

`default_nettype wire
