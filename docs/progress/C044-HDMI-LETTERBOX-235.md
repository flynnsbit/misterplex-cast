# C044 — 2.35:1 HDMI letterbox (624×272 / visible 260)

Glass 2026-10-10, `lab-runs/hdmi-int-1010/glass-195526/run-C/`. The remote PMS,
`/library/metadata/348` "3:10 to Yuma", source 1920×800, no ladder proxy, offset
1800000 ms. PMS universal was asked for `videoResolution=624x480` and delivered
coded 624×272, visible 624×260, `crop_px=0,0,0,12`, `source_dar=47:20`. HDMI
fills the 480-line raster with those 260 rows (faces about 480/260 ≈ 1.85× tall,
cropdetect finds no bars).

The placement this picture should have, if the core placed the visible rows
instead of scaling them, is the c013 / G2 rule already in
`host/libmisterplex/sps_crop_letterbox.hpp`:

```text
H         = 260
lb_top    = ((480 - 260) / 2) & ~1 = 110
lb_bottom = 480 - 260 - 110       = 110
```

That is not a DAR fit. DAR 47:20 across a 640-wide raster is about 272 lines
(`640 * 20 / 47`). Glass acknowledged PLXA 47:20 and still filled all 480
lines, so the core is not sizing the picture from that DAR.

4:3 624×480 with visible 468, and a full 624×480 frame (lab BBB), must keep
today's bytes. Those are 480-coded banks. G2 on the 468 crop would be 6+6
bars; that must not be applied.

## Trace

### Daemon

`MediaPlayer::fpgaThreadMain` reads the first AU's host SPS geometry and logs
it. It does not turn crop or visible height into a letterbox.

- `arm/misterplexd/media_player.cpp:4268` `firstAuSourceAspect`, then
  `packet.geometry` copied into `lastSummary_` at `:4276-4286`.
- The log at `:4288-4307` is the glass line: coded, visible, `crop_px`,
  `source_dar`. The same function says storage stride is FPGA-owned and there
  is no ARM padding (`:4307`).
- The only geometry word sent for that AU is `source_dar` via
  `fpga_.sendSourceAspect` at `:4334`.
- `arm/misterplexd/fpga_spi.cpp:3340` `FpgaSpi::sendSourceAspect` encodes a
  PLXA ioctl (index 4, 9 bytes) of the two DAR terms and a token. The doorbell
  word (`ddrDoorbellHi` in `host/libmisterplex/ddr_frame_layout.hpp`) carries
  bank, format, and sequence. It does not carry `present_y`, crop, or visible
  height.
- The present bank chosen for 640×480 is `plex480pDdrFrameGeometry()`
  (`ddr_frame_layout.hpp:337-348`, selected by
  `ddrFrameGeometryForPresentedSize` at `:383-386`): coded 624×480, display
  618×480, crop right 6, `present_y = 0`. That is the DDR reader contract for
  every 480p play, including this 260-line stream. `setDdrFrameLayout`
  (`fpga_spi.cpp:1145`) stores that struct on the host. The 480p reader does
  not load `present_y` back out of DDR.
- `FbPresent::blitRgb24` (`arm/misterplexd/fb_present.cpp:83-87`) letterboxes
  with `(dstH - h) / 2`. Product HDMI is the core scanout, not this blit.
- `makeDdrFrameGeometry` (`ddr_frame_layout.hpp:313-334`) can set `present_y`
  for `DdrFramePlacement::Pillarbox`. The 480p bank overwrites that with 0
  (`:347`). Nothing passes SPS crop into it.

PMS fit-inside, not a pad, is why the bitstream is 272 lines tall.
`plex_resolve.cpp:2242` and `:2417` already say a 480 coded path needs
`pad+setsar` to 624×480, and that PMS universal emits a short DAR frame
instead. The lab ladder does that pad only when a proxy is configured. This
repro had no proxy, so resolve stayed on universal `624x480` and PMS scaled
1920×800 to width 624 (`800 * 624 / 1920 = 260`, padded to 17 macroblock rows).

### Core (RTL only; the RBF is unchanged)

The core never receives a letterbox offset, and it does not use visible height
to place the picture. It scans the whole 480-line bank onto the template.

- `fpga/Plex_MiSTer/rtl/sps_parser.sv:301-352` parses `frame_cropping_flag` and
  the four offsets, then publishes only the cropped `width` / `height`
  (`(w_mbs * 16) - ((cl + cr) << 1)`, same for height). There is no crop port
  and no `lb_top`.
- `fpga/Plex_MiSTer/rtl/stream_path.sv:192-199` wires those to `sps_width` /
  `sps_height`. There is no `picture_*` crop copy in this file (the Q006 note
  that cited `stream_path.sv:692` does not match this tree).
