# Phase 3 inter prediction scope

This is the host-side scoping note for moving beyond IDR-only decode. No device access or
Quartus fit was used.

## Survey first: what Baseline inter actually leaves

Available host-side assets already used as Plex-library/test content are H.264 Constrained
Baseline and contain **I/P only** (no B frames). Exported decoder motion vectors show quarter-pel
motion (`motion_scale=4`) even for Baseline content.

| Source | Frames | Types | B? | Motion-vector partitions | Max motion |
|---|---:|---|---:|---|---|
| `assets/avsync/sync_trekmatch_320x240_24_blip.mp4` | 720 | I=60 P=660 | 0 | 16x16=197307, 16x8=658, 8x16=296, 8x8=444 | 9.75×13.00 px |
| `assets/avsync/sync_24fps_blip.mp4` | 720 | I=60 P=660 | 0 | 16x16=195841, 16x8=376, 8x16=1264, 8x8=1436 | 12.50×19.00 px |
| `assets/avsync/sync_60fps_blip.mp4` | 1800 | I=30 P=1770 | 0 | 16x16=516271, 16x8=1314, 8x16=3334, 8x8=3936 | 12.50×20.00 px |
| `assets/avsync/sync_trekmatch_1080p24_blip.mp4` | 720 | I=35 P=685 | 0 | 16x16=5378814, 16x8=1730, 8x16=3562, 8x8=10356 | 55.00×37.00 px |

The checked-in inter vector deliberately pulls the cheap profile lever:

```text
scripts/gen_test_annexb_inter.py → plex_inter_p16_baseline_320x240_12f.264
bytes=27653 md5=fe5ba815b4d67b5b24d7de496facb15b
frames=12 I=1 P=11 B=0 refs=1 has_b_frames=0
x264: cabac=0:bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none
motion vectors=3095, partition set={16x16}, max motion=(72,36) quarter-pel = 18.00×9.00 px
```

`main` now requests the 480p PMS universal transcode as H.264 Baseline Level 3.0:

```text
videoResolution=640x480&maxVideoBitrate=2500&videoCodec=h264
&videoProfile=baseline&videoLevel=30
```

**Baseline changes the feasibility verdict:** inter prediction is no longer “general H.264”.
It is a tractable P-slice/CAVLC problem if the delivered stream really honors the request. Baseline
removes B-slices, CABAC, weighted prediction, and interlaced/field coding from the hardware scope.
That eliminates bidirectional reference lists, CABAC arithmetic decode, and weighted sample math.

**Critical PMS result (W-A4, 2026-07-26): the real server did not honor this constraint.** A
captured 640×480 universal transcode request with `videoProfile=baseline&videoLevel=30` delivered
H.264 **High** profile instead:

```text
W-A4 branch: feat/a4-sps-baseline @ b28e863
requested: videoResolution=640x480 maxVideoBitrate=2500 videoProfile=baseline videoLevel=30
ffprobe delivered stream: h264 High, width=618 height=480, level=30, 25/1
host SPS parse: profile_idc=100, level_idc=30, width=618, height=480
PPS parse: entropy_cabac=1
12 s VCL scan: vcl=302 idr=8 nonidr=294 i=22 p=165 b=115
video-only bitrate: ~1344.3 kbps over 12 s
PMS target note: mpegts universal target produced data; final mp4 target returned an empty body
```

So the Baseline-only decoder is feasible as an architecture, but **not yet a safe assumption for
actual Plex 480p output**. Until PMS delivery is proven Baseline/CAVLC/I-P, product code must detect
High/CABAC/B streams and fall back/report unsupported; silent decode is forbidden.

Follow-up: W-A4's full client-side sweep found Baseline forcing is impossible with URL/profile
parameters alone. The measured real-decoder scope for PMS-as-delivered (High/CABAC/B, 4 refs,
coded 624×480) is in [phase3-high-cabac-scope.md](phase3-high-cabac-scope.md).

What remains for 480p Baseline:

- P-slice macroblock syntax and CAVLC residuals.
- Motion-vector prediction and `ref_idx_l0` parsing.
- Luma quarter-pel interpolation and chroma eighth-pel/bilinear interpolation.
- P16×16 at minimum; likely P16×8/P8×16/P8×8 unless PMS/x264 can be constrained to
  `partitions=none`.
- Reference-picture storage and frame-num/MMCO guardrails.

If PMS can force `bframes=0`, `ref=1`, `weightp=0`, and P16×16-only partitions, the first hardware
inter rung is much smaller: one past reference frame and one vector per inter MB. Quarter-pel
interpolation remains mandatory. If PMS cannot disable sub-MB partitions, the survey says we must
support at least P16×16, P16×8, P8×16, and P8×8. Direct H.264 Parts can still be Main/High/CABAC
and must be detected/rejected rather than silently fed to the Baseline decoder.

