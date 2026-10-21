// Present core: color bars OR external frame_store, cadence, tone + audio FIFO.
// Display owns VSync; unique content advances only when present_cadence says so.
//
// 720p / ascal-native present path (landed; DEFAULT OFF):
//   `define PLEX_PRESENT_720P_L4 — L4 product 720p24 path:
//       present_beam_content_de DE 1280×720, H_TOTAL=1312, V_TOTAL=762
//       (w-clock measured: 24e6/(1312*762)=24.006 Hz @ preferred 24 MHz clk_sys;
//        at default 20 MHz beam runs ~20.005 Hz until fit grants 24 MHz PLL).
//       Instantiates present_content_window (store map). Requires FRAME_W=1280
//       FRAME_H=720 in the same QSF enable recipe. Plex.sv wires geom_latch+mux.
//   `define PRESENT_BEAM_960     — present_beam_content_de true-DE (max tier 960×540)
//   `define PRESENT_MULTI_PIXEL  — CEA 720p beam + present_npx_path (PPC path)
//   `define PRESENT_PX_PER_CLK N — 1|2|4 with MULTI_PIXEL (product land uses 1 until
//                                  ddr_frame_store grows N-wide RGB ports)
//   `define PRESENT_CLK_PIX_PLL  — separate clk_pix + rate-match (optional)
// Macros off → bit-identical Template H_DE=529 / DE_LAG=3 path (v0.3.0 baseline).
// Mutually exclusive: L4 vs BEAM_960 vs MULTI_PIXEL. Parent enables in fit QSF only.

`ifdef PRESENT_MULTI_PIXEL
	`ifndef PRESENT_PX_PER_CLK
		`define PRESENT_PX_PER_CLK 1
	`endif
`endif

module present_core #(
	parameter int FRAME_W = 320,
	parameter int FRAME_H = 240,
	parameter int FRAME_STRIDE = FRAME_W,
	parameter int SDRAM_REFRESH_CYCLES = 780,
	// Template/FBAR paint window (colorbars DE). Defaults reproduce v0.3.0 /
	// G-VID1 exactly: H_DE=529, V_STORE=240, scale ref 320×240, mul 39647.
	// These are NOT FRAME_W/FRAME_H — colorbars DE is fixed by the Template
	// path. Override only in simulation; product must keep defaults.
	parameter int TPL_H_DE = 529,
	parameter int TPL_V_STORE = 240,
	parameter int TPL_SCALE_REF_W = 320,
	parameter int TPL_SCALE_REF_H = 240,
	parameter int TPL_STORE_X_MUL = 39647,
	// L4 720p24 beam (used only under PLEX_PRESENT_720P_L4). Defaults match
	// w-clock NATIVE_720P kit. Product enable is QSF-gated default-off.
	parameter int L4_H_DE_P = 1280,
	parameter int L4_V_ACT_P = 720,
	parameter int L4_H_TOTAL_P = 1312,
	parameter int L4_V_TOTAL_P = 762,
	parameter int L4_H_FP_P = 8,
	parameter int L4_H_SW_P = 8,
	parameter int L4_V_FP_P = 8,
	parameter int L4_V_SW_P = 6,
`ifdef FRAME_CMD_FIFO_AW4
	parameter int FRAME_CMD_FIFO_AW = 4,
`elsif FRAME_CMD_FIFO_AW6
	parameter int FRAME_CMD_FIFO_AW = 6,
`else
	parameter int FRAME_CMD_FIFO_AW = 5,
`endif
`ifdef FRAME_LINES_1
	parameter int FRAME_LINE_COUNT = 1,
`elsif FRAME_LINES_4
	parameter int FRAME_LINE_COUNT = 4,
`elsif FRAME_LINES_8
	parameter int FRAME_LINE_COUNT = 8,
`elsif FRAME_LINES_16
	parameter int FRAME_LINE_COUNT = 16,
`else
	parameter int FRAME_LINE_COUNT = 4,
`endif
	// Match ddr_frame_store desired_y fill to Template→store_y scale:
	// store_y ≈ py * (FRAME_H / TPL_SCALE_REF_H). When that ratio is an
	// integer ≥1, stock only scanned coded lines (product480 FOAR: 480/240=2).
	// Keeps LINE_COUNT=8 while covering the beam set without FRAME_LINES_16.
	parameter int FRAME_Y_FILL_STRIDE =
		((TPL_SCALE_REF_H > 0) && ((FRAME_H % TPL_SCALE_REF_H) == 0) &&
		 ((FRAME_H / TPL_SCALE_REF_H) >= 1))
			? (FRAME_H / TPL_SCALE_REF_H) : 1
)(
	input  wire        clk,
	input  wire        clk_sdram,
	input  wire        clk_audio,
	// Optional pix clock (PRESENT_CLK_PIX_PLL). Product ties to clk_sys.
	input  wire        clk_pix,
	input  wire        reset,

	input  wire        pal,
	input  wire        scandouble,
	input  wire [7:0]  content_fps,
	input  wire [7:0]  display_hz,
	input  wire [1:0]  pattern,
	input  wire        audio_en,        // OSD tone enable (when no FIFO audio)
	input  wire        use_frame_store, // OSD force bars when 1

	// Delivered content geometry (PLXG / future mux). 0 → max-tier fallback when
	// PRESENT_BEAM_960. Ignored on default Template path.
	input  wire [10:0] content_w,
	input  wire [10:0] content_h,

	// frame_store write (from ingest)
	input  wire        fs_wr_en,
	input  wire [15:0] fs_wr_pixel,
	input  wire        fs_wr_reset,
	input  wire        fs_swap,
	output wire        fs_wr_ready,

	// SDRAM-backed frame_store port
	input  wire [15:0] sdram_dout,
	input  wire        sdram_ready,
	output wire        sdram_sel,
	output wire [26:1] sdram_addr,
	output wire [15:0] sdram_din,
	output wire        sdram_wr,
	output wire        sdram_rd,
	output wire  [1:0] sdram_bs,
	output wire        sdram_refresh,

`ifdef DDR_FRAME_STORE
	input  wire        ddr_start_req,
	input  wire        ddr_bank_sel,
	input  wire [15:0] ddr_status_osd,
	input  wire        ddr_input_cmd_valid,
	input  wire  [7:0] ddr_input_cmd,
	input  wire        ioctl_download,
	input  wire        ioctl_wr,
	input  wire [7:0]  ioctl_dout,
	input  wire [15:0] ioctl_index,
	input  wire  [3:0] ddr_sdram_test_state,
	input  wire  [3:0] ddr_sdram_size_code,
	input  wire [15:0] ddr_sdram_error_count,
	input  wire        clk_ddr,
	output wire        DDRAM_CLK,
	input  wire        DDRAM_BUSY,
	output wire  [7:0] DDRAM_BURSTCNT,
	output wire [28:0] DDRAM_ADDR,
	input  wire [63:0] DDRAM_DOUT,
	input  wire        DDRAM_DOUT_READY,
	output wire        DDRAM_RD,
	output wire [63:0] DDRAM_DIN,
	output wire  [7:0] DDRAM_BE,
	output wire        DDRAM_WE,
	output wire [15:0] ddr_frames_done,
	output wire        ddr_doorbell_ok,
