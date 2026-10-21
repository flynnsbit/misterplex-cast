# Phase 3 inter-prediction fixtures

Stable host-side fixtures for scoping the first FPGA inter-prediction rung.

- `plex_inter_p16_baseline_320x240_12f.264` — deterministic H.264 Constrained Baseline Annex-B vector from `scripts/gen_test_annexb_inter.py`; 12 frames, 320×240, 24 fps, one IDR plus 11 P frames, CAVLC, no B-frames, one reference, weighted prediction off, and P16×16 partitions only. It is intentionally separate from the 6739-byte IDR-only vector.
- `pframe1_mb_v1.json` — `format=misterplex.p3.inter_mb.v1`; frame 1 per-MB motion-vector partition data and reconstructed Y samples.
- `frame_mae_v1.csv` — `format=misterplex.p3.inter_frame_mae.v1`; all 12 frames, every macroblock, Y-plane MAE versus FFmpeg decode.

Do not change field names, ordering, or vector bytes without bumping the format version and coordinating with RTL consumers.
