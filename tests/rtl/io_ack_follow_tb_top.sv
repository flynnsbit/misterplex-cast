// Top for io_ack_follow Verilator TB — product path + FAULT_STUCK_WAIT twin.
`timescale 1ns / 1ps

module io_ack_follow_tb_top (
	input  wire        clk_sys,
	input  wire        reset,
	input  wire        io_clk,
	input  wire        io_wait,
	input  wire        vs_wait,
	output wire        io_ack_good,
	output wire        rack_good,
	output wire        io_strobe_good,
	output wire [15:0] lat_good,
	output wire        seen_good,
	output wire        io_ack_fault,
	output wire [15:0] lat_fault,
	output wire        seen_fault
);

	io_ack_follow #(.FAULT_STUCK_WAIT(1'b0)) u_good (
		.clk_sys(clk_sys),
		.reset(reset),
		.io_clk(io_clk),
		.io_wait(io_wait),
		.vs_wait(vs_wait),
		.io_ack(io_ack_good),
		.rack(rack_good),
		.io_strobe(io_strobe_good),
		.ack_latency_cycles(lat_good),
		.ack_seen(seen_good)
	);

	io_ack_follow #(.FAULT_STUCK_WAIT(1'b1)) u_fault (
		.clk_sys(clk_sys),
		.reset(reset),
		.io_clk(io_clk),
		.io_wait(io_wait),
		.vs_wait(vs_wait),
		.io_ack(io_ack_fault),
		.rack(),
		.io_strobe(),
		.ack_latency_cycles(lat_fault),
		.ack_seen(seen_fault)
	);

endmodule
