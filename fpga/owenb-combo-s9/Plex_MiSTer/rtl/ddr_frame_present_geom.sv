// Sim/static probe of ddr_frame_store present-window + bank math.
// Formulas MUST match ddr_frame_store.sv (PRESENT_END_*, rd_visible, BASE_W*,
// Y/C plane qwords). Not in product files.qip — Verilator TB only.
//
// Purpose: prove L4 720p layout selection yields full 1280x720 visibility and
// correct bank bounds without fitting the full DDR reader.

module ddr_frame_present_geom #(
	parameter int FRAME_W = 640,
	parameter int FRAME_H = 480,
	parameter int CODED_W = 624,
	parameter int CODED_H = 480,
	parameter int DISPLAY_W = 618,
	parameter int DISPLAY_H = 480,
	parameter int CROP_LEFT = 0,
	parameter int CROP_TOP = 0,
	parameter int PRESENT_X = 11,
	parameter int PRESENT_Y = 0,
	parameter [31:0] PHYS_BASE = 32'h3000_0000,
	parameter int HPS_BANK_STRIDE_BYTES = 32'h0008_0000,
	parameter [31:0] DOORBELL_PHYS = 32'h300F_F000
)(
	input  wire [$clog2(FRAME_W)-1:0] rd_x,
	input  wire [$clog2(FRAME_H)-1:0] rd_y,
	output wire                       rd_visible,
	output wire [$clog2(CODED_W)-1:0] src_x,
	output wire [$clog2(CODED_H)-1:0] src_y,
	output wire [31:0]                bank0_base_bytes,
	output wire [31:0]                bank1_base_bytes,
	output wire [31:0]                bank0_end_bytes,
	output wire [31:0]                frame_bytes,
	output wire [31:0]                y_plane_bytes,
	output wire [31:0]                doorbell_bytes,
	output wire [15:0]                present_end_x,
	output wire [15:0]                present_end_y,
	output wire [15:0]                y_line_qwords,
	output wire [15:0]                c_line_qwords
);
	localparam int X_W = $clog2(FRAME_W);
	localparam int Y_W = $clog2(FRAME_H);
	localparam int CODED_X_W = $clog2(CODED_W);
	localparam int CODED_Y_W = $clog2(CODED_H);

	// Match ddr_frame_store.sv:84-105
	localparam [X_W-1:0] PRESENT_X_L = X_W'(PRESENT_X);
	localparam [Y_W-1:0] PRESENT_Y_L = Y_W'(PRESENT_Y);
	localparam [X_W-1:0] PRESENT_END_X = X_W'(PRESENT_X + DISPLAY_W);
	localparam [Y_W-1:0] PRESENT_END_Y = Y_W'(PRESENT_Y + DISPLAY_H);
	localparam [CODED_X_W-1:0] CROP_LEFT_L = CODED_X_W'(CROP_LEFT);
	localparam [CODED_Y_W-1:0] CROP_TOP_L = CODED_Y_W'(CROP_TOP);

	localparam int Y_LINE_QWORDS = CODED_W / 8;
	localparam int C_LINE_QWORDS = CODED_W / 16;
	localparam int FRAME_BYTES_I = (CODED_W * CODED_H * 3) / 2;
	localparam int Y_PLANE_BYTES_I = CODED_W * CODED_H;

	wire rd_x_at_or_after_origin = (PRESENT_X == 0) ? 1'b1 : (rd_x >= PRESENT_X_L);
	wire rd_y_at_or_after_origin = (PRESENT_Y == 0) ? 1'b1 : (rd_y >= PRESENT_Y_L);
	wire rd_x_visible = rd_x_at_or_after_origin && (rd_x < PRESENT_END_X);
	wire rd_y_visible = rd_y_at_or_after_origin && (rd_y < PRESENT_END_Y);
	assign rd_visible = rd_x_visible && rd_y_visible;

	wire [X_W-1:0] display_x = rd_x - PRESENT_X_L;
	wire [Y_W-1:0] display_y = rd_y - PRESENT_Y_L;
	assign src_x = rd_visible ? (display_x + CROP_LEFT_L) : '0;
	assign src_y = rd_visible ? (display_y + CROP_TOP_L) : '0;

	assign bank0_base_bytes = PHYS_BASE;
	assign bank1_base_bytes = PHYS_BASE + HPS_BANK_STRIDE_BYTES;
	assign frame_bytes = 32'(FRAME_BYTES_I);
	assign bank0_end_bytes = PHYS_BASE + 32'(FRAME_BYTES_I);
	assign y_plane_bytes = 32'(Y_PLANE_BYTES_I);
	assign doorbell_bytes = DOORBELL_PHYS;
	assign present_end_x = 16'(PRESENT_X + DISPLAY_W);
	assign present_end_y = 16'(PRESENT_Y + DISPLAY_H);
	assign y_line_qwords = 16'(Y_LINE_QWORDS);
	assign c_line_qwords = 16'(C_LINE_QWORDS);
endmodule
