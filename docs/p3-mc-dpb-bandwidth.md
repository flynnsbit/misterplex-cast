# Phase 3 MC/DPB bandwidth budget

Profile locked for this budget: Baseline L3.0, `ref=1`, no B frames, coded
624x480, I420, 39x30 = 1170 macroblocks/frame, 25 fps.

## Per-frame storage

- Y: 624 * 480 = 299,520 bytes
- U: 312 * 240 = 74,880 bytes
- V: 312 * 240 = 74,880 bytes
- One I420 picture: 449,280 bytes
- One reference plus one current reconstruction: 898,560 bytes

## Per-macroblock traffic

- Filtered writeback into the current picture:
  - Y 16x16 = 256 bytes
  - U 8x8 = 64 bytes
  - V 8x8 = 64 bytes
  - total write = 384 bytes/MB
- Reference fetch for P_Skip/P_L0_16x16:
  - luma qpel window 21x21 = 441 bytes
  - chroma epel windows 9x9 U + 9x9 V = 162 bytes
  - total read = 603 bytes/MB
- Total DPB traffic = 987 bytes/MB.

At 1170 MB/frame, the DPB path consumes 1,154,790 bytes/frame, or
28,869,750 bytes/s at 25 fps (27.5 MiB/s). With 64-bit memory beats this is
about 124 beats/MB before burst/alignment overhead: 76 read beats and 48 write
beats.

## Latency budget

**CORRECTED 2026-07-27:** the original claim here used 100 MHz, but `stream_path`
runs on `clk_sys` = 20 MHz (`pll_0002.v outclk_0`; `Plex.sv` line 588:
`.clk(clk_sys)`). The 100 MHz figure had no provenance — it was the default
SDRAM PLL output, not the decode clock. See `docs/decode-throughput.md` for the
traced instantiation chain.

The real budget is **20 MHz / (1170 × 25) ≈ 684 cycles/MB**, not 3,418.

~~The decode schedule has about 100 MHz / (1170 * 25) = 3418 fabric cycles/MB.~~

A raw byte-serial fetch+write would be 987 cycles/MB plus arbitration latency.
**At the real 684 cycles/MB budget, raw byte-serial DPB traffic alone (987
cycles/MB) EXCEEDS the budget by 1.44×.** 64-bit coalesced transactions
reduce this to ~124 cycles/MB, which fits, but leaves only ~560 cycles/MB
for CAVLC, inverse transform, deblock, control, and arbitration overhead.
If arbitration ever forces worst-case single-byte DDR transactions, this budget
does not close and the DPB requester must be upgraded before silicon gating.

## Memory choice

Use the proven HPS DDR path first, but keep the DPB as a separate module from
`ddr_frame_store`. Its access pattern is random displaced reference reads plus
filtered MB writeback, while presentation is sequential line streaming. Current
presentation bandwidth at 624x480 RGB565/60 is ~36 MB/s, so adding ~29 MB/s DPB
traffic remains below even a conservative DDR budget. SDRAM remains the planned
escape hatch once the project controller is product-ready; if SDRAM bring-up
slips, MC is not blocked because the initial DPB controller targets DDR.

## Correctness contract

Only filtered/deblocked samples may be written to the DPB current picture.
`IDR` invalidates any prior reference. A current picture is promoted to the sole
reference only at frame boundary after all filtered MBs have been committed.
The reference fetcher returns raw bordered windows with normative edge
replication; interpolation and weighted arithmetic stay in the MC block.
