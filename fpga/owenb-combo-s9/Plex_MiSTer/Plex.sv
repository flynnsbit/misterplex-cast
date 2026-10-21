//============================================================================
//  MiSTerPlex — native Plex present core
//  Phase 1: color bars + cadence + tone
//  Phase 3.0: dual-bank RGB565 frame_store via ioctl F1
//  Phase 3.1b/C3: DDRAM/f2sdram HPS YUV420p frame store (beat SPI F1)
//  Phase 3.2: present-domain audio_fifo via ioctl F2
//  Phase 3.3: elementary bitstream FIFO + NAL scanner via ioctl F3
//  Phase 3.3b: NAL typed stats + decode_stub → frame_store on VCL
//  Phase 3.3j: hybrid host F1 owns present; stub residual paint F3-only
//  Phase 3.3k: first residual CAVLC levels/runs → residual_dc paint/status
//  Phase 3.3l-1: full coeff[0:15] place + residual_csum status (prep inv_quant)
//  Copyright (C) 2026 MiSTerPlex contributors
//  GPL-2.0-or-later (MiSTer core convention)
//============================================================================

module emu
(
	`include "sys/emu_ports.vh"
);

///////// Defaults for unused ports /////////
assign ADC_BUS  = 'Z;
assign USER_OUT = '1;
assign {UART_RTS, UART_TXD, UART_DTR} = 0;
assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
// SDRAM is driven by the bring-up controller/tester below (single MiSTer stick).
// DDRAM driven by ddram_frame_rd or the C3 DDR frame store; not tied off.

assign VGA_SL = 0;
assign VGA_F1 = 0;
assign VGA_SCALER  = 0;
assign VGA_DISABLE = 0;
assign HDMI_FREEZE = 0;
assign HDMI_BLACKOUT = 0;
assign HDMI_BOB_DEINT = 0;

assign AUDIO_MIX = 0;
assign LED_DISK = 0;
assign LED_POWER = 0;
assign BUTTONS = 0;

//////////////////////////////////////////////////////////////////

wire [1:0] ar = status[122:121];
assign VIDEO_ARX = (!ar) ? 12'd4 : (ar - 1'd1);
assign VIDEO_ARY = (!ar) ? 12'd3 : 12'd0;

`include "build_id.v"
localparam CONF_STR = {
	"Plex;;",
	"-;",
	"F1,raw,RGB565 frame (320x240);",
	"F2,raw,s16le stereo PCM @48k;",
	"F3,264,H.264 annex-B elementary;",
	"-;",
	"O[122:121],Aspect ratio,Original,Full Screen,[ARC1],[ARC2];",
	// Default first option = NTSC (status[2]=0). Bump v, so saved PAL is cleared.
	"O[2],TV Mode,NTSC,PAL;",
	// O[5:4] CONTENT resolution → PMS weak ladder / DECODE (misterplexd).
	// O[15:14] DISPLAY resolution → FPGA present bank (may differ for lab A/V).
	// 0=240p (320x240), 1=480p (640x480 bank), 2/3=720p (1280x720).
	"O[5:4],Content resolution,240p,480p,720p,720p;",
	// 0=Follow content (v8-safe default); 1/2/3 force present bank independent of PMS.
	"O[15:14],Display resolution,Follow content,240p,480p,720p;",
	"-;",
	// misterplexd reads these back over UIO and applies them live (no restart).
	// Positive = hold the frame back = video LATER. Raise it when audio sounds
	// late. The MrAudio ring depth (~185ms, session-dependent) is measured and
	// subtracted by the daemon, so this knob only trims what is left downstream:
	// HDMI + the display's own video processing. Negative is normal there.
	"O[9:6],Video delay,0ms,+20ms,+40ms,+60ms,+80ms,+100ms,+120ms,+140ms,-160ms,-140ms,-120ms,-100ms,-80ms,-60ms,-40ms,-20ms;",
	"O[1],A/V auto resync,On,Off;",
	"O[3],Audio clock trim,On,Off;",
	// Idle screen is conf IDLE_SCREEN= (logo/black/screensaver/last) — bits freed
	// for Display resolution above so users can A/B content vs glass bank.
	"-;",
	"T[10],Flush audio FIFO;",
	"T[11],Flush bitstream FIFO;",
	"-;",
	"T[0],Reset;",
	"R[0],Reset and close OSD;",
	// J1 maps to joystick_0 bits 4..7; names feed MiSTer's controller mapper.
	"J1,Play/Pause,Stop,Skip Fwd,Skip Back;",
	"v,9;", // reset OSD: v9 O[15:14]=Display res (was Idle); Content still O[5:4]
	"V,v",`BUILD_DATE
};

wire forced_scandoubler;
wire   [1:0] buttons;
wire [127:0] status;
wire  [10:0] ps2_key;
wire  [31:0] joystick_0;

wire        ioctl_download;
wire        ioctl_wr;
wire [26:0] ioctl_addr;
wire  [7:0] ioctl_dout;
wire [15:0] ioctl_index;
wire is_frame_dl = (ioctl_index[5:0] == 6'd1);
wire fs_wr_ready;
wire sdram_startup_busy;
wire ioctl_wait = is_frame_dl && (sdram_startup_busy || !fs_wr_ready);

// Core→HPS status (UIO_GET_STATUS / 0x29). See docs/phase3-decode.md layout.
wire [127:0] status_in;
reg          status_set;
wire         has_frame, has_audio, has_stream, audio_underrun;
wire         has_idr, stub_busy, sps_valid, pps_valid, slice_valid, slice_is_i, has_mb_type;
wire         residual_ok;
wire [15:0]  nalu_count, stub_frames, sps_width, sps_height;
wire [7:0]   last_nal_type, idr_count, sps_count, pps_count, slice_count;
wire [7:0]   sps_profile, sps_level, slice_type, sps_mb_w, sps_mb_h, first_mb_type;
wire [5:0]   slice_qp;
wire [4:0]   residual_tc;
wire [1:0]   residual_t1;
wire signed [7:0] residual_dc;
wire [7:0]   residual_csum;
wire signed [15:0] residual_coeff [0:15];
wire         residual_place_pulse;
wire [7:0]   recon_sig;
wire [7:0]   recon_dbg;
wire         recon_dbg_valid;
wire         recon_valid;
wire [31:0]  stream_bytes_in, stream_bytes_seen;
wire [15:0]  stream_fifo_level;
wire [31:0]  wr_count;

hps_io #(.CONF_STR(CONF_STR)) hps_io
(
	.clk_sys(clk_sys),
	.HPS_BUS(HPS_BUS),
	.EXT_BUS(),
	.gamma_bus(),

	.forced_scandoubler(forced_scandoubler),

	.buttons(buttons),
	.status(status),
	.status_in(status_in),
	.status_set(status_set),
	.status_menumask(0),

	.ps2_key(ps2_key),
	.joystick_0(joystick_0),

	.ioctl_download(ioctl_download),
	.ioctl_index(ioctl_index),
	.ioctl_wr(ioctl_wr),
	.ioctl_addr(ioctl_addr),
	.ioctl_dout(ioctl_dout),
	.ioctl_wait(ioctl_wait)
);

// Playback input capture. MiSTer Main owns/grabs evdev, so commands must arrive
// through hps_io and then be published to the daemon via the DDR mailbox.
// Keyboard defaults (PS/2 Set 2): Space=Play/Pause, Esc=Stop,
// E0 Right=Skip Fwd, E0 Left=Skip Back. Controller defaults are J1 buttons 1..4.
localparam [7:0] PLX_CMD_PLAY_PAUSE = 8'h01;
localparam [7:0] PLX_CMD_STOP       = 8'h02;
localparam [7:0] PLX_CMD_SKIP_FWD   = 8'h03;
localparam [7:0] PLX_CMD_SKIP_BACK  = 8'h04;

reg        ps2_toggle_d;
reg [31:0] joystick_0_d;
reg        playback_cmd_valid;
reg  [7:0] playback_cmd;

wire       ps2_event = ps2_key[10] ^ ps2_toggle_d;
wire       ps2_press = ps2_event & ps2_key[9];
wire [7:0] ps2_code  = ps2_key[7:0];
wire       ps2_ext   = ps2_key[8];
wire [31:0] joy_rise = joystick_0 & ~joystick_0_d;

wire key_play_pause = ps2_press & ~ps2_ext & (ps2_code == 8'h29); // Space
wire key_stop       = ps2_press & ~ps2_ext & (ps2_code == 8'h76); // Esc
wire key_skip_fwd   = ps2_press &  ps2_ext & (ps2_code == 8'h74); // Right arrow
wire key_skip_back  = ps2_press &  ps2_ext & (ps2_code == 8'h6B); // Left arrow

always @(posedge clk_sys) begin
	if (reset) begin
		ps2_toggle_d       <= ps2_key[10];
		joystick_0_d       <= 32'd0;
		playback_cmd_valid <= 1'b0;
		playback_cmd       <= 8'd0;
	end else begin
		ps2_toggle_d       <= ps2_key[10];
		joystick_0_d       <= joystick_0;
		playback_cmd_valid <= 1'b0;
		playback_cmd       <= 8'd0;

		if (key_play_pause | joy_rise[4]) begin
			playback_cmd_valid <= 1'b1;
			playback_cmd       <= PLX_CMD_PLAY_PAUSE;
		end else if (key_stop | joy_rise[5]) begin
			playback_cmd_valid <= 1'b1;
			playback_cmd       <= PLX_CMD_STOP;
		end else if (key_skip_fwd | joy_rise[6]) begin
			playback_cmd_valid <= 1'b1;
			playback_cmd       <= PLX_CMD_SKIP_FWD;
		end else if (key_skip_back | joy_rise[7]) begin
			playback_cmd_valid <= 1'b1;
			playback_cmd       <= PLX_CMD_SKIP_BACK;
		end
	end
end

///////////////////////   CLOCKS   ///////////////////////////////

wire clk_sys;
wire clk_sdram;
wire clk_ddr;
`ifdef PRESENT_CLK_PIX_PLL
wire clk_pix_pll;
`endif
wire pll_locked;
pll pll
(
	.refclk(CLK_50M),
	.rst(0),
	.outclk_0(clk_sys),
	.outclk_1(clk_sdram),
	.outclk_2(clk_ddr),
`ifdef PRESENT_CLK_PIX_PLL
	.outclk_3(clk_pix_pll),
`endif
	.locked(pll_locked)
);

wire reset = RESET | status[0] | buttons[1];

// O[5:4] CONTENT (PMS/DECODE) — misterplexd OSD mailbox.
// O[15:14] DISPLAY present bank (v9+). Glass geometry follows display; content
// may differ for lab A/B. Softc FRAME_W/H may still be 1280x720 compile-time;
// runtime DDR layout comes from SPI. L4 mux still uses content 480p bit for the
// 320/640 ladder when PLXG is idle (720p via FABRIC_NATIVE / PLXG).
wire [1:0]  content_res_sel     = status[5:4];
// Display: 0=follow content, 1=240p, 2=480p, 3=720p
wire [1:0]  display_res_menu    = status[15:14];
wire [1:0]  display_res_sel     = (display_res_menu == 2'd0) ? content_res_sel :
                                  (display_res_menu == 2'd1) ? 2'd0 :
                                  (display_res_menu == 2'd2) ? 2'd1 : 2'd2;
wire        content_res_640x480 = (content_res_sel == 2'd1);
wire        content_res_720p    = (content_res_sel >= 2'd2);
wire        display_res_640x480 = (display_res_sel == 2'd1);
wire        display_res_720p    = (display_res_sel >= 2'd2);
wire [10:0] content_width       = display_res_720p ? 11'd1280 :
                                  (display_res_640x480 ? 11'd640 : 11'd320);
wire [10:0] content_height      = display_res_720p ? 11'd720 :
                                  (display_res_640x480 ? 11'd480 : 11'd240);

// ---------------------------------------------------------------------------
// L4 720p present geom hierarchy (DEFAULT OFF via PLEX_PRESENT_720P_L4).
// Instantiates present_geom_latch + plex_present_geom_mux so they are not
// QIP-only dead code. Poller (plxg_ddr_poller) is w-mem — not this land;
// latch wr_en/commit stay 0 here. Enable recipe also sets
// FABRIC_NATIVE_720P_GEOM so mux forces 1280×720 static geometry until
// poller lands. Product default (macro off) does not elaborate this block.
// ---------------------------------------------------------------------------
`ifdef PLEX_PRESENT_720P_L4
wire        plxg_wr_en = 1'b0;
wire [2:0]  plxg_wr_idx = 3'd0;
wire [63:0] plxg_wr_data = 64'd0;
wire        plxg_commit = 1'b0;
wire        plxg_frame_boundary = 1'b0; // promote unused until poller+vsync wire
wire        plxg_win_en, plxg_geom_en, plxg_live_valid;
wire [10:0] plxg_cw, plxg_ch, plxg_cx0, plxg_cy0;
wire [11:0] plxg_hde12, plxg_vde12;
wire [10:0] plxg_hde = plxg_hde12[10:0];
wire [10:0] plxg_vde = plxg_vde12[10:0];
wire [10:0] plxg_coded_w, plxg_coded_h;
wire [11:0] plxg_y_stride;
wire [10:0] plxg_c_stride, plxg_dw, plxg_dh, plxg_px, plxg_py, plxg_cl, plxg_ct;
wire        plxg_dar_valid, plxg_fps_valid;
wire [11:0] plxg_dar_x, plxg_dar_y;
wire [7:0]  plxg_content_fps;
wire [15:0] plxg_live_seq;
wire [13:0] plxg_live_epoch;
wire        plxg_pending_valid, plxg_promote_pulse;

(* noprune *) present_geom_latch u_plxg_latch (
	.clk(clk_sys),
	.reset(reset),
	.wr_en(plxg_wr_en),
	.wr_idx(plxg_wr_idx),
	.wr_data(plxg_wr_data),
	.commit(plxg_commit),
	.frame_boundary(plxg_frame_boundary),
	.win_enable(plxg_win_en),
	.geom_enable(plxg_geom_en),
	.content_w(plxg_cw),
	.content_h(plxg_ch),
	.content_x0(plxg_cx0),
	.content_y0(plxg_cy0),
	.h_de(plxg_hde12),
	.v_de(plxg_vde12),
	.coded_w(plxg_coded_w),
	.coded_h(plxg_coded_h),
	.y_stride(plxg_y_stride),
	.chroma_stride(plxg_c_stride),
	.display_w(plxg_dw),
	.display_h(plxg_dh),
	.present_x(plxg_px),
	.present_y(plxg_py),
	.crop_left(plxg_cl),
	.crop_top(plxg_ct),
	.dar_valid(plxg_dar_valid),
	.dar_x(plxg_dar_x),
	.dar_y(plxg_dar_y),
	.fps_valid(plxg_fps_valid),
	.content_fps(plxg_content_fps),
	.live_valid(plxg_live_valid),
	.live_seq(plxg_live_seq),
	.live_epoch(plxg_live_epoch),
	.pending_valid(plxg_pending_valid),
	.promote_pulse(plxg_promote_pulse)
);

wire        present_win_enable, present_geom_enable;
wire [10:0] present_content_w, present_content_h;
wire [10:0] present_content_x0, present_content_y0;
wire [10:0] present_win_h_de, present_win_v_de;
wire [10:0] present_geom_coded_w, present_geom_coded_h;
wire [11:0] present_geom_y_stride;
wire [10:0] present_geom_chroma_stride;
wire [10:0] present_geom_display_w, present_geom_display_h;
wire [10:0] present_geom_present_x, present_geom_present_y;
wire [10:0] present_geom_crop_left, present_geom_crop_top;

(* noprune *) plex_present_geom_mux u_present_geom_mux (
	.content_res_640x480(content_res_640x480),
	.plxg_live_valid(plxg_live_valid),
	.plxg_win_en(plxg_win_en),
	.plxg_geom_en(plxg_geom_en),
	.plxg_cw(plxg_cw),
	.plxg_ch(plxg_ch),
	.plxg_cx0(plxg_cx0),
	.plxg_cy0(plxg_cy0),
	.plxg_hde(plxg_hde),
	.plxg_vde(plxg_vde),
	.plxg_coded_w(plxg_coded_w),
	.plxg_coded_h(plxg_coded_h),
	.plxg_y_stride(plxg_y_stride),
	.plxg_c_stride(plxg_c_stride),
	.plxg_dw(plxg_dw),
	.plxg_dh(plxg_dh),
	.plxg_px(plxg_px),
	.plxg_py(plxg_py),
	.plxg_cl(plxg_cl),
	.plxg_ct(plxg_ct),
	.present_win_enable(present_win_enable),
	.present_geom_enable(present_geom_enable),
	.content_width(present_content_w),
	.content_height(present_content_h),
	.present_content_x0(present_content_x0),
	.present_content_y0(present_content_y0),
	.present_win_h_de(present_win_h_de),
	.present_win_v_de(present_win_v_de),
	.present_geom_coded_w(present_geom_coded_w),
	.present_geom_coded_h(present_geom_coded_h),
	.present_geom_y_stride(present_geom_y_stride),
	.present_geom_chroma_stride(present_geom_chroma_stride),
	.present_geom_display_w(present_geom_display_w),
	.present_geom_display_h(present_geom_display_h),
	.present_geom_present_x(present_geom_present_x),
	.present_geom_present_y(present_geom_present_y),
	.present_geom_crop_left(present_geom_crop_left),
	.present_geom_crop_top(present_geom_crop_top)
);

// Anti-DCE: L4 hierarchy must survive map even before poller.
(* keep = 1 *) wire _keep_l4_geom =
	present_win_enable | present_geom_enable | |present_content_w | |present_content_h |
	|present_win_h_de | |present_win_v_de | plxg_live_valid | |plxg_live_seq |
	plxg_pending_valid | plxg_promote_pulse | |present_geom_y_stride |
	|present_content_x0 | |present_content_y0 | |present_geom_display_w;
`endif

// Legacy cadence input is now fixed; the daemon handles exact content pacing.
wire [7:0] content_fps = 8'd24;

// Refresh interval = floor((f_MHz * 64_000us / 8192 rows) - 1). The
// counter toggles refresh after REFRESH_CYCLES+1 clocks, so every option
// refreshes at least as often as the 7.8125us SDRAM row budget.
`ifdef SDRAM_CLK_142
localparam int SDRAM_CLK_HZ = 142_000_000;
localparam int SDRAM_REFRESH_CYCLES = 1108;
`elsif SDRAM_CLK_133
localparam int SDRAM_CLK_HZ = 133_333_333;
localparam int SDRAM_REFRESH_CYCLES = 1040;
`elsif SDRAM_CLK_120
localparam int SDRAM_CLK_HZ = 120_000_000;
localparam int SDRAM_REFRESH_CYCLES = 936;
`elsif SDRAM_CLK_110
localparam int SDRAM_CLK_HZ = 110_000_000;
localparam int SDRAM_REFRESH_CYCLES = 858;
`elsif SDRAM_CLK_80
localparam int SDRAM_CLK_HZ = 80_000_000;
localparam int SDRAM_REFRESH_CYCLES = 624;
`elsif SDRAM_CLK_75
localparam int SDRAM_CLK_HZ = 75_000_000;
localparam int SDRAM_REFRESH_CYCLES = 584;
`elsif SDRAM_CLK_50
localparam int SDRAM_CLK_HZ = 50_000_000;
localparam int SDRAM_REFRESH_CYCLES = 389;
`else
localparam int SDRAM_CLK_HZ = 100_000_000;
localparam int SDRAM_REFRESH_CYCLES = 780;
`endif

`ifndef FRAME_W
`define FRAME_W 320
`endif
`ifndef FRAME_H
`define FRAME_H 240
`endif
localparam int FRAME_W = `FRAME_W;
localparam int FRAME_H = `FRAME_H;
`ifdef FRAME_STRIDE
localparam int FRAME_STRIDE = `FRAME_STRIDE;
`else
localparam int FRAME_STRIDE = FRAME_W;
`endif
`ifdef DDR_FRAME_STORE
localparam int FRAME_BYTES = FRAME_W * FRAME_H * 3 / 2;
`else
localparam int FRAME_BYTES = FRAME_STRIDE * FRAME_H * 2;
`endif
localparam int HPS_BANK_STRIDE_BYTES =
	(FRAME_BYTES <= 262144)  ? 262144  :
	(FRAME_BYTES <= 1048576) ? 1048576 :
	(FRAME_BYTES <= 2097152) ? 2097152 : 4194304;

// Single-stick SDRAM controller. At cold start the destructive B1 memtest owns
// the stick, publishes PLXM, then hands the port to the B2 frame store.
wire        sdram_ctl_sel;
wire [26:1] sdram_ctl_addr;
wire [15:0] sdram_dout;
wire [15:0] sdram_ctl_din;
wire        sdram_ctl_wr;
wire        sdram_ctl_rd;
wire  [1:0] sdram_ctl_bs;
wire        sdram_ready;
wire        sdram_ctl_refresh;
wire        sdram_test_sel;
wire [26:1] sdram_test_addr;
wire [15:0] sdram_test_din;
wire        sdram_test_wr;
wire        sdram_test_rd;
wire  [1:0] sdram_test_bs;
wire        sdram_test_refresh;
wire        frame_sdram_sel;
wire [26:1] frame_sdram_addr;
wire [15:0] frame_sdram_din;
wire        frame_sdram_wr;
wire        frame_sdram_rd;
wire  [1:0] frame_sdram_bs;
wire        frame_sdram_refresh;
wire  [3:0] sdram_test_state;
wire  [3:0] sdram_size_code;
wire [15:0] sdram_error_count;
wire [15:0] sdram_read_sample;
wire        sdram_first_fail_valid;
wire [25:0] sdram_first_fail_addr;
wire [15:0] sdram_first_fail_expect;
wire        sdram_test_done;
wire        sdram_test_pass;
wire [15:0] frame_underruns;
wire  [7:0] frame_sdram_state;
wire        sdram_test_active = !sdram_test_done;

`ifdef DDR_FRAME_STORE
assign SDRAM_DQ = 'Z;
assign SDRAM_A = '0;
assign SDRAM_DQML = 1'b1;
assign SDRAM_DQMH = 1'b1;
assign SDRAM_BA = '0;
assign SDRAM_nCS = 1'b1;
assign SDRAM_nWE = 1'b1;
assign SDRAM_nRAS = 1'b1;
assign SDRAM_nCAS = 1'b1;
assign SDRAM_CKE = 1'b0;
assign SDRAM_CLK = 1'b0;
assign sdram_dout = 16'd0;
assign sdram_ready = 1'b0;
assign sdram_test_state = 4'd0;
assign sdram_size_code = 4'd0;
assign sdram_error_count = 16'd0;
assign sdram_read_sample = 16'd0;
assign sdram_first_fail_valid = 1'b0;
assign sdram_first_fail_addr = 26'd0;
assign sdram_first_fail_expect = 16'd0;
assign sdram_test_done = 1'b1;
assign sdram_test_pass = 1'b0;
assign sdram_startup_busy = 1'b0;
`else
sdram_memtest #(
	.REFRESH_CYCLES(SDRAM_REFRESH_CYCLES)
) sdram_test (
	.clk(clk_sdram),
	.reset(reset | ~pll_locked),
	.sdram_dout(sdram_dout),
	.sdram_ready(sdram_ready),
	.sdram_sel(sdram_test_sel),
	.sdram_addr(sdram_test_addr),
	.sdram_din(sdram_test_din),
	.sdram_wr(sdram_test_wr),
	.sdram_rd(sdram_test_rd),
	.sdram_bs(sdram_test_bs),
	.sdram_refresh(sdram_test_refresh),
	.state_code(sdram_test_state),
	.size_code(sdram_size_code),
	.error_count(sdram_error_count),
	.read_sample(sdram_read_sample),
	.first_fail_valid(sdram_first_fail_valid),
	.first_fail_addr(sdram_first_fail_addr),
	.first_fail_expect(sdram_first_fail_expect),
	.done(sdram_test_done),
	.pass(sdram_test_pass)
);
wire _sdram_test_pass_unused = sdram_test_pass;

assign sdram_ctl_sel     = sdram_test_active ? sdram_test_sel     : frame_sdram_sel;
assign sdram_ctl_addr    = sdram_test_active ? sdram_test_addr    : frame_sdram_addr;
assign sdram_ctl_din     = sdram_test_active ? sdram_test_din     : frame_sdram_din;
assign sdram_ctl_wr      = sdram_test_active ? sdram_test_wr      : frame_sdram_wr;
assign sdram_ctl_rd      = sdram_test_active ? sdram_test_rd      : frame_sdram_rd;
assign sdram_ctl_bs      = sdram_test_active ? sdram_test_bs      : frame_sdram_bs;
assign sdram_ctl_refresh = sdram_test_active ? sdram_test_refresh : frame_sdram_refresh;

reg sdram_test_done_s1, sdram_test_done_s2;
always @(posedge clk_sys) begin
	if (reset) begin
		sdram_test_done_s1 <= 1'b0;
		sdram_test_done_s2 <= 1'b0;
	end else begin
		sdram_test_done_s1 <= sdram_test_done;
		sdram_test_done_s2 <= sdram_test_done_s1;
	end
end
assign sdram_startup_busy = !sdram_test_done_s2;

sdram #(
	.SDRAM_CLK_HZ(SDRAM_CLK_HZ)
) sdram_ctl (
	.init(reset | ~pll_locked),
	.clk(clk_sdram),
	.SDRAM_DQ(SDRAM_DQ),
	.SDRAM_A(SDRAM_A),
	.SDRAM_DQML(SDRAM_DQML),
	.SDRAM_DQMH(SDRAM_DQMH),
	.SDRAM_BA(SDRAM_BA),
	.SDRAM_nCS(SDRAM_nCS),
	.SDRAM_nWE(SDRAM_nWE),
	.SDRAM_nRAS(SDRAM_nRAS),
	.SDRAM_nCAS(SDRAM_nCAS),
	.SDRAM_CKE(SDRAM_CKE),
	.SDRAM_CLK(SDRAM_CLK),
	.SDRAM_EN(1'b1),
	.sel(sdram_ctl_sel),
	.addr(sdram_ctl_addr),
	.dout(sdram_dout),
	.din(sdram_ctl_din),
	.wr(sdram_ctl_wr),
	.bs(sdram_ctl_bs),
	.rd(sdram_ctl_rd),
	.ready(sdram_ready),
	.refresh(sdram_ctl_refresh),
	.cpsel(1'b0),
	.cpaddr(26'd0),
	.cpdin(16'd0),
	.cprd(),
	.cpreq(1'b0),
	.cpbusy()
);
`endif

`ifdef DDR_FRAME_STORE
wire present_reset = reset;
`else
wire present_reset = reset | sdram_startup_busy;
`endif

wire [7:0] display_hz = status[2] ? 8'd50 : 8'd60; // PAL/NTSC family

// F1 = frame (1), F2 = audio (2), F3 = elementary bitstream (3)
wire is_audio_dl = (ioctl_index[5:0] == 6'd2);
wire is_stream_dl = (ioctl_index[5:0] == 6'd3);

// Frame ingest from F1
wire        f1_wr_en;
wire [15:0] f1_wr_pixel;
wire        f1_wr_reset;
wire        f1_swap;
wire [31:0] ingest_pixels;
wire        ingest_dl;

frame_ingest finst (
	.clk(clk_sys),
	.reset(reset),
	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_dout(ioctl_dout),
	.ioctl_addr(ioctl_addr),
	.ioctl_index(ioctl_index),
	.enable(is_frame_dl),
	.wr_en(f1_wr_en),
	.wr_pixel(f1_wr_pixel),
	.wr_reset_ptr(f1_wr_reset),
	.swap_req(f1_swap),
	.pixels_written(ingest_pixels),
	.downloading(ingest_dl)
);

// Phase 3.1b: HPS mmap @ 0x30000000 → DDRAM burst → frame_store
// status[12]=start (rising edge), status[13]=bank (0/1)
// swap_pending comes from present_core/frame_store (declared below; wire OK early).
wire        ddr_wr_en;
wire [15:0] ddr_wr_pixel;
wire        ddr_wr_reset;
wire        ddr_swap;
wire        ddr_busy;
wire [15:0] ddr_frames;
wire        ddr_doorbell_ok;
wire        swap_pending;

`ifdef DDR_FRAME_STORE
assign ddr_wr_en = 1'b0;
assign ddr_wr_pixel = 16'd0;
assign ddr_wr_reset = 1'b0;
assign ddr_swap = 1'b0;
assign ddr_busy = swap_pending;
`else
ddram_frame_rd #(
	.WIDTH(FRAME_W),
	.HEIGHT(FRAME_H),
	.PHYS_BASE(32'h3000_0000),
	.BURST(32)
) ddr_fr (
	.clk(clk_sys),
	.reset(reset),
	.start_req(status[12]),
	.bank_sel(status[13]),
	.swap_pending(swap_pending),
	// Publish the live OSD word to HPS DDR so misterplexd never has to read it
	// back over the SPI bus that Main_MiSTer owns.
	.status_osd(status[15:0]),
	.input_cmd_valid(playback_cmd_valid),
	.input_cmd(playback_cmd),
	.sdram_test_state(sdram_test_state),
	.sdram_size_code(sdram_size_code),
	.sdram_error_count(sdram_error_count),
	.sdram_read_sample(sdram_read_sample),
	.sdram_first_fail_valid(sdram_first_fail_valid),
	.sdram_first_fail_addr(sdram_first_fail_addr),
	.sdram_first_fail_expect(sdram_first_fail_expect),
	.frame_sdram_state(frame_sdram_state),
	.frame_underrun_count(frame_underruns),
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
	.wr_en(ddr_wr_en),
	.wr_pixel(ddr_wr_pixel),
	.wr_reset_ptr(ddr_wr_reset),
	.swap_req(ddr_swap),
	.wr_ready(fs_wr_ready && !sdram_startup_busy),
	.busy(ddr_busy),
	.frames_done(ddr_frames)
);
assign ddr_doorbell_ok = 1'b0;
`endif

// Audio ingest from F2
wire        af_wr_en;
wire [31:0] af_wr_data;
wire        af_wr_flush;
wire        af_active;

audio_ingest ainst (
	.clk(clk_sys),
	.reset(reset | sdram_startup_busy),
	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_dout(ioctl_dout),
	.ioctl_index(ioctl_index),
	.enable(is_audio_dl),
	.wr_en(af_wr_en),
	.wr_data(af_wr_data),
	.wr_flush(af_wr_flush),
	.active(af_active)
);

// Phase 3.3/3.3b/3.3c: annex-B → NAL stats → SPS parse + decode_stub → frame_store
wire        stub_wr_en;
wire [15:0] stub_wr_pixel;
wire        stub_wr_reset;
wire        stub_swap;
wire        stream_ddr_active;
wire [31:0] stream_ddr_bytes_out;
wire [15:0] stream_ddr_underruns;
wire [15:0] stream_ddr_overruns;
wire [31:0] stream_ddr_host_write;
wire [31:0] stream_ddr_fpga_read;
wire        stream_ddr_bus_want;
wire        stream_ddr_busy;
wire  [7:0] stream_ddr_burstcnt;
wire [28:0] stream_ddr_addr;
wire [63:0] stream_ddr_dout;
wire        stream_ddr_dout_ready;
wire        stream_ddr_rd;
wire [63:0] stream_ddr_din;
wire  [7:0] stream_ddr_be;
wire        stream_ddr_we;
`ifdef DDR_FRAME_STORE
wire        stream_ddr_enable = 1'b1;
`else
wire        stream_ddr_enable = 1'b0;
`endif

`ifndef DDR_FRAME_STORE
assign stream_ddr_busy = 1'b1;
assign stream_ddr_dout = 64'd0;
assign stream_ddr_dout_ready = 1'b0;
`endif

// L4 720p present is ARM ffmpeg + ddr_frame_store. stream_path is the fabric
// H.264 decoder (l4-dyn2: 10088 ALUTs / 291 M10K) and holds clk_sys Fmax at
// 13.99 MHz. Product (macro off) still instantiates it.
`ifndef PLEX_PRESENT_720P_L4
stream_path #(
	.FRAME_W(FRAME_W),
	.FRAME_H(FRAME_H)
) spath (
	.clk(clk_sys),
	.reset(reset),
	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_dout(ioctl_dout),
	.enable(is_stream_dl),
	.flush(status[11]),
	.ddr_stream_enable(stream_ddr_enable),
	.ddr_bus_want(stream_ddr_bus_want),
	.ddr_busy(stream_ddr_busy),
	.ddr_burstcnt(stream_ddr_burstcnt),
	.ddr_addr(stream_ddr_addr),
	.ddr_dout(stream_ddr_dout),
	.ddr_dout_ready(stream_ddr_dout_ready),
	.ddr_rd(stream_ddr_rd),
	.ddr_din(stream_ddr_din),
	.ddr_be(stream_ddr_be),
	.ddr_we(stream_ddr_we),
	.has_stream(has_stream),
	.nalu_count(nalu_count),
	.last_nal_type(last_nal_type),
	.bytes_in(stream_bytes_in),
	.bytes_seen(stream_bytes_seen),
	.fifo_level(stream_fifo_level),
	.stream_ddr_active(stream_ddr_active),
	.stream_ddr_bytes_out(stream_ddr_bytes_out),
	.stream_ddr_underruns(stream_ddr_underruns),
	.stream_ddr_overruns(stream_ddr_overruns),
	.stream_ddr_host_write(stream_ddr_host_write),
	.stream_ddr_fpga_read(stream_ddr_fpga_read),
	.has_idr(has_idr),
	.idr_count(idr_count),
	.sps_count(sps_count),
	.pps_count(pps_count),
	.slice_count(slice_count),
	.stub_frames(stub_frames),
	.stub_busy(stub_busy),
	.sps_valid(sps_valid),
	.sps_profile(sps_profile),
	.sps_level(sps_level),
	.sps_width(sps_width),
	.sps_height(sps_height),
	.sps_mb_w(sps_mb_w),
	.sps_mb_h(sps_mb_h),
	.pps_valid(pps_valid),
	.slice_valid(slice_valid),
	.slice_type(slice_type),
	.slice_is_i(slice_is_i),
	.first_mb_type(first_mb_type),
	.has_mb_type(has_mb_type),
	.slice_qp(slice_qp),
	.residual_tc(residual_tc),
	.residual_t1(residual_t1),
	.residual_ok(residual_ok),
	.residual_dc(residual_dc),
	.residual_csum(residual_csum),
	.residual_coeff(residual_coeff),
	.residual_place_pulse(residual_place_pulse),
	.recon_sig(recon_sig),
	.recon_dbg(recon_dbg),
	.recon_dbg_valid(recon_dbg_valid),
	.recon_valid(recon_valid),
	.fs_wr_en(stub_wr_en),
	.fs_wr_pixel(stub_wr_pixel),
	.fs_wr_reset(stub_wr_reset),
	.fs_swap(stub_swap)
);
`else
assign has_stream = 1'b0;
assign nalu_count = 16'd0;
assign last_nal_type = 8'd0;
assign stream_bytes_in = 32'd0;
assign stream_bytes_seen = 32'd0;
assign stream_fifo_level = 16'd0;
assign stream_ddr_active = 1'b0;
assign stream_ddr_bytes_out = 32'd0;
assign stream_ddr_underruns = 16'd0;
assign stream_ddr_overruns = 16'd0;
assign stream_ddr_host_write = 32'd0;
assign stream_ddr_fpga_read = 32'd0;
assign stream_ddr_bus_want = 1'b0;
assign stream_ddr_rd = 1'b0;
assign stream_ddr_we = 1'b0;
assign stream_ddr_burstcnt = 8'd0;
assign stream_ddr_addr = 29'd0;
assign stream_ddr_din = 64'd0;
assign stream_ddr_be = 8'd0;
assign has_idr = 1'b0;
assign idr_count = 8'd0;
assign sps_count = 8'd0;
assign pps_count = 8'd0;
assign slice_count = 8'd0;
assign stub_frames = 16'd0;
assign stub_busy = 1'b0;
assign sps_valid = 1'b0;
assign sps_profile = 8'd0;
assign sps_level = 8'd0;
assign sps_width = 16'd0;
assign sps_height = 16'd0;
assign sps_mb_w = 8'd0;
assign sps_mb_h = 8'd0;
assign pps_valid = 1'b0;
assign slice_valid = 1'b0;
assign slice_type = 8'd0;
assign slice_is_i = 1'b0;
assign first_mb_type = 8'd0;
assign has_mb_type = 1'b0;
assign slice_qp = 6'd0;
assign residual_tc = 5'd0;
assign residual_t1 = 2'd0;
assign residual_ok = 1'b0;
assign residual_dc = 8'sd0;
assign residual_csum = 8'd0;
assign residual_place_pulse = 1'b0;
assign recon_sig = 8'd0;
assign recon_dbg = 8'd0;
assign recon_dbg_valid = 1'b0;
assign recon_valid = 1'b0;
assign stub_wr_en = 1'b0;
assign stub_wr_pixel = 16'd0;
assign stub_wr_reset = 1'b0;
assign stub_swap = 1'b0;
genvar sp_i;
generate
	for (sp_i = 0; sp_i < 16; sp_i = sp_i + 1) begin : g_l4_res_coeff
		assign residual_coeff[sp_i] = 16'sd0;
	end
endgenerate
`endif

// Phase 3.3j / 3.1b hybrid present:
//   Host F1 SPI or DDR bulk owns product frame_store once any host frame has
//   swapped. decode_stub F3 diagnostic paint is suppressed after that.
//   Priority while writing: F1 ioctl download > DDR DMA > stub.
reg host_owns_fs;
always @(posedge clk_sys) begin
	if (reset)
		host_owns_fs <= 1'b0;
	else if (f1_swap | ddr_swap)
		host_owns_fs <= 1'b1;
end

wire        stub_allow  = ~host_owns_fs & ~ingest_dl & ~ddr_busy;
wire        host_wr     = ingest_dl | f1_wr_en | ddr_wr_en;
wire        fs_wr_en    = ingest_dl ? f1_wr_en
	                      : ddr_busy  ? ddr_wr_en
	                      : (stub_allow ? (stub_wr_en | f1_wr_en) : f1_wr_en);
wire [15:0] fs_wr_pixel = ingest_dl ? f1_wr_pixel
	                      : ddr_wr_en ? ddr_wr_pixel
	                      : (f1_wr_en ? f1_wr_pixel : stub_wr_pixel);
wire        fs_wr_reset = f1_wr_reset | ddr_wr_reset | (stub_wr_reset & stub_allow);
wire        fs_swap     = f1_swap | ddr_swap | (stub_swap & stub_allow);
wire        _host_wr_unused = host_wr;

wire ce_pix, HBlank, HSync, VBlank, VSync;
wire [7:0] r, g, b;
wire [15:0] al, ar_audio;
wire [31:0] disp_i, cont_i;
wire advance;
// swap_pending declared above (fed back into ddram_frame_rd hold-off)

`ifdef DDR_FRAME_STORE
wire        present_ddr_busy;
wire  [7:0] present_ddr_burstcnt;
wire [28:0] present_ddr_addr;
wire [63:0] present_ddr_dout;
wire        present_ddr_dout_ready;
wire        present_ddr_rd;
wire [63:0] present_ddr_din;
wire  [7:0] present_ddr_be;
wire        present_ddr_we;
`endif

present_core #(
	.FRAME_W(FRAME_W),
	.FRAME_H(FRAME_H),
	.FRAME_STRIDE(FRAME_STRIDE),
	.SDRAM_REFRESH_CYCLES(SDRAM_REFRESH_CYCLES)
) present (
	.clk(clk_sys),
	.clk_sdram(clk_sdram),
	.clk_audio(CLK_AUDIO),
	// clk_pix: product default ties to clk_sys. PRESENT_CLK_PIX_PLL (default
	// OFF) drives outclk_3 = 29.70 MHz (or 74.25 with PRESENT_CLK_PIX_74_25).
`ifdef PRESENT_CLK_PIX_PLL
	.clk_pix(clk_pix_pll),
`else
	.clk_pix(clk_sys),
`endif
	.reset(present_reset),
`ifdef DDR_FRAME_STORE
	.ioctl_download(ioctl_download),
	.ioctl_wr(ioctl_wr),
	.ioctl_dout(ioctl_dout),
	.ioctl_index(ioctl_index),
`endif
	.pal(status[2]),
	.scandouble(forced_scandoubler),
	.content_fps(content_fps),
	.display_hz(display_hz),
	// v3+ reclaimed the debug bits O[9:6]/O[8]/O[9] for playback controls, so the
	// pattern generator and the bars test tone are hardwired to their previous
	// defaults (Pattern=None, Audio tone=Off, Force bars=No).
	.pattern(2'd0),
	.audio_en(1'b0),
	// use_frame_store=1 FORCES colorbars (disables external frame). Keep 0 so
	// DDR/has_frame can feed the store when present. Not an "enable store" bit.
	.use_frame_store(1'b0),
	// L4: content from plex_present_geom_mux (FABRIC_NATIVE_720P_GEOM → 1280×720).
	// Default Template path ignores these (tied 0).
`ifdef PLEX_PRESENT_720P_L4
	.content_w(present_content_w),
	.content_h(present_content_h),
`else
	.content_w(11'd0),
	.content_h(11'd0),
`endif
	.fs_wr_en(fs_wr_en),
	.fs_wr_pixel(fs_wr_pixel),
	.fs_wr_reset(fs_wr_reset),
	.fs_swap(fs_swap),
	.fs_wr_ready(fs_wr_ready),
	.sdram_dout(sdram_dout),
	.sdram_ready(sdram_ready),
	.sdram_sel(frame_sdram_sel),
	.sdram_addr(frame_sdram_addr),
	.sdram_din(frame_sdram_din),
	.sdram_wr(frame_sdram_wr),
	.sdram_rd(frame_sdram_rd),
	.sdram_bs(frame_sdram_bs),
	.sdram_refresh(frame_sdram_refresh),
`ifdef DDR_FRAME_STORE
	.ddr_start_req(status[12]),
	.ddr_bank_sel(status[13]),
	.ddr_status_osd(status[15:0]),
	.ddr_input_cmd_valid(playback_cmd_valid),
	.ddr_input_cmd(playback_cmd),
	.ddr_sdram_test_state(sdram_test_state),
	.ddr_sdram_size_code(sdram_size_code),
	.ddr_sdram_error_count(sdram_error_count),
	.clk_ddr(clk_ddr),
	.DDRAM_CLK(DDRAM_CLK),
	.DDRAM_BUSY(present_ddr_busy),
	.DDRAM_BURSTCNT(present_ddr_burstcnt),
	.DDRAM_ADDR(present_ddr_addr),
	.DDRAM_DOUT(present_ddr_dout),
	.DDRAM_DOUT_READY(present_ddr_dout_ready),
	.DDRAM_RD(present_ddr_rd),
	.DDRAM_DIN(present_ddr_din),
	.DDRAM_BE(present_ddr_be),
	.DDRAM_WE(present_ddr_we),
	.ddr_frames_done(ddr_frames),
	.ddr_doorbell_ok(ddr_doorbell_ok),
`endif
	.af_wr_en(af_wr_en),
	.af_wr_data(af_wr_data),
	// OSD T[10] or SPI status bit 10 pulses flush
	.af_wr_flush(af_wr_flush | status[10]),
	.ce_pix(ce_pix),
	.HBlank(HBlank),
	.HSync(HSync),
	.VBlank(VBlank),
	.VSync(VSync),
	.r(r),
	.g(g),
	.b(b),
	.audio_l(al),
	.audio_r(ar_audio),
	.stat_display_index(disp_i),
	.stat_content_index(cont_i),
	.stat_advance(advance),
	.stat_has_frame(has_frame),
	.stat_wr_count(wr_count),
	.stat_has_audio(has_audio),
	.stat_audio_underrun(audio_underrun),
	.stat_swap_pending(swap_pending),
	.stat_frame_underruns(frame_underruns),
	.stat_frame_sdram_state(frame_sdram_state)
);

