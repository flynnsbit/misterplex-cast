# MiSTerPlex v0.9.0-pre

> Historical. The current package is rc5 and does ship `cores/Plex.rbf` as the fallback core. Install from `README.md`. This note describes an older named-pair package.

Pre-release of the **three gold named pairs** (`Plex_480p`, `Plex_240p15`,
`Plex_480i`) plus a **720p24 lab pair** that played a 1280×720 title at
about 24 Hz with audio keep-up.

This is not a tagged 1.0. Every sibling
reports `CORENAME=Plex`, so a leftover `Plex.rbf` mis-pairs 720p24 onto the
480p daemon (black / test pattern, no chevron).

Install: follow `README.md` in this package. `install_paired_all.sh` is not included.

## Pairs (do not mix)

| Load this (Utility) | Use it for | RBF md5 | Daemon | Notes |
|---------------------|------------|---------|--------|-------|
| **Plex_480p** | HDMI 240p or 480p content | `07f54d9f8f0eda2fe75d9cc314f6de54` | `misterplexd.480p` (`5f1c8614` / sha256 `acfa03d7…e7c`) | Gold. Display **must** be 480p |
| **Plex_240p15** | 15 kHz 240p RGB CRT | `4d6efef954acf7b33747f35ac2878c1b` | same bytes as 480p | Gold. `[Plex] vga_scaler=0` |
| **Plex_480i** | 15 kHz 480i RGB CRT | `61db00e7d54efad7c1a456b127b798bd` | same bytes as 480p | Gold. `[Plex] vga_scaler=0` |
| **Plex_720p24** | HDMI 1280×720 24p (lab) | `0eea3580a5a0dacf60bf65c50499bcc9` | `misterplexd.720p24` (`0ffc1483` / sha256 `c3a69352…0bb`) | Lab-pre, not a withheld gold sibling |

Daemons are the lab-proven binaries. Do **not** replace `misterplexd.480p` with
a current-tree rebuild (that stall class is why the 480p pair is frozen).
`misterplexd.240p15` / `.480i` are named copies of the 480p gold daemon.

Watcher overlay on all four cores:

```
AV_PRESENT_LEAD_MS=40
AV_RESYNC_DROP_MS=0
FFMPEG_SWS_FLAGS=fast_bilinear
FFMPEG_FPS_FILTER=off
OSD_CONTROL=0
```

720p24 also gets `DECODE=1280x720` / `TRANSCODE_PROFILE=720p`. Leftover F12
480p bits must not retarget this RBF onto the 480p ladder.

## Gold pair rates (unchanged)

| Core | Content | Lab unique |
|------|---------|------------|
| `Plex_480p` | 240p (Display 480p) | 23.99 |
| `Plex_480p` | 480p 24p | 23.96; Star Trek-class ~23.7 |
| `Plex_240p15` | 15 kHz 240p CRT | 23.94 |
| `Plex_480i` | 15 kHz 480i CRT | 23.71 |

ascal 15 kHz `video_mode` is RED. Display=240p on `Plex_480p` is a dead store.

---

## 720p24 findings (lab)

Star Trek TNG S1E1 is **1440×1080 HEVC 24p**, not native 720. Product path is
PMS `transcode/universal` `videoResolution=1280x720` into the L4 core
(`0eea3580`, doorbell `0x3047F000`, 1280×720 @ ~24.00–24.1 Hz, `PLEX_CLK_SYS_24`).
HDMI idle is the two-color orange chevron; play must be Trek, not SMPTE/testsrc.

### What stopped unique 24 fps

The 24 Hz HDMI beam was already correct. Unique present was 12–21 fps.

| Bottleneck | Symptom | Fix |
|------------|---------|-----|
| Combined I420 pipe | unique ~14.5 | inproc libav H.264 (annex-B fifo), not undersized I420 pipe |
| Dual-bank memcpy | `copy_us=5280`, unique 21.5 | dest-only write-combine stick ingest (`MPX_STICK_I420=1`); no mirror into the other bank |
| 720p hold-to-audio | queue 180–310 ms, unique off 24 | skip `holdAudioToPictures` on combined 720p; kick-on-swap |
| Prefetch-to-tmpfs | 20 s `waitPid` killed the live 91 min transcode → truncated TS, 0 frames | always stream HTTP (`prefetched=0`, `url_local=0`) |
| Identity clip theater | `farpoint_1280x720.mp4` / `MPX_720P_CLIP` sendfile | deleted; exclusive path is live universal 40868 |

### What stopped audio keep-up

| Bottleneck | Symptom | Fix |
|------------|---------|-----|
| Second HTTP audio (`spawnAudioOnly`) | `av_drift_ms` locked ≈ −5000; `audio_s` ≈ 0.5× wall | one remux: HTTP → annex-B fifo + 48 kHz s16le PCM fifo. ARM libav is `--disable-network --disable-everything --enable-decoder=h264` (no HTTP, no AAC) |
| Dual-output fifo deadlock | PCM filled during 2 MiB probe; `avformat_open` stall | probesize 128 KiB, `analyzeduration 0`, drain/gate PCM during open |
| 120 ms gated PCM cap | discarded ~1.3 s of t=0 audio; `av_drift_ms` ≈ −1100…−1400 | keep-front cap 2500 ms; release the gate at `avformat_open` |

Abort if the remux PCM fd is missing. Do not fall back to `spawnAudioOnly`.

### Measured soaks (40868, daemon `0ffc1483`, RBF `0eea3580`)

| Soak | pfps | hw_fps | audio_s / wall_s | av_drift_ms | drops | path |
|------|------|--------|------------------|-------------|-------|------|
| A | 23.9 | 24.01 | 42.24 / 42.06 | +16 | 0 | inproc, `prefetched=0`, `remux_pcm` |
| B | 23.9 | 24.01 | 43.20 / 43.05 | −21 | 0 | same |

HDMI: play MEAN=20.7 `ORANGE_PX=0` (Trek, not chevron). Idle MEAN=39.7
`ORANGE_PX=26999` (two-color orange chevron). Grey chevron is a defect — no soak.

`Plex_720p24` is **lab-pre**. It is packaged so the box can load it, but it is
not a gold sibling of 480p / 240p15 / 480i.

Host-side 720p transcode of the 1440×1080 HEVC Part is
`scripts/pms_720p_proxy.py` (libx264 baseline 1280×720 24000/1001 + AAC 48 k).
It must ffmpeg the Part, never sendfile a 720p identity file.

## Install

This package does not include `install_paired_all.sh`. Follow `README.md`.

Set `PLEX_BASE=http://YOUR-PLEX-SERVER:32400` in
`/media/fat/linux/misterplex.conf`. Keep `PRESENT=fpga`.

Load **one** named core from `_Utility`. The watcher starts the matching
daemon from `/media/fat/misterplex/rbf_daemon_pairs.txt`.

720p WC ingest needs `/dev/mplex_ddr` (`kmod/mplex_ddr`). Without it the
720p daemon falls back off dest-only stick ingest.

## What this pre-release is not

- Not a tagged 1.0.
- 720p24 is lab-pre, not gold.
- ascal 15 kHz `video_mode` is not supported.
- Display=240p on `Plex_480p` is not a working 240p framebuffer.
- Do not restore a generic `Plex.rbf`.
