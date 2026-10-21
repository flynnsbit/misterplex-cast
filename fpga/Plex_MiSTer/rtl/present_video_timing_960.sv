// present_video_timing_960 — CONSTANT / MATH pack only (w-clock).
//
// B2b OWNERSHIP (parent 2026-08-03):
//   * This module is NOT a raster generator and NOT a fit timing SoT.
//   * Product beam counters live ONLY in present_beam_content_de.sv.
//   * Role here: longint fps/geometry arithmetic for oracles + host docs.
//   * Do not instantiate this as the sole timing path in a fit QSF.
//
// Primary MODE=0 numbers match beam defaults: 1182×564 @ 20 MHz → fps_milli=30000.
// MODE=1: 1190×560. MODE=2 film legacy: 1389×600 → fps_milli trunc 23997..23998.
// Film companion preferred: same H=1182, V=705 via beam rt_vtotal (not MODE=2).

module present_video_timing_960 #(
	parameter int MODE = 0,
	parameter int CLK_PIX_HZ = 20_000_000
)(
	output wire [11:0] h_de,
	output wire [11:0] h_total,
	output wire [11:0] v_active,
	output wire [11:0] v_total,
	output wire [11:0] h_sync_s,
	output wire [11:0] h_sync_e,
	output wire [11:0] v_sync_s,
	output wire [11:0] v_sync_e,
	output wire [15:0] fps_eff_milli,
	output wire        mode_30hz,
	output wire        needs_wide_fifo
);
	localparam int H_DE_L = 960;
	localparam int V_ACT_L = 540;

	localparam int H_TOTAL_30A = 1182;
	localparam int V_TOTAL_30A = 564;
	localparam int H_TOTAL_30B = 1190;
	localparam int V_TOTAL_30B = 560;
	localparam int H_TOTAL_24  = 1389;
	localparam int V_TOTAL_24  = 600;

	localparam int HT = (MODE == 2) ? H_TOTAL_24  :
	                    (MODE == 1) ? H_TOTAL_30B : H_TOTAL_30A;
	localparam int VT = (MODE == 2) ? V_TOTAL_24  :
	                    (MODE == 1) ? V_TOTAL_30B : V_TOTAL_30A;

	localparam int HS_S = H_DE_L + 32;
	localparam int HS_E = HS_S + 64;
	localparam int VS_S = V_ACT_L + 8;
	localparam int VS_E = VS_S + 6;

	localparam longint PIX_FRAME = longint'(HT) * longint'(VT);
	// 64-bit only: CLK*1000 = 20e9 overflows 32-bit int (B2 closed: was −2212).
	localparam longint FPS_MILLI =
		(longint'(CLK_PIX_HZ) * 64'd1000) / PIX_FRAME;

	localparam longint ACT_MPIX_MILLI =
		(longint'(H_DE_L) * longint'(V_ACT_L) * FPS_MILLI) / 64'd1000;

	assign h_de      = 12'(H_DE_L);
	assign h_total   = 12'(HT);
	assign v_active  = 12'(V_ACT_L);
	assign v_total   = 12'(VT);
	assign h_sync_s  = 12'(HS_S);
	assign h_sync_e  = 12'(HS_E);
	assign v_sync_s  = 12'(VS_S);
	assign v_sync_e  = 12'(VS_E);
	assign fps_eff_milli = 16'(FPS_MILLI);
	assign mode_30hz = (MODE != 2) ? 1'b1 : 1'b0;
	wire _act_over_serialize_wall = (ACT_MPIX_MILLI > 13330);
	assign needs_wide_fifo = 1'b0;
	wire _unused_act = _act_over_serialize_wall;
endmodule