- `stream_path.sv:292-305` gives `decode_stub` the macroblock grid and
  `HEIGHT(FRAME_H)`. `decode_stub.sv:736-762` paints `WIDTH * HEIGHT` pixels,
  the compile-time raster (`Plex.qsf:84` is `FRAME_H=480`), not a centered
  260-line window.
- `h264_sps_geometry_parser` (`h264_syntax_primitives.sv:1133-1137`) does
  publish coded size, display size, and luma crops (`offset << 1`). The product
  top does not instantiate it. The only instance is the geometry testbench.
- `Plex.sv:950-955` ties `content_w` and `content_h` to 0 on the non-L4 path.
  The PLXG content window is not the 480p scanout.
- `present_core.sv:573-598` builds `ddr_frame_store` with the constants in
  `ddr_frame_layout_params.svh:1-14`: coded 624×480, display 618×480,
  `CROP_TOP = 0`, `PRESENT_Y = 0`.
- `present_core.sv:431-432` and `:514-517` (the non-L4, non-960 branch):
  `STORE_Y_SCALE = (FRAME_H * 65536) / TPL_SCALE_REF_H` with `FRAME_H=480` and
  `TPL_SCALE_REF_H=240`, so `store_y = py * 2`. Every template line reads the
  full 480-line bank. Visible height is not a term.
- `ddr_frame_store.sv:103-180`: `rd_visible` is `PRESENT_Y .. PRESENT_Y+DISPLAY_H`.
  With those parameters that is all 480 rows. `src_y = display_y + CROP_TOP`
  and `CROP_TOP` is 0. Outside the window, `:465-468` drives RGB 0. Nothing
  here reads a runtime `lb_top` or the SPS visible height.
- `present_geom_latch.sv:168-169` and `:230` do latch PLXG `present_y`. The
  480p `ddr_frame_store` instance does not take that wire. `present_y` on the
  store is the parameter 0 above.
- `source_aspect_ack.sv:50-54` latches PLXA `ack_x` / `ack_y`. `ddr_frame_store.sv:1023-1028`
  copies them into the PLXJ mailbox. That is the ACK the daemon waited for.
  It does not change `rd_y` or `STORE_Y_SCALE`.
- `present_content_window.sv:7-8` stretches `content_w×content_h` across the
  whole DE when `win_enable=1`. `content_y0` is a store-read origin, not a bar
  in the raster. The 480p product path does not enable that window.

So: the core does not place the visible rows. It always maps the 480-line bank
onto the raster. A 260-line picture that fills that bank is stretched
(480/260 ≈ 1.85). A DAR-scaled height would have left bars; glass did not.

## Root cause

Two gaps stack.

1. PMS universal treats `624x480` as a fit-inside box. A 1920×800 source becomes
   624×260 (coded 624×272, bottom crop 12). No bars are in the bitstream.
2. The 480p scanout has no runtime letterbox. `PRESENT_Y` and `DISPLAY_H` are
   compile-time, `present_y` is 0, and the template scale covers the whole
   bank. PLXA DAR and PLXG `present_y` are not placement inputs on this path.

## Chosen fix

The HDMI fix is RTL. No existing daemon register moves this raster: the
doorbell has no `present_y`, PLXA is an ACK, and the 480p store ignores the
PLXG latch. A padded universal URL is not available from PMS, and forcing the
lab pad ladder onto every 480p play would re-encode the 468 and 480 streams.
The daemon playback path is unchanged.

Minimal RTL (do not apply it in this lane; the RBF must not change):

On the 480p `ddr_frame_store` / `present_core` scanout, when the decoded coded
height is below 480, stop treating `DISPLAY_H` as 480 and `PRESENT_Y` as 0.
Program

```text
H         = coded_height - crop_top - crop_bottom
present_y = ((480 - H) / 2) & ~1
display_h = H
```

using the same even-top rule as `spsCropLetterbox480` (for this stream, 110 and
260). Pixels with `rd_y < present_y` or `rd_y >= present_y + display_h` stay
black, which `ddr_frame_store.sv:465-468` already does outside `rd_visible`.
Keep `STORE_Y_SCALE` as the full-bank 480/240 map so the bars are lines in the
480-row raster, not a second scale of H onto 480.

Leave the compile-time window (`PRESENT_Y=0`, `DISPLAY_H=480`, crop top 0)
when coded height is 480. That is the visible-468 4:3 crop and the full
624×480 bank. Do not feed G2's 6+6 bars into those frames.

`host/libmisterplex/sps_crop_letterbox.hpp` `streamRasterPlacement480` is that
decision for a later owner. It calls `spsCropLetterbox480` only when the coded
picture is shorter than 480. A 480-coded frame returns `letterbox=false` and
`lb_top=lb_bottom=0`. misterplexd does not call it, because programming the
result cannot change this core.
