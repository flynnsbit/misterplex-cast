# Phase 3 real PMS decoder scope: High/CABAC/B

This sizes the stream PMS actually delivered after the client-side Baseline sweep failed. No device
access or Quartus fit was used.

## Load-bearing CABAC throughput number

Measured PMS 480p delivery:

```text
requested: videoResolution=640x480 maxVideoBitrate=2500 videoProfile=baseline videoLevel=30
delivered: profile_idc=100 (High), level_idc=30, PPS entropy_cabac=1
slices over 12 s: vcl=302 idr=8 nonidr=294 i=22 p=165 b=115
refs: max_num_ref_frames=4
geometry: coded 624x480, display 618x480, crop LRTB=0,3,0,0 unit=2x2
bitrate: ~1344.3 kbps video-only
```

The coded frame is exactly `39×30 = 1170` macroblocks; at 25 fps that is **29,250 macroblocks/s**.
The compressed bitrate is only a lower bound on CABAC bin rate. For sizing, use:

| Model | Assumption | Required CABAC rate |
|---|---:|---:|
| Hard lower bound | one decoded bin per coded bit | **1.344 Mbin/s** |
| Planning | 300 bins/macroblock (~6.5 bins/coded-bit at this bitrate) | **8.775 Mbin/s** |
| Stress | 600 bins/macroblock (~13 bins/coded-bit) | **17.550 Mbin/s** |

Current product decode logic is in `clk_sys`. In this tree, `pll_0002.v` drives
`outclk_0 = 20.000000 MHz`, `Plex.sv` maps `outclk_0(clk_sys)`, `CLK_VIDEO = clk_sys`, and
MiSTer DDRAM gets `DDRAM_CLK = CLK_VIDEO`. At **20 MHz**:

| CABAC engine | Throughput | Verdict against planning/stress |
|---|---:|---|
| 1 bin/cycle optimistic serial engine | 20.0 Mbin/s | Passes planning 2.28×; passes stress only 1.14× |
| 2 cycles/bin first-cut engine | 10.0 Mbin/s | Barely passes planning 1.14×; fails stress |
| 3 cycles/bin sequenced engine | 6.67 Mbin/s | Fails planning |

**Verdict on CABAC:** at the current 20 MHz decode clock, CABAC is not safely real-time unless we
build an optimized one-bin-per-cycle decoder with essentially no stalls. A conventional sequenced
2–3 cycle/bin implementation has no usable margin and can miss real time on CABAC alone before
motion compensation, transform, memory arbitration, and display traffic are counted. A product-safe
High decoder needs either a faster decode clock domain or a proven 1-bin/cycle CABAC core with
real Verilator throughput tests and timing closure.

## DPB storage and memory path for High/B/4 refs

For coded 624×480 YUV420:

```text
one decoded frame = 624 * 480 * 1.5 = 449,280 B
4 reference frames = 1,797,120 B
4 refs + current reconstruction = 2,246,400 B
4 refs + current + one YUV present/reorder frame = 2,695,680 B
one RGB565 output frame = 599,040 B
```

BRAM is out. SDRAM is still not hardware-proven. The only plausible storage path is the DDR3-backed
frame/reference store, but the current integration is still clocked through the 20 MHz video/DDR
path above; the previously discussed ~160 MB/s class is the optimistic 64-bit-at-20 MHz ceiling,
not a proven decoded-video subsystem with CABAC and B-frame arbitration.

Approximate 624×480@25 memory traffic for the measured slice mix:

- one-reference quarter-pel read budget scaled from 640×480: ~0.70 MB/frame
- B/P mix reference-read multiplier: `(165 P + 2*115 B) / 302 = 1.31`
- average reference reads: ~0.92 MB/frame
- current YUV write: 0.45 MB/frame
- present/writeout: 0.45 MB/frame if YUV stays native, 0.60 MB/frame for RGB565
- narrow average: ~1.82 MB/frame YUV path = **45 MB/s @25**, or ~1.97 MB/frame RGB path =
  **49 MB/s @25**
- practical planning with sub-MB partitions, cache-line/halo waste, deblock/reorder, and arbitration:
  **60–90 MB/s @25**

Capacity is easy in DDR3; correctness and sustained arbitration are the risks. SDRAM should not be
assumed for this path until hardware evidence exists.

## What survives from the proven CAVLC path

The proven work is not lost, but High/CABAC does replace the weakest part:

- **Survives:** inverse quant, 4×4 IDCT, reconstruction add/clip, and intra prediction remain valid
  for blocks that are still coded as 4×4 residuals. The silicon-proven residual checksum and the
  host/RTL IDCT goldens are still useful below the entropy layer.
- **Does not survive as-is:** the CAVLC residual/token walker. CABAC must produce the same
  coefficient levels, coded-block flags, `mb_type`, `sub_mb_type`, MVDs, `ref_idx`, skip/direct
  decisions, and QP deltas through a new context-adaptive arithmetic front end.
- **High-profile caveat:** if PMS enables `transform_8x8_mode_flag` and uses 8×8 transform blocks,
  the current 4×4 IQ/IDCT is insufficient for those blocks. The product must parse/detect this and
  either add 8×8 transform support or fail closed.

## Scope of future A vs B

### (A) Decode PMS as delivered

Required new hardware/software scope:

- Full H.264 CABAC arithmetic decoder: range/offset renorm, bypass/terminate bins, 460 context
  states, context initialization by slice type/QP/cabac_init_idc, and all binarizations used by
  macroblock, residual, ref-index, MVD, and transform syntax.
- B-slice decode: POC, DPB management, reference list 0/1 construction, ref-list reordering/MMCO
  guardrails, direct/skip modes, bidirectional prediction, and frame reordering.
- Motion compensation for at least P16×16/P16×8/P8×16/P8×8 and likely sub-8×8 partitions if PMS
  uses them; quarter-pel luma and chroma interpolation remain mandatory.
- High-profile feature detection: 8×8 transform/scaling matrices, weighted prediction/weighted
  bipred, and any unsupported interlace/field syntax must be explicit fail-closed gates.
- DDR3-backed YUV420 reference store with at least 4 refs plus current/present buffers and measured
  arbitration at 60–90 MB/s for this stream class.

**Honest verdict for A:** technically possible in principle, but not feasible in a sane near-term
MiSTerPlex schedule at the current 20 MHz decode/DDR clock and current RTL maturity. CABAC alone is
on the edge; adding B-slices, four-reference DPB, High-profile feature detection, and DDR3
arbitration makes this a full High-profile decoder project, not an extension of the current CAVLC
rungs.

### (B) Server-side XML profile to force Baseline/CAVLC

If W-A4 can make PMS actually deliver Baseline/CAVLC/I-P, the scope collapses back to the tractable
path already modeled in `phase3-inter-prediction.md`: no CABAC, no B-slices, no weighted prediction,
and ideally `ref=1`/P16×16-only for the first hardware rung. This is now a strategic prerequisite
for moving decode off ARM in a practical timeframe unless the project deliberately chooses to build
a full High/CABAC/B decoder.

## Recommendation

Keep the existing unsupported-stream guard. Treat server-side Baseline delivery, or ARM/FFmpeg
fallback for High/CABAC/B streams, as a hard product requirement. Do not start High/CABAC RTL until
there is an explicit decision to fund a full High-profile decoder and a faster/proven DDR3/decoder
clock plan.
