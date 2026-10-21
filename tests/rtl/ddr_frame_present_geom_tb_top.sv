// Dual-geometry present-window probe:
//   u480 — product 480p layout (default-off L4 path must stay bit-identical)
//   u720 — L4 720p layout from ddr_frame_layout_params.svh
//   u240 — 320x240 presented with identity coded/display (working glass path dims)
`include "ddr_frame_layout_params.svh"

module ddr_frame_present_geom_tb_top (
	input  wire [10:0] rd_x,
	input  wire [10:0] rd_y,
	// 480p product
	output wire        v480,
	output wire [15:0] s480_x,
	output wire [15:0] s480_y,
	output wire [31:0] b480_0,
	output wire [31:0] b480_1,
	output wire [31:0] b480_end,
	output wire [31:0] f480,
	output wire [31:0] d480,
	output wire [15:0] pe480_x,
	output wire [15:0] pe480_y,
	output wire [15:0] yq480,
	output wire [15:0] cq480,
	// 720p L4
	output wire        v720,
	output wire [15:0] s720_x,
	output wire [15:0] s720_y,
	output wire [31:0] b720_0,
	output wire [31:0] b720_1,
	output wire [31:0] b720_end,
	output wire [31:0] f720,
	output wire [31:0] d720,
	output wire [15:0] pe720_x,
	output wire [15:0] pe720_y,
	output wire [15:0] yq720,
	output wire [15:0] cq720,
	// 320x240 identity (glass-working dims)
	output wire        v240,
	output wire [15:0] pe240_x,
	output wire [15:0] pe240_y,
	output wire [31:0] f240,
	output wire [31:0] b240_end
);
	wire [9:0]  x480 = rd_x[9:0];
	wire [8:0]  y480 = rd_y[8:0];
	wire [10:0] x720 = rd_x;
	wire [9:0]  y720 = rd_y[9:0];
	wire [8:0]  x240 = rd_x[8:0];
	wire [7:0]  y240 = rd_y[7:0];

	wire [$clog2(624)-1:0] srcx480;
	wire [$clog2(480)-1:0] srcy480;
	wire [$clog2(1280)-1:0] srcx720;
	wire [$clog2(720)-1:0] srcy720;
	wire [$clog2(320)-1:0] srcx240;
	wire [$clog2(240)-1:0] srcy240;

	ddr_frame_present_geom #(
		.FRAME_W(640), .FRAME_H(480),
		.CODED_W(DDR_FRAME_CODED_WIDTH),
		.CODED_H(DDR_FRAME_CODED_HEIGHT),
		.DISPLAY_W(DDR_FRAME_DISPLAY_WIDTH),
		.DISPLAY_H(DDR_FRAME_DISPLAY_HEIGHT),
		.CROP_LEFT(DDR_FRAME_CROP_LEFT),
		.CROP_TOP(DDR_FRAME_CROP_TOP),
		.PRESENT_X(DDR_FRAME_PILLARBOX_LEFT),
		.PRESENT_Y(0),
		.PHYS_BASE(32'h3000_0000),
		.HPS_BANK_STRIDE_BYTES(DDR_FRAME_YUV420P_BANK_STRIDE),
		.DOORBELL_PHYS(DDR_FRAME_YUV420P_DOORBELL_PHYS)
	) u480 (
		.rd_x(x480), .rd_y(y480),
		.rd_visible(v480), .src_x(srcx480), .src_y(srcy480),
		.bank0_base_bytes(b480_0), .bank1_base_bytes(b480_1),
		.bank0_end_bytes(b480_end), .frame_bytes(f480),
		.y_plane_bytes(), .doorbell_bytes(d480),
		.present_end_x(pe480_x), .present_end_y(pe480_y),
		.y_line_qwords(yq480), .c_line_qwords(cq480)
	);
	assign s480_x = 16'(srcx480);
	assign s480_y = 16'(srcy480);

	ddr_frame_present_geom #(
		.FRAME_W(1280), .FRAME_H(720),
		.CODED_W(DDR_FRAME_720P_CODED_WIDTH),
		.CODED_H(DDR_FRAME_720P_CODED_HEIGHT),
		.DISPLAY_W(DDR_FRAME_720P_DISPLAY_WIDTH),
		.DISPLAY_H(DDR_FRAME_720P_DISPLAY_HEIGHT),
		.CROP_LEFT(0), .CROP_TOP(0),
		.PRESENT_X(DDR_FRAME_720P_PILLARBOX_LEFT),
		.PRESENT_Y(0),
		.PHYS_BASE(DDR_FRAME_720P_PHYS_BASE),
		.HPS_BANK_STRIDE_BYTES(DDR_FRAME_720P_YUV420P_BANK_STRIDE),
		.DOORBELL_PHYS(DDR_FRAME_720P_YUV420P_DOORBELL_PHYS)
	) u720 (
		.rd_x(x720), .rd_y(y720),
		.rd_visible(v720), .src_x(srcx720), .src_y(srcy720),
		.bank0_base_bytes(b720_0), .bank1_base_bytes(b720_1),
		.bank0_end_bytes(b720_end), .frame_bytes(f720),
		.y_plane_bytes(), .doorbell_bytes(d720),
		.present_end_x(pe720_x), .present_end_y(pe720_y),
		.y_line_qwords(yq720), .c_line_qwords(cq720)
	);
	assign s720_x = 16'(srcx720);
	assign s720_y = 16'(srcy720);

	// 320x240 identity: coded=display=presented, no pillar/crop (glass path dims)
	ddr_frame_present_geom #(
		.FRAME_W(320), .FRAME_H(240),
		.CODED_W(320), .CODED_H(240),
		.DISPLAY_W(320), .DISPLAY_H(240),
		.CROP_LEFT(0), .CROP_TOP(0),
		.PRESENT_X(0), .PRESENT_Y(0),
		.PHYS_BASE(32'h3000_0000),
		.HPS_BANK_STRIDE_BYTES(32'h0004_0000),
		.DOORBELL_PHYS(32'h3007_F000)
	) u240 (
		.rd_x(x240), .rd_y(y240),
		.rd_visible(v240), .src_x(srcx240), .src_y(srcy240),
		.bank0_base_bytes(), .bank1_base_bytes(),
		.bank0_end_bytes(b240_end), .frame_bytes(f240),
		.y_plane_bytes(), .doorbell_bytes(),
		.present_end_x(pe240_x), .present_end_y(pe240_y),
		.y_line_qwords(), .c_line_qwords()
	);
endmodule