`ifdef DDR_FRAME_STORE
ddr_bus_arbiter ddr_arb (
	.clk(clk_ddr),
	.clk_m1(clk_sys),
	.reset(reset),
	.m1_want(stream_ddr_bus_want),
	.m0_busy(present_ddr_busy),
	.m0_burstcnt(present_ddr_burstcnt),
	.m0_addr(present_ddr_addr),
	.m0_dout(present_ddr_dout),
	.m0_dout_ready(present_ddr_dout_ready),
	.m0_rd(present_ddr_rd),
	.m0_din(present_ddr_din),
	.m0_be(present_ddr_be),
	.m0_we(present_ddr_we),
	.m1_busy(stream_ddr_busy),
	.m1_burstcnt(stream_ddr_burstcnt),
	.m1_addr(stream_ddr_addr),
	.m1_dout(stream_ddr_dout),
	.m1_dout_ready(stream_ddr_dout_ready),
	.m1_rd(stream_ddr_rd),
	.m1_din(stream_ddr_din),
	.m1_be(stream_ddr_be),
	.m1_we(stream_ddr_we),
	.DDRAM_BUSY(DDRAM_BUSY),
	.DDRAM_BURSTCNT(DDRAM_BURSTCNT),
	.DDRAM_ADDR(DDRAM_ADDR),
	.DDRAM_DOUT(DDRAM_DOUT),
	.DDRAM_DOUT_READY(DDRAM_DOUT_READY),
	.DDRAM_RD(DDRAM_RD),
	.DDRAM_DIN(DDRAM_DIN),
	.DDRAM_BE(DDRAM_BE),
	.DDRAM_WE(DDRAM_WE)
);
`endif

