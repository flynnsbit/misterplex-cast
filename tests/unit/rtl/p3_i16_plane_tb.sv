// Testbench wrapper for Intra 16x16 Plane prediction verification.
// Exposes h264_intra16x16_pred to the C++ Verilator harness.
module p3_i16_plane_tb (
	input  wire        clk,
	input  wire        start,
	input  wire [1:0]  mode,
	input  wire [7:0]  above [0:15],
	input  wire [7:0]  left [0:15],
	input  wire [7:0]  top_left,
	input  wire        has_above,
	input  wire        has_left,
	output wire        unsupported,
	output wire        valid,
	output wire [7:0]  pred [0:255]
);
	h264_intra16x16_pred uut (
		.clk(clk),
		.start(start),
		.mode(mode),
		.above(above),
		.left(left),
		.top_left(top_left),
		.has_above(has_above),
		.has_left(has_left),
		.unsupported(unsupported),
		.valid(valid),
		.pred(pred)
	);
endmodule
