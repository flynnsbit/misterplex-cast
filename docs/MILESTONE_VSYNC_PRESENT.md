# Milestone: VSync present + product A/V cast (HDMI/VGA)

**Date:** 2026-07-24 → 2026-07-25  
**Lab:** `root@192.0.2.183`  
**RBF (verified):** `1441d409ad3f8ccc5dcb0033c32ff7c8`  
**Paths:** `fpga/Plex_MiSTer/output_files/Plex.rbf`, `releases/Plex_vsync_tear_1441d409.rbf`  
**Companion:** `misterplexd` with `PRESENT=fpga` `STREAM=0` `DECODE=320x240`

---

## User-visible result

- Plex Web (and companion cast) play full-motion video on MiSTer **HDMI and VGA**
- **No constant half-frame tears**; multi-panel logo glitches eliminated on holdoff2 verify
- **A/V clock locked:** wall-paced 48 kHz MrAudio, every RGB frame presented (no pfps=15 cap)
- **24 fps blip lipsync:** median flash↔beep offset ~**−13 ms** (within one 24p frame)
- Eyes-on: Thundercats and blip tests look correct (user confirmed 2026-07-25)
- **Check-in:** user reconfirmed *“looks good on video and vsync”* → git **`588e528`** + backlog stamp

---

## Problem (before)

| Symptom | Cause class |
|---------|-------------|
| Half frame: top one scene / bottom another | **Display bank flip mid-scan** (swap on DMA complete, not vsync) |
| Multi-panel crops of same logo in one frame | **Back buffer overwritten** before vsync flip while DMA kept writing |
| Black HDMI while companion “playing” | `has_frame=0` — swap_pending stuck when gated on write-idle |
| Judder / ~1 fps cast | `STREAM=1` IDR recon path used for interactive cast |
| pfps capped ~15 while vfps=24 | Present throttle |
| Audio drift / jumpy | MrAudio free-run; not wall-paced to 48 kHz |
| F12 / menu dead after play | SPI races leave Main wedged; need heal-on-stop |

Smoking-gun pre-fix frame: `captures/e2e/tc_glitch/t9/exact_9.03.png`  
(three mismatched Thundercats logo crops in one HDMI frame)

---

## What fixed it

### 1. VSync page-flip (`frame_store.sv`)

- Writes always go to the **non-display** bank (no mid-write tear).
- `swap_banks` only **latches** `swap_pending`.
- **Display bank flips only on `vsync_pulse`** (colorbars `frame_start` / display tick).
- While `swap_pending`, **ignore new writes** so the completed back buffer cannot be partially overwritten (kills multi-panel composites).

### 2. DMA hold-off (`ddram_frame_rd.sv`)

- **Do not start a new DDR→BRAM DMA while `swap_pending`.**
- Queue **one held start** (latest doorbell or SPI kick); launch when pending clears.
- Optional **mmap doorbell** at `0x3007F000` (`PLXK` magic + seq + bank) so the hot path can kick without SPI.
- SPI `status[12]/[13]` remains as fallback.

### 3. Present wiring (`present_core.sv`, `Plex.sv`)

- `fs_swap` + `fstart` → frame_store.
- `swap_pending` telem on status bit **78** (`{ddr_busy, swap_pending, slice_qp[5:0]}`).

### 4. Host product path (`misterplexd`)

- **`STREAM=0` + every-frame DDR F1** for cast (not STREAM recon ~1 fps).
- **Wall-48 kHz audio pace** so A/V share one clock; FFmpeg dual-pipe RGB+PCM.
- **`playing_` set early** (avoid race with zero frames).
- **Doorbell-preferred DDR present** with persistent `/dev/mem` map; short settle, no 100 ms SPI poll of pending (that collapsed pfps).
- ~~**Heal Main on stop** (`healMainReloadPlex`)~~ — **removed 2026-07-26.** That was a
  bandaid for corrupting Main's SPI handshake; the handshake is now protected and the OSD
  word comes from a DDR mailbox, so nothing needs healing. See `docs/MILESTONE_AVSYNC_SEEK.md`.

### 5. Lab conf (on device)

```
PRESENT=fpga
STREAM=0
DECODE=320x240
MATCH_SOURCE_HZ=off
```

LCD-safe `MiSTer.ini`: `video_mode=5` (800×600@60), `vsync_adjust=0`, `vga_scaler=1`.

---

## Verification evidence

| Test | Result | Path |
|------|--------|------|
| Blip 24 fps Web cast A/V | PASS_TIGHT, median offset ~−13 ms | `captures/e2e/blip24/` |
| Thundercats Web cast | playing, has_frame=1, picture OK | `captures/e2e/verify_hdmi/` |
| holdoff2 30 s HDMI metrics | half/mid/multi **0.00/s** | `captures/e2e/tc_glitch/holdoff2/REPORT.txt` |
| Pre multi-panel | `exact_9.03.png` | `captures/e2e/tc_glitch/t9/` |
| E2E blip suite notes | | `captures/e2e/REPORT.md` |
| Plex Web cast path notes | | `captures/e2e/REPORT_PLEXWEB_CAST.md` |

**Metrics (holdoff2 REPORT):**

| | half-frame/s | hard mid-seam* | multi-panel* |
|--|-------------|----------------|--------------|
| PRE | 0.14–0.21 | high (content-edge noise) | high |
| POST | **0.00** | **0.00** | **0.00** |

\* Pre hard/multi rates include content-edge false positives; half-frame blank-band and eyes-on stills are the robust SoT.

---

## Residual / non-goals this milestone

- Feature-film **dialogue lipsync** not fully quantified (blips prove clock; Trek lips still subjective).
- Main **still compromised during** heavy SPI if doorbell falls back; heal-on-stop is the recovery.
- Timing closure on Quartus may still show critical warnings (pre-existing class).
- Residual csum / WIDE colorbar work is adjacent history; this milestone is **present tear + product A/V**.

---

## How to re-verify

```bash
# Lab
ssh root@192.0.2.183
md5sum /media/fat/_Utility/Plex.rbf   # expect 1441d409…
curl -sS 'http://127.0.0.1:3005/player/timeline/poll?wait=0'

# Host: cast Thundercats or Sync 24fps Blip (Movies RK6) via Plex Web → MiSTerPlex
# Capture
ffmpeg -f v4l2 -input_format mjpeg -video_size 1920x1080 -framerate 30 -i /dev/video4 \
  -t 30 -c:v libx264 -preset ultrafast out.mkv
# Status while playing: has_frame=1
ssh root@192.0.2.183 '/media/fat/misterplex/bin/set_status --status'
```

---

## File map (this milestone)

| Area | Files |
|------|--------|
| RTL | `rtl/frame_store.sv`, `rtl/ddram_frame_rd.sv`, `rtl/present_core.sv`, `Plex.sv` |
| ARM | `arm/misterplexd/fpga_spi.{cpp,hpp}`, `media_player.{cpp,hpp}`, `main.cpp` |
| Tools | `tools/set_status.cpp` |
| A/V assets | `assets/avsync/*` |
| Evidence (text) | `captures/e2e/REPORT*.md`, `SUMMARY.txt`, `tc_glitch/holdoff2/REPORT.txt` |