assign CLK_VIDEO = clk_sys;
assign CE_PIXEL  = ce_pix;
assign VGA_DE = ~(HBlank | VBlank);
assign VGA_HS = HSync;
assign VGA_VS = VSync;
assign VGA_R  = r;
assign VGA_G  = g;
assign VGA_B  = b;

// Signed audio samples
assign AUDIO_S = 1;
assign AUDIO_L = al;
assign AUDIO_R = ar_audio;

// Heartbeat LED; faster blink with audio; very fast when bitstream NALs seen.
reg [26:0] act_cnt;
always @(posedge clk_sys) act_cnt <= act_cnt + 1'd1;
wire led_base = has_frame ? act_cnt[24] : (act_cnt[26] ? act_cnt[25:18] > act_cnt[7:0] : act_cnt[25:18] <= act_cnt[7:0]);
assign LED_USER = has_stream ? (act_cnt[20] ^ nalu_count[0] ^ last_nal_type[0])
	: (has_audio ? act_cnt[22] : led_base);

// --- Core status → HPS (UIO_GET_STATUS / status_set) ---
//
// When status_set rises, Main_MiSTer *replaces* the entire OSD status word with
// status_in (then clears bit 0). Telemetry MUST NOT occupy CONF_STR O/T bits or
// Pattern / FPS / TV Mode freeze, and HPS status[12]/[13] DDR kick/bank survive.
//
// Layout v2 (OSD-safe, 128b):
//   [15:0]    status[15:0]     // OSD O/T + DDR start[12] bank[13]
//   [23:16]   flags            // has_frame..pps_valid
//   [31:24]   last_nal_type
//   [47:32]   nalu_count
//   [55:48]   first_mb_type    [63:56] slice_type
//   [71:64]   {residual_ok, residual_tc[4:0], residual_t1[1:0]}
//   [79:72]   {ddr_busy, swap_pending, slice_qp[5:0]}
//   [87:80]   sps_mb_w         [95:88] sps_mb_h  (pixels = mb*16)
//   [103:96]  residual_dc      (3.3k regression; clean of AR)
//   [111:104] residual_csum    (3.3l-1 XOR sat8(coeff[0:15]); Baseline golden 0x14)
//   [119:112] recon_sig        (3.3l-2 XOR recon Y[0:15]; MB0 block0 golden 0x3b)
//   [127:120] p3_recon_dbg     (coeff/dequant/idct/recon non-zero flags for silicon RCA)
//   [122:121] forced from status (Aspect ratio) — overlaps stream debug only
wire [7:0] telem_flags = {
	pps_valid, sps_valid, stub_busy, has_idr,
	audio_underrun, has_stream, has_audio, has_frame
};

