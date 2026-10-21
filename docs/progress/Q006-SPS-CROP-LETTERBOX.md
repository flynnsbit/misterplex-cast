# Q006 — SPS frame crop and 480-line letterbox

Streams whose visible height is shorter than the 480-line raster (720×404 stored as
coded 720×416 with `frame_cropping_flag` and a bottom offset, and the same pattern
at 640×360 and 640×350) must be cropped from the SPS and letterboxed into 480 lines.
Conformance golden G2 places the top bar with

```text
lb_top    = ((480 - H) / 2) & ~1
lb_bottom = 480 - H - lb_top
```

`H` is the SPS display height in luma samples. Integer division truncates. `& ~1`
forces `lb_top` even. The leftover line, when `(480-H)/2` is odd, goes to
`lb_bottom`. Identity: `lb_top + H + lb_bottom = 480`.

At H=350, `(480-350)/2 = 65` and `65 & ~1 = 64`, so the bars are 64 and 66.
That is the case that separates G2 from a plain centering divide.

## Crop formula

Product path is progressive Constrained Baseline, 4:2:0, `frame_mbs_only_flag=1`.
H.264 crop units are then `CropUnitX = CropUnitY = 2` luma samples.

```text
coded_width  = pic_width_in_mbs * 16
coded_height = pic_height_in_map_units * 16          # frame_mbs_only
crop_left    = frame_crop_left_offset  * 2          # luma samples
crop_right   = frame_crop_right_offset * 2
crop_top     = frame_crop_top_offset   * 2
crop_bottom  = frame_crop_bottom_offset * 2
display_w    = coded_width  - crop_left - crop_right
display_h    = coded_height - crop_top  - crop_bottom
H            = display_h
```

Coded width and height sit on the macroblock grid (multiples of 16). Display
size is what remains after the crop. Encoders that need a non-aligned visible
height pad the bottom of the coded picture and set `frame_crop_bottom_offset`
only. The four examples use that pad. The helper still accepts left/top/right
offsets; only the worked rows are bottom-only.

Bars are even when `H` is even, because 480 is a multiple of 4 and `lb_top` is
forced even, so `lb_bottom = 480 - H - lb_top` is even too. 4:2:0 pairs chroma
on even luma rows. An even `lb_top` keeps the first content line on an even
row, so the bar/content edge does not split a chroma pair. `H ≡ 2 (mod 4)` is
the interesting residue: the truncated center is odd, G2 rounds the top bar
down by one line, and the bottom bar is two lines taller than the top.

## Worked examples

Crop offsets are SPS `frame_crop_*_offset` values (crop units). Luma crop is
that value times 2. `lb_*` is in raster lines.

| Display | Coded | Offsets L,R,T,B | Luma crop L,R,T,B | H | lb_top | lb_bottom |
| --- | --- | --- | --- | --- | --- | --- |
| 720×404 | 720×416 | 0,0,0,6 | 0,0,0,12 | 404 | 38 | 38 |
| 640×360 | 640×368 | 0,0,0,4 | 0,0,0,8 | 360 | 60 | 60 |
| 704×480 | 704×480 | 0,0,0,0 | 0,0,0,0 | 480 | 0 | 0 |
| 640×350 | 640×352 | 0,0,0,1 | 0,0,0,2 | 350 | 64 | 66 |

Checks:

- 720×404: 45×26 macroblocks is 720×416. Bottom pad is 12 luma samples, 6 crop
  units. `(480-404)/2 = 38`, already even, so both bars are 38.
- 640×360: 40×23 macroblocks is 640×368. Bottom pad is 8 luma samples, 4 crop
  units. `(480-360)/2 = 60`, already even.
- 704×480: 44×30 macroblocks, crop flag clear, display equals coded, bars are 0.
  704 is wider than the 640-wide 480p bank. This helper only places vertical
  bars. Horizontal placement stays with the existing pillarbox contract.
