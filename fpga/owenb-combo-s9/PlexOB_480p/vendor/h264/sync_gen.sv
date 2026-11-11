// h264/sync_gen.sv -- the core's raster: 720x480 / 720x576, progressive or interlaced, at 27 MHz
//
// MPX-FORK (240p, branch cbox/owenb-240p-1023): upstream eac1064 sync_gen.sv plus ONE new mode,
// the 240p progressive analog raster, selected by std == 3 with interlaced == 0 (std 3 was an
// alias of 0 upstream; nothing drove it). Everything else is bit-exact upstream.
//   240p: 262 lines at 27 MHz, 190 lines of 1719 dots + 72 lines of 1720 dots = 450,450 dots
//     a frame = 59.94006 Hz = the same dots as a 480p frame (525 x 858) or a 480i field pair /
//     2, so disp_ctl's 59.94 refresh grid (std 3 already falls on it) is unchanged. The long
//     (1720) lines are spread by a Bresenham accumulator over the 262 lines (p_acc / p_long,
//     registered; the end-of-line compare is h_cnt[10:1]==859 && h_cnt[0]==p_long, no mux into
//     hsync): every hsync edge stays within 1 dot of a uniform 1719.2748-dot grid.
//   V: active lines 0-239, vsync window [244, 247) (OPEN ITEM for the 240p lead: v3 uses 243-245),
//     line-aligned (sampled at dot 0, no half line), lines 262, last_line 261, field = 0 (VGA_F1).
//   H (parameter P240_WIDE): 1 (default, the only one emu wires) = 1440 active dots, ce_pix every
//     2nd dot (720 samples, the opt-in 480-line-class mode: the same pixel repetition the 480i
//     raster has, so the reader, fit and overlay need no change), hsync [1472, 1596) = 480i's;
//     0 = P320, 1280 active dots, ce_pix every 4th dot (320 samples), hsync [1395, 1519) (front
//     115, back 200/201: keeps the picture centre where 480i has it). P320 is NOT wired in emu
//     (the reader / fit / overlay have pixel repetition x2 only): timing-only, bench-tested.
//   v_pos = {v_cnt, FIELD1_VPOS}: the reader and overlay see the 480i SHORT, line-aligned field
//     (262 lines) every frame, and csync_smpte (prog = 0) decodes it as that field: v_pos[11:1]
//     is the line, v_pos[0] = FIELD1_VPOS the line-aligned parity (csync_240p equivalence is
//     bench-checked against prog = 1 fed the raw line).
//
// Inherited verbatim from MiSTer_UMD_Video (umd/sync_gen.sv). It keeps MiSTer_DVD's
// syncgen coordinate convention, which every overlay and display module harvested from
// those projects consumes:
//
//   h_pos  = dot index within the line, 0..H_TOTAL-1, 0 at the first active dot
//   v_pos  = line index within the frame, 0..V_TOTAL-1, 0 at the first active line
//   in interlaced mode v_pos = {v_cntr[10:0], ~odd_field}: even numbers on the odd
//   field, odd numbers on the even field (syncgen.v:415-426) -- v_pos is the FRAME line.
//
// Interlaced (P6, docs/analog.md: 480i on the 27 MHz clock with pixel repetition, the
// CEA 720(1440)x480i shape, one raster for HDMI and the analog pins) -- MiSTer_DVD's
// "N64 model", proven on real CRTs there (rtl/mpeg2/syncgen.v, its DVD-FORK notes):
//   H: every progressive horizontal value doubled -- 1440 active, 32 front porch, 124
//      sync, 120 back porch = 1716 dots (15.734 kHz); h_pos counts DOTS (0..1715), a
//      pixel is two dots, ce_pix marks the first of each pair (CE_PIXEL).
//   V: per field 240 active lines, vsync window lines [244, 247); the fields' totals
//      ALTERNATE, 262 and 263 lines, and the longer field samples its vsync window at
//      the half-line (dot 858), the shorter at dot 0 -- vsync edges exactly 262.5 lines
//      apart every field, the 2:1 interlace a CRT locks to, and 525 x 1716 = 900,900
//      dots a frame = exactly 29.97 frames / 59.94 fields a second (equal 262-line
//      fields would run 0.19 % fast). WHICH field is the short, line-aligned one is
//      h264/field_polarity.vh's FIELD1_VPOS (SMPTE field 1 = the v_pos-ODD field, set on
//      a hardware measurement in DVD -- one constant shared with csync_smpte.sv).
//   frame_tick marks the first active dot of every FIELD (a field is a refresh);
//   `field` = v_pos[0] (1 = the v_pos-odd field): VGA_F1, 0 on the top field, what
//   ascal's Weave needs (DVD issue #41).
//   A change of `interlaced` restarts the counters at the top field's first dot.
//
// The standards (P6 steps 5 / 6, docs/analog.md §10; `std`, decided 2026-09-29 with the
// maintainer): 0 = 59.94 Hz (the values below), 1 = 60.00 Hz, 2 = 50 Hz (625 lines), 3 = 240p (MPX-FORK, below).
//   60.00 Hz: the 59.94 raster with the lines one dot shorter -- 857 (interlaced 1,714) --
//      except one line in seven (the frame line count % 7 == 0: 75 of 525), which keeps 858
//      (1,716): 450,000 dots a frame (interlaced 900,000 over two fields of 450,858 /
//      449,142), exactly 60.000 Hz on the 27 MHz clock, every hsync edge within a dot of a
//      uniform grid. The short line ends a dot early (the back porch's last dot); the vsync
//      sample dot stays 858 (37 ns off the 1,714-dot line's half).
//   50 Hz: CEA-861 720x576p (720 + 12 / 64 / 68 = 864 dots; 576 + 5 / 5 / 39 = 625 lines,
//      vsync [581, 586)) = 540,000 dots = 50.000 Hz; interlaced the 1,728-dot line (1,440
//      active, hsync [1,464, 1,592)), fields of 312 / 313 lines, 288 active, vsync [292, 295),
//      half-line 864 = 1,080,000 a frame -- MiSTer_DVD's values (proven there on HDMI; PAL on
//      a CRT never hardware-confirmed, see field_polarity.vh's open risk).
//   A change of `std` restarts the counters like a change of `interlaced`.
//
// THE NATIVE RASTER (docs/hdmi_raster.md §4, stage 2; `nat`, progressive only): the active area is
//   g_w x g_h, every line g_l dots but one -- the first blank line, line g_h: g_last dots (the
//   frame's remainder there, where the reader fetches nothing: as the frame's last line, a short
//   one left the reader too little time for the next frame's first rows) -- h264/raster_geom.sv
//   makes g_nt lines total exactly the standard's dots a frame, so the refresh grid (disp_ctl's
//   free-running counter) is the same as the fitted raster's. HS on the dots [len - 14, len - 6) of
//   every line (inside the blank: g_l - g_w >= 20), VS on lines [g_h + 2, g_h + 5). The geometry
//   (and `nat` itself) is taken ONLY at a frame's first dot, never by a restart: a switch between
//   the fitted and the native raster, or between two native sizes, keeps the frame's length and
//   disp_ctl's alignment. lines / last_line report the CURRENT frame's active lines and its last
//   line's index (the reader's and the overlay's frame wrap: 480 / 524, 576 / 624 fitted).
//
// Timing (CEA-861-D 720x480p, 27.000 MHz):
//   H: 720 active, 16 front porch, 62 sync, 60 back porch  = 858 dots   (15.734 kHz)
//   V: 480 active,  9 front porch,  6 sync, 30 back porch  = 525 lines  (59.94 Hz)
// Sync pulses are emitted ACTIVE-HIGH; sys_top's sync_fix block normalises polarity
// before the pins, and hps_io measures the raster from DE and the sync edges.
//
// Latency: every output is registered once from the counters. h_pos/v_pos are aligned
// with pixel_en (the dot they describe is the dot pixel_en is high for).
//
// THE DOT ENABLE (docs/hd_raster.md S3 / S4): clk is clk_vid (81 MHz) and clk_en the 27 MHz tick --
// one clock in three. `dot` is the raster's dot: the tick on the fitted rasters, EVERY clock on the
// native one (stage B: the native raster on the 81 MHz dot, 3F dots a frame) -- but only once its
// frame has begun on a tick (nat_go), so a native frame always starts on a tick and, 3F clocks long,
// ends the clock before one: the frames keep their time against disp_ctl's grid across a switch
// either way. Everything on the raster side advances on `dot` (emu: ce_dot). Every output is a
// level for its dot (held until the next one), frame_tick included: its consumers advance on the
// same enable and see it once. With clk_en = 1 (a single-clock bench) dot = 1.

