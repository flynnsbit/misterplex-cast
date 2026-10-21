// present_vtotal_bresenham — 2-value V_TOTAL dither for exact long-run frame rate
//
// H_TOTAL fixed 1182 @ clk_sys 20 MHz. Mean V chosen so
//   hz = 20e6 / (1182 * V_mean) matches target exactly.
//
// NTSC film 24000/1001: V ∈ {705,706}, duty@706 = 1285/1773 → mean 1251250/1773
// NTSC tv   30000/1001: V ∈ {564,565}, duty@565 = 1028/1773 → mean 1001000/1773
// Exact 24:             duty@706 = 35/1773  → mean 1250000/1773
// Exact 30:             duty@565 = 28/1773  → mean 1000000/1773
//
// PARENT MISS: "72.48% at 705" is wrong — 0.72476 is the fraction at **706**.
//
// ABI: bit34 fps_1001 still RESERVED (#4). q5 uint8 cannot distinguish exact
// 24/30 from 24000/1001 / 30000/1001 (w-path buckets both to 24/30).
// PRODUCT DEFAULT = NTSC duties (library majority). Exact-integer 24/30 sources
// are therefore scanned ~23.976/29.97 (producer gains ~1 frame / 41.7s or 33.4s)
// until an awarded rational-family bit or ARM source-clocked swap lands.
// FAULT_BRES_EXACT_INT = exact 24/30 only (lab). FAULT_FIXED_VTOTAL = red fixed V.
// No claim: "exact for both NTSC and integer" under current q5 — impossible.
//
// Advance: frame_tick should be beam frame_start (v_wrap). V_out is stable for
// the frame being scanned; updates after tick for the *next* frame (beam latches
// rt_vtotal at v_wrap from the pre-update value when ordered correctly — we
// update on frame_tick using registered path so beam samples old V same cycle).

`default_nettype none

module present_vtotal_bresenham (
	input  wire        clk,
	input  wire        reset,
	input  wire        enable,       // 0 → hold base fixed V (705/564 by class)
	input  wire        film_class,   // 1 → film pair; 0 → tv pair
	input  wire        frame_tick,   // beam frame_start
	output reg  [10:0] vtotal_out,
	output wire [10:0] v_lo_out,
	output wire [10:0] v_hi_out,
	output reg         chose_hi      // 1 if last tick selected Vhi (telemetry)
);
	// den shared (1773) for all four rates at H=1182 / 20 MHz
	localparam int DEN = 1773;
	localparam int FILM_LO = 705;
	localparam int FILM_HI = 706;
	localparam int TV_LO   = 564;
	localparam int TV_HI   = 565;
	// NTSC duties at HI
	localparam int NTSC_FILM_NUM = 1285; // 1285/1773 @706
	localparam int NTSC_TV_NUM   = 1028; // 1028/1773 @565
	// Exact integer duties at HI
	localparam int INT_FILM_NUM  = 35;   // 35/1773 @706
	localparam int INT_TV_NUM    = 28;   // 28/1773 @565

`ifdef FAULT_BRES_EXACT_INT
	wire [11:0] num = film_class ? 12'(INT_FILM_NUM) : 12'(INT_TV_NUM);
`else
	// Product default: NTSC long-run ONLY (not exact 24/30 — q5 cannot tell).
	wire [11:0] num = film_class ? 12'(NTSC_FILM_NUM) : 12'(NTSC_TV_NUM);
`endif

	wire [10:0] vlo = film_class ? 11'(FILM_LO) : 11'(TV_LO);
	wire [10:0] vhi = film_class ? 11'(FILM_HI) : 11'(TV_HI);
	assign v_lo_out = vlo;
	assign v_hi_out = vhi;

	reg [11:0] acc; // 0 .. DEN-1+num headroom → 12 bits enough (1773+1285<4096)

`ifdef FAULT_FIXED_VTOTAL
	// RED twin: fixed V — long-run Hz ≠ NTSC (and ≠ exact 24/30)
	always @(posedge clk) begin
		if (reset) begin
			vtotal_out <= film_class ? 11'(FILM_LO) : 11'(TV_LO);
			chose_hi   <= 1'b0;
			acc        <= 12'd0;
		end else begin
			vtotal_out <= film_class ? 11'(FILM_LO) : 11'(TV_LO);
			chose_hi   <= 1'b0;
		end
	end
`else
	reg film_class_d;
	always @(posedge clk) begin
		if (reset) begin
			acc          <= 12'd0;
			vtotal_out   <= vlo;
			chose_hi     <= 1'b0;
			film_class_d <= film_class;
		end else if (!enable) begin
			acc          <= 12'd0;
			vtotal_out   <= vlo;
			chose_hi     <= 1'b0;
			film_class_d <= film_class;
		end else if (film_class != film_class_d) begin
			// Policy: class transition clears Bresenham phase (no retained acc
			// across film↔TV). Transient only; next frame_tick starts clean.
			acc          <= 12'd0;
			vtotal_out   <= vlo; // new class base
			chose_hi     <= 1'b0;
			film_class_d <= film_class;
		end else if (frame_tick) begin
			// Bresenham: acc += num; if acc >= DEN → HI and acc -= DEN else LO
			// Use next-acc so first frame after reset uses vlo, then evolves.
			if (acc + num >= 12'(DEN)) begin
				acc        <= acc + num - 12'(DEN);
				vtotal_out <= vhi;
				chose_hi   <= 1'b1;
			end else begin
				acc        <= acc + num;
				vtotal_out <= vlo;
				chose_hi   <= 1'b0;
			end
		end
	end
`endif
endmodule

`default_nettype wire