// R-csum6 Rank1+2+3 product sticky residual pack (owner R-csum6; DIAG STRIPPED):
// Lab SoT H-gate-rcsum5: 8832824e MULTI_DRIVE +0x53/push; level res_pair_sticky FAIL.
// Intent: freeze published residual half ONLY on place completion (or ok-rise for tc=0).
//
// Rank1: st_res_word_sticky samples on residual_place_pulse (primary) or residual_ok_rise
//        (tc=0 / no-place paths). NEVER continuous/level while residual_ok (walk class).
// Rank2: residual/recon/debug bytes forced from sticky via status_telem_masked.
// Rank3: slice residual_place_pulse + place_* private; product residual_csum<=cs at ST_PLACE.
// DIAG STRIPPED: no residual_csum<=8'h14 force (parent product path).
// Layout for ARM: raw[12]=dc, raw[13]=csum, raw[14]=recon_sig, raw[15]=P3 recon RCA flags.
(* preserve *) reg [15:0] st_res_word;         // live bond {csum,dc} debug ONLY — never status
(* preserve *) reg [15:0] st_res_word_sticky;  // ONLY status residual source {csum,dc}
(* preserve *) reg [7:0]  st_recon_sig_sticky; // ONLY status recon signature source
(* preserve *) reg [7:0]  st_recon_dbg_sticky; // P3 silicon RCA flags; separate from residual/recon
(* preserve *) reg [127:0] status_telem_r;
(* preserve *) reg        residual_ok_d_st;
wire residual_ok_rise = residual_ok & ~residual_ok_d_st;
wire [7:0] st_res_csum = st_res_word_sticky[15:8];
wire signed [7:0] st_res_dc = st_res_word_sticky[7:0];

