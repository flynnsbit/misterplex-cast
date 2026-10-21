# Decode throughput budget

This note is a realtime ratchet for the FPGA decode path. It deliberately
separates measured cost from unknown future cost; unknown stages are not counted
as free.

## Clock provenance (traced and verified 2026-07-27)

Every decode stage runs on a single clock domain. The chain is:

```
FPGA_CLK2_50 (50 MHz board osc)
 └─ pll_0002.v: altera_pll outclk_0 = "20.000000 MHz"     → clk_sys
    └─ Plex.sv line 215:   .outclk_0(clk_sys)
       └─ Plex.sv line 588: stream_path spath .clk(clk_sys)
          └─ stream_path.sv: every submodule gets .clk(clk)
             ├─ stream_ingest    .clk(clk)   line 103
             ├─ ddr_bitstream_reader .clk(clk) line 116
             ├─ bitstream_fifo   .clk(clk)   line 148
             ├─ nalu_scanner     .clk(clk)   line 164
             ├─ sps_parser       .clk(clk)   line 190
             ├─ pps_parser       .clk(clk)   line 205
             ├─ slice_hdr_parser .clk(clk)   line 232
             └─ decode_stub      .clk(clk)   line 293
```

**The decode clock is 20 MHz. There is no 100 MHz decode path.**

The 100 MHz SDRAM clock (`clk_sdram`, outclk_1) and 90 MHz DDR bridge clock
(`clk_ddr`, outclk_2) are separate outputs of the same PLL. They drive the
SDRAM controller and f2sdram bridge respectively, NOT the decode pipeline.

**Provenance of the "100 MHz / 3,418 cycles/MB" figure:** `docs/p3-mc-dpb-bandwidth.md`
used the SDRAM default clock (100 MHz) in the budget calculation instead of the
actual decode clock (20 MHz). This error has been corrected. The number had no
traced provenance — it was an assumption that was never verified against the RTL.

## Declared assumptions

| Parameter | Value | Source |
| --- | ---:| --- |
| Target frame rate | 25 fps | PMS 480p stream target |
| 480p coded geometry | 624×480 = 39×30 MBs = 1170 MB/frame | SPS/profile contract |
| `stream_path` clock | 20.000 MHz | `pll_0002.v` `outclk_0`; `Plex.sv` connects `stream_path.clk(clk_sys)` |
| SDRAM clock | selectable, default 100 MHz; `SDRAM_CLK_142` makes `clk_sdram` 142 MHz | `Plex.sv`/PLL |

The decode pipeline is clocked by `clk_sys`, not `clk_sdram`. Therefore the
480p realtime budget is:

```
1170 MB/frame × 25 fps = 29,250 MB/s
20,000,000 cycles/s ÷ 25 fps = 800,000 cycles/frame
800,000 cycles/frame ÷ 1170 MB/frame = 683.761 cycles/MB
```

## Raw measured cycle counts

Measured with Verilator 5.051 using the existing
`tests/unit/test_stream_path_full_frame_compare.sh` harness. The authoritative
cycle source is `build/p3_full_frame/frame_planes_compare.json` field
`summary.cycles`; this is full testbench time from reset/byte injection through
diagnostic frame output. It is not a native decode-quality metric and has no
stage breakdown. `build/p3_full_frame/native_frame_score.json` is the companion
quality scoreboard and has no cycle fields.

| Fixture | Frames | Geometry | Total cycles | cycles/frame | cycles/MB | Budget cycles/MB | Margin |
| --- | ---:| --- | ---:| ---:| ---:| ---:| ---:|
| `plex_inter_p16_baseline_320x240_12f.264` | 12 | 20×15 MB | 969,210 | 80,767.500 | 269.225 | 2,666.667 | 9.905× |
| `wcap_residual14_idr_plus_p.264` | 2 | 20×15 MB | 160,095 | 80,047.500 | 266.825 | 2,666.667 | 9.994× |
| `plex_inter_p16_baseline_624x480_12f.264` | 12 | 39×30 MB | 3,641,853 | 303,487.750 | 259.391 | 683.761 | 2.636× |

Additional raw integration signal:

| Harness | Raw signal | Value | Meaning |
| --- | --- | ---:| --- |
| `test_stream_path_deblock_integration.sh` on `wcap_residual14_idr_plus_p.264` | `recon_sig_3b_cycles` | 16,523 | deblock/stream integration liveness counter, not a per-stage cycle cost |
| `test_h264_multinal_stream_path.sh` 320×240 12f fixture | `recon_sig_3b_cycles` / `cycles` | 39,780 / 55,319 | P-slice DPB/MC liveness context from w-cabac; not full-frame P quality and not a stage cost |

