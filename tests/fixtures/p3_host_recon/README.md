# Phase 3 host reconstruction fixtures

These files are stable inputs/outputs for RTL testbenches.

- `plex_real_baseline_320x240_1f.264` — checked-in H.264 Baseline Annex-B IDR vector,
  intentionally **6739 bytes**.
- `mb0_luma_v1.json` — `format=misterplex.p3.luma_mb.v1`; MB0 Y-plane prediction,
  dequantized 4×4 coefficients, signed post-IDCT residual samples, and reconstructed pixels.
- `frame_mae_v1.csv` — `format=misterplex.p3.frame_mae.v1`; per-MB Y-plane MAE against
  FFmpeg `-skip_loop_filter all`.

Do not change field names, ordering, or vector bytes without updating the format version and
coordinating with RTL consumers.

## Shared macroblock golden extractor

`tools/extract_h264_golden.cpp` emits deterministic per-macroblock JSON in
`format=misterplex.p3.mb_golden.v1`. Each luma 4×4 block records CAVLC diagnostics
needed by entropy/inter/deblock consumers: `nA`/`nB` availability and TotalCoeff,
predicted `nC`, selected `coeff_token_table`, parsed `total_coeff`, bit offsets,
scan-order coefficients, dequant, IDCT, prediction, and reconstruction samples.
The current checked-in red/green unit uses the proven 320×240 IDR vector and
independently grades MB0 against `mb0_luma_v1.json`, `residual_gold::kCsum8 == 0x14`,
and the established first-4×4 recon signature `0x3b`.

The JSON also carries latency red-checks for the simulation-versus-silicon trap seen
on P3-3l2: MB0 true recon is `0x3b`, while pred-only and one-cycle-delayed residual
models are both `0x00`. Hardware consumers must align residual valid/data with the
prediction stage and reject these latency-error signatures; a fixture that accepts
pred-only `0x00` is certifying the exact failure observed on the DE10-Nano.

For integration bugs that only appear after a later NAL clears transient parser state,
use the additive multi-NAL sequence fixture in `tests/fixtures/p3_multinal/`. It keeps
`misterplex.p3.mb_golden.v1` stable while adding `misterplex.p3.nal_sequence.v1` metadata
for byte-exact ioctl injection and IDR→P parser re-entry.

Regenerate the deterministic MB0 JSON used by the unit guard:

```bash
make h264-golden-tools
./build/extract_h264_golden \
  --input tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264 \
  --mb 0 \
  --output build/p3_golden/mb0.json \
  --verify-mb0-reference tests/fixtures/p3_host_recon/mb0_luma_v1.json
```

The JSON schema reserves the inter-prediction fields (`motion_vectors`, partition mode, and
per-block residual bit offsets) so `w-cabac` and `w-rel` consume the same shape. In this branch,
the extractor deliberately supports Baseline/CAVLC **I-slice luma** macroblocks only; it must not
fabricate P-slice residual or motion-vector goldens before the real 624×480 stream asset is
captured and graded.

## Real 624×480 Baseline stream fixture

The required real PMS Baseline/CAVLC stream asset is still capture-dependent: this worktree has no
scheduled MiSTer/PMS device token, so no 624×480 bytes are checked in here. When the device window is
granted, capture the stream without committing URLs or tokens:

```bash
MISTERPLEX_STREAM_URL='http://YOUR-PLEX-SERVER:32400/...&X-Plex-Token=REDACTED' \
  OUT=build/p3_baseline_480p/plex_real_baseline_624x480_12s.264 \
  scripts/capture_baseline_annexb_fixture.sh
```

Expected geometry for that future asset is coded **624×480** = **39×30** macroblocks, display
**618×480** via right crop of 6 px. The committed extractor computes coded size as the macroblock
grid (`mb_width * 16`) so consumers must not assume 640-wide frames.
