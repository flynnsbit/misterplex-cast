// Frozen pre-refactor Template store-map (hardcoded 529/240/39647/320).
// Dual-DUT identity gate only — not product RTL.
`timescale 1ns / 1ps
`default_nettype none

module present_store_geom_prerefactor #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240
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
	localparam H_DE    = 10'd529;
	localparam V_STORE = 10'd240;
	localparam int STORE_X_SCALE = (FRAME_W * 39647) / 320;
	localparam int STORE_Y_SCALE = (FRAME_H * 65536) / 240;
	localparam [15:0] FRAME_LAST_X_16 = 16'(FRAME_W - 1);
	localparam [15:0] FRAME_LAST_Y_16 = 16'(FRAME_H - 1);

	assign in_content = (hc < H_DE) && (py < V_STORE) && ~hb && ~vb;
	assign past_last_row = (py >= 10'd240);
	wire [9:0] store_y_clamped = past_last_row ? 10'd239 : py;

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
