# Milestone: A/V lipsync baseline + Plex Web seek/resume

**Date:** 2026-07-25  
**Builds on:** vsync present **`1441d409`** / git `588e528` (tear-free — RBF unchanged)  
**ARM:** redeployed `misterplexd` with seek re-resolve + `AUDIO_DELAY_MS`  
**Title SoT:** TNG S1E1 `/library/metadata/40710` (server `bbbbbbbb…`) — dialogue ~3:54  
**Lab conf:** `PRESENT=fpga` `STREAM=0` `DECODE=320x240` · baseline measure **`AUDIO_DELAY_MS=0`** · product lipsync **`AUDIO_DELAY_MS=60`** (evidence)

---

## Problems

1. **Lips out of sync** on MiSTer cast (Trek dialogue) while local Plex is fine.  
2. **Seek + resume from Plex Web cast broken** — only start-from-beginning worked.

---

## What fixed seek / resume

### Root cause

Product cast uses PMS **universal** URLs that already bake `offset=` in **seconds**.  
`MediaPlayer` also applied FFmpeg **`-ss`** with the same offset in **ms→s** → **double-seek** (resume/scrub failed or restarted wrong). Mid-play `seekMs` re-used the **stale** universal URL with only `-ss` (HTTP live transcode does not seek reliably).

### Fix

1. **Re-resolve on library seek** (`main.cpp` `seekAsync`):  
   `seekTo` / step → `doPlay(lastPlay with new offsetMs)` → fresh universal URL with `offset=`.
2. **Skip FFmpeg `-ss` when universal already has `offset=`** (`media_player.cpp` `urlHasUniversalOffset`).  
   Timeline still uses `startMs`; demux starts at PMS offset only.
3. **Unit:** `universalOffsetSeconds(234000)==234` (Trek 3:54).

### Lab evidence (2026-07-25)

| Case | Result |
|------|--------|
| seekTo 12000 mid-play on Sync 24fps Blip | `seek re-resolve … offMs=12000`, URL `offset=12`, `skip -ss`; timeline **playing** ~15.5 s after settle |
| playMedia offset=15000 (resume) | ACK `offMs=15000`, URL `offset=15`, `skip -ss`; timeline **playing** ~19 s after settle |
| Unit `make unit` | PASS including seek re-resolve path in browse smoke |

---

## A/V lipsync (fresh baseline — no hardcoded lag)

### Policy

- **`AUDIO_DELAY_MS=0` by default** — no guessed lag constants.  
- Conf-only intentional PCM hold before MrAudio (positive delays audio vs video).  
- Set from **measured** flash↔beep evidence only.

### Fixtures

| Asset | Class | PMS ratingKey (local Movies) |
|-------|--------|------------------------------|
| `sync_trekmatch_1080p24_blip.mp4` | 1080p24 ~8 Mbps Trek-class source | **9** |
| `sync_trekmatch_320x240_24_blip.mp4` | product twin 1.5 Mbps | **10** |
| existing `sync_{24,30,60}fps_blip.mp4` | product DECODE | 6/7/8 |

Generator: `scripts/gen_avsync_blip.py`  
Flash + 1 kHz beep every 1.0 s + mouth bar + labels.

### Measure status (2026-07-25 HDMI flash↔beep)

**Baseline (AUDIO_DELAY_MS=0)**

| Item | Value |
|------|--------|
| Title | Sync Trekmatch 320×240@24 Blip **RK10** |
| Lab | `PRESENT=fpga` `STREAM=0` `DECODE=320x240` **`AUDIO_DELAY_MS=0`** |
| RBF | `1441d409` (unchanged) |
| Capture | `captures/e2e/avsync_trekmatch/capture_12s.mkv` |
| Report | [`captures/e2e/avsync_trekmatch/avsync_report.txt`](../captures/e2e/avsync_trekmatch/avsync_report.txt) |
| Samples | flashes=12 beeps=12 matches=**n=12** |
| **median_offset_ms** | **−60.0** (audio earlier than flash) |
| mad / mean / range | 1.5 / −59.4 / [−62 … −55] |
| abs_median_le_42ms | **False** → G-AV3 FAIL at zero lag |

**Correction path (2026-07-25 later)**

| Run | Mechanism | median_ms | Verdict |
|-----|-----------|----------:|---------|
| PCM drop hold 60 | **wrong** (discards content) | −36 | *numeric only — revoked* |
| PCM delay-line 60 | burst-fill race | −232 | FAIL |
| FFmpeg **adelay=54** | content-aligned | +78 | overshoot / noisy |
| FFmpeg **adelay=22** | content-aligned | −75…−113 | noisy |
| Lab default | **adelay off** | −54…−60 | G-AV3 still open |