// Always block A: residual sticky ONLY (inputs: place pulse / ok rise / residual_*)
// NEVER mentions stream_bytes_in — structural isolate Rank2.
always @(posedge clk_sys) begin
	if (reset) begin
		st_res_word        <= 16'd0;
		st_res_word_sticky <= 16'd0;
		st_recon_sig_sticky <= 8'd0;
		st_recon_dbg_sticky <= 8'd0;
		residual_ok_d_st   <= 1'b0;
	end else begin
		st_res_word      <= {residual_csum, residual_dc}; // debug bond
		residual_ok_d_st <= residual_ok;
		// Primary: ST_PLACE pulse freezes place-time product pair (blocks +0x53 walk)
		if (residual_place_pulse)
			st_res_word_sticky <= {residual_csum, residual_dc};
		// Secondary: residual_ok rise for tc=0 / paths without ST_PLACE
		else if (residual_ok_rise)
			st_res_word_sticky <= {residual_csum, residual_dc};
		if (recon_valid)
			st_recon_sig_sticky <= recon_sig;
		if (recon_dbg_valid)
			st_recon_dbg_sticky <= recon_dbg;
		// else HOLD sticky — frozen between intentional place/ok edges
	end
end

// Always block B: assemble status_telem from sticky residual/recon + P3 RCA flags
always @(posedge clk_sys) begin
	if (reset) begin
		status_telem_r <= 128'd0;
	end else begin
		status_telem_r[15:0]    <= status[15:0];
		status_telem_r[23:16]   <= telem_flags;
		status_telem_r[31:24]   <= last_nal_type;
		status_telem_r[47:32]   <= nalu_count;
		status_telem_r[55:48]   <= first_mb_type;
		status_telem_r[63:56]   <= slice_type;
		status_telem_r[71:64]   <= {residual_ok, residual_tc, residual_t1};
		status_telem_r[79:72]   <= {ddr_busy, swap_pending, slice_qp};
		status_telem_r[87:80]   <= sps_mb_w;
		status_telem_r[95:88]   <= sps_mb_h;
		// residual half from sticky ONLY (never live residual_csum, never stream)
		status_telem_r[103:96]  <= st_res_word_sticky[7:0];
		status_telem_r[111:104] <= st_res_word_sticky[15:8];
		status_telem_r[119:112] <= st_recon_sig_sticky;
		status_telem_r[127:120] <= st_recon_dbg_sticky;
	end
