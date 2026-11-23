// h264/video_out_resolve.sv -- Video Output Auto / forced resolve (analog vs HDMI).
//
// Analog VGA pins follow this core raster (emu VGA_SCALER=0). HDMI H-rate is ascal's
// video_mode, not this module.
//
// 443b1d4 live: Video Output Progressive always selected 480p (858 dots @ 27 MHz =
// 31.469 kHz) on analog.
// bfc2389e: analog Progressive stayed 15 kHz but as 480i unless Analog Raster was 240p.
// reviewer N1 2026-11-07: Progressive on a 15 kHz analog dest MUST be true 240p
// (262-line progressive, 1719/1720 dots, 450,450 dots/frame = 59.94006 Hz), even when
// Analog Raster is 480i. Explicit Interlaced + Analog Raster 480i stays 480i. Analog
// Raster 240p on Interlaced/Auto still 240p. 31 kHz VGA Progressive stays 480p, including
// when analog_want and vga31_want are both set (browse-lane P2-1: mode 2 never forces 15 kHz there).
// HDMI-only Progressive stays 480p. HDMI-only / VGA31 Interlaced is 480i.
module video_out_resolve (
    input  wire [1:0] video_out_mode,     // status[6:5]: 0 Auto, 1 Interlaced, 2 Progressive, 3 Native
    input  wire       analog_want,        // latched 15 kHz analog destination
    input  wire       vga31_want,         // latched 31 kHz VGA destination
    input  wire       analog_raster_240p, // status[24]
    output wire [1:0] video_out_eff,
    output wire       il_pix_sys,
    output wire       p240_eff,
    output wire       interlaced_eff
);
    // analog_prog15: Progressive on a 15 kHz analog dest. Forces the 240p raster.
    // 443b1d4 live bug is analog Progressive as 480p (il_pix_sys from video_out_eff==1).
    // bfc2389e N1: analog_prog15 did not set p240_eff (480i when Analog Raster is 480i).
    // browse-lane P2-1: mode 2 keeps main's "31kHz VGA" meaning when the setup also asks for 31 kHz
    // (vga31_want: forced_scandoubler with direct_video or yc_analog_out gives analog_want and
    // vga31_want together). Only a pure 15 kHz analog dest (vga31_want=0) forces true 240p.
    wire analog_prog15 = analog_want && !vga31_want && (video_out_mode == 2'd2);
    assign video_out_eff  = (video_out_mode != 2'd0) ? video_out_mode :
                            analog_want              ? 2'd1 :
                            vga31_want               ? 2'd2 : 2'd3;
    // Interlaced (explicit or analog Auto) uses the 15 kHz family; analog Progressive
    // is 240p. HDMI-only / VGA31 Interlaced must not fall through to 480p.
    assign il_pix_sys     = (video_out_eff == 2'd1) || analog_prog15;
    assign p240_eff       = il_pix_sys && (analog_raster_240p || analog_prog15);
    assign interlaced_eff = il_pix_sys && ~p240_eff;
endmodule
