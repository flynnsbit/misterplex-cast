# SDRAM bring-up (B1)

MiSTerPlex now drives the single MiSTer SDRAM stick instead of tying the pins off.  The pinout comes from the upstream MiSTer template `sys/sys.tcl` single-stick `SDRAM_*` assignments already sourced by `Plex.qsf`; `sys/sys_dual_sdram.tcl` remains unused because the lab unit has one stick.

`fpga/Plex_MiSTer/rtl/sdram.sv` is vendored from MiSTer-devel NeoGeo:

- URL: <https://github.com/MiSTer-devel/NeoGeo_MiSTer/blob/227d4f418fd908a66712329400a6f619ca4fee77/rtl/mem/sdram.sv>
- License: GPL-3.0-or-later, per the file header.
- Local adaptation: synthesis macro `SDRAM_CL3=1` selects CAS 3; without it the controller uses CAS 2.

## Clock sweep macros

The default SDRAM PLL output is 100 MHz; post-MemTest hardware bring-up targets `SDRAM_CLK_142` with `SDRAM_CL3`. Define one of these Quartus Verilog macros for sweep variants: `SDRAM_CLK_50`, `SDRAM_CLK_75`, `SDRAM_CLK_80`, `SDRAM_CLK_110`, `SDRAM_CLK_120`, `SDRAM_CLK_133`, `SDRAM_CLK_142`. `Plex.sv` also adjusts the refresh toggle interval for the selected clock.

Refresh interval arithmetic uses `floor(f_MHz * 64000 / 8192) - 1` because the refresh counter fires after `REFRESH_CYCLES+1` clocks and each of 8192 rows must be refreshed within 64 ms.

| SDRAM clock | `SDRAM_REFRESH_CYCLES` |
|---|---:|
| 50 MHz | 389 |
| 75 MHz | 584 |
| 80 MHz | 624 |
| 100 MHz | 780 |
| 110 MHz | 858 |
| 120 MHz | 936 |
| 133.333 MHz | 1040 |

## Memory test

`rtl/sdram_memtest.sv` runs after reset and destructively tests the detected address range:

1. alias probe for 16/32/64/128 MB as visible through the standard controller;
2. walking-one pattern across every 16-bit word;
3. walking-zero pattern across every 16-bit word;
4. address-derived uniqueness pattern across every 16-bit word.

## HPS DDR mailbox

The SDRAM result is published without SPI at physical address `0x3007F110`, next to the existing `PLXS` mailbox and away from W-A01's `0x3007F108` input mailbox.

Summary word at `0x3007F110`, 64-bit little-endian layout:

| Bits | Field |
|---|---|
| `[31:0]` | magic `0x504C584D` (`PLXM`) |
| `[39:32]` | sequence counter |
| `[43:40]` | state: 1 init, 2 detect, 3 walk1, 4 walk0, 5 address, 6 pass, 7 fail |
| `[47:44]` | size code: 0 unknown, 2 = 16 MB, 3 = 32 MB, 4 = 64 MB, 5 = 128 MB |
| `[63:48]` | saturated error count |

Diagnostic word at `0x3007F120`, 64-bit little-endian layout:

| Bits | Field |
|---|---|
| `[4:0]` | layout version = 1 |
| `[20:5]` | expected value at first failing address |
| `[46:21]` | first failing 16-bit word address |
| `[47]` | first-fail valid |
| `[63:48]` | read sample: first failing read value if valid, otherwise latest read |

Host-side decoding lives in `host/libmisterplex/sdram_mailbox.hpp`.

The mailbox is published on change and heartbeat by `rtl/ddram_frame_rd.sv`.

In the B2 product path the SDRAM memtest runs at cold start before the frame
store takes ownership of the SDRAM controller. While it is active, F1 backpressure
is asserted and the frame/audio present path is held reset; once `done` is set,
the port mux hands the stick to `frame_store`. This keeps `PLXM` meaningful in
the same RBF used for frame-store parity instead of requiring a separate
destructive bring-up image.

## B2 frame-store migration notes

Branch `feat/b2` moves the 320x240 double-buffered `frame_store` payload from M10K BRAM to the single SDRAM stick. Static timing closed at 100 MHz CL2, but that does not prove the external SDRAM chip returns valid CAS-2 data on silicon. `SDRAM_CL3=1` is the conservative fallback and should be tried first for hardware bring-up.

The video path still consumes the same 320x240 source coordinates generated in `present_core.sv`, including the G-VID1 invariants: `DE_LAG=3`, no blank-time `store_x` reset, and `past_last_row` blank/clamp.  The external interface (`wr_en`, `wr_reset_ptr`, `swap_banks`, `vsync_pulse`, `swap_pending`) is kept so F1, DDR DMA, and decode-stub writers retain the tear-free contract.

### Line-buffer sizing

At 20 MHz video, the active line period from the template counters is about 31.8 us.  The stock MiSTer `sdram.sv` controller uses one activate/read/precharge sequence per 16-bit word; the measured build timing and controller state sequence budget this as roughly 8 SDRAM clocks per word.  At 100 MHz CL2 that is about 12.5 Mword/s, so one 320-word line fills in ~25.6 us.  The default four-line buffer carries the current line plus three lines of lookahead.  One 320-word line still fills in ~25.6 us against a ~31.8 us raster line, so each individual line has about 6.2 us (~19.5%, ~620 SDRAM clocks) of local slack, and four lines give about three full line periods of queued lookahead before visible scanout can catch the prefetcher.  A five-way farm sweep built 1/2/4/8-line CL2 variants plus a 2-line CL3 fallback; 4-line CL2 closed timing cleanly while 8-line CL2 did not.

The first silicon run proved the original single line-buffer set was structurally wrong: when `swap_pending` asserted, the prefetcher switched that live scanout buffer to the back bank before the VSync page flip.  That overwrote/invalidated lines the current frame still needed, producing the observed comb/scattered output and a saturated `PLXF` underrun counter.  The RCA simulation `tests/unit/test_frame_store_sdram_sim.cpp` reproduces 38,400 old-bank/new-bank mismatches from that ownership rule.  The fix is two bank-tagged line-buffer sets: one remains owned by the displayed bank, while the other warms row 0+ of the pending bank; the VSync flip is allowed only after the pending set is ready.

### Frame-store mailbox

A second frame-store status mailbox is published by `ddram_frame_rd` at physical `0x3007F118`, next to the B1 SDRAM-test mailbox and still away from W-A01's `0x3007F108` input mailbox.

64-bit little-endian layout:

| Bits | Field |
|---|---|
| `[31:0]` | magic `0x504C5846` (`PLXF`) |
| `[39:32]` | sequence counter |
| `[47:40]` | frame-store SDRAM/debug state (`[7:5]` line-count code, `[4]` any line valid, `[3:0]` FSM) |
| `[63:48]` | saturated line-buffer underrun count |

A nonzero underrun count means scanout requested a visible pixel before the SDRAM line prefetch had delivered it; this should be treated as video corruption, not ignored.
