// present_content_window — NN content→DE store address map (fabric scaler V1).
//
// Purpose: evacuate ARM swscale/pad. 720p is the reward for that evacuation —
// widths/counters are sized for 1280×720 from day one (one luma line = 1 M10K).
//
// Maps DE beam (hc, py) into DDR/frame_store read coordinates.
//   win_enable=0 → legacy full-bank FRAME_W×FRAME_H (bit-compatible 480p path)
//   win_enable=1 → stretch content_w×content_h at (content_x0,content_y0) across DE
//
// Quality:
//   V1 NN (default) — SHIPS first integration fit. 0 M10K, 0 DSP.
//   V2 bilinear — `PRESENT_WINDOW_BILINEAR` (default OFF). This module exports
//     floor/ceil sample coords + Q8 fracs; `present_bilinear_lerp.sv` does 2×2.
//     Tap fetch / line hold is NOT auto-wired into present_core (fit risk).
//     Cost when fully wired: lerp 0 M10K; dual Y lines ≈2 M10K @1280 or 2nd
//     DDR fetch. Fmax: mul+>> only — OK at clk_pix 29.7 MHz (720p24 CEA) and
//     74.25 MHz class; no pixel-path divide. 4-tap polyphase: do not build
//     (ascal owns HDMI polyphase).
//
// clk_pix note (w-clock): product may run ce_pix above clk_sys 20 MHz once
// pixel PLL lands (~29.7 for true 720p24). This mapper has no dependence on
// 20 MHz — SX/SY update off ce_pix critical path; pixel path is one mul.
//
// Pixel path = mul-shift + add + clamp only. Scale dividers run when window
// regs change and land in sx_r/sy_r — off the ce_pix critical path.
//
// Arbitrary source → arbitrary DE (PLXG): content_w/h are bank/content size.
// Ship product source (parent measured): **960×540** → h_de/v_de **1280×720**
// (~4/3 non-integer — NN shimmers; enable PRESENT_WINDOW_BILINEAR on glass if ugly).
// Also: PMS 720×404 degradation tier. ARM never swscales when win_enable=1.
//
// Product 480p: h_de=529 (FBAR/Template lock), v_de=480. Do not change DE
// timing in this module; h_de/v_de are runtime so 720p DE is a parameter
// write, not a redesign.

`default_nettype none

module present_content_window #(
	// Legacy full-map domain when win_enable=0 (product 480p: 640×480).
	parameter int FRAME_W = 640,
	parameter int FRAME_H = 480,
	// Max addressable store/content (720p-native). 1280 = one M10K luma line.
	parameter int STORE_W = 1280,
	parameter int STORE_H = 720,
	// Default DE geometry (overridden by h_de/v_de ports when non-zero).
	parameter int H_DE_DEFAULT = 529,
	parameter int V_DE_DEFAULT = 480
)(
	input  wire        clk,
	input  wire        reset,
	input  wire        ce_pix,

	// DE beam — 11b covers hc to 1280 and py to 720+.
	input  wire [10:0] hc,
	input  wire [10:0] py,
	input  wire        in_content,

	// Runtime window (host mailbox / OSD). Ignored when win_enable=0.
	input  wire        win_enable,
	input  wire [10:0] content_w,   // 0..1280+
	input  wire [10:0] content_h,   // 0..720+
	input  wire [10:0] content_x0,
	input  wire [10:0] content_y0,
	// Runtime DE size for scale. 0 → use H_DE_DEFAULT / V_DE_DEFAULT.
	// 480p product holds h_de=529. 720p mode programs 1280×720 (or Template).
	input  wire [10:0] h_de,
	input  wire [10:0] v_de,

	output reg  [$clog2(STORE_W)-1:0] store_x,
	output reg  [$clog2(STORE_H)-1:0] store_y,
	output reg         de_r,
	// py past active DE rows — present_core ORs into VBlank (G-VID1).
	output wire        past_last_row
	// Bilinear sample neighbourhood (meaningful when PRESENT_WINDOW_BILINEAR).
	// When macro OFF, x1==x0, y1==y0, frac==0 → NN-equivalent if a consumer
	// still wires lerp (safe default).
`ifdef PRESENT_WINDOW_BILINEAR
	,
	output reg  [$clog2(STORE_W)-1:0] store_x1,
	output reg  [$clog2(STORE_H)-1:0] store_y1,
	output reg  [7:0]  frac_x,
	output reg  [7:0]  frac_y