## Stage coverage

| Stage | Status | Current measured cost |
| --- | --- | ---:|
| Aggregate current `stream_path` simulation | measured | 259.391 cycles/MB at 624×480 |
| Annex-B/SPS/PPS/slice parse | included in aggregate | no separate counter |
| CAVLC/dequant/IDCT/intra prediction | included in aggregate for implemented I path | no separate counter |
| MC/DPB fetch for correct P-slices | **UNKNOWN** | not yet bit-exact in this branch |
| Deblock cost | **UNKNOWN** | liveness tested, no per-frame cost counter |
| Product DDR writeback/present arbitration | **UNKNOWN** | current full-frame sim emits diagnostic RGB/I420 comparison, not full product arbitration |

## Stage coverage (per-stage cycle accounting, 2026-07-27)

The aggregate 259 cycles/MB was **98.7% decode_stub diagnostic paint overhead**.
Per-stage instrumentation (via `stub_busy`/`fs_wr_reset`/`fs_swap` phase
transitions in the Verilator testbench) reveals the true breakdown:

| Stage | cycles/MB | Status | Method |
| --- | ---:| --- | --- |
| parse_cavlc | 3.2 | **measured** | stub_busy rise → fs_wr_reset transition |
| dequant_idct | 0 | **measured** | combinational (h264_dequant4x4 + h264_idct4x4) |
| intra_pred | 0 | **measured** | combinational (DC pred=128 in h264_recon4x4) |
| diagnostic_paint | 256 | measured, **NOT PRODUCTION** | fs_wr_reset → fs_swap; WxH pixels at 1 px/cycle |
| mc_interpolation | **?** | **NOT IMPLEMENTED** | h264_inter_pred.sv is diagnostic-only |
| deblock | **?** | **NOT IMPLEMENTED** | h264_deblock.sv exists but not in pipeline |
| ddr_write | **?** | **NOT IMPLEMENTED** | ddr_frame_store not instrumented |
| injection_overhead | 10 | measured, **TESTBENCH ARTIFACT** | ioctl 2 cycles/byte; product uses DDR DMA |

**What "MARGINAL 2.64x" actually measured:** the ratio of the realtime budget
(684 cycles/MB) to the aggregate cycle count (259 cycles/MB), where the
aggregate is dominated by decode_stub painting 624×480 diagnostic pixels per
frame — an operation that does not exist in the production decoder. The three
most expensive production stages (MC, deblock, DDR write) contribute exactly
**zero measured cycles** because they are not built yet. The margin is
**illusory** and will collapse when those stages are implemented.

**Note on parse_cavlc timing:** the 3.2 cycles/MB figure includes P-frame
decode_stub WAIT_MAX timeout (4096 cycles each) because `residual_place_pulse`
does not fire for unimplemented inter frames. The IDR-only parse cost is
~0.25 cycles/MB (287 cycles for 1170 MBs). Once inter is implemented and
residual arrives for every frame, this number will change.


Do not use stale `build/p3_full_frame_624/native_score_624.json` artifacts if
present. The full-frame candidate stream is `I420_FROM_RGB565` diagnostic output;
the cycle number is useful, but its pixel quality is not native-I420 evidence.

## DDR bandwidth

The current DPB fetch/write shape on w-rel's branch issues:

- MC reference fetch: 441 luma + 81 U + 81 V = **603 reads/MB**.
- Filtered/current writeback: 256 luma + 64 U + 64 V = **384 writes/MB**.
- Total DPB traffic: **987 B/MB**.

At 29,250 MB/s:

```
987 B/MB × 29,250 MB/s = 28,869,750 B/s = 28.87 MB/s
```

The display frame store also consumes one 624×480 I420 frame per presented
frame:

```
624×480×1.5×25 = 11,232,000 B/s = 11.23 MB/s
```

The 624×480 12-frame fixture is 70,348 bytes, or 146,558 B/s at 25 fps. Even
doubling that for compressed bitstream write/read is less than 0.30 MB/s.

Known modelled traffic is therefore about **40.4 MB/s** before refresh/arbitration
losses. Raw bandwidth should be enough, but this is not a pass: the bitstream
reader, DPB, and frame store contend for the same `DDRAM_*` path, and the live
PLXF-missing fault means arbitration/liveness remains a first-class risk.

## Per-stage budget allocation (estimated, 2026-07-27)

**Owner: w-c1. Status: ESTIMATE — no unbuilt stage is measured.**