`endif

	// audio_fifo write (from audio_ingest)
	input  wire        af_wr_en,
	input  wire [31:0] af_wr_data,
	input  wire        af_wr_flush,

	output wire        ce_pix,
	output wire        HBlank,
	output wire        HSync,
	output wire        VBlank,
	output wire        VSync,
	output wire [7:0]  r,
	output wire [7:0]  g,
	output wire [7:0]  b,

	output wire [15:0] audio_l,
	output wire [15:0] audio_r,

	output wire [31:0] stat_display_index,
	output wire [31:0] stat_content_index,
	output wire        stat_advance,
	output wire        stat_has_frame,
	output wire [31:0] stat_wr_count,
	output wire        stat_has_audio,
	output wire        stat_audio_underrun,
	output wire        stat_swap_pending,
	output wire [15:0] stat_frame_underruns,
	output wire  [7:0] stat_frame_sdram_state
);

	wire frame_start;
	wire advance;
	wire [31:0] disp_i, cont_i;

	present_cadence cadence (
		.clk(clk),
		.reset(reset),
		.display_tick(frame_start),
		.content_fps(content_fps),
		.display_hz(display_hz),
		.advance_unique(advance),
		.display_index(disp_i),
		.content_index(cont_i)
	);

	wire [7:0] br, bg, bb;
	wire       ce_pix_i, hb, hs, vb, vs, fstart;
	wire [9:0] hc, vc; // from colorbars — same counters as full-DE bar paint

	// Pattern encoding (status[7:6]): 0=None, 1=Bars, 2=Bars+Block, 3=Grid.
	// O[9] Force bars=Yes → always Bars (1), never None.
	wire [1:0] eff_pattern = use_frame_store ? 2'd1 : pattern;

	localparam int FRAME_X_W = $clog2(FRAME_W);
	localparam int FRAME_Y_W = $clog2(FRAME_H);

`ifdef PRESENT_MULTI_PIXEL
	localparam int PRESENT_PPC = `PRESENT_PX_PER_CLK;
`else
	localparam int PRESENT_PPC = 1;
`endif

	// ------------------------------------------------------------------
	// Always-on timing packs (noprune): survive map so post-fit hierarchy
	// can prove the 720p/960 generators are in the product netlist even
	// when BEAM/MULTI macros are off. Outputs are observational only on
	// the default path — they do not drive HDMI.
	// ------------------------------------------------------------------
	(* noprune *) wire [11:0] keep_720_hde, keep_720_htot, keep_720_vact, keep_720_vtot;
	(* noprune *) wire [11:0] keep_720_hss, keep_720_hse, keep_720_vss, keep_720_vse;
	(* noprune *) wire [15:0] keep_720_fps_milli;
	(* noprune *) wire        keep_720_needs_fast;
	present_video_timing_720p #(
		.CLK_PIX_HZ(20_000_000)
	) u_keep_timing_720p (
		.h_de(keep_720_hde),
		.h_total(keep_720_htot),
		.v_active(keep_720_vact),
		.v_total(keep_720_vtot),
		.h_sync_s(keep_720_hss),
		.h_sync_e(keep_720_hse),
		.v_sync_s(keep_720_vss),
		.v_sync_e(keep_720_vse),
		.fps_eff_milli(keep_720_fps_milli),
		.cea_24_needs_faster_pix(keep_720_needs_fast)
	);
	(* noprune *) wire [11:0] keep_960_hde, keep_960_htot, keep_960_vact, keep_960_vtot;
	(* noprune *) wire [11:0] keep_960_hss, keep_960_hse, keep_960_vss, keep_960_vse;
	(* noprune *) wire [15:0] keep_960_fps_milli;
	(* noprune *) wire        keep_960_mode30, keep_960_wide_fifo;
	present_video_timing_960 #(
		.MODE(0),
		.CLK_PIX_HZ(20_000_000)
	) u_keep_timing_960 (
		.h_de(keep_960_hde),
		.h_total(keep_960_htot),
		.v_active(keep_960_vact),
		.v_total(keep_960_vtot),
		.h_sync_s(keep_960_hss),
		.h_sync_e(keep_960_hse),
		.v_sync_s(keep_960_vss),
		.v_sync_e(keep_960_vse),
		.fps_eff_milli(keep_960_fps_milli),
		.mode_30hz(keep_960_mode30),
		.needs_wide_fifo(keep_960_wide_fifo)
	);

`ifdef PLEX_PRESENT_720P_L4
	// =====================================================================
	// L4 720p24 true-DE beam (DEFAULT OFF). w-clock: H=1312 V=762 @ 24 MHz
	// → 24.006 Hz (1:1 with measured PMS 24/1 asset; no pulldown).
	// =====================================================================
	// synthesis translate_off
	initial begin
		if (FRAME_W != 1280 || FRAME_H != 720)
			$error("PLEX_PRESENT_720P_L4 requires FRAME_W=1280 FRAME_H=720 (got %0d x %0d)",
				FRAME_W, FRAME_H);