`endif
);

	localparam int STORE_X_W = $clog2(STORE_W);
	localparam int STORE_Y_W = $clog2(STORE_H);

	// Legacy synthesis scales (win_enable=0, H_DE_DEFAULT=529 path).
	// store_x ≈ floor(hc * FRAME_W / 529); 39647/65536 ≈ 320/529.
	// Kept bit-compatible with pre-window present_core product math.
	localparam int STORE_X_SCALE = (FRAME_W * 39647) / 320;
	localparam int STORE_Y_SCALE = (FRAME_H * 65536) / V_DE_DEFAULT;
	localparam [15:0] FRAME_LAST_X_16 = 16'(FRAME_W - 1);
	localparam [15:0] FRAME_LAST_Y_16 = 16'(FRAME_H - 1);
	localparam [15:0] STORE_LAST_X_16 = 16'(STORE_W - 1);
	localparam [15:0] STORE_LAST_Y_16 = 16'(STORE_H - 1);

	// Effective DE denominators (0 → default). Never 0 after this.
	wire [10:0] h_de_raw = (h_de == 11'd0) ? 11'(H_DE_DEFAULT) : h_de;
	wire [10:0] v_de_raw = (v_de == 11'd0) ? 11'(V_DE_DEFAULT) : v_de;
	wire [10:0] h_de_eff = (h_de_raw == 11'd0) ? 11'd1 : h_de_raw;
	wire [10:0] v_de_eff = (v_de_raw == 11'd0) ? 11'd1 : v_de_raw;

	// Effective content geometry. win_enable=0 forces full FRAME (legacy).
	wire [10:0] cw_raw = win_enable ? content_w : 11'(FRAME_W);
	wire [10:0] ch_raw = win_enable ? content_h : 11'(FRAME_H);
	wire [10:0] cw_eff = (cw_raw == 11'd0) ? 11'd1 : cw_raw;
	wire [10:0] ch_eff = (ch_raw == 11'd0) ? 11'd1 : ch_raw;
	wire [10:0] x0_eff = win_enable ? content_x0 : 11'd0;
	wire [10:0] y0_eff = win_enable ? content_y0 : 11'd0;

	// Scale recompute (NOT on hc/py pixel multiply path).
	// Window mode — endpoint-exact NN via ceil Q16 (720p-ready, any DE):
	//   SX = ceil( (cw-1) * 65536 / (h_de-1) )
	//     = ( (cw-1)<<16 + (h_de-2) ) / (h_de-1)
	//   so (h_de-1)*SX >> 16 == cw-1 (hc last → content last).
	// Floor-only forms undershoot the last pixel after the >>16 (320→318, 1280→1278).
	// Legacy mode: STORE_X_SCALE = FRAME_W*39647/320 bit-exact 480p product.
	wire [10:0] cw_m1 = (cw_eff <= 11'd1) ? 11'd0 : (cw_eff - 11'd1);
	wire [10:0] ch_m1 = (ch_eff <= 11'd1) ? 11'd0 : (ch_eff - 11'd1);
	wire [10:0] hd_m1 = (h_de_eff <= 11'd1) ? 11'd1 : (h_de_eff - 11'd1);
	wire [10:0] vd_m1 = (v_de_eff <= 11'd1) ? 11'd1 : (v_de_eff - 11'd1);
	wire [31:0] sx_num_gen = {21'd0, cw_m1} * 32'd65536;
	wire [31:0] sy_num_gen = {21'd0, ch_m1} * 32'd65536;
	// ceil(a/b) = (a + b - 1) / b for a>=0,b>0
	wire [31:0] sx_num_ceil = sx_num_gen + {21'd0, hd_m1} - 32'd1;
	wire [31:0] sy_num_ceil = sy_num_gen + {21'd0, vd_m1} - 32'd1;
	// Combo / from live hde_act is STA-red at 24 MHz (l4-dyn3 Fmax 13.8).
	// Identity 65536 greys chroma on glass (e7097c6c). Latch DE/content
	// then divide from those regs so hde_act is not in the Div0 cone.
	reg [10:0] hd_m1_r, vd_m1_r, cw_m1_r, ch_m1_r;
	always @(posedge clk) begin
		if (reset) begin
			hd_m1_r <= 11'd1;
			vd_m1_r <= 11'd1;
			cw_m1_r <= 11'd0;
			ch_m1_r <= 11'd0;
		end else begin
			hd_m1_r <= hd_m1;
			vd_m1_r <= vd_m1;
			cw_m1_r <= cw_m1;
			ch_m1_r <= ch_m1;
		end
	end
	wire [31:0] sx_num_ceil_r = {21'd0, cw_m1_r} * 32'd65536 + {21'd0, hd_m1_r} - 32'd1;
	wire [31:0] sy_num_ceil_r = {21'd0, ch_m1_r} * 32'd65536 + {21'd0, vd_m1_r} - 32'd1;
	wire [19:0] sx_win_true = 20'(sx_num_ceil_r / {21'd0, hd_m1_r});
	wire [19:0] sy_win_true = 20'(sy_num_ceil_r / {21'd0, vd_m1_r});
	// FAULT twins (sim-only +define) — green must be capable of going red:
	//   IDENTITY_SCALE — sx=sy=65536 (midpoint fails on 720→1280)
	//   FLOOR_SCALE    — floor Q16 instead of ceil (last DE pixel undershoots)
	//   INVERT_RATIO   — (de-1)/(cw-1) instead of (cw-1)/(de-1) (ratio inverted)
`ifdef PRESENT_WINDOW_FAULT_IDENTITY_SCALE
	wire [19:0] sx_win = 20'd65536;
	wire [19:0] sy_win = 20'd65536;