`include "h264/field_polarity.vh"

module sync_gen #(
    parameter P240_WIDE = 1          // 240p: 1 = 1440 dots / ce_pix every 2nd (wired), 0 = P320 (timing only)
) (
    input  wire        clk,
    input  wire        clk_en,       // the 27 MHz tick (1 = every clock: a single-clock bench)
    output wire        dot,          // the raster's dot (hd_raster): the tick, or every clock native
    input  wire        rst_n,
    input  wire        interlaced,   // 1 = the 480i / 576i raster (pixel repetition, fields)
    input  wire  [1:0] std,          // 0 = 59.94 Hz, 1 = 60.00 Hz (525 lines), 2 = 50 Hz (625 lines); 3 = 240p (interlaced 0)
    // the native raster (h264/raster_geom.sv; taken at a frame's first dot, progressive only)
    input  wire        nat,
    input  wire [11:0] g_w,
    input  wire [11:0] g_h,
    input  wire [11:0] g_l,
    input  wire [11:0] g_nt,
    input  wire [11:0] g_last,
    output reg  [11:0] lines,        // the current frame's active lines (progressive: 480 / 576 / g_h)
    output reg  [11:0] last_line,    // ... its last line's index (524 / 624 / g_nt - 1)

    output reg  [11:0] h_pos,        // the dot (interlaced: 0..1715, a pixel is two dots)
    output reg  [11:0] v_pos,        // the frame line (interlaced: {field line, ~odd_field})
    output reg         pixel_en,     // active picture area (DE)
    output reg         h_sync,
    output reg         v_sync,
    output reg         h_blank,
    output reg         v_blank,
    output reg         frame_tick,   // the first active dot of a frame (interlaced: of a field) -- held for the dot
    output reg         field,        // interlaced: v_pos[0] (VGA_F1); 0 in progressive
    output reg         ce_pix,       // the dot starts a pixel (CE_PIXEL): always 1 in progressive
    // (hd_raster) the mode the counters run in, registered with the dot: the tag disp_ctl's frame
    // alignment is checked against on clk_sys (a frame start of the OLD raster must not align the
    // grid after a mode change -- docs/hd_raster.md S3)
    output reg         mode_il,
    output reg  [1:0]  mode_std
);

// the standards' geometry (per the table above); 525-line values first, 625-line second
localparam H_ACTIVE  = 720;
localparam H_SS_N    = 736,  H_SS_P    = 732;           // hsync start (720 + front porch)
localparam H_SE_N    = 798,  H_SE_P    = 796;           // hsync end
localparam H_TOT_N   = 858,  H_TOT_P   = 864;
localparam V_ACT_N   = 480,  V_ACT_P   = 576;
localparam V_VS_N    = 489,  V_VS_P    = 581;           // vsync [start, end)
localparam V_VE_N    = 495,  V_VE_P    = 586;
localparam V_TOT_N   = 525,  V_TOT_P   = 625;
// the interlaced rasters: horizontal values doubled (pixel repetition), per field
localparam IH_ACTIVE = 2 * H_ACTIVE;                     // 1440
localparam IV_ACT_N  = 240,  IV_ACT_P  = 288;
localparam IV_VS_N   = 244,  IV_VS_P   = 292;            // the vsync window [VS, VE)
localparam IV_VE_N   = 247,  IV_VE_P   = 295;
localparam IV_SHORT_N = 262, IV_SHORT_P = 312;           // field 1's total; the other field one more
// the 240p raster (MPX-FORK): 262 lines, 190 x 1719 + 72 x 1720 dots = 450,450 dots a frame
localparam P_LINES   = 262, P_LONG = 72;                 // Bresenham: P_LONG long lines in P_LINES
localparam P_HA      = P240_WIDE ? 1440 : 1280;          // active dots
localparam P_HS      = P240_WIDE ? 1472 : 1395;          // hsync start; the pulse is 124 dots (480i's)
localparam P_HW      = 124;
localparam P_STEP    = P_LONG;                           // accumulator step per line
localparam P_WRAP    = P_LINES - P_LONG;                 // 190: acc >= P_WRAP -> the next line is long

reg [11:0] h_cnt;   // 0..total-1, 0 = first active dot
reg [11:0] v_cnt;   // progressive: 0..V_TOTAL-1; interlaced: the field line, 0 = first active
reg        il_q;    // the mode the counters run in
reg  [1:0] std_q;   // the standard the counters run in
reg        odd_field;                                    // 1: the field scanning v_pos-EVEN lines (top)
reg        vs_il;   // the interlaced vsync, sampled once a line at the field's reference dot
reg  [2:0] m7;      // 60.00 Hz: the frame line count mod 7 (0 = a long line)
reg        nat_q;   // the native geometry the counters run (taken at a frame's first dot)
reg [11:0] gw_q, gh_q, gl_q, gnt_q, glast_q;
reg  [8:0] p_acc;   // 240p: (line index x 72) mod 262, at the line's start
reg        p_long;  // 240p: the current line is 1720 dots (registered; decided at the previous line's end)
reg        nat_go;  // the native frame has begun on a tick (every clock a dot from then)
assign dot = clk_en | (nat_q & nat_go);
always @(posedge clk or negedge rst_n)
    if (!rst_n)       nat_go <= 1'b0;
    else if (!nat_q)  nat_go <= 1'b0;
    else if (clk_en)  nat_go <= 1'b1;

wire        p240  = (std_q == 2'd3) && !il_q;    // MPX-FORK: the 240p progressive raster
wire        pal   = (std_q == 2'd2);
wire        s60   = (std_q == 2'd1);
// the line's last dot: 59.94 858 / 1716; 60.00 857 / 1714 but 858 / 1716 on a long line; 50 864 / 1728
wire [11:0] h_tot = nat_q ? ((v_cnt == gh_q) ? glast_q : gl_q) :
                   pal ? H_TOT_P : (s60 && m7 != 3'd0) ? (H_TOT_N - 1) : H_TOT_N;
wire [11:0] ih_tot = {h_tot[10:0], 1'b0};
wire        field1 = ((~odd_field) == `FIELD1_VPOS);    // this field is SMPTE field 1 (short, line-aligned)
wire [11:0] iv_short = pal ? IV_SHORT_P : IV_SHORT_N;
wire [11:0] v_tot  = pal ? V_TOT_P : V_TOT_N;
wire        h_last_p = (h_cnt[10:1] == 10'd859) && (h_cnt[0] == p_long);   // dot 1718 / 1719
wire        h_last = p240 ? h_last_p : il_q ? (h_cnt == ih_tot - 12'd1) : (h_cnt == h_tot - 12'd1);
wire        v_last = p240 ? (v_cnt == P_LINES - 1) : il_q ? (v_cnt == (field1 ? iv_short - 12'd1 : iv_short)) :
                     nat_q ? (v_cnt == gnt_q - 12'd1) : (v_cnt == v_tot - 12'd1);
wire        nat_in = nat && !interlaced && (std != 2'd3);
wire        mode_chg = (interlaced != il_q) || (std != std_q);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        h_cnt <= 12'd0;
        v_cnt <= 12'd0;
        il_q <= 1'b0;
        std_q <= 2'd0;
        odd_field <= 1'b1;
        m7 <= 3'd0;
        p_acc <= 9'd0; p_long <= 1'b0;
        nat_q <= 1'b0; gw_q <= '0; gh_q <= '0; gl_q <= '0; gnt_q <= '0; glast_q <= '0;
    end else if (dot) begin
        if (mode_chg) begin
            // a clean restart in the new mode, at the top field's first dot
            il_q <= interlaced;
            std_q <= std;
            h_cnt <= 12'd0;
            v_cnt <= 12'd0;
            odd_field <= 1'b1;
            m7 <= 3'd0;
            p_acc <= 9'd0; p_long <= 1'b0;
            nat_q <= nat_in; gw_q <= g_w; gh_q <= g_h; gl_q <= g_l; gnt_q <= g_nt; glast_q <= g_last;
        end else begin
            h_cnt <= h_last ? 12'd0 : h_cnt + 12'd1;
            if (h_last) begin
                v_cnt <= v_last ? 12'd0 : v_cnt + 12'd1;
                // the native geometry, taken only where a frame begins (progressive: never mid-frame)
                if (v_last && !il_q) begin
                    nat_q <= nat_in; gw_q <= g_w; gh_q <= g_h; gl_q <= g_l; gnt_q <= g_nt; glast_q <= g_last;
                end
                if (v_last && il_q) odd_field <= ~odd_field;
                // 240p: the next line's length. acc = the line's (n x 72) mod 262; a line is long
                // when acc + 72 reaches 262 (so acc >= 190); the frame ends back at acc = 0.
                if (p240) begin
                    p_acc  <= v_last ? 9'd0 : p_long ? (p_acc - (9'd190)) : (p_acc + P_STEP);
                    p_long <= v_last ? 1'b0 : p_long ? ((p_acc - 9'd190) >= P_WRAP) : ((p_acc + P_STEP) >= P_WRAP);
                end
                // the frame line count mod 7, from each frame's (the top field's) first line
                m7 <= (v_last && (!il_q || !odd_field)) ? 3'd0 : (m7 == 3'd6) ? 3'd0 : m7 + 3'd1;
            end
        end
    end