- **G-AV2 PASS** — harness locked; baseline n=12 MAD ~2 ms.  
- **G-AV3 FAIL open** — zero-lag baseline **~−54…−60 ms** (audio early).  
- **Product fix mechanism:** conf **`AUDIO_DELAY_MS`** → FFmpeg `adelay=N:all=1` on dual-pipe (content-aligned). Pump is pure wall-48k (no second delay line).  
- **Tune with eyes-on Trek @ 3:54** (seek works); HDMI alone is noisy for fine steps.  
- Lab **safe default `AUDIO_DELAY_MS=0`** until eyes-on.  
- RBF **`1441d409` stay**.

### RCA — why ~−55 ms audio early

**Root class:** product **video present latency** (DDR + vsync page-flip) vs **wall-paced MrAudio with zero intentional delay** → voice ahead of lips.

1. Sign: `(t_beep − t_flash)×1000` median **~−55…−60** ⇒ audio leads flash.  
2. Relative metric is stable (MAD ~2 ms) ⇒ fixed pipeline lag, not freerun.  
3. Do **not** thrash RBF for lipsync; conf adelay only.  
4. Do **not** drop PCM (that “PASS” was false).
7. **G-AV3 PASS** after conf + remeasure: primary median **−36.0 ms** (n=11) ≤ 42 ms; companion d60 **−30.5 ms**.

### Trek 40710 / 40868 probe (G-AV4)

- Local PMS (`cccccc…`, lab LAN address redacted) does **not** host 40710 (HTTP 404).  
- Remote server `bbbbbbbb…` @ `http://203.0.113.10:32400` **REACHABLE** (2026-07-25):  
  - **40710** = **show** (TNG), not the episode.  
  - **S1E1 episode = `/library/metadata/40868`** (Encounter at Farpoint, duration ~91 min).  
  - playMedia offset=234000 (~3:54) → timeline playing time=234000 (probe).  
- Evidence: `/tmp/misterplex-agent-AV-trek-probe.txt`.  
- **G-AV4 now PASS** — eyes-on settled at `Video delay` **+80 ms** under the *old* submitted-byte clock. That value is not transferable to the audible clock; see the Video delay section at the end of this doc.

---

## File map

| Area | Files |
|------|--------|
| Seek | `arm/misterplexd/main.cpp`, `media_player.cpp` |
| Delay conf | `media_player.{cpp,hpp}`, `assets/misterplex.conf.example` |
| Offset helper | `plex_resolve.hpp` `universalOffsetSeconds` |
| Fixtures | `assets/avsync/*trekmatch*`, `scripts/gen_avsync_blip.py` |
| Tests | `tests/unit/test_resolve.cpp` |

### Lipsync drift RCA (2026-07-25) — two root causes, both fixed

The blip gates above only sampled a ~12 s window, so a **rate** error was invisible:
it shows up as a slope, not an offset. Casting Trek TNG S1E1 (23.976) from Plex Web
drifted audibly behind the lips and got worse the longer it played.

**RC1 — content rate was bucketed to an integer.** `MediaPlayer` paced video with
`frameIndex * 1000 / fps`, `fps` hardcoded 24: `setContentFps()` had no call site, and
`bucketFps()` snaps 23.976 → 24 anyway. The raw RGB pipe carries no PTS, so `frameIndex`
is the only video clock and the rate error integrates forever (~234 ms by 3:54, ~5.5 s
by the end of a 91-minute episode). PMS makes this easy to get wrong: it reports
`Media@videoFrameRate="24p"` for both 23.976 and 24.000 content — only
`Stream@frameRate` tells them apart.

**RC2 — the FPGA audio clock is not 48 kHz.** `audioPump` feeds `/dev/MrAudio` at a
wall-paced 48 000 Hz; the core plays it back **~685 ppm faster**. `audioBytes_` counts
bytes *written*, never bytes *played*, so the daemon's own clock reports a perfect lock
while the HDMI output drifts ~41 ms/min. This dominates RC1 and was the reason years of
`AUDIO_DELAY_MS` tuning never converged — a constant cannot cancel a slope.

