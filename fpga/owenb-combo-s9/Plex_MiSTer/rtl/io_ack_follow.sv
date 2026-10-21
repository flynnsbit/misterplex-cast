// io_ack_follow — extracted HPS SPI/UIO ACK follower from sys_top.v.
//
// Named defect (L30 / Sweep 115): host capped ACK wait at 2 ms wall and aborted
// mid-transaction → silent partial SPI writes → garbage HDMI. Product ACK is
// NOT millisecond-scale. In sys_top the follower is:
//
//   wire io_strobe = ~rack & io_clk;
//   always @(posedge clk_sys)
//     if (~(io_wait | vs_wait) | io_strobe) begin
//       rack   <= io_clk;
//       io_ack <= rack;
//     end
//
// So with wait=0, io_ack rises one clk_sys after rack samples io_clk=1 —
// typically ≤2 clk_sys cycles after HPS raises io_clk. At 50 MHz that is 40 ns.
// A 2 ms host wall is ~50_000× longer than the legitimate product path and is
// the wrong tool for CPU savings (use yield + poll budget, never short abort).
//
// This module owns that contract in fabric so it can be simulated without
// elaborating full sys_top, and so a FAULT twin (stuck wait) is a named RED.

`timescale 1ns / 1ps

module io_ack_follow #(
	// FAULT_STUCK_WAIT=1: never clears wait — io_ack stays 0 (RED twin).
	parameter [0:0] FAULT_STUCK_WAIT = 1'b0
) (
	input  wire clk_sys,
	input  wire reset,
	input  wire io_clk,     // HPS-driven SPI/UIO bit clock (gp_out)
	input  wire io_wait,    // core backpressure
	input  wire vs_wait,    // optional vsync gate (sys_top)
	output reg  io_ack,
	output reg  rack,
	output wire io_strobe,
	// Cycles from reset-release while io_clk held 1 until io_ack==1 (0 if never).
	output reg  [15:0] ack_latency_cycles,
	output reg         ack_seen
);

	wire wait_eff = FAULT_STUCK_WAIT ? 1'b1 : (io_wait | vs_wait);
	assign io_strobe = ~rack & io_clk;

	reg        counting;
	reg [15:0] cnt;

	always @(posedge clk_sys) begin
		if (reset) begin
			rack               <= 1'b0;
			io_ack             <= 1'b0;
			ack_latency_cycles <= 16'd0;
			ack_seen           <= 1'b0;
			counting           <= 1'b0;
			cnt                <= 16'd0;
		end else begin
			// Exact sys_top follower (sys_top.v ~256-262).
			if (~wait_eff | io_strobe) begin
				rack   <= io_clk;
				io_ack <= rack;
			end

			// Latency measure: start when HPS raises io_clk with ack clear.
			if (!ack_seen) begin
				if (io_clk && !io_ack && !counting) begin
					counting <= 1'b1;
					cnt      <= 16'd1;
				end else if (counting) begin
					if (io_ack) begin
						ack_seen           <= 1'b1;
						ack_latency_cycles <= cnt;
						counting           <= 1'b0;
					end else if (cnt != 16'hffff) begin
						cnt <= cnt + 16'd1;
					end
				end
			end
		end
	end

endmodule