end

// Decoded from the counters, registered once so the outputs share one edge.
wire h_act  = p240 ? (h_cnt < P_HA) : il_q ? (h_cnt < IH_ACTIVE) : nat_q ? (h_cnt < gw_q) : (h_cnt < H_ACTIVE);
wire v_act  = p240 ? (v_cnt < IV_ACT_N) : il_q ? (v_cnt < (pal ? IV_ACT_P : IV_ACT_N)) : nat_q ? (v_cnt < gh_q) : (v_cnt < (pal ? V_ACT_P : V_ACT_N));
wire [11:0] hs_s = pal ? H_SS_P : H_SS_N;
wire [11:0] hs_e = pal ? H_SE_P : H_SE_N;
wire hs_now = p240  ? ((h_cnt >= P_HS) && (h_cnt < P_HS + P_HW))
            : il_q  ? ((h_cnt >= {hs_s[10:0], 1'b0}) && (h_cnt < {hs_e[10:0], 1'b0}))
            : nat_q ? ((h_cnt >= h_tot - 12'd14) && (h_cnt < h_tot - 12'd6))
                    : ((h_cnt >= hs_s) && (h_cnt < hs_e));
wire vs_now = p240  ? ((v_cnt >= IV_VS_N) && (v_cnt < IV_VE_N)) :
              nat_q ? ((v_cnt >= gh_q + 12'd2) && (v_cnt < gh_q + 12'd5)) :
              pal ? ((v_cnt >= V_VS_P) && (v_cnt < V_VE_P)) : ((v_cnt >= V_VS_N) && (v_cnt < V_VE_N));