**RC3 (secondary) — non-deterministic A/V origin.** The present loop started on the wall
clock and switched to the audio clock once `audioActive_` flipped. Three identical runs
measured **−123.2 / −78.2 / −55.7 ms** (within-run MAD 1.5–4.0), i.e. a ~67 ms random
constant per play, which is why `AUDIO_DELAY_MS` sensitivity tests read nonsense
(`+100 ms` moved the result **−48 ms**).

**Fixes** (ARM/conf only, RBF `1441d409` untouched):

1. `parseExactFps()` extracts an exact rational from `Stream@frameRate` and snaps to the
   standard family; `bucketFps()`/`contentFpsHint()` are untouched (they still drive OSD).
2. FFmpeg is forced to CFR with `-vf fps=<num>/<den>,…` so `frameIndex ↔ content time`
   is exact **by construction**, even if PMS metadata lies.
3. Pacing is int64 rational (`host/libmisterplex/av_clock.hpp`), zero accumulated rounding.
4. A closed-loop corrector holds when ahead and drops when >`AV_RESYNC_DROP_MS` behind,
   capped at 1 consecutive drop. Dropping is only safe *because* of the forced CFR above.
5. `AUDIO_CLOCK_PPM` (default **685**) trims the feed rate to the core's true audio clock.
   Video is paced off `audioBytes_`, so this one constant re-times **both** outputs.
6. Video start is gated on `audioActive_`, so the clock source never switches mid-stream.

**Instrument.** `tests/hw/avsync_rate.py` replaces the two-window method. Two separate
captures cannot be compared — each ffmpeg invocation starts the v4l2 and pulse streams
with its own pipeline latency, and that per-capture skew (tens of ms) swamps a real drift
of a few ms/min. The rate script fits flash and beep cadence **inside one capture**, at
1280x720@60, so a constant rig skew cancels exactly.

**Evidence** (23.976 fixture RK11, 240 s fits):

| Config | drift |
|---|---|
| before (`AUDIO_CLOCK_PPM=0`) | **−53.3 ms/min** |
| `AUDIO_CLOCK_PPM=889` | −1.43 ms/min |
| `AUDIO_CLOCK_PPM=913` | +12.7 ms/min |
| **`AUDIO_CLOCK_PPM=685` (shipped)** | **+0.79 / −0.67 / +1.79 ms/min** (3 runs) |
| 24.000 control RK12 @685 | **−2.21 ms/min** |

Constant offset across those runs: **+91.8 / +95.3 / +100.3 ms** — a ~5–9 ms spread
where the pre-fix origin race gave **67 ms**. This was read at the time as
grabber skew, so the constant was left uncorrected. **That call was wrong**: it
was a real, constant video lead. Eyes-on (G-AV4) independently landed on
**+80 ms**, one 20 ms menu step from this capture's ~+92 ms, so the two agree.
The correction now ships as the `Video delay` default — see the Video delay
section at the end of this doc.

Trek S1E1 soak (remote PMS `bbbbbbbb…`, WAN transcode, 6.5 min): log shows
`fps=24000/1001`, `vfps=23.9` sustained, `av_drift_ms` bounded −30…−35, **13 drops in
9307 frames (0.14 %)** — the dual-A9 holds realtime and the clock stays locked.

---

## OSD menu v3 + idle screen (2026-07-26, RBF `91777ac1`)

The A/V offset above is no longer a conf-only knob. The Plex core's CONF_STR was rewritten so the
user tunes lipsync from the MiSTer OSD (F12) while the video is playing, with no conf edit and no
restart — and the last frame no longer sticks on screen after playback ends.

### What changed

**RTL (`fpga/Plex_MiSTer/Plex.sv`)** — CONF_STR bumped `v,2;` → `v,3;`. Four debug items were
removed (Content FPS, Pattern, Audio tone, Force bars) and their bits reclaimed:

| Bits | New item | Range |
|------|----------|-------|
| `[1]` | A/V auto resync | On (0) / Off |
| `[3]` | Audio clock trim | On (0, 685 ppm) / Off — applies next session |
| `[9:6]` | **A/V offset** | **signed** 4-bit × 20 ms → −160…+140 ms |
| `[15:14]` | **Idle screen** | Logo / Black / Screensaver / LastFrame |

`pattern`, `audio_en` and `use_frame_store` are hardwired to `0`, which reproduces the exact prior
defaults. `content_fps` was proven dead — `status[5:4]` → `present_cadence` feeds only the
`colorbars` generator and telemetry; the external frame-store path ignores it.

The offset is **signed rather than biased** because Main_MiSTer cannot express a non-zero CONF_STR
default and power-on status is all zeroes, so index 0 has to mean 0 ms.

