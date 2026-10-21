// Testbench wrapper for Chroma 8x8 Plane prediction verification.
// Exposes h264_chroma8x8_pred to the C++ Verilator harness.
module p3_chroma_plane_tb (
	input  wire        clk,
	input  wire        start,
	input  wire [1:0]  mode,
	input  wire [7:0]  above [0:7],
	input  wire [7:0]  left [0:7],
	input  wire [7:0]  top_left,
	input  wire        has_above,
	input  wire        has_left,
	output wire        valid,
	output wire [7:0]  pred [0:63]
);
	h264_chroma8x8_pred uut (
		.clk(clk),
		.start(start),
		.mode(mode),
		.above(above),
		.left(left),
		.top_left(top_left),
		.has_above(has_above),
		.has_left(has_left),
		.valid(valid),
		.pred(pred)
	);
endmodule