end

// Rank2 structural mask: force residual/recon bytes from sticky before AR splice
wire [127:0] status_telem_masked = {
	st_recon_dbg_sticky,          // P3 recon RCA flags
	st_recon_sig_sticky,          // recon signature forced from sticky
	st_res_word_sticky[15:8],     // csum forced from sticky
	st_res_word_sticky[7:0],      // dc forced from sticky
	status_telem_r[95:0]
};

// Preserve Aspect ratio OSD bits (may stomp recon_dbg bits [2:1] — OK)
// status_set replaces entire word in Main; residual bits stay below AR splice.
assign status_in = {
	status_telem_masked[127:123],
	status[122:121],
	status_telem_masked[120:0]
};

// Pulse status_set ~1 kHz or when nalu/sps/slice/residual telem change so Main/ARM can poll.
reg [15:0] prev_nalu;
reg [7:0]  prev_sltype;
reg        prev_sps, prev_pps;
reg [7:0]  prev_csum;
reg signed [7:0] prev_dc;
reg [7:0]  prev_recon_sig;
reg [7:0]  prev_recon_dbg;
reg [14:0] st_div;
always @(posedge clk_sys) begin
	if (reset) begin
		status_set <= 0;
		prev_nalu  <= 0;
		prev_sltype <= 0;
		prev_sps   <= 0;
		prev_pps   <= 0;
		prev_csum  <= 0;
		prev_dc    <= 0;
		prev_recon_sig <= 0;
		prev_recon_dbg <= 0;
		st_div     <= 0;
	end else begin
		status_set <= 0;
		st_div <= st_div + 1'd1;
		if (nalu_count != prev_nalu || slice_type != prev_sltype || sps_valid != prev_sps ||
		    pps_valid != prev_pps || st_res_csum != prev_csum || st_res_dc != prev_dc ||
		    st_recon_sig_sticky != prev_recon_sig || st_recon_dbg_sticky != prev_recon_dbg ||
		    st_div == 0) begin
			status_set <= 1'b1;
			prev_nalu  <= nalu_count;
			prev_sltype <= slice_type;
			prev_sps   <= sps_valid;
			prev_pps   <= pps_valid;
			prev_csum  <= st_res_csum;
			prev_dc    <= st_res_dc;
			prev_recon_sig <= st_recon_sig_sticky;
			prev_recon_dbg <= st_recon_dbg_sticky;
		end
	end