Total budget: **684 cycles/MB** at 20 MHz / 25 fps / 1170 MB per frame.

After 64-bit coalesced DPB traffic (~124 beats/MB for reference fetch + frame
write), approximately **560 cycles/MB** remain for computation.

| Stage | Allocated | Reasoning |
| --- | ---:| --- |
| parse_cavlc (full) | 50 | Current 3 cycles/MB is first-MB-only. Full MB parsing (mb_type, CBP, per-block CAVLC for 24 blocks I420, Intra_16x16 Plane, I_PCM) will be much higher. CAVLC is fundamentally serial. |
| dequant_idct | 48 | 24 blocks/MB at 2 cycles/block in a 2-stage pipeline. Currently combinational for single blocks; needs registered outputs for all-block throughput. |
| intra_pred | 30 | DC is trivial but directional modes need neighbor reads. Intra_16x16 Plane requires per-pixel MAC. I_PCM bypasses 384 bytes. |
| **mc_interpolation** | **250** | **DOMINANT COST.** 21×21 luma qpel window through 6-tap filter + DDR reference fetch (~100 beats with arbitration). Without adjacent-MB overlap exploitation: ~250. With overlap: ~150. Sub-MB partitions multiply fetch count. |
| deblock | 100 | 16 luma edges × ~6 cycles/edge (BS→threshold→filter) + chroma. Pipeline-able if filter has 2-cycle throughput. |
| ddr_write | 50 | 48 coalesced 64-bit beats + arbitration turnaround. |
| control_overhead | 30 | Pipeline stalls, MB-boundary bookkeeping, QP delta, slice header re-parse, arbitration bubbles. |
| **TOTAL** | **558** | |
| **Remaining margin** | **126** | 684 − 558 = 126 cycles/MB = **1.23× margin** |

### Adjacent-MB reference overlap (key to MC feasibility)

For P_L0_16x16 with similar MVs on horizontally adjacent MBs, the 21×21
reference window overlaps by ~16 columns. A line buffer can amortise this to
~40 fresh DDR reads/MB instead of ~100, saving ~60 cycles. **This is the
difference between MC being feasible and MC blowing the budget.** w-rel should
design the reference fetch with overlap exploitation from the start.

### What this excludes

- **Intra_16x16 Plane mode** (not implemented per w-cabac coverage audit, QP 5–27 only)
- **I_PCM** (not implemented in RTL)
- Arbitration contention from display reads (frame_store consumer)
- Content variation (high-motion P-frames with sub-MB partitions)
- CABAC (not relevant — Baseline profile uses CAVLC only)

## Verdict

### Aggregate current path

**MARGINAL.** The current 480p aggregate path measures 259.391 cycles/MB against
a 683.761 cycles/MB 20 MHz budget, leaving 2.636× headroom. That is encouraging,
but not a FEASIBLE verdict because the remaining P-slice correctness path,
deblock cost, and product DDR arbitration are still UNKNOWN. Treat any missing
stage as a named gap, not as zero cost.

### Estimated full-product budget

**TIGHT BUT FEASIBLE — conditionally.** The estimated 558 cycles/MB against the
684 budget leaves a 1.23× margin. This is NOT comfortable:

- MC interpolation alone (250 cycles) consumes 37% of the entire budget
- Deblock (100 cycles) consumes another 15%
- The 126-cycle margin is consumed by ONE bad assumption

The budget is achievable IF:
1. MC exploits adjacent-MB reference overlap (line buffer for shared columns)
2. DDR coalesces to 64-bit beats (not byte-serial — byte-serial at 987 cycles/MB is 1.44× over budget before any computation)
3. The deblock filter pipelines at ≤6 cycles/edge
4. Parse/CAVLC for full MBs stays under 50 cycles/MB

**If ANY of these conditions fails, 25 fps at 20 MHz is not achievable** and the
project needs an architecture conversation about moving decode stages to the
90 MHz `clk_ddr` domain or re-deriving the PLL. That conversation is cheaper
now than after MC is built.

The honest answer to "is this feasible?": **yes, but with essentially no safety
margin, and MC is the make-or-break stage.**


## Ratchet

`tools/check_decode_throughput.py` consumes the existing full-frame comparison
JSON and a `misterplex.decode_throughput_ratchet.v1` manifest. The full-frame
RTL gate now writes a separate report under `build/realtime_throughput/` for
known fixtures, copying run label, fixture source, and geometry into the JSON.
`tests/unit/test_decode_throughput_gate.sh` proves RED by lowering the cycle and
margin thresholds.