## Goldens added

`tests/fixtures/p3_inter_pred/` contains:

- `plex_inter_p16_baseline_320x240_12f.264` — deterministic Annex-B source vector.
- `pframe1_mb_v1.json` (`format=misterplex.p3.inter_mb.v1`) — frame 1 per-MB motion partition
  data plus reconstructed Y samples.
- `frame_mae_v1.csv` (`format=misterplex.p3.inter_frame_mae.v1`) — all 12 frames × 300 MBs,
  Y-plane MAE versus FFmpeg decode.

`test_p3_inter_pred_vectors` regenerates the vector byte-for-byte, decodes with libav motion-vector
export, verifies the narrow profile (`profile_idc=66`, `level_idc<=30`, CAVLC PPS, `I=1 P=11 B=0
refs=1 parts=16x16`), and checks the JSON/CSV fixtures. Red perturbations cover MV data, MAE rows,
byte-identical regeneration, and unsupported-profile handling. The guard is intentionally
fail-closed: an unexpected B/CABAC or non-Baseline stream must be reported as unsupported, not
decoded incorrectly.

## Hardware cost and memory path under Baseline Level 3.0

Inter prediction is external-memory work, not BRAM work.

| Target | YUV420 reference frame | RGB565 output frame | Notes |
|---|---:|---:|---|
| 320×240 | 115,200 B | 153,600 B | feasible bandwidth even on modest external memory |
| 640×480 | 460,800 B | 614,400 B | not BRAM-resident; needs external memory |
| 720p | 1,382,400 B | 1,843,200 B | DDR3-only territory |

Level 3.0 caps the decoded picture buffer at **8100 macroblocks**. A 640×480 frame is
`40×30 = 1200` macroblocks, so the Level 3.0 worst-case DPB is:

```text
floor(8100 / 1200) = 6 decoded reference frames
6 × 640×480 YUV420 = 6 × 460,800 B = 2,764,800 B
current reconstruction frame = +460,800 B
worst-case YUV decode/reference working set ≈ 3.23 MB
```

That is impossible in BRAM but easy in HPS DDR3 capacity. It is still more complex than the desired
`ref=1` subset because ref-index parsing and reference-list addressing become real hardware state.
Therefore the first product target should **measure the actual delivered SPS `max_num_ref_frames`**
and fail closed above the implemented reference count.

For 640×480 P16×16 quarter-pel with one active reference, a rough per-frame memory budget is:

- reference read: ~0.72 MB/frame (luma 6-tap halo + chroma bilinear halo)
- decoded YUV reference write: 0.46 MB/frame
- present/output write:
  - RGB565 path: 0.61 MB/frame
  - YUV420 DDR path (`w-c2` direction): 0.46 MB/frame
- total:
  - RGB565 present: ~1.8 MB/frame → ~54 MB/s at 30 fps, ~108 MB/s at 60 fps before inefficiency
  - YUV420 present/reference: ~1.6 MB/frame → ~47 MB/s at 30 fps, ~94 MB/s at 60 fps

If P8×8/sub-MB partitions are allowed, interpolation halos increase reference reads; budget closer
to ~2.2–2.6 MB/frame at 640×480 is a safer planning number (~66–78 MB/s at 30 fps, ~132–156 MB/s
at 60 fps on the RGB565 path, about 25% lower if YUV420 remains native through DDR/present).
Multiple references primarily increase storage and address/ref-list complexity; each partition
still reads from one selected reference, but the decoder must keep enough prior frames resident.

**Architecture implication:** inter prediction should target the DDR3-backed YUV reference store.
The SDRAM path is currently not dependable, and BRAM cannot hold even one 640×480 RGB565 frame.
The reported DDR3 path around ~160 MB/s is enough for 640×480p30 and plausibly enough for
640×480p60 in the narrow P16×16/YUV420 case, but it is tight for sub-MB partitions plus RGB565
conversion traffic. Raising the DDRAM clock toward the expected ~800 MB/s class would make 480p60
comfortable and leave headroom for sub-MB partitions and deblock.

## Coordination note for `w-a4`

`w-a4` landed the 480p profile request (`videoProfile=baseline&videoLevel=30`) and PMS accepted the
request, and follow-up branch `feat/a4-sps-baseline` keeps guards that the request remains
Baseline/L3.0. W-A4's delivered-stream probe is still **negative**: High/CABAC/B was delivered
despite the request, with mpegts as the working PMS universal target and final mp4 returning an
empty body. Before hardware depends on Baseline, either make PMS actually deliver profile_idc=66
with PPS `entropy_coding_mode_flag=0`, I/P only, and `max_num_ref_frames` within the implemented
count, or keep the ARM/FFmpeg fallback for that stream.