end

// Silence unused
wire _unused = |{disp_i, cont_i, advance, ingest_pixels, ingest_dl, af_active, ioctl_addr,
	sps_count, pps_count, slice_count, wr_count, stream_bytes_seen, sps_profile, sps_level,
	// 3.3l-1: keep residual_coeff visible for inv_quant (3.3l-2); csum already in status
	residual_coeff[0], residual_coeff[1], residual_coeff[2], residual_coeff[3],
	residual_coeff[4], residual_coeff[5], residual_coeff[6], residual_coeff[7],
	residual_coeff[8], residual_coeff[9], residual_coeff[10], residual_coeff[11],
	residual_coeff[12], residual_coeff[13], residual_coeff[14], residual_coeff[15],
	stub_frames, slice_valid, slice_is_i, sps_mb_w, sps_mb_h, has_mb_type, idr_count,
	stream_fifo_level, ddr_frames, stream_ddr_active, stream_ddr_bytes_out,
	stream_ddr_underruns, stream_ddr_overruns, stream_ddr_host_write,
	stream_ddr_fpga_read, stream_ddr_bus_want, stream_ddr_burstcnt, stream_ddr_addr,
	stream_ddr_rd, stream_ddr_din, stream_ddr_be, stream_ddr_we, _host_wr_unused};

endmodule