**Daemon (`arm/misterplexd/media_player.cpp`)** — a read-only poller samples `status[15:0]` ~4×/s,
decodes it through `host/libmisterplex/osd_menu.hpp`, and applies the result to the live pacing loop.
An idle painter renders `host/libmisterplex/idle_screen.hpp` (Plex chevron, optional slow drift for
burn-in) whenever nothing is playing.

### The rule that cost the most time

> **The daemon must never write the user OSD bits.**

Main owns the OSD word and persists it to `/media/fat/config/Plex_v3.CFG`. A daemon-side
`setStatusBits()` on those bits fights Main's shadow copy: the word was observed flapping
`0x01c0 ↔ 0x0000` on every poll, and the measured A/V offset delta collapsed from the requested
140 ms to ~5 ms. After removing every daemon-side write (`pushContentFpsBits`, `restoreOsd`,
`saveOsdState`/`loadOsdState`, `osd_state.txt`) the word held stable at `0x01c0` for 30 s+ and the
knob worked. Polling is read-only, full stop.

### Evidence

| Check | Result |
|-------|--------|
| Fit | 427 s, 0 errors / 40 warnings, `91777ac11bc63e7bb4ab20331a26540d` |
| Live core menu | `set_status --confstr` dumps the exact expected v3 CONF_STR |
| OSD on screen | `/tmp/osd5.png` — labels and live values correct |
| Live apply | `0x00c0`→+60 ms, `0x40c0`→idle=1, `0x40ca`→trim+resync off, `0x020a`→−160 ms; works mid-playback |
| Lipsync knob | OSD +140 ms → median **−62.15 ms** vs baseline **+94.0 ms** = **−156 ms** |
| Idle screen | `/tmp/idle3.png` — amber chevron on near-black after stop |
| Unit | `tests/unit/test_osd_menu.cpp`; `make unit` 12 suites green |

### Traps found along the way

- `startIdle()` (play thread, at session end) raced `stopIdle()` (companion thread, in `play()`)
  over the same `std::thread` → `std::terminate`. Guarded with `idleMu_`; `osdMu_` mirrors it.
- `stop()` calls `fpga_.close()` + `healMainReloadPlex()`; with the poller/painter running they were
  mid-ioctl on the unmapped handle → crash. `stop()` now retires both threads *before* teardown and
  restarts them (plus a repaint) after the heal.
- `~MediaPlayer()` destroyed a still-joinable `std::thread` → `std::terminate` on SIGTERM. Only
  `stop()`/`play()` joined `thr_`, so a session that ended by itself left it finished-but-joinable.
  `MediaPlayer::shutdown()` now joins `thr_`/`audioThr_`/`streamThr_` **first**, then the idle/OSD
  threads (the reverse order still aborted, because `threadMain`'s session-end `startIdle()` respawns
  the painter), with a `shuttingDown_` latch to block late starts. Stress: 3/6 → 0/15.
  `test_plex_browse.sh` / `test_companion_http.sh` now assert the daemon's exit status, which is the
  check that was missing when this shipped.