`elsif PRESENT_WINDOW_FAULT_FLOOR_SCALE
	wire [19:0] sx_win = sx_num_gen[31:0] / {21'd0, hd_m1};
	wire [19:0] sy_win = sy_num_gen[31:0] / {21'd0, vd_m1};
`elsif PRESENT_WINDOW_FAULT_INVERT_RATIO
	// Invert: map as if content were the DE size — huge sx, wrong mid & last.
	wire [31:0] sx_inv_num = {21'd0, hd_m1} * 32'd65536 + {21'd0, cw_m1} - 32'd1;
	wire [31:0] sy_inv_num = {21'd0, vd_m1} * 32'd65536 + {21'd0, ch_m1} - 32'd1;
	wire [10:0] cw_den = (cw_m1 == 11'd0) ? 11'd1 : cw_m1;
	wire [10:0] ch_den = (ch_m1 == 11'd0) ? 11'd1 : ch_m1;
	wire [19:0] sx_win = sx_inv_num / {21'd0, cw_den};
	wire [19:0] sy_win = sy_inv_num / {21'd0, ch_den};
`else
	wire [19:0] sx_win = sx_win_true;
	wire [19:0] sy_win = sy_win_true;
`endif
	wire [19:0] sx_leg = 20'(STORE_X_SCALE);
	wire [19:0] sy_leg = 20'(STORE_Y_SCALE);
	wire [19:0] sx_comb = win_enable ? sx_win : sx_leg;
	wire [19:0] sy_comb = win_enable ? sy_win : sy_leg;

	wire [15:0] last_x_span = {5'd0, cw_eff} - 16'd1;
	wire [15:0] last_y_span = {5'd0, ch_eff} - 16'd1;
	wire [15:0] last_x_abs  = {5'd0, x0_eff} + last_x_span;
	wire [15:0] last_y_abs  = {5'd0, y0_eff} + last_y_span;

	// Clamp into STORE domain (not just FRAME — 720p content may exceed 640).
	wire [15:0] last_x_lim =
		(last_x_abs > STORE_LAST_X_16) ? STORE_LAST_X_16 : last_x_abs;
	wire [15:0] last_y_lim =
		(last_y_abs > STORE_LAST_Y_16) ? STORE_LAST_Y_16 : last_y_abs;

	reg [19:0] sx_r;
	reg [19:0] sy_r;
	reg [15:0] x0_r;
	reg [15:0] y0_r;
	reg [15:0] last_x_r;
	reg [15:0] last_y_r;
	reg [10:0] v_de_r;

	always @(posedge clk) begin
		if (reset) begin
			sx_r     <= 20'(STORE_X_SCALE);
			sy_r     <= 20'(STORE_Y_SCALE);
			x0_r     <= 16'd0;
			y0_r     <= 16'd0;
			last_x_r <= FRAME_LAST_X_16;
			last_y_r <= FRAME_LAST_Y_16;
			v_de_r   <= 11'(V_DE_DEFAULT);
		end else begin
			sx_r     <= sx_comb;
			sy_r     <= sy_comb;
			x0_r     <= {5'd0, x0_eff};
			y0_r     <= {5'd0, y0_eff};
			last_x_r <= win_enable ? last_x_lim : FRAME_LAST_X_16;
			last_y_r <= win_enable ? last_y_lim : FRAME_LAST_Y_16;
			v_de_r   <= v_de_eff;
		end
	end

	// Pixel path: mul-shift + origin + clamp. No divide.
	// hc(11) * sx(20) fits in 31b; Q16 field = prod[31:16] after 32b mul.
	wire [10:0] read_hc = hc;
	wire [31:0] store_x_prod = {21'd0, read_hc} * {12'd0, sx_r};
	wire [15:0] store_x_q16  = store_x_prod[31:16];
	wire [16:0] store_x_sum  = {1'b0, x0_r} + {1'b0, store_x_q16};
	wire [15:0] store_x_comb =
		(store_x_sum > {1'b0, last_x_r}) ? last_x_r : store_x_sum[15:0];

	assign past_last_row = (py >= v_de_r);
	wire [10:0] v_last = (v_de_r == 11'd0) ? 11'd0 : (v_de_r - 11'd1);
	wire [10:0] py_clamped = past_last_row ? v_last : py;
	wire [31:0] store_y_prod = {21'd0, py_clamped} * {12'd0, sy_r};
	wire [15:0] store_y_q16  = store_y_prod[31:16];
	wire [16:0] store_y_sum  = {1'b0, y0_r} + {1'b0, store_y_q16};
	wire [15:0] store_y_comb =
		(store_y_sum > {1'b0, last_y_r}) ? last_y_r : store_y_sum[15:0];

	wire [STORE_X_W-1:0] store_x_clamped = store_x_comb[STORE_X_W-1:0];
	wire [STORE_Y_W-1:0] store_y_addr    = store_y_comb[STORE_Y_W-1:0];

	// Bilinear neighbourhood: ceil sample + Q8 frac from Q16 residue.
	// frac = prod[15:8] (upper 8 of fractional 16). NOT hc[1:0]/dst-mod-4 —
	// that bug was 4/3-specialisation only (present_scale_4_3); general path
	// always takes the multiply residue. At last column/row, x1=x0 and frac
	// forced 0 so lerp collapses to NN (no OOB read).
wire [16:0] x1_sum = {1'b0, store_x_comb} + 17'd1;
	wire [16:0] y1_sum = {1'b0, store_y_comb} + 17'd1;
	wire [16:0] last_x_17 = {1'b0, last_x_r};
	wire [16:0] last_y_17 = {1'b0, last_y_r};
	wire at_x_last = ({1'b0, store_x_comb} >= last_x_17);
	wire at_y_last = ({1'b0, store_y_comb} >= last_y_17);
	wire [15:0] store_x1_comb = at_x_last ? store_x_comb :
		((x1_sum > last_x_17) ? last_x_r : x1_sum[15:0]);
	wire [15:0] store_y1_comb = at_y_last ? store_y_comb :
		((y1_sum > last_y_17) ? last_y_r : y1_sum[15:0]);
	wire [7:0] frac_x_raw = store_x_prod[15:8];
	wire [7:0] frac_y_raw = store_y_prod[15:8];
	wire [7:0] frac_x_comb = at_x_last ? 8'd0 : frac_x_raw;
	wire [7:0] frac_y_comb = at_y_last ? 8'd0 : frac_y_raw;

	always @(posedge clk) begin
		if (reset) begin
			store_x <= '0;
			store_y <= '0;
			de_r    <= 1'b0;
`ifdef PRESENT_WINDOW_BILINEAR
			store_x1 <= '0;
			store_y1 <= '0;
			frac_x   <= 8'd0;
			frac_y   <= 8'd0;
`endif
		end else if (ce_pix) begin
			de_r    <= in_content;
			store_x <= store_x_clamped;
			store_y <= store_y_addr;
`ifdef PRESENT_WINDOW_BILINEAR
			store_x1 <= store_x1_comb[STORE_X_W-1:0];
			store_y1 <= store_y1_comb[STORE_Y_W-1:0];
			frac_x   <= frac_x_comb;
			frac_y   <= frac_y_comb;
`endif
		end
	end

endmodule

`default_nettype wire
