// present_beam_content_de — progressive content-sized DE beam (ascal-native).
//
// B2b RASTER SOURCE OF TRUTH — the only module that emits hc/vc/DE/HS/VS.
// present_video_timing_960.sv is math/constant pack only (not a generator).
//
// Emits true content DE: HBlank low for H_DE cycles, VBlank low for V_ACTIVE lines.
// Default: 1182×564 @ 20 MHz → ~30.0008 Hz (fps_milli=30000).
// Film companion: same H_TOTAL=1182, V_TOTAL=705 via rt_vtotal → ~24.0007 Hz.
//
// Runtime geometry (use_rt_geom / use_rt_vtotal):
//   Host may change requests anytime. Captured every cycle; *applied* only at
//   frame wrap (v_wrap) or reset. Mid-frame apply is FORBIDDEN (stall / torn DE).
//   H_DE/H_TOTAL/V_ACTIVE latch together; V_TOTAL may also move via rt_vtotal.
//   Sync windows are derived from latched actives (+fixed porch/width) so a single
//   true_de extent always matches content without separate sync programming.
//
// STA (honest): exact Fmax with runtime compares is UNKNOWN until Plex.sta.rpt.
// Structure is 11-bit magnitude compares vs registers (same class as colorbars).
// Pre-register: expect meet 20 MHz if nostub-class; stub Fmax 23.46 leaves ~17% —
// thin if this cone ever becomes critical. Never invent BUILD_OK.