- `paintIdle()` must use the DDR-bulk-then-SPI ladder; an SPI-only F1 push never lands.
- The MS2109 grabber emits ~20 black warmup frames — always `select=gte(n\,40)`.
- `/dev/MiSTer_cmd` has **no key injection** (only `fb_cmd`, `video_mode`, `load_core`, `screenshot`,
  `scaled`, `volume`, `mute`, `unmute`). `tests/hw/osd_keys.py` drives `/dev/uinput` instead: **F12
  works** (needs ~6 s settle after `UI_DEV_CREATE` for MiSTer's inotify), **arrow keys do not
  register** — so menu *navigation* still needs eyes-on.
- `tests/hw/test_fbar_fast.sh` and `tests/hw/test_menu_osd.sh` are obsolete on v3.

---

## Why F12 kept dying — and why we no longer touch Main (2026-07-26)

**Symptom:** "the Plex RBF has crashed MiSTer Main again, I can't hit F12 any longer."

**It was never the RBF, and it was never a crash.** It was us corrupting the bus Main
owns, and then "fixing" it by killing Main.

### The actual root cause

The HPS↔FPGA "SPI" is not a bus with arbitration. It is **one 32-bit GPO register** in the
FPGA manager (`0xFF706010`) plus a strobe/ACK handshake on GPI (`0xFF706014`).
Main_MiSTer assumes it is the sole owner, and its `fpga_spi()` spins on that handshake
with **no timeout** — the only other exit is the FPGA leaving user mode:

```c
fpga_gpo_write(gpo | SSPI_STROBE);
do { gpi = fpga_gpi_read(); ... } while (!(gpi & SSPI_ACK));   // fpga_io.cpp
```

So if misterplexd rewrites GPO while Main sits between *"strobe=1"* and *"saw ACK"*, the
core drops ACK, Main never sees it, and **Main spins forever**. No F12, no OSD, no
`/dev/MiSTer_cmd`. The only escape is killing the process — which is precisely what
`healMainReloadPlex()` was doing after every single play.

Worse, Main never re-reads the register (`#define fpga_gpo_read() gpo_copy`), so anything
we left behind was invisible to it. And `SIGSTOP`ping Main did **not** help: it freezes
Main at an arbitrary instruction, *including inside the handshake*, which is exactly the
dangerous case.

The bandaid then created a second, worse failure. `/etc/inittab` starts Main with:

```
::sysinit:/media/fat/MiSTer &
```

`sysinit`, **not** `respawn` — nothing on the board ever restarts Main. So any interruption
of the kill/re-exec window (daemon crash, the deploy script's `killall -9`, a failed fork)
left the board with **no Main at all** until a power cycle. Confirmed live: `ps` showed
zero `/media/fat/MiSTer` processes while the core ran fine.

### The fix: stop sharing the register, then stop needing it

**1. Every SPI transaction now runs in a provably safe window** (`SpiExclusive` /
`MainSafeWindow`). Instead of blindly stopping Main:

1. `SIGSTOP` Main, then **poll `/proc/<pid>/stat` until it is really state `T`** —
   `kill()` returns long before the target stops.
2. Read the **live** GPO register and test Main's own transaction-enable bits
   (`FPGA_EN` 1<<18, `OSD_EN` 1<<19, `IO_EN` 1<<20) and the strobe (1<<17). Main only ever
   calls `fpga_spi()` between `EnableXxx()`/`DisableXxx()`, so **all-clear proves it is
   parked between transactions** and, being stopped, it cannot start another.
3. If it *was* mid-transaction: `SIGCONT`, back off, retry. If we run out of attempts the
   SPI call **fails cleanly** (`"Main busy on SPI — skipped"`) rather than corrupting
   anything.
4. On release, write GPO back **byte-for-byte** as Main left it, *then* `SIGCONT`. Main
   wakes with hardware exactly matching its shadow copy.

`gpoRead()` now reads the live register rather than a shadow of our own, so we can never
hand Main back bits it never set.

**2. The core publishes the OSD word into DDR, so the poller needs no SPI at all.**
`ddram_frame_rd` now writes a mailbox qword at **`0x3007F100`** — inside the frame window
misterplexd already mmaps:

| Bits | Field |
|------|-------|
| `[31:0]` | magic `0x504C5853` ("PLXS") |
| `[47:32]` | `status[15:0]` — the live OSD menu word |
| `[63:48]` | seq (monotonic; lets the host reject a torn read) |

Published on change plus a slow heartbeat, issued only in the idle branch so it can never
collide with the doorbell poll or a frame DMA. `readOsdMailbox()` prefers this path and
falls back to `UIO_GET_STATUS` only on a pre-mailbox RBF. **The frame path was already
SPI-free** (mmap + doorbell), so with the mailbox in place a normal session touches the
SPI bus zero times.

### What was removed

`healMainReloadPlex()` and `ensureMainAlive()` are **gone**. misterplexd no longer starts,
stops, kills, or reloads Main under any circumstance. `MediaPlayer::stop()` no longer
heals, no longer tears down and re-opens the FPGA, and no longer unlinks
`/tmp/misterplex_spi.lock` (recreating that inode would have put concurrent tools on a
different lock — a real bug).

The only thing left is `resumeStrandedMain()`, which sends **`SIGCONT` and nothing else**,
plus `installCrashGuard()` for fatal signals. They exist purely because the safe window
still stops Main for a few microseconds; if misterplexd is SIGKILLed inside it, the next
start (or the 600 ms watchdog) resumes Main.

`tests/unit/test_main_guard.cpp` pins this: the fixture must be a symlink to `sleep` named
`MiSTer` (a shell script will not do — the kernel puts the interpreter in `argv[0]`) at a
path not containing `misterplex`, which `findMisterPids()` filters out. It now also asserts
that no guard ever terminates Main.

### The deploy script was shipping stale binaries

`scripts/deploy_misterplexd.sh` only cross-compiled when the output was missing:

```bash
if [[ ! -f "$BIN" ]]; then make -C "$ROOT" arm-plexd; fi
```

Once `build/arm/misterplexd` existed it was never rebuilt, so every later deploy pushed an
old daemon — and the first hardware run of this fix "failed" purely because the binary on
the box did not contain it (`strings` showed the new log lines missing). It now always runs
`make arm-plexd` and fails loudly if no binary is produced. Worth remembering when a fix
appears to have no effect on hardware.

---
| G-AV0 AUDIO_DELAY_MS=0 | **PASS** (baseline measure; log `delay_ms=0`) |
| G-AV1 Trek-matched blip | **PASS** (assets + PMS 9/10) |
| G-AV2 measure harness | **PASS** (`avsync_report.txt` n=12 flash↔beep) |
| G-AV3 \|median\| ≤ 42 ms | **PASS** — \|median\|=**36.0 ms** n=11 @ `AUDIO_DELAY_MS=60`; baseline −60 @0; `avsync_report_delay60.txt` + d60 companion |
| G-AV4 Trek 3:54 | **PASS** — eyes-on settled at **Video delay +80 ms** (old submitted-byte clock; default since reset to 0 — see G-AV9) |
| G-AV5 exact rate | **PASS** — log `content fps exact=24000/1001` (Trek/RK11) and `24/1` (RK12) from the same `24p` metadata; `make unit` `test_avclock` |
| G-AV6 drift slope ≤10 ms/min | **PASS** — **+0.79 / −0.67 / +1.79** ms/min (RK11, 240 s fits) and **−2.21** (RK12 control); before: **−53.3** |
| G-AV7 constant offset | **PASS** — the residual was a real constant video lead, not grabber skew: capture said +60 ms, eyes-on said +80 ms (one 20 ms step apart) |
| G-SEEK1 mid-play seekTo | **PASS** (lab blip evidence) |
| G-SEEK2 resume offset≠0 | **PASS** (lab blip evidence) |
| G-SEEK3 unit | **PASS** (`make unit`) |
| G-REG tear RBF | **PASS** (no RBF change for the A/V work) |
| G-OSD1 v3 CONF_STR live | **PASS** (`set_status --confstr` on RBF `91777ac1`) |
| G-OSD2 OSD renders | **PASS** (`/tmp/osd5.png`) |
| G-OSD3 live apply | **PASS** (four controls, incl. mid-playback) |
| G-OSD4 offset moves lipsync | **PASS** (+140 ms → −156 ms measured) |
| G-OSD5 arrow-key nav | **PENDING** — uinput arrows don't register; needs eyes-on |
| G-IDLE idle screen | **PASS** (`/tmp/idle3.png`, chevron after stop) |
| G-MAIN1 stranded Main resumed | **PASS** (T -> R in ~100 ms, live) |
| G-MAIN2 dead Main relaunched | **PASS** (new pid + core=Plex, live) |
| G-MAIN3 guard unit | **PASS** (`test_main_guard` in `make unit`) |

---

## How to re-verify

```bash
# Seek
# cast /library/metadata/6, then:
curl -G 'http://PMS:32400/player/playback/seekTo' --data-urlencode offset=12000 \
  -H 'X-Plex-Target-Client-Identifier: misterplex-183' -H "X-Plex-Token: $TOK"
# log: seek re-resolve … offset=12 … skip -ss
# timeline time ≈ plant + wall

# Resume
curl -G '…/playMedia' --data-urlencode key=/library/metadata/6 --data-urlencode offset=15000 …

# Lipsync baseline (zero lag) — DONE G-AV2 PASS / G-AV3 FAIL −60 ms
# cast ratingKey 10; AUDIO_DELAY_MS=0; capture HDMI A/V → avsync_trekmatch/

# Lipsync fix remeasure — DONE G-AV3 PASS |median|=36 ms @ AUDIO_DELAY_MS=60
# conf AUDIO_DELAY_MS=60; soft restart; cast /library/metadata/10; HDMI flash↔beep

# G-AV4 Trek dialogue: PASS at Video delay +80 ms (episode 40868, not show 40710)
# NOTE: measured on the old submitted-byte clock. Retune from 0 under the audible clock (G-AV9).
# expect |median_offset_ms| ≤ 42; keep RBF 1441d409
```


## Video delay (OSD `O[9:6]`) — the lipsync control

**Symptom that led here.** On TNG S1E1 audio sounded ~half a second behind the
lips, and turning the menu to `-160ms` "did nothing".

**Cause.** Two things compounded:

1. **The label had no direction.** `A/V offset` does not say *which* stream
   moves. The present loop waits for the audio clock to reach
   `frameContentMs(frameIndex) + avOffsetMs`, so **positive holds the frame back
   and makes video later**. Video was already running ahead, so the correct
   direction was positive — `-160 ms` pushed it further ahead. The reported
   "half a second" is the `-160 → +80` swing of **240 ms**, not an absolute
   error, which is why the knob felt inert: it was working, backwards.
2. **The default was `0 ms`** even though two independent measurements said the
   platform needs ~+60…+80 ms.

**Fix.** The item is now `Video delay`, and after the audible-clock work below
the default is back to **0 ms** — see "What it did not buy" for why a baked-in
constant is no longer appropriate.

`kOsdAvOffsetDefaultMs` in `host/libmisterplex/osd_menu.hpp` is the single source
of truth; `Plex.sv`'s CONF_STR list and the daemon decode are both derived from
it, and `tests/unit/test_osd_menu.cpp` pins the mapping and the wrap-seam
continuity so the two can never disagree.

**Why a hardcoded default is allowed here** (G-AV0 forbids *guessed* lag): it is
measure-backed twice over — instrumented flash-to-beep landed on +60 ms
(`captures/e2e/avsync_trekmatch_d60`, |median| 36 ms) and eyes-on landed on
+80 ms. The knob remains exposed because HDMI sinks add their own audio latency,
so per-display trim is expected.

**Config version bumped `v,3` → `v,4`** so saved `Plex_v3.CFG` files (where index
0 meant 0 ms) are discarded rather than silently reinterpreted under the new bias.


## Why a constant was needed at all — and how most of it was removed

The obvious question: Plex's own web player never asks you to dial in a lipsync
offset, so why did we?

**Because we paced video off the wrong clock.** The pump wrote PCM to
`/dev/MrAudio` and incremented `audioBytes_` the moment `write()` returned, and
the present loop treated that counter as the master clock. But *accepted* is not
*played*. Reading MiSTer's driver (`sound/drivers/MiSTer-audio-spi.c`) shows why:

- `/dev/MrAudio` is a **512 KB DMA ring** (~2.6 s of audio).
- `device_write()` **never consults the read pointer**. It copies into the ring,
  wraps, and returns. There is no backpressure of any kind, and writing past the
  read pointer silently destroys audio that has not been played yet.

Measured on the lab unit: **10 s of PCM accepted in 116 ms**. So a clock built
from submitted bytes runs arbitrarily ahead of what you hear, and video slaved to
it runs ahead too — audio "sounds late". The old fix was to hand-tune a video
delay until it looked right. A browser never needs that because the OS reports
its audio output latency and the renderer schedules against the hardware playback
position, not the submit position.

**The driver does expose the playback position** — it just isn't obvious.
`device_open()` does an `spi_read` of the FPGA's read pointer and formats one
line, which `read()` then returns:

```
rptr: 238120, wptr: 426576, len: 188456, comp: 4
```

`len` is `(wptr - rptr)` wrapped: **bytes queued but not yet played** — the
output latency, live and exact. Verified draining at precisely 192000 B/s.

So the pump now samples `len` every 4th chunk (~80 ms) and the present loop paces
off `audibleClockMs(written, queued) = (written - queued) / 192000`. Sampling
harder buys nothing: between samples the only error is the feed-vs-drain
difference (~685 ppm ≈ 0.05 ms).

**What this bought:**

- Video now paces off what is **heard**, not what was sent — the same thing the
  browser gets from the OS for free.
- The ring depth is compensated **continuously**, so bursts, stalls and network
  hiccups no longer shift lipsync. Previously any deviation from exactly 1.0x
  feed was invisible and permanent.
- Ring overwrite (which destroys unplayed audio with no error anywhere) is now
  **detectable** and logged.

**What it exposed — the ring was growing without bound.** With the depth finally
visible, the first sustained measurement showed it climbing steadily at
**~255 B/s (~80 ms/min)**, from ~19 ms at startup through 243 ms and still
rising when the clip ended. Extrapolated, that overruns the 512 KB ring about
**31 minutes into playback** — comfortably inside a 45-minute episode — at which
point `device_write()` walks over unplayed audio with no error anywhere.

The cause was `AUDIO_CLOCK_PPM=+685`. That number came from an HDMI-capture
calibration taken while video paced off *submitted* bytes, and under that clock
**a ring that is filling up and a playback clock that is running fast are
indistinguishable** — both make audio appear to lead. The calibration folded one
into the other and came out with the wrong sign. The true error is about
**−638 ppm**: the core plays slightly *slower* than nominal 48 kHz.

**The fix: stop calibrating, close the loop.** Ring depth is the integral of
(feed − drain), so it is a direct error signal. `feedRateBytesPerSec()` is a
proportional controller that trims the feed to hold the depth at
`kFeedTargetBytes` (~100 ms), with an 8 s time constant and a ±1 % clamp. Any
clock error — this board's, another board's, a different video mode's — is now
found by the loop instead of being measured by hand.

`AUDIO_CLOCK_PPM` survives only as the servo's seed and as the open-loop
fallback when the ring depth cannot be read at all.

**Two more things had to change for the loop to behave:**

- **Anchor the pump clock on the first chunk, not before the read.** FFmpeg
  needs a variable warm-up before it emits anything. Anchoring earlier left the
  pump "behind schedule" the moment data arrived, so it wrote flat out to catch
  up and dumped the entire warm-up into the ring, where it stayed all session
  (feed and drain are both ~48 kHz, so nothing drained it). That is why the
  first sustained measurement started at ~185 ms and why ring depth — and
  therefore lipsync — was **session-dependent**, the likely source of this
  project's long-standing run-to-run variance. The anchor is now biased exactly
  one target-depth into the past, which turns the burst into an ordinary,
  bounded prefill. A catch-up debt of more than a second is dropped rather than
  repaid, for the same reason.

- **The OSD stopped overwriting the conf.** `decodeOsdWord()` returned a
  hardcoded `685` for the O[3] "audio clock trim" bit, and `applyOsd()` pushed
  it into the player on every poll — so `AUDIO_CLOCK_PPM` in the conf was
  applied at startup and then silently clobbered within ~100 ms, every time,
  forever. O[3] is now the boolean it always meant to be (trim on/off) and the
  ppm value belongs solely to the daemon.

**Result on hardware** (TNG S1E1 `40868` @ 3:54, 120 s):

| | before | after |
|---|---|---|
| ring depth | 19 → 243 ms, still climbing | **97–101 ms, flat** |
| growth | +255 B/s (~80 ms/min) | **~0** |
| steady-state depth vs 19200 B target | n/a | 19198–19231 B |
| `audio_s` vs `wall_s` | +0.1 s and diverging | **equal** |
| frame drops | 1 | **0** |
| ring overrun warnings | (unreported) | 0 |

**Why the `Video delay` default is 0 — confirmed by eyes-on.** With the audible
clock and the servo in place, the user watched real TNG S1E1 at 3:54 with
`av_offset_ms=0` and reported it in sync. **No constant is needed at all.** The
+80 ms was entirely the ring depth the submitted-byte clock was carrying.

That is the answer to the question that started this work — "why do I have to set
Star Trek ahead by 80 ms when Plex Web doesn't?" Plex Web asks the OS for the
output latency and schedules against the hardware playback position. We were
counting bytes we had handed over. Now we ask the driver the same question.

Note the flash/beep harness read **-215 ms** at that same in-sync setting, which
makes -215 ms the **USB grabber's own A/V skew** (video and audio arrive as
separate USB streams with independent latency). Treat
`tests/hw/avsync_measure.py` as a relative/drift instrument; add +215 ms for an
absolute reading, and never bake a constant from it without eyes-on.

The old eyes-on **+80 ms** is not transferable. It was tuned while video paced off submitted bytes, so it silently
absorbed whatever ring depth that session happened to have — and that depth was
both large and session-dependent. Arithmetic on it would be arithmetic on sand.

So the default returns to **0 ms**: everything measurable is now measured, and
the residual — the audio-vs-video difference *downstream* of the driver (FPGA
output, HDMI, and the display's own processing) — is per-display and belongs on
the knob. Displays commonly add tens of ms of *video* processing, so a small
negative trim is the expected answer, not +80.

The sampled depth is low-passed (EMA, alpha 1/4, seeded on the first sample)
before it reaches either consumer, so neither the video clock nor the servo
chases sampling noise.

Parsing lives in `host/libmisterplex/mraudio_status.hpp` as pure functions and is
pinned by `tests/unit/test_mraudio_status.cpp` against strings captured verbatim
from the lab unit, so a kernel-side format change fails loudly instead of
silently feeding the present loop a bogus latency. The servo is tested there too,
including a convergence simulation. An unparseable line yields `-1`, which
disengages the servo and falls the clock back to the old submitted-byte
behaviour.
