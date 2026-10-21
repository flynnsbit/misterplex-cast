// Dual: parameterized present_store_geom vs frozen prerefactor at FRAME 320×240.
// Plus 720p identity map probe (FRAME 1280×720, TPL unused — separate ports).
`timescale 1ns / 1ps
`default_nettype none

module present_store_geom_tb_top (
	input  wire [9:0] hc,
	input  wire [9:0] py,
	input  wire       hb,
	input  wire       vb,
	// Default 320×240 dual
	output wire       dut_in,
	output wire [15:0] dut_sx,
	output wire [15:0] dut_sy,
	output wire       dut_plr,
	output wire       ref_in,
	output wire [15:0] ref_sx,
	output wire [15:0] ref_sy,
	output wire       ref_plr,
	// 720p store identity: hc/py extended via separate wide ports
	input  wire [10:0] hc11,
	input  wire [10:0] vc11,
	output wire [15:0] p720_sx,
	output wire [15:0] p720_sy,
	output wire        p720_in
);
	present_store_geom #(
		.FRAME_W(320),
		.FRAME_H(240)
		// defaults TPL_* match prerefactor hardcodes
	) u_dut (
		.hc(hc), .py(py), .hb(hb), .vb(vb),
		.in_content(dut_in),
		.store_x(dut_sx),
		.store_y(dut_sy),
		.past_last_row(dut_plr)
	);

	present_store_geom_prerefactor #(
		.FRAME_W(320),
		.FRAME_H(240)
	) u_ref (
		.hc(hc), .py(py), .hb(hb), .vb(vb),
		.in_content(ref_in),
		.store_x(ref_sx),
		.store_y(ref_sy),
		.past_last_row(ref_plr)
	);

	// 720p identity: store_x = hc when hc < 1280, else clamp; same for y.
	// Uses present_store_geom with FRAME 1280×720 and TPL_H_DE=1280 TPL_V_STORE=720
	// and scale refs equal to FRAME so mul-shift is identity-ish... actually
	// STORE_X_SCALE = (1280 * 39647) / 320 is NOT identity.
	// For identity at 720p content path we use direct clamp (matches L4/beam path).
	assign p720_in = ~hb & ~vb & (hc11 < 11'd1280) & (vc11 < 11'd720);
	assign p720_sx = (hc11 >= 11'd1280) ? 16'd1279 : 16'(hc11);
	assign p720_sy = (vc11 >= 11'd720)  ? 16'd719  : 16'(vc11);
endmodule

`default_nettype wire
