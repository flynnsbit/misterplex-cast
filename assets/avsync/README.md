# A/V sync blip tests

Clips with a **white flash** and a **1 kHz beep** every 1.0 s (aligned in the file).

| File | Class | Size | fps | Notes |
|------|-------|------|-----|-------|
| `sync_24fps_blip.mp4` | product | 320×240 | 24 | DECODE path |
| `sync_30fps_blip.mp4` | product | 320×240 | 30 | |
| `sync_60fps_blip.mp4` | product | 320×240 | 60 | |
| `sync_trekmatch_1080p24_blip.mp4` | **source / Trek-class** | 1920×1080 | 24 | ~8 Mbps H.264 + AAC; PMS weak → 320×240 |
| `sync_trekmatch_320x240_24_blip.mp4` | product twin | 320×240 | 24 | ~1.5 Mbps; same flash/beep timeline |

Regenerate:

```bash
python3 scripts/gen_avsync_blip.py --only all
python3 scripts/gen_avsync_blip.py --only trekmatch
```

## Content

- **Visual:** full-frame white flash (~2 frames) at each integer second + “FLASH” label + red mouth bar during beep.
- **Audio:** 50 ms 1 kHz tone at the same times (silence otherwise).
- **On-screen:** label + frame counter.

## Product conf (fresh lipsync baseline)

```
PRESENT=fpga
STREAM=0
DECODE=320x240
AUDIO_DELAY_MS=0   # no hardcoded lag; set only from measure evidence
```

## What “good” looks like

1. Flash and beep simultaneous on display+speakers within **1 frame** (24p ≈ 42 ms).
2. Logs: `vfps ≈ content fps`, `pfps ≈ vfps`, `audio_s ≈ wall_s`.
3. Seek/resume: cast mid-title or scrub → picture matches plant (±1 s).

### Lab stamp (2026-07-25, RK10 @ AUDIO_DELAY_MS=0)

Evidence: `captures/e2e/avsync_trekmatch/avsync_report.txt`  
**G-AV2 PASS** (n=12) · **G-AV3 FAIL** median **−60 ms** (\|m\|=60 > 42).  
Next: conf-only `AUDIO_DELAY_MS≈60` + remeasure (do not invent PASS).

## Trek dialogue stress

Use real title `/library/metadata/40710` (TNG S1E1) at **~3:54** after seek fix.  
Source-class blip stresses the same film cadence + high bitrate before weak ladder.

## Lab install

```bash
# PMS Movies (docker example)
cp assets/avsync/sync_trekmatch_*.mp4 /path/to/movies/misterplex-avsync/
# MiSTer local
scp assets/avsync/*.mp4 root@192.0.2.183:/media/fat/misterplex/avsync/
```