`ifdef PRESENT_BEAM_960
		$error("PLEX_PRESENT_720P_L4 and PRESENT_BEAM_960 are mutually exclusive");
`endif
`ifdef PRESENT_MULTI_PIXEL
		$error("PLEX_PRESENT_720P_L4 and PRESENT_MULTI_PIXEL are mutually exclusive");
`endif
	end
	// synthesis translate_on

	// w-clock NATIVE_720P_GO_NOGO / Plex_native720p24.sdc — from module params
	localparam int L4_H_DE    = L4_H_DE_P;
	localparam int L4_V_ACT   = L4_V_ACT_P;
	localparam int L4_H_TOTAL = L4_H_TOTAL_P;
	localparam int L4_V_TOTAL = L4_V_TOTAL_P;
	localparam int L4_H_SYNC_S = L4_H_DE_P + L4_H_FP_P;
	localparam int L4_H_SYNC_E = L4_H_DE_P + L4_H_FP_P + L4_H_SW_P;
	localparam int L4_V_SYNC_S = L4_V_ACT_P + L4_V_FP_P;
	localparam int L4_V_SYNC_E = L4_V_ACT_P + L4_V_FP_P + L4_V_SW_P;

	wire [10:0] hc11, vc11, vtot_act11;
	wire [10:0] hde_act11, htot_act11, vact_act11;
	wire [10:0] beam_hde_req =
		(content_w == 11'd0) ? 11'(L4_H_DE) :
		(content_w > 11'(FRAME_W)) ? 11'(FRAME_W) : content_w;
	wire [10:0] beam_vact_req =
		(content_h == 11'd0) ? 11'(L4_V_ACT) :
		(content_h > 11'(FRAME_H)) ? 11'(FRAME_H) : content_h;
	wire [10:0] beam_htot_req = 11'(L4_H_TOTAL);
	wire [10:0] beam_vtot_req = 11'(L4_V_TOTAL);

	(* noprune *) present_beam_content_de #(
		.H_DE(L4_H_DE),
		.V_ACTIVE(L4_V_ACT),
		.H_TOTAL(L4_H_TOTAL),
		.V_TOTAL(L4_V_TOTAL),
		.H_SYNC_S(L4_H_SYNC_S),
		.H_SYNC_E(L4_H_SYNC_E),
		.V_SYNC_S(L4_V_SYNC_S),
		.V_SYNC_E(L4_V_SYNC_E)
	) u_beam_720p24 (
		.clk(clk),
		.reset(reset),
		.use_rt_vtotal(1'b1),
		.rt_vtotal(beam_vtot_req),
		.use_rt_geom(1'b1),
		.rt_h_de(beam_hde_req),
		.rt_h_total(beam_htot_req),
		.rt_v_active(beam_vact_req),
		.ce_pix(ce_pix_i),
		.HBlank(hb),
		.HSync(hs),
		.VBlank(vb),
		.VSync(vs),
		.frame_start(fstart),
		.hc_out(hc11),
		.vc_out(vc11),
		.vtot_active(vtot_act11),
		.hde_active(hde_act11),
		.htot_active(htot_act11),
		.vact_active(vact_act11)
	);
	assign hc = hc11[9:0];
	assign vc = vc11[9:0];
	assign br = 8'd0;
	assign bg = 8'd0;
	assign bb = 8'd0;
	wire _unused_beam_scandouble = scandouble;
	wire _unused_beam_pal = pal;
	wire _unused_eff_pattern = |eff_pattern;
	wire _unused_l4_beam = (|vtot_act11) | (|htot_act11) | (|hde_act11) | (|vact_act11);

`elsif PRESENT_BEAM_960
	// =====================================================================
	// Ascal-native TRUE content DE. Default OFF. Replaces colorbars Template.
	// =====================================================================
	// synthesis translate_off
	initial begin
		if (FRAME_W != 960 || FRAME_H != 540)
			$error("PRESENT_BEAM_960 requires FRAME_W=960 FRAME_H=540 (got %0d x %0d)",
				FRAME_W, FRAME_H);
`ifdef PRESENT_MULTI_PIXEL
		$error("PRESENT_BEAM_960 and PRESENT_MULTI_PIXEL are mutually exclusive");
`endif
	end
	// synthesis translate_on

	wire [10:0] hc11, vc11, vtot_act11;
	wire [10:0] hde_act11, htot_act11, vact_act11;
	wire [10:0] beam_hde_req =
		(content_w == 11'd0) ? 11'd960 :
		(content_w > 11'(FRAME_W)) ? 11'(FRAME_W) : content_w;
	wire [10:0] beam_vact_req =
		(content_h == 11'd0) ? 11'd540 :
		(content_h > 11'(FRAME_H)) ? 11'(FRAME_H) : content_h;
	wire [10:0] beam_htot_req = 11'd1182;
	wire [10:0] beam_vtot_req = (content_fps <= 8'd25) ? 11'd705 : 11'd564;

	(* noprune *) present_beam_content_de #(
		.H_DE(960),
		.V_ACTIVE(540),
		.H_TOTAL(1182),
		.V_TOTAL(564),
		.H_SYNC_S(992),
		.H_SYNC_E(1056),
		.V_SYNC_S(548),
		.V_SYNC_E(554)
	) u_beam_960 (
		.clk(clk),
		.reset(reset),
		.use_rt_vtotal(1'b1),
		.rt_vtotal(beam_vtot_req),
		.use_rt_geom(1'b1),
		.rt_h_de(beam_hde_req),
		.rt_h_total(beam_htot_req),
		.rt_v_active(beam_vact_req),
		.ce_pix(ce_pix_i),
		.HBlank(hb),
		.HSync(hs),
		.VBlank(vb),
		.VSync(vs),
		.frame_start(fstart),
		.hc_out(hc11),
		.vc_out(vc11),
		.vtot_active(vtot_act11),
		.hde_active(hde_act11),
		.htot_active(htot_act11),
		.vact_active(vact_act11)
	);
	assign hc = hc11[9:0];
	assign vc = vc11[9:0];
	assign br = 8'd0;
	assign bg = 8'd0;
	assign bb = 8'd0;
	wire _unused_beam_scandouble = scandouble;
	wire _unused_beam_pal = pal;
	wire _unused_eff_pattern = |eff_pattern;

`else
	// ---- Legacy Template path: colorbars H_DE=529 (FBAR) — product default ----
	colorbars bars (
		.clk(clk),
		.reset(reset),
		.pal(pal),
		.scandouble(scandouble),
		.content_index(cont_i),
		.pattern(eff_pattern),
		.ce_pix(ce_pix_i),
		.HBlank(hb),
		.HSync(hs),
		.VBlank(vb),
		.VSync(vs),
		.frame_start(fstart),
		.hc_out(hc),
		.vc_out(vc),
		.r(br),
		.g(bg),
		.b(bb)
	);
`endif

	localparam [FRAME_X_W-1:0] FRAME_LAST_X = FRAME_X_W'(FRAME_W - 1);
	localparam [FRAME_Y_W-1:0] FRAME_LAST_Y = FRAME_Y_W'(FRAME_H - 1);
	localparam [15:0] FRAME_LAST_X_16 = 16'(FRAME_W - 1);
	localparam [15:0] FRAME_LAST_Y_16 = 16'(FRAME_H - 1);

	// Stretch FRAME_W×FRAME_H frame_store across full Template DE — match colorbars in_content.
	// Prior attempts (combo ÷529, reconstructed hc Bresenham) still UVC-pillar 0.604 on
	// solid-red F1 while bars on same RBF span 0.998. Use colorbars hc + mul-shift.
	// PRESENT_BEAM_960: identity map uses hc/vc (still feeds store_x_clamped name so
	// G-VID1 scanout invariants remain a single assign pair).
	// Defaults: TPL_H_DE=529 TPL_V_STORE=240 — bit-identical to prior localparams.
	localparam H_DE    = 10'(TPL_H_DE);
	localparam V_STORE = 10'(TPL_V_STORE);
	localparam int STORE_X_SCALE = (FRAME_W * TPL_STORE_X_MUL) / TPL_SCALE_REF_W;
	localparam int STORE_Y_SCALE = (FRAME_H * 65536) / TPL_SCALE_REF_H;
	// Exact clone of colorbars in_content (full DE paint region).
	wire [9:0] py = scandouble ? (vc >> 1) : vc;
`ifdef PLEX_PRESENT_720P_L4
	// L4: present_content_window owns store map (identity when content==DE).
	// STORE domain tracks FRAME_* so QSF 1280×720 cannot disagree with 480p-era 1280/720 literals.
	wire in_content_l4 = ~hb & ~vb & (hc11 < hde_act11) & (vc11 < vact_act11);
	wire past_last_row; // driven by content_window
	wire [FRAME_X_W-1:0] store_x_clamped;
	wire [FRAME_Y_W-1:0] store_y_addr;
	wire de_r_win;
	(* noprune *) present_content_window #(
		.FRAME_W(FRAME_W),
		.FRAME_H(FRAME_H),
		.STORE_W(FRAME_W),
		.STORE_H(FRAME_H),
		.H_DE_DEFAULT(L4_H_DE),
		.V_DE_DEFAULT(L4_V_ACT)
	) u_content_window (
		.clk(clk),
		.reset(reset),
		.ce_pix(ce_pix_i),
		.hc(hc11),
		.py(vc11),
		.in_content(in_content_l4),
		.win_enable(1'b1),
		.content_w(beam_hde_req),
		.content_h(beam_vact_req),
		.content_x0(11'd0),
		.content_y0(11'd0),
		.h_de(hde_act11),
		.v_de(vact_act11),
		.store_x(store_x_clamped),
		.store_y(store_y_addr),
		.de_r(de_r_win),
		.past_last_row(past_last_row)
	);
	wire in_content = in_content_l4;
	// Clamp from FRAME_H (not 239) so 720p does not inherit 480p last-row.
	wire [9:0] store_y_clamped =
		past_last_row ? 10'(FRAME_H > 0 ? FRAME_H - 1 : 0) : py;
	wire _unused_l4_win = de_r_win | |store_y_clamped;
`elsif PRESENT_BEAM_960
	wire in_content = ~hb & ~vb & (hc11 < hde_act11) & (vc11 < vact_act11);
	wire       past_last_row = (py >= 10'(FRAME_H));
	wire [9:0] store_y_clamped =
		past_last_row ? 10'(FRAME_H > 0 ? FRAME_H - 1 : 0) : py;
	wire [FRAME_X_W-1:0] store_x_clamped =
		(hc11 >= 11'(FRAME_W)) ? FRAME_LAST_X : FRAME_X_W'(hc11);
	wire [FRAME_Y_W-1:0] store_y_addr =
		(vc11 >= 11'(FRAME_H)) ? FRAME_LAST_Y : FRAME_Y_W'(vc11);
	wire _unused_beam_store_y_clamped = |store_y_clamped;
`else
	wire in_content = (hc < H_DE) && (py < V_STORE) && ~hb && ~vb;

	// store_x = floor(hc * TPL_SCALE_REF_W / TPL_H_DE)
	//   ≈ (hc * TPL_STORE_X_MUL) >> 16  (default 39647/65536 ≈ 320/529)
	// Drive the address straight from the clamped counter, with no blank-time special
	// case. Forcing store_x to 0 during blank used to hand column 0 to any display
	// pixel whose address was issued outside `in_content` — with the sync delayed by
	// DE_LAG that includes the last pixels of every line, which is what wrapped the
	// first column onto the RIGHT edge. Free-running, hc keeps counting past H_DE so
	// the clamp naturally holds column 319 through the right overhang, and hc wraps to
	// 0 early in the left blank so column 0 is ready before DE opens.
	wire [9:0] read_hc = hc;
	wire [31:0] store_x_prod = read_hc * STORE_X_SCALE;
	wire [15:0] store_x_comb = store_x_prod[31:16];
	wire [FRAME_X_W-1:0] store_x_clamped =
		(store_x_comb > FRAME_LAST_X_16) ? FRAME_LAST_X : store_x_comb[FRAME_X_W-1:0];

	// colorbars moves the V blank edges at hc == H_SYNC_S, i.e. AFTER each line's
	// active region, so VBlank releases a line early with respect to the content
	// window and line 240 is still displayed -- 241 active rows instead of 240.
	// Measured on hardware with scripts/gen_edge_markers.py's stripe pattern: the
	// bottom stripes land on a 1080/241 = 4.4813 row pitch, not 1080/240 = 4.5.
	// That surplus row is the "bottom line": nothing gates it on py, so it reads
	// store_y = 240, one row past the end of the 240-row store.
	// Blank it, and clamp the address so an out-of-range row can never be fetched.
	// Default TPL_V_STORE=240 → past_last_row=(py>=10'd240), clamp 239 (G-VID1).
	wire       past_last_row = (py >= 10'(TPL_V_STORE));
	wire [9:0] store_y_clamped =
		past_last_row ? 10'(TPL_V_STORE > 0 ? TPL_V_STORE - 1 : 0) : py;
	wire [31:0] store_y_prod = store_y_clamped * STORE_Y_SCALE;
	wire [15:0] store_y_comb = store_y_prod[31:16];
	wire [FRAME_Y_W-1:0] store_y_addr =
		(store_y_comb > FRAME_LAST_Y_16) ? FRAME_LAST_Y : store_y_comb[FRAME_Y_W-1:0];
`endif

	reg [FRAME_X_W-1:0] store_x;
	reg [FRAME_Y_W-1:0] store_y;
	reg       de_r; // registered in_content for frame_store read align
	always @(posedge clk) begin
		if (reset) begin
			store_x <= 0;
			store_y <= '0;
			de_r    <= 1'b0;
		end else if (ce_pix_i) begin
			de_r    <= in_content;
			store_y <= store_y_addr;
			store_x <= store_x_clamped;
		end
	end

	wire [7:0] fr, fg, fb;
	wire       has_frame;
	wire       swap_pending;
	wire [31:0] wr_count;
	wire        wr_done;
	wire [15:0] frame_underruns;
	wire [7:0]  frame_sdram_state;

`ifdef DDR_FRAME_STORE
	assign fs_wr_ready = 1'b1;
	assign wr_count = 32'd0;
	assign wr_done = 1'b0;
	assign sdram_sel = 1'b0;
	assign sdram_addr = 26'd0;
	assign sdram_din = 16'd0;
	assign sdram_wr = 1'b0;
	assign sdram_rd = 1'b0;
	assign sdram_bs = 2'b11;
	assign sdram_refresh = 1'b0;

`include "ddr_frame_layout_params.svh"

	// Active DDR reader geometry. Default = 480p layout (coded 624 / display
	// 618 / pillar 11 / stride 0x80000). L4 selects the 720p block from the
	// same svh — without this, FRAME_W/H=1280x720 still fed 624x480 into the
	// store and visibility ended at x≈628 y=479 (reviewer point 5).
`ifdef PLEX_PRESENT_720P_L4
	localparam int FS_CODED_W     = DDR_FRAME_720P_CODED_WIDTH;
	localparam int FS_CODED_H     = DDR_FRAME_720P_CODED_HEIGHT;
	localparam int FS_DISPLAY_W   = DDR_FRAME_720P_DISPLAY_WIDTH;
	localparam int FS_DISPLAY_H   = DDR_FRAME_720P_DISPLAY_HEIGHT;
	localparam int FS_CROP_LEFT   = 0; // 720p: display == coded (host kPlex720p*)
	localparam int FS_CROP_TOP    = 0;
	localparam int FS_PRESENT_X   = DDR_FRAME_720P_PILLARBOX_LEFT;
	localparam int FS_PRESENT_Y   = 0;
	localparam [31:0] FS_PHYS_BASE = DDR_FRAME_720P_PHYS_BASE;
	localparam int FS_BANK_STRIDE = DDR_FRAME_720P_YUV420P_BANK_STRIDE;
	localparam [31:0] FS_DOORBELL = DDR_FRAME_720P_YUV420P_DOORBELL_PHYS;
`else
	localparam int FS_CODED_W     = DDR_FRAME_CODED_WIDTH;
	localparam int FS_CODED_H     = DDR_FRAME_CODED_HEIGHT;
	localparam int FS_DISPLAY_W   = DDR_FRAME_DISPLAY_WIDTH;
	localparam int FS_DISPLAY_H   = DDR_FRAME_DISPLAY_HEIGHT;
	localparam int FS_CROP_LEFT   = DDR_FRAME_CROP_LEFT;
	localparam int FS_CROP_TOP    = DDR_FRAME_CROP_TOP;
	localparam int FS_PRESENT_X   = DDR_FRAME_PILLARBOX_LEFT;
	localparam int FS_PRESENT_Y   = 0;
	localparam [31:0] FS_PHYS_BASE = 32'h3000_0000;
	localparam int FS_BANK_STRIDE = DDR_FRAME_YUV420P_BANK_STRIDE;
	localparam [31:0] FS_DOORBELL = DDR_FRAME_YUV420P_DOORBELL_PHYS;
`endif

	ddr_frame_store #(
		.FRAME_W(FRAME_W),
		.FRAME_H(FRAME_H),
		.FRAME_STRIDE(FRAME_STRIDE),
		.CODED_W(FS_CODED_W),
		.CODED_H(FS_CODED_H),
		.DISPLAY_W(FS_DISPLAY_W),
		.DISPLAY_H(FS_DISPLAY_H),
		.CROP_LEFT(FS_CROP_LEFT),
		.CROP_TOP(FS_CROP_TOP),
		.PRESENT_X(FS_PRESENT_X),
		.PRESENT_Y(FS_PRESENT_Y),
		.LINE_COUNT(FRAME_LINE_COUNT),
`ifdef PLEX_PRESENT_720P_L4
		.Y_FILL_STRIDE(1),
		.C_INTERLEAVE_ON_Y_BEAM(1'b1),
`else
		.Y_FILL_STRIDE(FRAME_Y_FILL_STRIDE),
`endif
		.PHYS_BASE(FS_PHYS_BASE),
		.HPS_BANK_STRIDE_BYTES(FS_BANK_STRIDE),
		.DOORBELL_PHYS(FS_DOORBELL),
`ifdef PLEX_PRESENT_720P_L4
		.MAILBOX_PHYS(FS_DOORBELL + 32'h100),
		.INPUT_MAILBOX_PHYS(FS_DOORBELL + 32'h108),
		.SDRAM_MAILBOX_PHYS(FS_DOORBELL + 32'h110),
		.FRAME_MAILBOX_PHYS(FS_DOORBELL + 32'h118),
		.BANK_MAILBOX_PHYS(FS_DOORBELL + 32'h128),
		.ASPECT_ACK_PHYS(FS_DOORBELL + 32'h130),
		.DYN_BASE_EN(1'b1)
`else
		.DYN_BASE_EN(1'b0)
`endif
	) fstore (
		.clk(clk),
		.clk_ddr(clk_ddr),
		.reset(reset),
		.rd_x(store_x),
		.rd_y(store_y),
		.rd_active(de_r),
		.rd_r(fr),
		.rd_g(fg),
		.rd_b(fb),
		.start_req(ddr_start_req),
		.bank_sel(ddr_bank_sel),
		.status_osd(ddr_status_osd),
		.input_cmd_valid(ddr_input_cmd_valid),
		.input_cmd(ddr_input_cmd),
		.ioctl_download(ioctl_download),
		.ioctl_wr(ioctl_wr),
		.ioctl_dout(ioctl_dout),
		.ioctl_index(ioctl_index),
		.sdram_test_state(ddr_sdram_test_state),
		.sdram_size_code(ddr_sdram_size_code),
		.sdram_error_count(ddr_sdram_error_count),
		.DDRAM_CLK(DDRAM_CLK),
		.DDRAM_BUSY(DDRAM_BUSY),
		.DDRAM_BURSTCNT(DDRAM_BURSTCNT),
		.DDRAM_ADDR(DDRAM_ADDR),
		.DDRAM_DOUT(DDRAM_DOUT),
		.DDRAM_DOUT_READY(DDRAM_DOUT_READY),
		.DDRAM_RD(DDRAM_RD),
		.DDRAM_DIN(DDRAM_DIN),
		.DDRAM_BE(DDRAM_BE),
		.DDRAM_WE(DDRAM_WE),
		.vsync_pulse(fstart),
		.has_frame(has_frame),
		.swap_pending(swap_pending),
		.underrun_count(frame_underruns),
		.frames_done(ddr_frames_done),
		.doorbell_ok(ddr_doorbell_ok),
		.debug_state(frame_sdram_state)
	);
`else
	frame_store #(
		.FRAME_W(FRAME_W),
		.FRAME_H(FRAME_H),
		.FRAME_STRIDE(FRAME_STRIDE),
		.REFRESH_CYCLES(SDRAM_REFRESH_CYCLES),
		.CMD_FIFO_AW(FRAME_CMD_FIFO_AW),
		.LINE_COUNT(FRAME_LINE_COUNT)
	) fstore (
		.clk(clk),
		.clk_sdram(clk_sdram),
		.reset(reset),
		.wr_en(fs_wr_en),
		.wr_pixel(fs_wr_pixel),
		.wr_reset_ptr(fs_wr_reset),
		.wr_ready(fs_wr_ready),
		.wr_count(wr_count),
		.wr_frame_done(wr_done),
		.rd_x(store_x),
		.rd_y(store_y),
		// Full content DE (registered); hold inside frame_store across !ce_pix
		.rd_active(de_r),
		.rd_r(fr),
		.rd_g(fg),
		.rd_b(fb),
		.sdram_dout(sdram_dout),
		.sdram_ready(sdram_ready),
		.sdram_sel(sdram_sel),
		.sdram_addr(sdram_addr),
		.sdram_din(sdram_din),
		.sdram_wr(sdram_wr),
		.sdram_rd(sdram_rd),
		.sdram_bs(sdram_bs),
		.sdram_refresh(sdram_refresh),
		// Request on DMA/F1 complete; apply only at display frame_start (vsync)
		.swap_banks(fs_swap),
		.vsync_pulse(fstart),
		.has_frame(has_frame),
		.swap_pending(swap_pending),
		.underrun_count(frame_underruns),
		.debug_state(frame_sdram_state)
	);
`endif

	// Product: once a frame is ingested, always show frame_store unless O[9] Force bars.
	// Pattern no longer steals cast (old pattern!=0 force caused bars/grid under video).
	// Pattern=None (0) + no frame → black (nothing “runs” behind cast).
	wire use_ext = has_frame && !use_frame_store;
	wire show_pattern = !use_ext && (use_frame_store || (pattern != 2'd0));
	// --- Align sync/blank to the pixel pipeline -------------------------------
	// hc -> fr costs 5 clk: store_x registers on ce_pix (2 clk) then frame_store
	// does rd_addr_r -> rd_q -> rd_r (3 clk). hc -> hb/hs costs only 1 clk. So the
	// RGB data is DE_LAG clk behind the sync it belongs to, which put the previous
	// line's last column in the first visible pixel (the 1 px left-edge wrap on VGA)
	// and blanked the true first column.
	//
	// Advancing the READ ADDRESS cannot fix this: during blank store_x is forced to
	// 0, so the blank-time prefetch is column 0 no matter what read_hc says. Delay
	// the sync/blank outputs instead — that aligns DE to the data by construction.
	// Only the HORIZONTAL sync/blank need this alignment: the pipeline delay is a few
	// clk, which is far shorter than a line, so delaying VBlank/VSync would only push
	// the bottom edge out and expose a row past the frame (the bottom bar).
	// DE_LAG=3 was measured, not guessed: scripts/gen_edge_markers.py paints the first
	// source column white and the last mid-grey, and scripts/check_edges.py captures
	// HDMI and reports where each landed. Sweeping 3..6 on hardware:
	//   3 -> col0 w=6px, col319 w=4px   (correct)
	//   4 -> col0 w=4px, col319 w=7px
	//   5 -> col0 MISSING, col319 w=11px
	//   6 -> col0 MISSING, col319 w=14px
	// Each extra clk of lag eats ~0.6 of a source column off the left and repeats it
	// on the right, which is precisely the right-edge "bar".
	localparam DE_LAG = 3'd3;
	reg [DE_LAG-1:0] hb_sr, hs_sr;
	always @(posedge clk) begin
		hb_sr <= {hb_sr[DE_LAG-2:0], hb};
		hs_sr <= {hs_sr[DE_LAG-2:0], hs};
	end
	wire hb_d = hb_sr[DE_LAG-1];
	wire hs_d = hs_sr[DE_LAG-1];
	wire vb_d = vb | past_last_row;
	wire vs_d = vs;

	// Gate the frame pixels with EXACTLY the delayed signal that drives VGA_DE.
	wire de_out = ~hb_d & ~vb_d;
	wire [7:0] leg_r = use_ext ? (de_out ? fr : 8'd0) : (show_pattern ? br : 8'd0);
	wire [7:0] leg_g = use_ext ? (de_out ? fg : 8'd0) : (show_pattern ? bg : 8'd0);
	wire [7:0] leg_b = use_ext ? (de_out ? fb : 8'd0) : (show_pattern ? bb : 8'd0);

`ifdef PRESENT_MULTI_PIXEL
	// ------------------------------------------------------------------
	// CEA 720p multi-pixel path (macro ON only). Default OFF → leg_* above.
	// This land uses scalar ddr_frame_store RGB (PPC must be 1 until store
	// grows N-wide ports). Beam + npx + timing packs are product-wired.
	// ------------------------------------------------------------------
	// synthesis translate_off
	initial begin
		if (PRESENT_PPC != 1)
			$error("PRESENT_MULTI_PIXEL land requires PRESENT_PX_PER_CLK=1 until ddr_frame_store N-wide RGB (got %0d)",
				PRESENT_PPC);
`ifdef PRESENT_BEAM_960
		$error("PRESENT_MULTI_PIXEL and PRESENT_BEAM_960 are mutually exclusive");
`endif
	end
	// synthesis translate_on

	wire                mp_in_ready;
	wire                mp_beam_ce;
	wire [11:0]         mp_glass_x0, mp_glass_y;
	wire [PRESENT_PPC-1:0] mp_lane_de;
	wire                mp_hb, mp_hs, mp_vb, mp_vs, mp_fstart;
	wire                mp_out_ce;
	wire [7:0]          mp_out_r, mp_out_g, mp_out_b;
	wire                mp_out_hb, mp_out_hs, mp_out_vb, mp_out_vs, mp_out_fs;
	wire                mp_wr_full, mp_wr_af, mp_rd_ur, mp_rd_empty;

`ifdef PRESENT_CLK_PIX_PLL
	localparam int MP_CLK_PIX_HZ = 29_700_000;
`else
	localparam int MP_CLK_PIX_HZ = 20_000_000;
`endif
	localparam bit MP_INCLUDE_SYNC = 1'b1;

	// Live timing instance (in addition to keep_* packs) drives beam params via
	// parameters below — pack remains hierarchical proof of CEA constants.
	wire mp_beam_en = ~reset & mp_in_ready;
	present_beam_ppc #(
		.PX_PER_CLK(PRESENT_PPC),
		.H_DE(1280),
		.H_TOTAL(1650),
		.V_ACTIVE(720),
		.V_TOTAL(750),
		.H_SYNC_S(1390),
		.H_SYNC_E(1430),
		.V_SYNC_S(725),
		.V_SYNC_E(730)
	) u_mp_beam (
		.clk(clk),
		.reset(reset),
		.enable(mp_beam_en),
		.beam_ce(mp_beam_ce),
		.glass_x0(mp_glass_x0),
		.glass_y(mp_glass_y),
		.lane_de(mp_lane_de),
		.HBlank(mp_hb),
		.HSync(mp_hs),
		.VBlank(mp_vb),
		.VSync(mp_vs),
		.frame_start(mp_fstart)
	);

	// Scalar store sample → N-wide group (PPC=1 → passthrough).
	wire [PRESENT_PPC*8-1:0] mp_npx_r = {PRESENT_PPC{fr}};
	wire [PRESENT_PPC*8-1:0] mp_npx_g = {PRESENT_PPC{fg}};
	wire [PRESENT_PPC*8-1:0] mp_npx_b = {PRESENT_PPC{fb}};
	wire [PRESENT_PPC-1:0]   mp_npx_lv = mp_lane_de & {PRESENT_PPC{has_frame}};

	// Align store response: 1 (store_x reg) + typical ddr RGB pipe ≈ 4.
	localparam int MP_STORE_LAT = 4;
	reg [MP_STORE_LAT-1:0] mp_tq_v;
	reg mp_tq_hb [0:MP_STORE_LAT-1];
	reg mp_tq_hs [0:MP_STORE_LAT-1];
	reg mp_tq_vb [0:MP_STORE_LAT-1];
	reg mp_tq_vs [0:MP_STORE_LAT-1];
	reg mp_tq_fs [0:MP_STORE_LAT-1];
	reg [PRESENT_PPC-1:0] mp_tq_lde [0:MP_STORE_LAT-1];
	integer mp_ti;
	always @(posedge clk) begin
		if (reset) begin
			mp_tq_v <= '0;
			for (mp_ti = 0; mp_ti < MP_STORE_LAT; mp_ti = mp_ti + 1) begin
				mp_tq_hb[mp_ti]  <= 1'b1;
				mp_tq_hs[mp_ti]  <= 1'b0;
				mp_tq_vb[mp_ti]  <= 1'b1;
				mp_tq_vs[mp_ti]  <= 1'b0;
				mp_tq_fs[mp_ti]  <= 1'b0;
				mp_tq_lde[mp_ti] <= '0;
			end
		end else begin
			for (mp_ti = MP_STORE_LAT-1; mp_ti > 0; mp_ti = mp_ti - 1) begin
				mp_tq_v[mp_ti]   <= mp_tq_v[mp_ti-1];
				mp_tq_hb[mp_ti]  <= mp_tq_hb[mp_ti-1];
				mp_tq_hs[mp_ti]  <= mp_tq_hs[mp_ti-1];
				mp_tq_vb[mp_ti]  <= mp_tq_vb[mp_ti-1];
				mp_tq_vs[mp_ti]  <= mp_tq_vs[mp_ti-1];
				mp_tq_fs[mp_ti]  <= mp_tq_fs[mp_ti-1];
				mp_tq_lde[mp_ti] <= mp_tq_lde[mp_ti-1];
			end
			mp_tq_v[0]   <= mp_beam_ce;
			mp_tq_hb[0]  <= mp_beam_ce ? mp_hb : 1'b1;
			mp_tq_hs[0]  <= mp_beam_ce ? mp_hs : 1'b0;
			mp_tq_vb[0]  <= mp_beam_ce ? mp_vb : 1'b1;
			mp_tq_vs[0]  <= mp_beam_ce ? mp_vs : 1'b0;
			mp_tq_fs[0]  <= mp_beam_ce ? mp_fstart : 1'b0;
			mp_tq_lde[0] <= mp_beam_ce ? mp_lane_de : '0;
		end
	end
	wire mp_push = mp_tq_v[MP_STORE_LAT-1];

`ifdef PRESENT_CLK_PIX_PLL
	(* noprune *) reg mp_rst_pix0, mp_rst_pix1;
	always @(posedge clk_pix or posedge reset) begin
		if (reset) begin
			mp_rst_pix0 <= 1'b1;
			mp_rst_pix1 <= 1'b1;
		end else begin
			mp_rst_pix0 <= 1'b0;
			mp_rst_pix1 <= mp_rst_pix0;
		end
	end
	wire mp_reset_pix = mp_rst_pix1;
`else
	wire mp_reset_pix = reset;
`endif

	present_npx_path #(
		.PX_PER_CLK(PRESENT_PPC),
		.FIFO_AW(6),
		.INCLUDE_SYNC(MP_INCLUDE_SYNC),
		.PREFILL_GROUPS(16)
	) u_mp_npx_path (
		.clk_sys(clk),
		.reset_sys(reset),
		.clk_pix(clk_pix),
		.reset_pix(mp_reset_pix),
		.in_valid(mp_push),
		.in_r(mp_npx_r),
		.in_g(mp_npx_g),
		.in_b(mp_npx_b),
		.in_lane_valid(mp_tq_lde[MP_STORE_LAT-1] & {PRESENT_PPC{has_frame}}),
		.in_hblank(mp_tq_hb[MP_STORE_LAT-1]),
		.in_hsync(mp_tq_hs[MP_STORE_LAT-1]),
		.in_vblank(mp_tq_vb[MP_STORE_LAT-1]),
		.in_vsync(mp_tq_vs[MP_STORE_LAT-1]),
		.in_fstart(mp_tq_fs[MP_STORE_LAT-1]),
		.in_ready(mp_in_ready),
		.out_ce(mp_out_ce),
		.out_r(mp_out_r),
		.out_g(mp_out_g),
		.out_b(mp_out_b),
		.out_hblank(mp_out_hb),
		.out_hsync(mp_out_hs),
		.out_vblank(mp_out_vb),
		.out_vsync(mp_out_vs),
		.out_fstart(mp_out_fs),
		.wr_full(mp_wr_full),
		.wr_almost_full(mp_wr_af),
		.rd_underrun(mp_rd_ur),
		.rd_empty(mp_rd_empty)
	);

	// When MULTI_PIXEL is on, store read follows beam glass coords (identity clamp).
	// Note: fstore still wired to store_x/y from Template regs above — full MULTI
	// store remap is a follow-up when PPC>1 lands. PPC=1 same-clock uses leg path
	// geometry unless parent also sets FRAME 1280×720.
	wire _unused_mp_glass = |{mp_glass_x0, mp_glass_y, mp_npx_lv, mp_wr_full, mp_wr_af, mp_rd_ur, mp_rd_empty, mp_out_fs};
	wire _unused_mp_clk_hz = (MP_CLK_PIX_HZ == 0);

	assign r = mp_out_r;
	assign g = mp_out_g;
	assign b = mp_out_b;
	assign ce_pix = mp_out_ce;
	assign HBlank = mp_out_hb;
	assign HSync  = mp_out_hs;
	assign VBlank = mp_out_vb;
	assign VSync  = mp_out_vs;
	assign frame_start = mp_out_fs;
	wire _unused_leg_rgb = |{leg_r, leg_g, leg_b, ce_pix_i, hb_d, hs_d, vb_d, vs_d, fstart};
`else
	assign r = leg_r;
	assign g = leg_g;
	assign b = leg_b;
	assign ce_pix = ce_pix_i;
	assign HBlank = hb_d;
	assign HSync  = hs_d;
	assign VBlank = vb_d;
	assign VSync  = vs_d;
	assign frame_start = fstart;
`endif

	// Silence unused keep packs on default path (still noprune for hierarchy).
	wire _unused_keep_timing = |{
		keep_720_hde, keep_720_htot, keep_720_vact, keep_720_vtot,
		keep_720_hss, keep_720_hse, keep_720_vss, keep_720_vse,
		keep_720_fps_milli, keep_720_needs_fast,
		keep_960_hde, keep_960_htot, keep_960_vact, keep_960_vtot,
		keep_960_hss, keep_960_hse, keep_960_vss, keep_960_vse,
		keep_960_fps_milli, keep_960_mode30, keep_960_wide_fifo,
		content_w, content_h, clk_pix
	};

	// --- Audio: FIFO preferred, else OSD tone ---
	wire [15:0] tone_l, tone_r;
	wire [15:0] fifo_l, fifo_r;
	wire        has_audio;
	wire        fifo_underrun;

	// Test tone only for debug/idle: never under product frames (cast). When
	// has_frame is set, silence the bars-style beep even if audio_fifo underruns
	// or F2 is skipped in favor of MrAudio.
	audio_tone tone (
		.clk_audio(clk_audio),
		.reset(reset),
		.enable(audio_en && !has_audio && !has_frame),
		.freq_div(16'd54),
		.sample_l(tone_l),
		.sample_r(tone_r),
		.underrun()
	);

	audio_fifo #(
		.DEPTH(2048)
	) afifo (
		.clk_wr(clk),
		.clk_rd(clk_audio),
		.reset(reset),
		.wr_en(af_wr_en),
		.wr_data(af_wr_data),
		.wr_flush(af_wr_flush),
		.wr_full(),
		.wr_level(),
		.rd_enable(1'b1),
		.sample_l(fifo_l),
		.sample_r(fifo_r),
		.underrun(fifo_underrun),
		.has_audio(has_audio)
	);

	assign audio_l = has_audio ? fifo_l : tone_l;
	assign audio_r = has_audio ? fifo_r : tone_r;

	assign stat_display_index = disp_i;
	assign stat_content_index = cont_i;
	assign stat_advance       = advance;
	assign stat_has_frame     = has_frame;
	assign stat_wr_count      = wr_count;
	assign stat_has_audio     = has_audio;
	assign stat_audio_underrun = fifo_underrun;
	assign stat_swap_pending  = swap_pending;
	assign stat_frame_underruns = frame_underruns;
	assign stat_frame_sdram_state = frame_sdram_state;

endmodule