`default_nettype none

module present_beam_content_de #(
	parameter int H_DE      = 960,
	parameter int V_ACTIVE  = 540,
	parameter int H_TOTAL   = 1182,
	parameter int V_TOTAL   = 564,
	// Legacy fixed sync (used when use_rt_geom=0). Runtime mode derives sync.
	parameter int H_SYNC_S  = 960 + 32,
	parameter int H_SYNC_E  = 960 + 32 + 64,
	parameter int V_SYNC_S  = 540 + 8,
	parameter int V_SYNC_E  = 540 + 8 + 6,
	parameter int H_FP      = 32,
	parameter int H_SW      = 64,
	parameter int V_FP      = 8,
	parameter int V_SW      = 6
)(
	input  wire        clk,
	input  wire        reset,
	// Runtime V_TOTAL only (existing path). Applied at v_wrap/reset.
	input  wire        use_rt_vtotal,
	input  wire [10:0] rt_vtotal,
	// Full runtime geometry (H_DE/H_TOTAL/V_ACTIVE + optional V_TOTAL). Default OFF.
	input  wire        use_rt_geom,
	input  wire [10:0] rt_h_de,
	input  wire [10:0] rt_h_total,
	input  wire [10:0] rt_v_active,
	output reg         ce_pix,
	output reg         HBlank,
	output reg         HSync,
	output reg         VBlank,
	output reg         VSync,
	output reg         frame_start,
	output wire [10:0] hc_out,
	output wire [10:0] vc_out,
	// Active totals for the frame currently in progress (post-latch).
	output wire [10:0] vtot_active,
	output wire [10:0] hde_active,
	output wire [10:0] htot_active,
	output wire [10:0] vact_active
);
	// --- request mux + clamps (combinational; only sampled into regs at boundary) ---
	wire [10:0] hde_min  = 11'd16;
	wire [10:0] vact_min = 11'd16;
	wire [10:0] htot_min_pad = 11'd32; // at least 32 clocks blanking
	wire [10:0] vtot_min_pad = 11'd16;

	wire [10:0] hde_req0  = use_rt_geom ? rt_h_de     : 11'(H_DE);
	wire [10:0] htot_req0 = use_rt_geom ? rt_h_total  : 11'(H_TOTAL);
	wire [10:0] vact_req0 = use_rt_geom ? rt_v_active : 11'(V_ACTIVE);
	wire [10:0] vtot_src  = use_rt_geom ? (use_rt_vtotal ? rt_vtotal : 11'(V_TOTAL))
	                                    : (use_rt_vtotal ? rt_vtotal : 11'(V_TOTAL));

	wire [10:0] hde_req  = (hde_req0 < hde_min) ? hde_min : hde_req0;
	wire [10:0] vact_req = (vact_req0 < vact_min) ? vact_min : vact_req0;
	wire [10:0] htot_floor = hde_req + htot_min_pad;
	wire [10:0] htot_req = (htot_req0 < htot_floor) ? htot_floor : htot_req0;
	wire [10:0] vtot_floor = vact_req + vtot_min_pad;
	wire [10:0] vtot_req = (vtot_src < vtot_floor) ? vtot_floor : vtot_src;

	// Elab pins — parameter defaults only (runtime not known at elab).
	// synthesis translate_off
	initial begin
		if (H_TOTAL <= H_DE) $error("present_beam_content_de: H_TOTAL must exceed H_DE");
		if (V_TOTAL <= V_ACTIVE) $error("present_beam_content_de: V_TOTAL must exceed V_ACTIVE");
		if (H_SYNC_E >= H_TOTAL) $error("present_beam_content_de: H_SYNC past line");
		if (V_SYNC_E >= V_TOTAL) $error("present_beam_content_de: V_SYNC past default frame");
	end
	// synthesis translate_on

	reg [10:0] hc;
	reg [10:0] vc;
	reg [10:0] hde_act;
	reg [10:0] htot_act;
	reg [10:0] vact_act;
	reg [10:0] vtot_act;

	assign hc_out = hc;
	assign vc_out = vc;
	assign hde_active  = hde_act;
	assign htot_active = htot_act;
	assign vact_active = vact_act;
	assign vtot_active = vtot_act;

	wire [10:0] h_last = htot_act - 11'd1;
	wire [10:0] v_last = vtot_act - 11'd1;

	// Derived sync from latched actives (runtime) or fixed params (legacy).
	wire [10:0] hs_s = use_rt_geom ? (hde_act + 11'(H_FP)) : 11'(H_SYNC_S);
	wire [10:0] hs_e = use_rt_geom ? (hde_act + 11'(H_FP) + 11'(H_SW)) : 11'(H_SYNC_E);
	wire [10:0] vs_s = use_rt_geom ? (vact_act + 11'(V_FP)) : 11'(V_SYNC_S);
	wire [10:0] vs_e = use_rt_geom ? (vact_act + 11'(V_FP) + 11'(V_SW)) : 11'(V_SYNC_E);

	wire        h_wrap = (hc == h_last);
	wire        v_wrap = h_wrap && (vc == v_last);
	wire [10:0] hc_n   = h_wrap ? 11'd0 : (hc + 11'd1);
	wire [10:0] vc_n   = h_wrap ? (v_wrap ? 11'd0 : (vc + 11'd1)) : vc;

	always @(posedge clk) begin
		ce_pix <= 1'b1;
		frame_start <= 1'b0;

		if (reset) begin
			hc <= 11'd0;
			vc <= 11'd0;
			hde_act  <= hde_req;
			htot_act <= htot_req;
			vact_act <= vact_req;
			vtot_act <= vtot_req;
			ce_pix <= 1'b0;
			HBlank <= 1'b0;
			HSync  <= 1'b0;
			VBlank <= 1'b0;
			VSync  <= 1'b0;
		end else begin
			// Apply pending geometry only when completing a frame.
			if (v_wrap) begin
				hde_act  <= hde_req;
				htot_act <= htot_req;
				vact_act <= vact_req;
				vtot_act <= vtot_req;
				frame_start <= 1'b1;
			end

			hc <= hc_n;
			vc <= vc_n;

			// Blanks/sync from *next* counters so levels match hc_out/vc_out after NBA.
			HBlank <= (hc_n >= hde_act);
			VBlank <= (vc_n >= vact_act);
			HSync  <= (hc_n >= hs_s) && (hc_n < hs_e);
			VSync  <= (vc_n >= vs_s) && (vc_n < vs_e);
		end
	end
endmodule

`default_nettype wire