- 640×350: 40×22 macroblocks is 640×352. Bottom pad is 2 luma samples, 1 crop
  unit. `(480-350)/2 = 65`, G2 top is 64, bottom is 66.

Cross-check from 516a0a1a (live PMS, not one of the four rows): coded 624×480,
crop units L,R,T,B = 0,3,0,6, display 618×468. Same formulas give luma right
crop 6, luma bottom crop 12, H=468, `lb_top = lb_bottom = 6`.

## Who computes what

The daemon owns G2. The core owns the SPS parse and the coded-versus-display
sample addresses. The two centering expressions disagree when the half-bar is
odd (350 → core 65, G2 64). This lane does not change RTL, so the core keeps
its shift. The host helper is the G2 number a later presentation site must
program.

### Daemon (this lane)

No `arm/misterplexd` function reads an SPS and places a 480-line letterbox.

- `misterplexd` does not parse SPS. `parseSpsRbsp` lives in
  `host/libmisterplex/h264_sps.hpp` and is used by the host unit test. The
  daemon sources have no `parseSps` / `SpsInfo` call.
- Bank geometry is chosen from the decode output size, not from SPS crop.
  `MediaPlayer::threadMain` takes `ddrFrameGeometryForPresentedSize(outW_, outH_)`
  when the native scaler is on (`arm/misterplexd/media_player.cpp:3124`).
  The same helper is used at `arm/misterplexd/main.cpp:764` and `:1009`.
  For a 640×480 bank that returns the fixed 624×480 coded / 618×480 display
  pillarbox with `present_y = 0` (`host/libmisterplex/ddr_frame_layout.hpp:321`).
- `makeDdrFrameGeometry` (`ddr_frame_layout.hpp:297`) puts every missing sample
  on the right and bottom edges (`crop_left = crop_top = 0`) and, only for
  `DdrFramePlacement::Pillarbox`, sets `present_y` to a truncating
  `(presented_height - display_height) / 2` (`ddr_frame_layout.hpp:316`).
  That is the pre-G2 center. It is not called with SPS crop offsets.
- `FbPresent::blitRgb24` letterboxes an already-sized RGB image with
  `(dstH - h) / 2` (`arm/misterplexd/fb_present.cpp:83`). It never sees SPS
  fields. The product HDMI raster is the core scanout, not this blit.
- The scale chain comment at `media_player.cpp:3147` states that the native
  scaler owns aspect and that product frames fill the raster without host
  letterbox bars. That is the current playback policy.
- 516a0a1a (`arm/misterplexd/plex_resolve.cpp:769`) keeps the 480p PMS ladder
  at coded 624×480 instead of aspect-fitting it to 624×468, and allows the
  live guard to accept a shorter SPS display height. That is an accept/reject
  check. It does not compute `lb_top`.

Phase 2 therefore adds the pure helper and does not call it from misterplexd.
There is no site yet whose inputs are SPS crop offsets and whose output is the
480-line bar. Calling it from `fb_present` or `makeDdrFrameGeometry` would
change a path that does not have those inputs.

The helper is `misterplex::spsCropLetterbox480` in
`host/libmisterplex/sps_crop_letterbox.hpp`. It is header-only, so
`tests/unit/test_sps_parse.cpp` can call it. Playback behavior stays as it is
until a site is given real SPS offsets and starts passing `lb_top` into the
presentation geometry.

### Core (unchanged)

`sps_parser` is the product parser (`fpga/Plex_MiSTer/rtl/stream_path.sv:307`).
On a successful crop it publishes display `width`/`height` and luma-sample
crops (`fpga/Plex_MiSTer/rtl/sps_parser.sv:520`):

- `coded_width = pic_width_in_mbs << 4`, `coded_height = pic_height_in_map_units << 4`
  (`sps_parser.sv:520`). For `frame_mbs_only` that matches the formula above.
  The `<< 4` on coded height does not double for field pictures; display height
  does (`sps_parser.sv:526`). Field pictures are outside this paper.
