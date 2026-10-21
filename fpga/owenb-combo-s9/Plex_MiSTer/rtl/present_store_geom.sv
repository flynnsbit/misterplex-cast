// present_store_geom — pure-parameter Template store-map (sim / unit only)
//
// Mirrors present_core Template-path math so dual-geometry and default-identity
// gates can elaborate without pulling colorbars/frame_store.
// NOT in files.qip (product netlist unchanged).

`timescale 1ns / 1ps
`default_nettype none

module present_store_geom #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240,
	parameter int TPL_H_DE = 529,
	parameter int TPL_V_STORE = 240,
	parameter int TPL_SCALE_REF_W = 320,
	parameter int TPL_SCALE_REF_H = 240,
	parameter int TPL_STORE_X_MUL = 39647
)(
	input  wire [9:0] hc,
	input  wire [9:0] py,
	input  wire       hb,
	input  wire       vb,
	output wire       in_content,
	output wire [15:0] store_x,
	output wire [15:0] store_y,
	output wire       past_last_row
);
	localparam int FRAME_X_W = (FRAME_W <= 1) ? 1 : $clog2(FRAME_W);
	localparam int FRAME_Y_W = (FRAME_H <= 1) ? 1 : $clog2(FRAME_H);
	localparam [15:0] FRAME_LAST_X_16 = 16'(FRAME_W - 1);
	localparam [15:0] FRAME_LAST_Y_16 = 16'(FRAME_H - 1);
	localparam int STORE_X_SCALE = (FRAME_W * TPL_STORE_X_MUL) / TPL_SCALE_REF_W;
	localparam int STORE_Y_SCALE = (FRAME_H * 65536) / TPL_SCALE_REF_H;

	assign in_content = (hc < 10'(TPL_H_DE)) && (py < 10'(TPL_V_STORE)) && ~hb && ~vb;
	assign past_last_row = (py >= 10'(TPL_V_STORE));

	wire [9:0] store_y_clamped =
		past_last_row ? 10'(TPL_V_STORE > 0 ? TPL_V_STORE - 1 : 0) : py;

	wire [31:0] store_x_prod = hc * STORE_X_SCALE;
	wire [15:0] store_x_comb = store_x_prod[31:16];
	wire [31:0] store_y_prod = store_y_clamped * STORE_Y_SCALE;
	wire [15:0] store_y_comb = store_y_prod[31:16];

	assign store_x =
		(store_x_comb > FRAME_LAST_X_16) ? FRAME_LAST_X_16 : store_x_comb;
	assign store_y =
		(store_y_comb > FRAME_LAST_Y_16) ? FRAME_LAST_Y_16 : store_y_comb;
endmodule

`default_nettype wire