// interlaced vsync: the window sampled at the field's reference dot (dot 0 on field 1,
// the half-line on the other) -- DVD's "CRT vsync sampler"
wire [11:0] vs_ref  = field1 ? 12'd0 : (pal ? H_TOT_P : H_TOT_N);
wire        vs_win  = pal ? ((v_cnt >= IV_VS_P) && (v_cnt < IV_VE_P)) : ((v_cnt >= IV_VS_N) && (v_cnt < IV_VE_N));
wire        vs_il_n = (h_cnt == vs_ref) ? vs_win : vs_il;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        h_pos <= 12'd0; v_pos <= 12'd0;
        pixel_en <= 1'b0; h_sync <= 1'b0; v_sync <= 1'b0;
        h_blank <= 1'b1; v_blank <= 1'b1; frame_tick <= 1'b0; field <= 1'b0; ce_pix <= 1'b1;
        vs_il <= 1'b0; lines <= 12'd480; last_line <= 12'd524;
        mode_il <= 1'b0; mode_std <= 2'd0;
    end else if (dot) begin
        vs_il      <= il_q ? vs_il_n : 1'b0;
        h_pos      <= h_cnt;
        v_pos      <= p240 ? {v_cnt[10:0], `FIELD1_VPOS} : il_q ? {v_cnt[10:0], ~odd_field} : v_cnt;
        pixel_en   <= h_act & v_act;
        h_sync     <= hs_now;
        v_sync     <= il_q ? vs_il_n : vs_now;
        h_blank    <= ~h_act;
        v_blank    <= ~v_act;
        // (not in a restart's own cycle: the counters are already at 0 there and stay at 0
        // into the next, which would tick twice -- a spurious refresh at every mode switch)
        frame_tick <= (h_cnt == 12'd0) && (v_cnt == 12'd0) && !mode_chg;
        field      <= il_q & ~odd_field;
        ce_pix     <= p240 ? (P240_WIDE ? ~h_cnt[0] : (h_cnt[1:0] == 2'd0)) : il_q ? ~h_cnt[0] : 1'b1;
        lines      <= p240 ? 12'd240 : nat_q ? gh_q : pal ? 12'd576 : 12'd480;                // V_ACT_P / V_ACT_N
        last_line  <= p240 ? (P_LINES - 1) : nat_q ? (gnt_q - 12'd1) : pal ? 12'd624 : 12'd524;     // V_TOT_P - 1 / V_TOT_N - 1
        mode_il    <= il_q;
        mode_std   <= std_q;
    end
end

endmodule