- Progressive 4:2:0 crop units are two luma samples: `crop_* <= offset << 1`
  (`sps_parser.sv:527`).
- `stream_path` copies those crops onto the picture (`stream_path.sv:692`).

`h264_mb_ctrl` turns that into a visible size and a paint origin:

- `present_height = present_coded_height - present_crop_top - present_crop_bottom`
  (`fpga/Plex_MiSTer/rtl/h264_mb_ctrl.sv:241`). Cropped rows stay inside the
  coded picture. They are not stored as a second buffer.
- Paint letterbox is `pillar_y = (bank_h - present_height) >> 1` when the
  visible picture is shorter than the present bank (`h264_mb_ctrl.sv:808`).
  Content addressing uses `content_y = y - pillar_y` (`h264_mb_ctrl.sv:814`).
  Cells outside the content rectangle are black and do not read the DPB
  (`h264_mb_ctrl.sv:3048`).
- `>> 1` is the truncating center. For H=350 it yields 65. G2 yields 64.
  The core does not apply `& ~1`.

`ddr_frame_store` does the same split at scanout when runtime geometry is on
(`fpga/Plex_MiSTer/rtl/ddr_frame_store.sv:200`):

- `visible_height = coded_height - crop_top - crop_bottom` (display rows).
- `viewport_y = (FRAME_H - visible_height) >> 1` (centering shift, not G2).
- `src_y = display_y + crop_top`, so the reader starts at the first visible
  coded row and skips the cropped pad (`ddr_frame_store.sv:232`).

Coded rows are the macroblock grid in memory, including the encoder pad that
`frame_crop_bottom_offset` names. Display rows are that grid minus the luma
crop. Letterbox rows are outside the display window: they are generated as
black at paint/scanout, not written as extra coded lines.

The host SPS parser (`host/libmisterplex/h264_sps.hpp:144`) applies the same
`* 2` crop and returns only the cropped `width`/`height`. It does not return
coded size, the four luma crops, or letterbox bars. That is why the new helper
takes the coded size and the four SPS offsets itself.

## What remains untested

- The four rows are locked by `tests/unit/test_sps_parse.cpp` after phase 3.
  That test fails to compile without the helper, and the 640×350 row fails if
  `lb_top` is the old truncating center 65.
- No RTL sim compares `pillar_y` / `viewport_y` against G2. The core still
  shifts. This lane does not fit or simulate the FPGA.
- No HDMI / TINK pass. The helper is not on the playback path.
- Horizontal fit of 704 or 720 into a 640-wide bank is not covered.
- `frame_mbs_only_flag=0` (crop unit Y = 4) is not covered. The product parser
  always shifts crop by 1 (`sps_parser.sv:528`).
- Top or left crop, and a bottom crop that is not "pad to next macroblock",
  are accepted by the helper and are not in the four rows.
- Sample aspect ratio is ignored. `H` is the cropped luma height. DAR-fitted
  height is a different number (`h264_mb_ctrl.sv` `present_dar_*`).

## Open questions

1. Which register wins on glass: a daemon `present_y` programmed with G2, or
   the core's `pillar_y` / `viewport_y` (`>> 1`)? Until one owner is chosen,
   a 350-line picture can sit one line off from the golden. This lane does
   not pick an RTL owner.
2. Should 720-wide and 704-wide display pictures be pillarboxed into 640,
   scaled, or refused on the 480p bank? Vertical bars are specified. Width is
   not.
3. Will every encoder pad only the bottom, or will some center the pad with
   both `frame_crop_top_offset` and `frame_crop_bottom_offset`? The helper
   honors both. The four rows assume bottom-only, matching the 720×404
   wording.
4. Is the 480-line raster always the present bank height, including when the
   coded picture is 720×416 rather than the 624×480 PMS contract? The helper
   assumes the raster is 480 lines and does not resize the DDR bank.
