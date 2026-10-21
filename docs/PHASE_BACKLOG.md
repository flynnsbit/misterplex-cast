# MiSTerPlex phase backlog (living)

Update this file when work finishes. Loop agents claim items and mark `DONE` / `IN_PROGRESS` / `BLOCKED`.

## PRODUCT MILESTONE — VSync present / product A/V cast (**DONE** 2026-07-25)

| | |
|--|--|
| **Status** | **DONE** — user eyes-on: *looks good on video and vsync* (2026-07-25) |
| **Git** | **`588e528`** `milestone(vsync-present): tear-free DDR present + product A/V cast` |
| **RBF** | **`1441d409ad3f8ccc5dcb0033c32ff7c8`** — `releases/Plex_vsync_tear_1441d409.rbf` + lab `/media/fat/_Utility/Plex.rbf` |
| **Conf** | `PRESENT=fpga` `STREAM=0` `DECODE=320x240` |
| **Notes** | Full write-up: [`docs/MILESTONE_VSYNC_PRESENT.md`](MILESTONE_VSYNC_PRESENT.md) · resume: [`docs/SESSION_RESUME.md`](SESSION_RESUME.md) |

**What fixed half-frame / multi-panel tears**

1. **`frame_store`**: dual-bank writes to non-display bank; `swap_pending`; **flip display bank only on `vsync_pulse`**; **hold writes while pending** (no mid-scan flip, no multi-panel overwrite).
2. **`ddram_frame_rd`**: **hold DDR→BRAM DMA while `swap_pending`**; queue one latest doorbell/SPI kick; mmap doorbell `0x3007F000` (`PLXK`).
3. **Host (`misterplexd`)**: product cast = **STREAM=0 every-frame DDR F1** (not STREAM recon); **wall-48 kHz MrAudio**; doorbell-preferred present; **heal Main on stop**.

**Evidence**

- holdoff2: half/mid/multi tear rates **0.00/s** — `captures/e2e/tc_glitch/holdoff2/REPORT.txt`
- Blip 24 fps Web cast: median flash↔beep **~−13 ms** — `captures/e2e/blip24/avsync_report.txt` (+ suite notes in `captures/e2e/REPORT*.md`)
- Do **not** thrash this RBF while product path is green unless a new present gate fails.
- Residual csum / WIDE / 3l2 below are **separate** serial tracks (historical RBF ids do not supersede product **`1441d409`** present path).

---

## ACTIVE — Hardware visual decode regression harness (**HW RED/GREEN PROVEN** 2026-07-27)

User directive: after simulation, build realtime, push to MiSTer, and validate on real DE10-Nano hardware with
visual unit tests/debug output instead of stopping at Verilator.

| | |
|--|--|
| **Branch** | `feat/c1-hw-visual` |
| **Harness** | `tests/hw/test_f3_visual_golden.sh` |
| **Comparator** | `scripts/hw_visual_compare.py` |
| **Goldens** | no silent default; `VISUAL_GOLDEN` must name a hardware-captured PNG with `.provenance.json` |
| **Docs** | [`hw-visual-regression.md`](hw-visual-regression.md) |
| **Status** | **EVIDENCE-BACKED for the legacy rollback instrument only:** real HDMI capture achieved; `57674f2e` grades GREEN and characterized bad `fe7673bc` grades RED. **UNSUBSTANTIATED for current product decode:** the `57674f2e` golden is RGB565/320×240 rollback evidence and must not grade the current YUV420/624×480 path. |

**Design:** push a known Baseline/CAVLC F3 vector, capture HDMI, derive thresholds from five repeated static
captures, compare the stable decoded ROI (default `VISUAL_COMPARE_BOX=11,0,160,120`, containing MB0), and emit
`golden | captured | amplified-diff` PNG plus JSON metrics. The comparator can still use the shared host/RTL DDR
layout active region for future 624×480 gates.

**Dry-run evidence:** `make unit` exit 0. Synthetic/checked-in noise floor is zero
(`max_pair_mae_rgb=[0,0,0]`, `max_abs_noise=0`, thresholds `[1,1,1]` / `2`), and the comparator's red-path unit
corrupts one active pixel and fails with exact worst coordinate + diff artifact.

**Capture/freshness/artifact integrity:** compare exit codes are distinct: `1` = real visual mismatch, `3` =
stale frame, `4` = V4L2/FFmpeg corrupted buffer/data, `5` = absent device, `6` = busy device, `7` = no fresh
frame delivery proven from status counters/token, `8` = loaded/golden RBF md5 does not match the declared
artifact, `9` = missing/mismatched golden provenance. Capture-rig, delivery, wrong-core, or unscoped-golden
failure is never accepted as either green or known-red core evidence.

**Hardware evidence (W-C1 token window):**

- Differential test: the exact W-CAP YUYV pipelines also report full-frame corrupt buffers on the MiSTer menu
  (`1843200` bytes at 1280×720@10 and `614400` bytes at 640×480@10), so this is a raw-YUYV capture-pipeline
  problem rather than evidence that Plex alone emits bad HDMI timing.
- Repaired capture path: `/dev/video4` MJPEG `1280x720@60`; menu capture was clean, YUYV 640×480@5 with
  120-frame warm-up still reported full-frame corruption. `v4l2-ctl` advertises MJPEG 720p60/1080p30 and YUYV
  720p only at 10 fps; dongle is on a shared 480M USB bus.
- W-CAP's real `fe7673bc959f37fd7da44e8a865f7db3` YUYV failure logs (`Dequeued v4l2 buffer contains corrupted
  data`, `/dev/video4`, 10 fps, 1280×720 corrupt bytes `1843200`, 640×480 corrupt bytes `614400`) are now checked
  in as capture-log fixtures and force `compare --capture-log ...` to return `rc=4` before pixel grading.
- Measured static capture noise floor on `57674f2e` default ROI: `max_pair_mae_rgb=[0,0,0]`, `max_abs_noise=0`;
  thresholds `[1,1,1]` / `2`.
- Green rollback `57674f2e`: exact ROI pixels `19200/19200`, MAE `[0,0,0]`, max_abs `0`, `rc=0`.
- Red specimen `fe7673bc`: telemetry has `res_csum=20`/`raw[13]=0x14` class signal while reconstruction is known
  bad; visual compare exact ROI pixels `0/19200`, MAE RGB `[112.7730,59.8615,61.0523]`, max_abs `242`,
  `rc=1`, diff artifact emitted.
- Tooling no longer has a proven default pair. The former
  `plex_real_baseline_320x240_57674f2e_mjpeg720_golden.png` is quarantined as
  legacy rollback evidence only; its sidecar declares `57674f2e`, 320×240,
  RGB565, BT.601/full, and ROI `11,0,160,120`. The 624×480 reference is
  retained only as a future target and is marked `generated_reference`, so the
  hardware harness refuses to grade it as a captured golden.
- Stale-screen follow-up: Plex reload+push captures can exact-match a golden while status reports `bytes_in=4`.
  Source audit proved this is a status-telemetry alias (`bytes_in == nalu == 4`, four NALs, not four bytes)
  after stream byte telemetry was reclaimed for residual/recon RCA. The comparator now names the
  `STATUS_TELEMETRY_LAYER` and refuses that phantom-green path before pixel grading (`rc=7`) using the natural
  captured fixture; unit coverage also proves a fresh legacy-style `bytes_in=6227` plus changed
  `{bank,format,seq}` token green path.
- Wrong-core follow-up: rollback `57674f2e` predates the current YUV420/624×480 frame-store contract, so it cannot
  validate the current ARM I420 delivery path. The hardware script now requires `VISUAL_RBF` or
  `VISUAL_EXPECTED_RBF_MD5`, verifies device `/media/fat/_Utility/Plex.rbf`, requires `VISUAL_GOLDEN`, and the
  comparator refuses loaded-vs-golden source mismatches before pixel grading (`rc=8`) with both core md5s in the
  error. Unit coverage proves wrong/undeclared RBF red, golden-source mismatch red, and matching md5 green.

**Hardware TODO:**

- [x] Get W-CAP's actual corrupted V4L2 command/log/device/artifacts and add the logs as rc=4 harness specimens.
- [x] Record real HDMI capture noise floor from `build/hw_visual_accept/noise_good_57674f2e.json`.
- [x] Prove green visual compare against rollback `57674f2e`.
- [x] Prove red path on hardware with known-bad `fe7673bc`.
- [x] Quarantine the former 320×240 `57674f2e` golden; no silent default golden remains.
- [x] Refuse full-frame `VISUAL_FULL_FRAME=1` coverage by default. Hardware investigation of the 618×480 active
  region false-reded on known-good `57674f2e` despite zero within-run noise: exact `281458/296640`,
  Y/U/V MAE `[5.1175,3.3041,1.9793]` vs checked golden, and `281249/296640`, Y/U/V MAE
  `[5.1944,3.3532,2.0109]` after rerun against a same-window full golden. Bad `fe7673bc` was red
  (`0/296640`, Y/U/V MAE `[63.9724,24.5320,26.1278]`), but a full-frame gate cannot ship until good is green.
- [x] Add a freshness/delivery guard so a stale frozen screen cannot grade as PASS. `compare --status-log`
  refuses the `bytes_in=4`/`nalu=4` status-telemetry alias before pixel grading (`rc=7`) and supports the shared DDR frame token
  `{bank,format,seq}` for w-a3/w-cap alignment.
- [x] Add loaded-artifact identity gating. The hardware script records and verifies `md5sum
  /media/fat/_Utility/Plex.rbf`; compare refuses wrong or undeclared RBF identity (`rc=8`) before grading pixels.
  Status `frame_debug=0xe1` is surfaced as a named non-YUV doorbell/debug refusal.
- [ ] Promote a checked-in host/good 624×480 golden only after that vector is live and green on rollback hardware.

---

## Decode evidence reconciliation (2026-07-27)

Status labels below distinguish measured evidence from retired instrumentation:

| Claim area | Evidence status | Measurement / instrument |
|------------|-----------------|--------------------------|
| MB0 first-pixel/root-cause localization | **EVIDENCE-BACKED** | Native-I420 full-frame scoreboard (`tools/score_h264_native_frames.cpp`, `tests/unit/test_stream_path_full_frame_compare.sh`) reports the old phantom first-pixel divergence as clean: `got=73 ref=73 abs=0`. The retired RGB565/presentation scoreboard had reported `got=142 ref=65`, so that older failure was an instrument artifact, not decoder evidence. |
| Full-frame intra prediction exactness | **EVIDENCE-BACKED NATIVE-I420 GREEN** | The former "300/300 exact"/`maeY=0` frame-wide green was measured through the RGB565 diagnostic/presentation path with border masking, then the first native-I420 ratchet still mismatched loop-filter state. Corrected no-deblock native-I420 ratchets now declare/refuse H.264 loop-filter state and compare against FFmpeg `-skip_loop_filter all`: 624×480 intra `1170/1170`, 320×240 intra `300/300`, and `wcap_residual14` intra `300/300`, all Y/U/V MAE 0. |
| P-slice / motion compensation full-frame output | **MEASURED RED / OWNER W-REL** | Product `stream_path` parser now drives DPB/MC liveness through the product deblock writeback commit barrier (`recon_sig_3b_cycles=39780`, forced-`recon_sig=0` red). Full-frame RGB565 diagnostic output remains a non-native expected-red (`0/3300` P MB exact, Y MAE `76.468417`). A new native-I420 inter candidate exported from the RTL DPB/MC predictions is scored by `score_i420_candidate.py` with colorspace/loop-filter refusal: 320×240 reports `intra=0/300`, `inter=0/3300`, first_bad_inter frame 1 MB0 Y `got=32 ref=80 abs=48`, `mb_type=P_L0_16x16`, `mv_l0=(0,0)`; 624×480 scorer self-check reports `intra=0/1170`, `inter=0/12870`, first_bad_inter `got=32 ref=77 abs=45`. Metadata declares `candidate.h264_loop_filter=disabled`, `reconstruction_stage=mc_prediction_only_pre_deblock_no_residual_add`, and `reference_picture_state=diagnostic_filtered_reference_via_deblock_writeback_ctrl`; this measures MC arithmetic/plumbing only, not end-to-end P conformance. |
| Visual hardware golden `plex_real_baseline_320x240_57674f2e_mjpeg720_golden.png` | **EVIDENCE-BACKED only for rollback `57674f2e`; contaminated for current-product claims** | Sidecar declares rollback RBF `57674f2e`, 320×240 RGB565, BT.601/full, ROI `11,0,160,120`. It is quarantined as legacy evidence and must not grade current YUV420/624×480 delivery or decode status. |
| `bytes_in` status freshness | **EVIDENCE-BACKED alias, not byte count** | ARM/status parser currently maps `bytes_in` to `nalu_count`; `bytes_in=4` means four NALs (SPS/PPS/SEI/IDR), not four bytes. Visual gates must use status/token freshness and reject the stale-screen phantom (`rc=7`) before pixel grading. |
| DDR present / frame-store mailbox on current silicon | **UNSUBSTANTIATED / HARDWARE BLOCKED** | W-CAP silicon evidence on loaded `eeff4eee` (proven source `b5c50c6`): valid `PLXK` doorbell (`lo=0x504c584b`, `hi=0xa0000068`, bank=1, format=YUV420P, seq=0x68) but `PLXF` frame mailbox stayed all-zero (`lo=0x00000000 hi=0x00000000`) for 40 samples over 12.3 s and `has_frame=0`. Simulation of the same mailbox path reads `MAGIC_F=0x504c5846`; a later line-read hang simulation is a distinct **stale** fault (`PLXF` magic remains `0x504c5846`, e.g. `plxf=0x8001504c5846`, while `has_frame=0`). Pre-deploy discriminator: `PLXF lo=0x00000000` means the first frame-mailbox write never reached/read back at `0x3007F118` (reset/clock/DDR grant/write-address/netlist class); `PLXF lo=0x504c5846` with static high word means the mailbox published and a later frame-fill/present path stalled. Until a provenance-correct post-`d803e4c`/`86558c4`/`97beb1d` RBF is fitted and observed, claims that the DDR frame-store/present path works on device are not current evidence. |
| Current localized intra defect | **RESOLVED / HOST REFERENCE FIXED** | The apparent MB 182 DC-collapse was a host-side QP_Y clamp where H.264 requires modulo-52 wrap. After the reference fix, no-deblock native-I420 intra is bit-exact on all checked fixtures; localize future decode reds to the first divergence in raster order before naming a mechanism. |

Any future green decode claim must name the exact evidence file, fixture, and instrument. A claim that cannot state whether it came from native I420 planes, RGB565 presentation diagnostics, hardware HDMI capture, or host-only reference code stays **UNSUBSTANTIATED**.

---

## ACTIVE — A/V lipsync + Plex Web seek/resume (**IN_PROGRESS** 2026-07-25)

| | |
|--|--|
| **Title SoT** | A remote TV episode (S1E1, ~3:54 = seek 234000) on the remote PMS; probe **REACHABLE** 2026-07-25 — **≠ G-AV4 PASS** |
| **RBF** | Keep **`1441d409`** (tear-free); ARM/conf only unless present regresses |
| **Conf** | `PRESENT=fpga` `STREAM=0` `DECODE=320x240` · **`AUDIO_DELAY_MS=0`** (safe default; adelay via conf when eyes-on tunes) |
| **Plan** | session plan A/V + seek · multi-agent fill · no Quartus sole |

| ID | Item | Status | Notes |
|----|------|--------|-------|
| P3-AVSYNC | Fresh lipsync + Trek-matched blip + measure | **PARTIAL** | G-AV0/1/2 PASS; G-AV3 **FAIL open** baseline HDMI **−54…−60 ms**; PCM-drop “−36 PASS” **revoked** (wrong polarity); FFmpeg **adelay** conf ready |
| P4-SEEK | Plex Web seek/resume: re-resolve universal on seek | **DONE** (lab blip) | G-SEEK1/2/3 **PASS**; see `MILESTONE_AVSYNC_SEEK.md` |
| P3-PRESENT | VSync tear-free present | **DONE** | do not thrash RBF **`1441d409`** |

**Gates**

- [x] G-AV0 zero intentional lag (`AUDIO_DELAY_MS=0`) — lab conf default; no hardcoded lag
- [x] G-AV1 Trek-matched blip fixture — `assets/avsync/sync_trekmatch_*` PMS keys **9/10**
- [x] G-AV2 measure harness flash↔beep — **PASS** HDMI trekmatch RK10 n=12; `captures/e2e/avsync_trekmatch/`
- [ ] G-AV3 blip \|median\| ≤ 1 frame @24p (≤42 ms) — **superseded by G-AV7**; the old absolute number was unmeasurable while the A/V origin race added ±35 ms per play
- [x] G-AV8 video paces off audible position — **PASS**: `/dev/MrAudio` has a 512 KB DMA ring with **no backpressure** (10 s of PCM accepted in 116 ms), so the old submitted-byte clock ran ahead of what was heard. The driver's `open()`+`read()` exposes `len:` = bytes queued-but-unplayed; the pump now samples it (EMA-smoothed) and the present loop paces off `(written - queued)`. Measured steady-state depth **~35,000 B ≈ 185 ms** — sampled *mid-playback*; the 19 ms/4 B first readings were startup and end-of-clip artifacts. The depth is **session-dependent** (it is FFmpeg's startup latency, dumped into the ring by the pump's catch-up burst and then held there because feed and drain are both 48 kHz), which is the likely source of this project's run-to-run lipsync variance. Ring overwrite is now logged instead of silently corrupting lipsync.
- [x] G-AV10 ring depth bounded and self-correcting — **PASS**: the first sustained measurement showed the ring growing **+255 B/s (~80 ms/min)**, which overruns the 512 KB ring ~31 min in and silently shreds unplayed audio. Cause: `AUDIO_CLOCK_PPM=+685` had the **wrong sign** (true error ~**-638 ppm**) because it was calibrated under the submitted-byte clock, where a filling ring and a fast playback clock look identical. Replaced the open-loop constant with a proportional servo on the measured depth (`feedRateBytesPerSec`, target ~100 ms, tau 8 s, ±1 % clamp); also anchored the pump clock on the first chunk (kills the session-dependent FFmpeg warm-up burst) and fixed `decodeOsdWord` returning a hardcoded 685 that clobbered `AUDIO_CLOCK_PPM` on every OSD poll. Hardware, TNG 40868 @3:54 120 s: depth **97-101 ms flat** (19198-19231 B vs 19200 B target), `audio_s == wall_s`, **drops 0** (was 1), 0 overrun warnings, Main pid stable.
- [x] G-AV4 Trek eyes-on — **PASS** (S1E1 **40868**): user eyes-on settled on **video delay +80 ms**. Root cause of the reported "half a second" was the knob turned the wrong way: the menu read `A/V offset` and never said which stream moved, so `-160 ms` pushed video *further* ahead (−160 → +80 is a 240 ms swing). Menu is now `Video delay` (positive = video later). **The +80 is not transferable to the new clock** — it was tuned while video paced off submitted bytes, so it silently absorbed that session's ring depth (see G-AV8). The default is therefore reset to **0 ms** and needs one fresh eyes-on pass; tracked as G-AV9.
- [x] G-AV9 retune `Video delay` under the audible clock — **PASS at 0 ms, no constant needed.** User eyes-on, real TNG S1E1 `40868` @ 3:54 (23.976 → `fps=24000/1001`, HEVC transcode), `osd lo=0x0000` / `av_offset_ms=0`: "audio looks in sync right now". The old +80 ms was **entirely** the MrAudio ring depth the submitted-byte clock was silently carrying; with the audible clock and the feed servo there is nothing left to trim. This is the answer to the original question — Plex Web needs no constant because it schedules against the hardware playback position, and now so do we.
- [x] G-AV11 HDMI grabber calibration — the flash/beep harness read **-215 ms** at the same setting the user's eyes call in-sync, so **the USB grabber's own A/V skew is about -215 ms** (video and audio arrive as independent USB streams). `tests/hw/avsync_measure.py` is therefore a **relative/drift instrument only**; add **+215 ms** to treat a reading as absolute, and never bake a constant from it without eyes-on confirmation.
- [x] G-VID1 VGA/HDMI 1 px edge wrap — **RESOLVED (build `dfebf2bf`).** Symptom: the column falling
  off the right edge reappeared as a vertical bar on the far LEFT ("like word wrap"), plus a
  duplicated bottom line.
  **Two independent defects, both in `present_core.sv`:**
  1. *Horizontal.* `hc → fr` costs ~4 clk (`store_x` on `ce_pix` = 2, `frame_store`
     `rd_addr_r → rd_q → rd_r` = 3) while `hc → hb` costs 1, so pixel data lagged its own DE.
     Fix: delay `hb`/`hs` by `DE_LAG` — **not** `vb`/`vs`. Advancing `read_hc` can never fix this
     and burned many cycles. `DE_LAG = 3` was *measured*, not guessed (sweep 3/4/5/6 built in
     parallel: 3 → col0 w=6px, col319 w=4px ✅; 4 → 4/7; 5 → col0 missing/11; 6 → missing/14).
  2. *The blank-time reset was the actual wrap.* `store_x <= in_content ? store_x_clamped : 0`
     handed **column 0** to any pixel addressed outside `in_content` — which, with DE delayed, is
     the tail of every line. It also *masked* left-edge errors. Fix: drive `store_x` unconditionally
     from the clamped counter.
  3. *Vertical.* `colorbars.sv` moves the V blank edges at `hc == H_SYNC_S` (544), i.e. **after**
     each line's active region, so `VBlank` releases a line early and line 240 is displayed;
     `de_out` never checked `py < 240`, so that row fetched `store_y = 240` — one past the store.
     Fix: `past_last_row = (py >= 240)` folded into `vb_d`, plus a clamped `store_y`.
     Proven by stripe pitch: 1035/1044/1053/1062/1071 = 1080/**241**; after the fix
     1038/1047/1056/1065/1074 and top at 2/9/18/27/36 = 1080/**240**.
  **Verification is now automated and self-checking** — `scripts/gen_edge_markers.py` pushes a
  synthetic marker frame and `scripts/check_edges.py` captures HDMI and grades all four edges on
  both position and run width (exit 0 = pass). Reports `PASS: all four edges correct`, and live
  TNG shows col0↔col319 r=0.33 / row0↔row239 r=0.13 (no wrap). **User confirmed all four edges
  eyes-on on VGA (2026-07-26)** — left/right after the horizontal fix, bottom after the 241st-row
  fix. Closed.
  *Measurement traps worth remembering:* MJPEG 4:2:0 **fabricates** colours at 1 px saturated
  boundaries (use `-input_format yuyv422` and luma-coded markers); `/dev/video4` needs ~60 warm-up
  frames; captures right after a DDR push can be torn — validate with solid white/black first.
- [ ] G-LIB1 missing PMS item silently plays the test pattern — a deleted/renumbered ratingKey 404s, but PMS returns an HTML error *body*, so the old `!xml.empty()` check passed and resolve fell through to `testsrc2`. Cost a full session of measurements attributed to Star Trek that were actually the test pattern. Now detected via `<MediaContainer` and reported as "no such item on PMS". **TNG is no longer in the library** (section 2 holds only ThunderCats; 40868/40710 both 404) — G-AV9 eyes-on needs it re-added.
- [x] G-AV5 exact content rate — log `content fps exact=24000/1001` (Trek/RK11) vs `24/1` (RK12) from identical `videoFrameRate="24p"` metadata; `test_avclock` in `make unit`
- [x] G-AV6 drift slope ≤ 10 ms/min — **+0.79 / −0.67 / +1.79** ms/min (RK11) and **−2.21** (RK12), 240 s single-capture fits; before **−53.3**; `tests/hw/avsync_rate.py`
- [x] G-AV7 constant offset ≤ 42 ms — **PASS via G-AV4**: the residual was a real, constant video lead, not grabber skew. Instrumented capture said +60 ms (|median| 36 ms, `avsync_trekmatch_d60`) and eyes-on said +80 ms — two independent measures one step apart, so the default is evidence-backed, not guessed.
- [x] G-SEEK1 mid-play seekTo ±1 s — blip key=6 seek 12s → playing ~15.5s
- [x] G-SEEK2 resume/continue-watching ≠0 — playMedia offset=15s → playing ~19s
- [x] G-SEEK3 unit regression — `make unit` PASS + `universalOffsetSeconds(234000)==234`
- [x] G-REG tear RBF unchanged unless proven needed — leave **`1441d409`** alone

---

## ACTIVE — OSD menu v3 + idle screen (**DONE** 2026-07-26)

User ask: *"add the different audio delay settings and any other controls in the config directly to the RBF
menu items so the user can change them there without having to edit a config"* and *"the last frame of the
last video is staying on screen and should go back to a default state, black screen or even better a Plex
logo screensaver option in the menu"*.

| | |
|--|--|
| **RBF** | **`91777ac1`** (`91777ac11bc63e7bb4ab20331a26540d`) — supersedes `1441d409` on the lab path. Fit 427 s, **0 errors / 40 warnings**. Not in the banned set. |
| **Deploy** | ONE `DEPLOY_LOAD=menu` bounce; lab `md5sum /media/fat/_Utility/Plex.rbf` = `91777ac1…` |
| **Conf** | `OSD_CONTROL=1` is now the shipped default (requires v3+); `AV_OFFSET_MS` / `IDLE_SCREEN` are power-on defaults only |

### v3 CONF_STR bit layout (`host/libmisterplex/osd_menu.hpp` is the SoT)

| Bits | Menu item | Notes |
|------|-----------|-------|
| `[0]` | Reset | core-owned |
| `[1]` | A/V auto resync | 0 = On (drift corrector) |
| `[2]` | TV Mode | core-owned |
| `[3]` | Audio clock trim | 0 = On (685 ppm); takes effect next session |
| `[5:4]` | *(dead)* | old Content FPS — removed from the menu, still inert in RTL |
| `[9:6]` | **A/V offset** | **4-bit signed** × 20 ms = −160…+140 ms; index 0 **must** be 0 ms because Main cannot express a non-zero CONF_STR default |
| `[10]`/`[11]` | flush triggers | core-owned |
| `[13:12]` | HPS DDR kick/bank | **never reuse** |
| `[15:14]` | **Idle screen** | Logo / Black / Screensaver / LastFrame |

`kOsdOwnedMask = 0xC3CA`. Removed from the menu: Content FPS, Pattern, Audio tone, Force bars —
`pattern`, `audio_en` and `use_frame_store` are hardwired to `0` in `Plex.sv`, preserving the prior defaults.

### Hard rule (non-obvious trap)

**The daemon must NEVER write user OSD bits.** Main_MiSTer owns the OSD word and persists it to
`/media/fat/config/Plex_v3.CFG`. Any daemon-side `setStatusBits()` on those bits fights Main's shadow copy —
observed flapping `0x01c0 ↔ 0x0000` every poll. Removing all daemon writes made it rock-stable for 30 s+.
This is why `pushContentFpsBits()` / `restoreOsd()` / `osd_state.txt` were reverted. Polling is read-only.

**Gates**

- [x] G-OSD1 live core carries the v3 menu — `set_status --confstr` dumps the exact expected CONF_STR from the running core
- [x] G-OSD2 OSD renders with live values — `/tmp/osd5.png` via `tests/hw/osd_keys.py` (uinput F12), labels + values correct
- [x] G-OSD3 all four controls decode and apply live — `0x00c0`→+60 ms, `0x40c0`→idle=1, `0x40ca`→trim+resync off, `0x020a`→−160 ms; verified **mid-playback**
- [x] G-OSD4 A/V offset measurably moves lipsync — OSD +140 ms → median **−62.15 ms** vs baseline **+94.0 ms** = **−156 ms** delta
- [x] G-IDLE idle screen replaces the stuck last frame — `/tmp/idle3.png`, `/tmp/idle_afterstop.png`; amber chevron on near-black after stop
- [x] G-OSD-UNIT bit layout pinned — `tests/unit/test_osd_menu.cpp` in `make unit` (12 suites green)
- [ ] G-OSD5 arrow-key menu navigation eyes-on — **PENDING user**: `/dev/uinput` F12 works, **arrows do not register**, so lab automation cannot drive the menu end-to-end
- [x] G-OSD6 **F12/OSD invisible (2026-07-26)** — **RESOLVED. Root cause: a wedged MiSTer `Main`.**
  F12 came back by itself after a reboot, with zero RTL changes. `Main` had spun forever in the
  GPO/SPI ACK handshake; while wedged it stops servicing the HPS<->FPGA link, so it never writes
  `io_osd_vga`/`io_strobe`/`io_din` and the OSD simply has nothing to draw. Same wedge also made it
  silently drop `/dev/MiSTer_cmd`, so `load_core` was a no-op (see G-VID1).
  **Two earlier diagnoses were WRONG — recorded so they are not repeated:**
  1. "SPI sharing in `d9941de` starves Main's OSD writes." Disproven: F12 stayed dead with
     `misterplexd` fully stopped.
  2. "`sys/video_mixer.sv` / `sys/osd.v` are never instantiated." Disproven: `video_mixer.sv`
     contains **no OSD at all**, and `sys/sys_top.v` **is** the top-level entity
     (`Plex.qsf:11: TOP_LEVEL_ENTITY sys_top`), instantiating `osd hdmi_osd` (line 1183) and
     `osd vga_osd` (line 1403) via `sys/sys.qip`. The framework composites the OSD regardless of
     whether the core uses `video_mixer`; routing through `video_mixer` would NOT have fixed F12.
  **Follow-up (open, tracked as G-STAB2):** find what wedges `Main` in the first place.
  **Liveness test — the only valid one:** `load_core /media/fat/menu.rbf`, then confirm
  `/tmp/CORENAME` actually becomes `MENU`. A `DEPLOY_LOAD=menu` bounce printing `CORENAME=Plex`
  proves **nothing** (it loads Menu then Plex, ending at `Plex` either way).
  **First diagnosis was WRONG — recorded so it is not repeated:** Main pid 30664 sits in `state=R`
  at ~48 % CPU with `utime` climbing, which looks exactly like the documented GPO-mid-handshake
  spin. It is not — the same Main serviced `/dev/MiSTer_cmd` during a `DEPLOY_LOAD=menu` bounce
  (`CORENAME=Plex`). **A busy-looking Main is normal while a core is loaded; only a dead
  `MiSTer_cmd` proves the wedge.** Keyboard is enumerated fine (`SIGMACHIP USB Keyboard` →
  `event0`; Main's `MiSTer virtual input` → `event4`).
- [x] G-STAB1 **`misterplexd` exits during session handoff** — **RESOLVED. Root cause: unhandled
  `SIGPIPE`.** Reproduced deterministically with a `seekTo` on live TNG: the new `PLAY`, the old
  session's `audio pump end` / `session end`, the session-start banner (`FPGA frame path OK`), then
  **nothing** — process gone, port 3005 refusing.
  The silence was the clue. All logging is `fprintf(stderr, …)` and stderr is unbuffered, so no
  message was lost; the process was dying from a signal that prints nothing. `SIGPIPE`'s default
  action terminates silently and leaves **no dmesg record**, which is exactly what we saw.
  Two defects, both fixed:
  1. `SIGPIPE` was never ignored — `main.cpp` installed handlers for SIGINT/SIGTERM/SIGCHLD only.
  2. `companion.cpp`'s `sendHttp()` called `::send(fd, …, 0)` without `MSG_NOSIGNAL`, so any client
     that hung up before the response flushed (Plex's long-poll timeline, a timed-out request)
     raised SIGPIPE on the HTTP thread.
  **This one defect also explains G-OSD6.** `SIGPIPE` is not in `installCrashGuard()`'s list
  (SIGSEGV/ABRT/BUS/ILL/FPE/QUIT), so the guard never ran, `Main` was left SIGSTOPped mid-SPI, and
  F12/OSD/`/dev/MiSTer_cmd` all died with the daemon — the "wedged Main" of G-STAB2.
  Verified: seek on live TNG now survives (same pid, new session plays, audio `av-lock`), and
  `Main` stays `state=R` with `CORENAME=Plex`.

- [ ] G-STAB2 **What wedges MiSTer `Main`?** (highest-value stability item) — during this session
  `Main` entered the GPO/SPI ACK spin (`state=R`, `utime` climbing, `wchan=0`) and stayed there.
  Consequences are severe and **silent**: F12/OSD dies (G-OSD6) *and* `/dev/MiSTer_cmd` stops being
  serviced, so `load_core` is accepted then dropped — the RBF on the SD card updates and its md5
  verifies, but **the FPGA keeps running whatever bitstream was loaded when Main wedged**. Four
  consecutive build/deploy/test cycles were invalidated this way before it was noticed.
  Notes: `resumeStrandedMain()` only rescues state **T**, never this spin; Main's handshake loop has
  no timeout. Only recovery found is a reboot (~75 s to SSH; the startup hook re-launches
  `misterplexd`).
  **Next:** determine whether `misterplexd` provokes it (it wedged while the daemon was running),
  and add an automatic detector — probe `load_core menu.rbf` + `/tmp/CORENAME` before trusting any
  deploy, and consider having `deploy_plex_core.sh` fail loudly instead of reporting success.
  **UPDATE — likely answered by G-STAB1.** The daemon was dying from an unhandled `SIGPIPE`, which
  is *not* in `installCrashGuard()`'s signal list. So the guard never ran and `Main` was left
  SIGSTOPped inside an SPI critical section — precisely this wedge. With SIGPIPE ignored and
  `MSG_NOSIGNAL` on the HTTP sends, the daemon no longer dies on that path. Keep this item open
  only to confirm no *other* silent-death path exists: any signal outside
  {SEGV,ABRT,BUS,ILL,FPE,QUIT} strands Main the same way, so consider widening the guard (or
  restoring GPO from an `atexit`/RAII owner) rather than enumerating signals.
  The automatic detector is **done**: `deploy_plex_core.sh` now verifies `CORENAME` actually
  becomes `MENU` and hard-fails (exit 3 wedged / exit 4 never returned to Plex) instead of
  reporting a false success. It fired correctly and caught a real wedge during the edge work.

### misterplexd no longer touches Main (**DONE** 2026-07-26)

**Root cause of "the Plex core keeps crashing Main / F12 is dead".** The HPS<->FPGA "SPI"
is a single GPO register (`0xFF706010`) plus a strobe/ACK handshake, owned by Main. Main's
`fpga_spi()` spins on that handshake with **no timeout**, so rewriting GPO while Main sits
between `strobe=1` and `saw ACK` makes the core drop ACK and Main spin **forever** — no
F12, no OSD, no `/dev/MiSTer_cmd`. `healMainReloadPlex()` then "fixed" that by killing and
re-exec'ing Main after every play; since `/etc/inittab` uses `sysinit` (not `respawn`), any
interruption of that left the board with **no Main at all** until a power cycle.

- [x] G-MAIN1 verified-safe SPI window — stop Main, wait for **every task** in
      `/proc/<pid>/task` to reach state `T`, require Main's enable bits
      (FPGA_EN/OSD_EN/IO_EN) + strobe all clear, restore GPO byte-for-byte, then SIGCONT.
      Retry on busy; fail the call cleanly rather than corrupt the handshake.
- [x] G-MAIN2 DDR OSD mailbox — core publishes `status[15:0]` at **`0x3007F100`**
      (`"PLXS"` + seq) from `ddram_frame_rd`; host reads it with a plain load. Live on
      hardware: `media: OSD via DDR mailbox (no SPI)`. Frame path was already SPI-free,
      so a normal session now touches SPI **zero** times.
- [x] G-MAIN3 bandaid removed — `healMainReloadPlex()` and `ensureMainAlive()` deleted.
      misterplexd never starts, stops, kills or reloads Main. Only `resumeStrandedMain()`
      remains, and it sends **SIGCONT only**.
- [x] G-MAIN4 hardware proof — 4 play/stop cycles, Main pid **28219 unchanged throughout**;
      `screenshot` via `/dev/MiSTer_cmd` produced a new file *after* playback (the exact
      thing that used to need a Main restart); final state `R`, core `Plex`.
- [x] G-MAIN5 unit regression — `tests/unit/test_main_guard.cpp` also asserts no guard
      ever terminates Main.

RBF `06b3cb4836436ba2a60a237dc604eb7a` — 0 errors, timing MET (worst setup slack
0.051 ns, pll_hdmi), ALM 23%, RAM blocks 74%.

**`scripts/deploy_misterplexd.sh` was shipping stale binaries** — it only cross-compiled
`if [[ ! -f "$BIN" ]]`, so once `build/arm/misterplexd` existed no later deploy rebuilt it.
It now always runs `make arm-plexd`. Check this first whenever a fix appears to have no
effect on hardware.

### Fallout / follow-ups

- `tests/hw/test_fbar_fast.sh` and `tests/hw/test_menu_osd.sh` are **OBSOLETE on v3** (they drive the removed
  debug menu; on v3 they would write a bogus A/V offset). Headers now say so.
- Two thread-lifecycle bugs fixed in `media_player.cpp`: `startIdle()`/`stopIdle()` raced the same
  `std::thread` (→ `std::terminate`), and `stop()`'s `fpga_.close()` + `healMainReloadPlex()` ran while the
  OSD poller / idle painter were mid-ioctl (→ crash). Guarded by `idleMu_` / `osdMu_` and an ordered
  retire-then-restart in `stop()`.
- A **third**, intermittent abort was found by stress-running `test_plex_browse.sh` (3/6 repro) and read
  out of a core dump: `~MediaPlayer()` destroyed a still-joinable `std::thread`. Only `stop()`/`play()`
  ever joined `thr_`, so a session that ended on its own left it finished-but-joinable, and SIGTERM →
  `main()` return → destructor → `std::terminate`. Fixed with `MediaPlayer::shutdown()`, which joins
  `thr_`/`audioThr_`/`streamThr_` **before** retiring the idle/OSD threads — the reverse order still
  aborted 1/10, because `threadMain` calls `startIdle()` at session end and would spawn a fresh painter
  after the old one was joined. A `shuttingDown_` latch makes `startIdle()`/`startOsdPoll()` refuse to
  start once teardown begins. **0/15 after the fix.**
- Both `test_plex_browse.sh` and `test_companion_http.sh` now `assert_clean_exit` (daemon must exit 0 or
  143). They previously only asserted HTTP responses, so the abort printed `terminate called without an
  active exception` and the script still exited 0 — that is why this shipped.
- `paintIdle()` must use the same DDR-bulk-then-SPI ladder as the present loop; SPI-only F1 does not land.
- HDMI grabber emits ~20 black warmup frames — single-frame captures must `select=gte(n\,40)`.

---

Evidence sources (2026-07-24; **J-backlog66** evidence-only stamp; **exclusive FREE**; lock **`R-csum6 DONE BUILD_OK 2026-07-24T14:10:26-05:00 NEW_RBF=94bbfe433feb562fabe0798e16b378c5 wall=438s LOCK_OK`**): `/tmp/misterplex-*-agent*.txt`. **R-csum6 BUILD_OK DONE** — log `/tmp/plex_quartus_rcsum6.log` Full Comp **0e/40w** wall **438s** exit **0**; NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5`; **LOCK_OK** claim **`c7a847f7`/`ca62d02b`/`904e9b2e`** DIAG=ABSENT Rank1+2+3. **H-deploy-rcsum6 ONE menu DEPLOY_OK** lab **LOADED `94bbfe43`** (`/tmp/misterplex-agent-H-deploy-rcsum6.txt` + user log + lab txt). **H-gate-rcsum6 hard residual IN_PROGRESS / PENDING** — **do NOT invent hard residual PASS**. **WIDE still FAIL open Fix-2** historical **`ec21e133`** span=**0.605**. **3l2 BLOCKED** until sticky **0x14**. **C-unit28/27 PASS** host green. Soft-skip ≠ hard PASS. **BUILD_OK + DEPLOY ≠ residual PASS ≠ WIDE PASS.** **J-backlog66 REFRESH_DONE**.

**W-wide-gate-fix2b — WIDE FAIL reconfirm on lab `ec21e133` (J-backlog61 primary wide cite):** report **`/tmp/misterplex-agent-W-wide-gate-fix2b.txt`**. Lab LOADED full `ec21e1330ddd75ad7f39099e5abfad49` pre+post; ZERO reload. **WIDE FAIL** span=**0.605** (60.5%) live **2..485** R5%=**0.0** L5=**201.6** class **PILLAR_320_of_529**; multi-format 0.605–0.608. **FBAR PASS** 7.0/82.9/94.4 EXIT=0. Fingerprint **IDENTICAL** **H-gate-sf2** / **W-wide-gate-sf2**. Captures `fix2b_*` + `fix2b_wide_analysis.json`. **P3-WIDE remains FAIL open** — do **not** invent WIDE PASS / Fix-3 PASS. Fix-3 hold FIT_GO=NO until residual serial / exclusive free.

**W-wide-gate-sf2 — WIDE FAIL on lab `ec21e133` (companion wide cite):** report **`/tmp/misterplex-agent-W-wide-gate-sf2.txt`**. Lab LOADED full `ec21e1330ddd75ad7f39099e5abfad49`. **WIDE FAIL** span=**0.605** (60.5%) class **PILLAR_320_of_529** (same as pre-Fix-2 / W-wide7); R5%=0.0; captures `wq2_*`. **FBAR soft PASS** 7.0/82.9/94.4 EXIT=0 — **≠ WIDE product PASS ≠ residual PASS**. **P3-WIDE remains FAIL open** — do **not** invent WIDE PASS / Fix-3 PASS.

**H-gate-sf2 — WIDE Fix-2 companion gate on lab `ec21e133`:** report **`/tmp/misterplex-agent-H-gate-sf2.txt`**. RBF full `ec21e1330ddd75ad7f39099e5abfad49`; colorbars SRC **`f1d9666a…`**. **FBAR PASS** 7.0/82.9/94.4 EXIT=0. **WIDTH FAIL** span=**0.605** x0=2 x1=485 class **PILLAR_320_of_529**; captures `captures/menu/sf2w_10.jpg`..`sf2w_12.jpg`. **BUILD_OK + DEPLOY + FBAR ≠ WIDE product PASS.** Next: RCA / Fix-3 plan only after residual exclusive frees — **do not invent Fix-3 PASS**.

**H-gate-ec21 — residual HARD_FAIL on LOADED wide `ec21e133` (residual still FAIL open on lab ec21e133; J-backlog65):** report **`/tmp/misterplex-agent-H-gate-ec21.txt`**; probes `/tmp/misterplex-H-gate-ec21-probes.txt`; reconfirm **`/tmp/misterplex-agent-H-res-ec21.txt`**. Lab md5 match full `ec21e1330ddd75ad7f39099e5abfad49`; ZERO redeploy. **res_csum HARD_FAIL**: sticky0x14=**0/3** (never raw[13]==**0x14**). Canonical +0x53 seq **08/5b/ae/01** (PRE→P1→P2→P3 all +0x53). Class **MULTI_DRIVE_OR_STILL_FAIL**. **res_dc PASS** (−24 / 0xe8) 3/3. FBAR soft EXIT=0 ≠ hard residual PASS. **H-res-ec21**: sticky0x14=**0/7** +0x53 6/6 raw13 **54 a7 fa 4d a0 f3 46** — **SAME CLASS** as historical **`8832824e`**. Wide Fix-2 bitfile does **not** carry residual sticky pack fix. **BUILD_OK + DEPLOY + FBAR + PACKAGE ≠ residual PASS.** Soft-skip ≠ PASS. **3l2 BLOCKED.** Thrash residual banned set **FORBIDDEN**; do not thrash-redeploy **ec21e133** for residual luck either. R-csum6 **BUILD_OK** host **`94bbfe43`** is the intentional multi-drive path — **not** luck redeploy of **ec21e133**; **ONE H-deploy-rcsum6** next.

**H-fbar-ec21b — FBAR soft PASS reconfirm lab `ec21e133` (J-backlog64 cite):** report **`/tmp/misterplex-agent-H-fbar-ec21b.txt`**. Lab LOADED full `ec21e1330ddd75ad7f39099e5abfad49`; ZERO redeploy/Quartus. **FBAR soft PASS** EXIT=0 **7.0/82.9/94.4**. **≠ WIDE product PASS ≠ residual hard PASS ≠ 3l2 UNBLOCK.**

**C-unit28 / C-unit27 — host unit PASS (J-backlog64 cite):** **C-unit28** report **`/tmp/misterplex-agent-C-unit28.txt`** — `make unit` **EXIT=0 PASS**; host goldens res_csum=**0x14** res_dc=**-24**; test_idct_quant + parse self-test OK. Prior **C-unit27** **`/tmp/misterplex-agent-C-unit27.txt`** / log `/tmp/misterplex-unit-C-unit27.log` same goldens. **HOST GREEN ≠ lab residual PASS.**

**B-ddr7 — optional DDR F1 reconfirm PASS on LOADED `ec21e133` (J-backlog63 cite):** report **`/tmp/misterplex-agent-B-ddr7.txt`**. Lab md5 full `ec21e1330ddd75ad7f39099e5abfad49`; ZERO deploy/menu/Quartus; mean **16.5 ms** 5/5 OK (~60.6 fps) ≥30 fps; has_frame 0→1 after pulse0. **DDR ≠ residual PASS ≠ WIDE PASS.** Prior **B-ddr6** dabdaeb0 same class.

**A-csum-host28 — HOST_GOLDEN_OK (J-backlog64 cite):** report **`/tmp/misterplex-agent-A-csum-host28.txt`**. Host residual goldens locked for post–R-csum6 H-gate compare: res_dc=**−24** (0xe8), res_csum=**XOR 0x14**, ideal raw class **`e8 14 xx`**; helper SELF-TEST OK; cite **C-unit28/C-unit27** PASS. **HOST_GOLDEN_OK ≠ lab residual PASS ≠ R-csum6 BUILD_OK ≠ 3l2 UNBLOCK.**

**W-wide-rca-sf2 — FIX2_INEFFECTIVE + W-sf3-plan READY / FIT_GO_WIDE=NO (J-backlog64 cite):** loop SoT `/tmp/misterplex-loop-status.txt` harvest: **W-wide-rca-sf2: FIX2_INEFFECTIVE** pillar **0.605** paint still **content320/DE529**; **W-sf3-plan: Fix-3 READY; FIT_GO_WIDE=NO** while residual LIVE. Plan/hold reports (when present) `/tmp/misterplex-agent-W-sf3-plan.txt` **FIT_GO Q-SF3=NO**; `/tmp/misterplex-agent-W-sf3-hold.txt` **HOLD_OK**. Measure still **W-wide-gate-sf2b / W-wide-gate-sf2 / H-gate-sf2** span=**0.605** **PILLAR_320_of_529**. **≠ WIDE product PASS.** **Q-SF3 sole only after R-csum6 exclusive frees** — do **not** invent WIDE PASS / Fix-3 BUILD_OK.

**H-gate-rcsum5d — DEFINITIVE HARD_FAIL historical lab `8832824e` (residual serial CLOSED FAIL; J-backlog64 cite):** report **`/tmp/misterplex-agent-H-gate-rcsum5d.txt`** consolidates **H-gate-rcsum5** + **5b** + **5c**. Primary `/tmp/misterplex-agent-H-gate-rcsum5.txt` + **`…5b.txt`**; probes `/tmp/misterplex-H-gate-rcsum5-probes.txt` + summary `/tmp/misterplex-H-gate-rcsum5-summary.txt`. Residual gate was on LOADED **`8832824e`** full `8832824e483cf6613f82ee3ba3e592b3` CORENAME=Plex. **res_csum HARD_FAIL**: sticky0x14=**0/12** (never raw[13]==**0x14**). Canonical +0x53/push seq **16/69/bc/0f/62/b5/08** (6/6 adjacent +0x53). Class **NOT PACK_PROVEN** / **MULTI_DRIVE_OR_STILL_FAIL**. **res_dc PASS** (−24 / 0xe8). FBAR soft EXIT=0 (≠ hard residual PASS). Soft residual EXIT=0 ≠ hard PASS. **3l2 BLOCKED.** **BUILD_OK + DEPLOY_OK + PACKAGE_OK ≠ hard residual PASS.** Thrash **`8832824e` forbidden.** Lab path **now** LOADED **`ec21e133`** (wide track) — residual **also HARD_FAIL on `ec21e133`** (**H-gate-ec21** / **H-res-ec21**); residual class **CLOSED FAIL historical + still FAIL open on current lab**.

**H-gate-rcsum4 — HARD_FAIL lab `75da8bb1` (historical; path superseded by 8832824e then ec21e133):** report `/tmp/misterplex-agent-H-gate-rcsum4.txt` + **`/tmp/misterplex-agent-H-gate-rcsum4b.txt`**. Lab md5 **`75da8bb10e36b3e068d66a9ed053cd2c`** match; CORENAME=Plex. **FBAR PASS** (7.0/82.9/94.4). **res_dc PASS** (−24 / **0xe8** sticky). **res_csum HARD_FAIL**: never raw[13]==**0x14**. Series A **64/147/230** (**0x40→0x93→0xe6**, +0x53); 4b reconfirm **0x85→0xd8→0x2b** (+0x53/wrap). Class **NOT PACK_PROVEN** / **MULTI_DRIVE_OR_STILL_FAIL**. Soft residual EXIT=0 ≠ hard PASS. Thrash **75da8bb1 forbidden**.

**R-csum6 sole — residual multi-drive Rank1+2+3 product sticky BUILD_OK DONE + ONE menu DEPLOY + exclusive FREE — J-backlog66:**
- Lock **DONE BUILD_OK**: **`R-csum6 DONE BUILD_OK 2026-07-24T14:10:26-05:00 NEW_RBF=94bbfe433feb562fabe0798e16b378c5 wall=438s LOCK_OK`** (`/tmp/plex_quartus.lock` verbatim).
- **Exclusive FREE** — no `quartus_fit`/docker sole.
- Log **`/tmp/plex_quartus_rcsum6.log`**: BUILD START **14:02:51**; Full Comp **0e/40w** successful; BUILD END **14:10:09** exit **0** wall **438s** (`/tmp/plex_quartus_rcsum6.exit`=`0`; `/tmp/plex_quartus_rcsum6.wall`=`438`).
- NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` size **3506356** — host `fpga/Plex_MiSTer/output_files/Plex.rbf` md5 MATCH; **banned_hit=NO**.
- Claim **`/tmp/plex_quartus_rcsum6.claim/`**: owner **R-csum6-sole**; **FIT_GO=YES** Rank1+2+3; **DIAG=ABSENT**; freeze **Plex `c7a847f7`** / **slice `ca62d02b`** / **stream `904e9b2e`** fulls MATCH live **LOCK_OK**; build_result/done **VERDICT=BUILD_OK**.
- Sole **`/tmp/misterplex-agent-R-csum6-sole.txt` VERDICT=BUILD_OK**; mon **M-fitmon-rcsum6 BUILD_OK YES** SRC_DRIFT **NO** idle=YES.
- **H-deploy-rcsum6 ONE menu DEPLOY_OK**: `/tmp/misterplex-agent-H-deploy-rcsum6.txt` **PROMOTE_OK|DEPLOY_OK**; `/tmp/misterplex-H-deploy-rcsum6-user.log` Soft reload Menu→Plex; lab `/tmp/misterplex-H-deploy-rcsum6-lab.txt` **LOADED `94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` CORENAME=Plex.
- **H-gate-rcsum6 hard residual IN_PROGRESS / PENDING** — **do not invent hard PASS / PACK_PROVEN**. Expect sticky0x14 ≥2; reject +0x53; res_dc=-24.
- **WIDE still FAIL open Fix-2** (historical **`ec21e133`** span=0.605). **3l2 BLOCKED** until hard product sticky **0x14**. Soft-skip ≠ PASS. **BUILD_OK + DEPLOY ≠ residual PASS ≠ WIDE PASS.**
- Evidence: `/tmp/plex_quartus.lock`; `/tmp/plex_quartus_rcsum6.log`; claim dir; `/tmp/misterplex-agent-R-csum6-sole.txt`; `/tmp/misterplex-agent-M-fitmon-rcsum6.txt`; `/tmp/misterplex-agent-H-deploy-rcsum6.txt`; `/tmp/misterplex-H-deploy-rcsum6-user.log`; `/tmp/misterplex-H-deploy-rcsum6-lab.txt`.

**R-csum5 sole — residual serial CLOSED FAIL on `8832824e` (BUILD_OK+DEPLOY_OK+PACKAGE_OK; H-gate HARD_FAIL) — historical; J-backlog62:**
- Build lock (historical): **`R-csum5 DONE BUILD_OK 2026-07-24T13:40:35-05:00`** — then exclusive FREE until Q-SF2 then R-csum6.
- Log residual build: **`/tmp/plex_quartus_rcsum5.log`** — Full Compilation **0e/37w**; wall **441s** exit **0**.
- Build report: `/tmp/misterplex-agent-R-csum5-build.txt` **VERDICT=BUILD_OK**.
- Residual NEW_RBF **`8832824e`** full `8832824e483cf6613f82ee3ba3e592b3` size **3510568**; ∉ banned at build.
- Claim@launch product sticky **`6422fb9a`/`8e6af3bb`** DIAG=ABSENT; mid-fit thrash **`6a5dcaaa`/`7d4a1d8b`** DIAG PRESENT → **PROVENANCE_UNTRUSTED** (map product sticky class midfit-rcsum5b).
- **H-deploy-rcsum5 DEPLOY_OK** ONE menu; lab was **LOADED `8832824e`** (now superseded on lab path by **`ec21e133`**).
- **PACKAGE_OK** (rcsum5 era) embeds **`8832824e`** (superseded by F-prep-qsf2 **`ec21e133`**).
- **H-gate-rcsum5d HARD_FAIL** (definitive; sources 5/5b/5c) MULTI_DRIVE sticky0x14=**0/12** +0x53 seq **16/69/bc/0f/62/b5/08**; **res_dc PASS**; FBAR soft; **NOT PACK_PROVEN**.
- **BUILD_OK + DEPLOY_OK + PACKAGE_OK ≠ hard residual PASS**. Soft-skip ≠ PASS. **3l2 BLOCKED.** Thrash **`8832824e` forbidden.**
- Evidence: **`/tmp/misterplex-agent-H-gate-rcsum5d.txt`**; H-gate 5/5b/5c; probes `/tmp/misterplex-H-gate-rcsum5-probes.txt` + summary; build/deploy/package reports; midfit-rcsum5*.

**Q-SF2 sole — wide Fix-2 TERMINAL BUILD_OK + DEPLOY + PACKAGE + WIDE FAIL open + residual HARD_FAIL on same RBF — historical; J-backlog62 cite:**
- Lock start historical **`Q-SF2 2026-07-24T13:45:49-05:00`**; log END **13:52:56** Full Comp **0e/38w** wall **415s** exit **0**; NEW_RBF **`ec21e133`** full `ec21e1330ddd75ad7f39099e5abfad49` size **3436288**.
- Mon **`M-fitmon-qsf2d` / `M-fitmon-qsf2b` BUILD_OK** — exclusive idle post-exit0 (pre–R-csum6).
- Lock was **`Q-SF2 DONE BUILD_OK 2026-07-24T13:52:56-05:00 NEW_RBF=ec21e133…`** (H-deploy-qsf2 harvest) — **superseded** by **`R-csum6 DONE BUILD_OK`** NEW **`94bbfe43`**.
- **H-deploy-qsf2: PROMOTE_OK | ALREADY_DEPLOYED** lab was **`ec21e133`** (ZERO second menu by H-deploy). Lab path **now LOADED `94bbfe43`** post H-deploy-rcsum6.
- **F-prep-qsf2 / F-prep-sf2: PACKAGE_OK** embeds **`ec21e133`** tarball `dist/misterplex-3c43a66-dirty.tar.gz`.
- **W-wide-gate-sf2: WIDE FAIL** span=**0.605** **PILLAR_320_of_529**; FBAR soft PASS — **WIDE still open**.
- **H-gate-sf2: FBAR PASS**; **WIDTH FAIL** span=**0.605** pillar.
- **H-gate-ec21 / H-res-ec21: residual HARD_FAIL** on LOADED **`ec21e133`** sticky0x14=0 +0x53 **08/5b/ae/01**; res_dc PASS — residual **FAIL open on current lab RBF**.
- **Wide track + residual measure** — ≠ residual product PASS; ≠ WIDE product PASS; ≠ 3l2 unblock; soft-skip ≠ PASS.
- Evidence: `/tmp/plex_quartus_sf2.log`; `/tmp/misterplex-agent-M-fitmon-qsf2d.txt`; `/tmp/misterplex-agent-M-fitmon-qsf2b.txt`; `/tmp/misterplex-agent-H-deploy-qsf2.txt`; `/tmp/misterplex-agent-F-prep-qsf2.txt`; `/tmp/misterplex-agent-F-prep-sf2.txt`; `/tmp/misterplex-agent-W-wide-gate-sf2.txt`; `/tmp/misterplex-agent-H-gate-sf2.txt`; `/tmp/misterplex-agent-H-gate-ec21.txt`; `/tmp/misterplex-agent-H-res-ec21.txt`.

**R-csum4 sole — BUILD_OK NEW_RBF `75da8bb1` + DRIFT caveat (historical path):** log `/tmp/plex_quartus_rcsum4.log`; Full Compilation **0e/35w**; wall **421s** exit **0**; NEW_RBF **`75da8bb1`** full `75da8bb10e36b3e068d66a9ed053cd2c`. Claim freeze DIAG **`94db41b7`/`9a2d10c5`**. Mid-fit **DRIFT_CRITICAL** → **RBF_PROVENANCE_UNTRUSTED**. **H-deploy-rcsum4 PROMOTE_OK | DEPLOY_OK**. **F-prep-rcsum4 PACKAGE_OK** embeds **75da8bb1**. **H-gate HARD_FAIL** (not PACK_PROVEN). **Thrash-redeploy forbidden.**

**R-csum-rtl5 FIT_GO (product sticky pack @ stamp; historical pre-rcsum5):** report `/tmp/misterplex-agent-R-csum-rtl5.txt` **VERDICT=FIT_GO**. Sticky pack PRESENT (`st_res_word_sticky` / `res_pair_sticky` family); status residual half from sticky ONLY; stream only [127:112]. **DIAG=ABSENT** product `residual_csum <= csum_acc` at stamp. Claim md5s **Plex `6422fb9a…` / slice `8e6af3bb…`**. No Quartus by rtl5. **FIT_GO ≠ BUILD_OK.**

**Lab LOADED `94bbfe43` after ONE menu (J-backlog66):** **H-deploy-rcsum6 PROMOTE_OK | DEPLOY_OK** — report `/tmp/misterplex-agent-H-deploy-rcsum6.txt`; user log `/tmp/misterplex-H-deploy-rcsum6-user.log` Soft reload Menu→Plex; lab `/tmp/misterplex-H-deploy-rcsum6-lab.txt` full `94bbfe433feb562fabe0798e16b378c5` CORENAME=Plex size **3506356**. Prior **`ec21e133`** WIDE FAIL span=0.605 + residual HARD_FAIL (**H-gate-ec21**); historical **`8832824e` HARD_FAIL** (**H-gate-rcsum5d**). **H-gate-rcsum6 hard residual IN_PROGRESS / PENDING** — **do not invent hard residual PASS / PACK_PROVEN / 3l2 UNBLOCK**. Soft-skip ≠ PASS. **3l2 BLOCKED** until hard product sticky **0x14**. Thrash **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**. **BUILD_OK + DEPLOY ≠ residual PASS ≠ WIDE PASS.** **WIDE still FAIL open Fix-2.**

**Prior product hard class (H-gate-rcsum3b / 3b2 / 3b3 — historical on `4d6ee356`):** **never sticky 0x14**. +0x53/push family. Soft residual ≠ hard PASS. **Thrash 4d6ee356 forbidden.**

**A-csum-probe7 HOST_PROBE_OK** / **A-csum-map1/map2 MAP_OK:** host map matches FPGA packing; blame residual_csum value / multi-drive pack. Ideal **`e8 14 53 1a` HARD_PASS** offline only.

**R-csum-postfail8 RCA (docs):** MULTI_DRIVE / PACK_FAIL on 75da8bb1; annex-len lo +0x53; sticky latch / multi-drive of status[111:104]; provenance co-cause. Report `/tmp/misterplex-agent-R-csum-postfail8.txt`. **H-gate-rcsum5d** reconfirms same class on **`8832824e`** (seq **16/69/bc/0f/62/b5/08**; sticky0x14=0/12).

**Next serial (R-csum6 TERMINAL BUILD_OK; exclusive FREE; J-backlog66):**
1. ~~**R-csum-rtl5** sticky pack FIT_GO~~ — **DONE**
2. ~~**R-csum5 sole**~~ — **DONE BUILD_OK** NEW_RBF **`8832824e`** wall **441s**; **PROVENANCE_UNTRUSTED**; residual gate **CLOSED FAIL**
3. ~~**Trust + H-deploy-rcsum5**~~ — **DONE DEPLOY_OK** ONE menu **`8832824e`** (lab path since superseded)
4. ~~**PACKAGE_OK embed `8832824e`**~~ — **DONE** (superseded by F-prep-qsf2 / F-prep-sf2)
5. ~~**H-gate-rcsum5 / 5b / 5c / 5d**~~ — **DONE HARD_FAIL** residual serial **CLOSED FAIL** MULTI_DRIVE sticky0x14=0 +0x53 **16/69/bc/0f/62/b5/08**; res_dc PASS; **NOT PACK_PROVEN** (**H-gate-rcsum5d** definitive)
6. ~~**Q-SF2 sole wide Fix-2**~~ — **DONE BUILD_OK** END **13:52:56**; Full Comp **0e/38w** wall **415s** exit **0**; NEW_RBF **`ec21e133`**; mon **M-fitmon-qsf2d/qsf2b BUILD_OK**
7. ~~**H-deploy-qsf2 promote + lab LOADED**~~ — **DONE PROMOTE_OK | ALREADY_DEPLOYED** lab was **`ec21e133`** (path superseded by **`94bbfe43`**)
8. ~~**F-prep-qsf2 / F-prep-sf2 PACKAGE_OK embed `ec21e133`**~~ — **DONE** ≠ product/WIDE PASS
9. ~~**W-wide-gate-sf2 / fix2b + H-gate-sf2 FBAR + WIDTH**~~ — **DONE FBAR soft PASS; WIDE/WIDTH FAIL** span=**0.605** **PILLAR_320_of_529** — **WIDE open; Fix-2 CLOSED ineffective**
10. ~~**H-gate-ec21 / H-res-ec21 residual hard on `ec21e133`**~~ — **DONE HARD_FAIL** sticky0x14=0 +0x53 **08/5b/ae/01** — historical FAIL on prior lab path (superseded by **`94bbfe43`**)
11. ~~**C-unit-sf2 / C-unit26 / C-unit27 / C-unit28 host unit**~~ — **DONE PASS** host (**C-unit28** `/tmp/misterplex-agent-C-unit28.txt`) ≠ lab residual PASS
12. ~~**H-fbar-ec21b FBAR soft reconfirm**~~ — **DONE PASS** 7.0/82.9/94.4 on lab **`ec21e133`**
13. ~~**R-multidrive-rca14 + H-proto-rcsum6 + L-csum-note37**~~ — **DONE** RCA_OK / PROTO_OK / DOCS_OK; parent **FIT_GO=YES**
14. ~~**R-csum6 sole**~~ — **DONE TERMINAL BUILD_OK** lock **14:10:26** wall **438s** exit **0** Full Comp **0e/40w**; NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5`; **LOCK_OK** claim **MATCH** Rank1+2+3 DIAG=ABSENT md5s **`c7a847f7`/`ca62d02b`/`904e9b2e`**; mon **M-fitmon-rcsum6c BUILD_OK**; mid-fit **SRC_DRIFT: NO** — **BUILD_OK ≠ residual hard PASS**
15. ~~**W-fix3-hold / W-fix3-hold2**~~ — **DONE HOLD_OK** **FIT_GO Q-SF3=NO** (hold during exclusive; still hold until residual gate serial / parent free pick)
16. ~~**ONE H-deploy-rcsum6**~~ — **DONE PROMOTE_OK|DEPLOY_OK** ONE menu lab **LOADED `94bbfe43`** (`/tmp/misterplex-agent-H-deploy-rcsum6.txt`)
17. **H-gate-rcsum6** hard residual on **`94bbfe43`** expect sticky0x14 ≥2; reject +0x53; res_dc=-24 — **IN_PROGRESS / PENDING** (do **not** invent hard PASS)
18. ~~**F-prep-rcsum6**~~ — **DONE PACKAGE_OK** embeds **`94bbfe43`** (`/tmp/misterplex-agent-F-prep-rcsum6.txt`)
19. **Wide Fix-3 / Q-SF3** — **WIDE FAIL open** historical **`ec21e133`** 0.605; exclusive FREE — **do not invent WIDE PASS / Fix-3 BUILD_OK**
20. Soft-skip ≠ PASS. **3l2 BLOCKED** until hard product sticky **0x14** on non-DIAG product residual RBF. **WIDE still FAIL open Fix-2.** **BUILD_OK+DEPLOY ≠ residual PASS ≠ WIDE product PASS.**

git **docs HEAD `3c43a66`**; **FPGA committed `7bee0a6`**; R-csum6 claim freeze **MATCH** **`c7a847f7`/`ca62d02b`/`904e9b2e`** DIAG=ABSENT Rank1+2+3 LOCK_OK; NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5`; Fix-2 colorbars **`f1d9666a`**. Host/lab RBF **`94bbfe43`**. Lab **LOADED `94bbfe43`** after ONE menu; exclusive **FREE**. **H-gate-rcsum6 IN_PROGRESS / PENDING** — **do not invent hard residual PASS**. **WIDE still FAIL open Fix-2** historical **`ec21e133`** span=**0.605**. **A-csum-host28 HOST_GOLDEN_OK** + **C-unit28/C-unit27 PASS** host ≠ lab residual PASS; **3l2 BLOCKED**. Soft-skip ≠ PASS. **BUILD_OK + DEPLOY ≠ residual PASS ≠ WIDE PASS.** **Do not invent hard-csum PASS / WIDE PASS / Fix-3 PASS / 3l2 UNBLOCK.** **J-backlog66**. Lab `192.0.2.183`.


## Gate: all green before “complete”
- [x] **Product present / tear-free HDMI+VGA** — **DONE** git **`588e528`** RBF **`1441d409`**; vsync page-flip + DMA hold-off; holdoff2 half/mid/multi **0.00/s**; user eyes-on OK 2026-07-25. Docs: `MILESTONE_VSYNC_PRESENT.md`. **≠ residual hard PASS ≠ WIDE PASS.**
- [x] **Product A/V cast (Plex Web → MiSTerPlex)** — **DONE** `PRESENT=fpga` `STREAM=0` wall-48k; blip24 median **~−13 ms**; pfps≈vfps. Evidence `captures/e2e/REPORT*.md` + blip24.
- [x] `make unit` — **GREEN** (**C-unit28 PASS** + **C-unit27 PASS** + **C-unit-sf2 PASS** + **C-unit26 PASS** reconfirm + **C-unit25/24/23/22/21/20/19/18/17** + **C-unit16/15/14**): EXIT=0; host golden res_csum=**0x14**; res_dc=-24; y00=73 mean=62; companion OK; parse self-test OK. **HOST GREEN ≠ lab hard residual PASS.** Soft-skip ≠ PASS. Reports `/tmp/misterplex-agent-C-unit28.txt`, `/tmp/misterplex-agent-C-unit27.txt`, `/tmp/misterplex-agent-C-unit-sf2.txt`, `/tmp/misterplex-agent-C-unit26.txt` … `/tmp/misterplex-agent-C-unit14.txt`
- [~] HW residual hard gate — **IN_PROGRESS / PENDING H-gate-rcsum6** on lab LOADED **`94bbfe43`** (BUILD_OK+DEPLOY done; **do not invent hard PASS**). Historical FAIL **`ec21e133`** + **`8832824e`**. Soft-skip ≠ hard PASS. **3l2 BLOCKED** until product sticky 0x14. **BUILD_OK + DEPLOY ≠ product hard residual PASS.** (Product present path uses **`1441d409`** separately.)
- [x] FBAR visual PASS — **DONE** on lab **`ec21e133`** (**H-fbar-ec21b** / **W-wide-gate-sf2** / **H-gate-sf2** / **H-gate-ec21** 7.0/82.9/94.4) and prior residual **`8832824e`** / **`75da8bb1`** / **`4d6ee356`**. **FBAR soft ≠ hard residual PASS ≠ WIDE product PASS.**
- [ ] Full-width VGA verified (HBlank@320) — **FAIL open** historical **`ec21e133`** (**W-wide-gate-sf2b** span=**0.605** **PILLAR_320_of_529**; **W-wide-gate-sf2** / **H-gate-sf2** same). Fix-2 BUILD_OK+DEPLOY+FBAR but **WIDTH FAIL** — **WIDE still FAIL open Fix-2**. **do not invent WIDE PASS / Fix-3 PASS**. **W-fix3-hold2 FIT_GO=NO**; exclusive FREE.
- [~] DDR F1 ≥30 fps — **HISTORICAL PASS / CURRENT SILICON UNSUBSTANTIATED**: B-ddr7 on LOADED **`ec21e133`** measured mean **16.5 ms** (~60.6 fps) (`/tmp/misterplex-agent-B-ddr7.txt`); prior B-ddr6 dabdaeb0 same class. Current live evidence on `eeff4eee` shows valid YUV420P `PLXK` but all-zero `PLXF` and `has_frame=0`; do not cite historical DDR PASS as proof that the current DDR frame-store/present path runs on-device.
- [x] `make package` — **PACKAGE_OK** embeds **`94bbfe43`** (**F-prep-rcsum6** `/tmp/misterplex-agent-F-prep-rcsum6.txt`). Prior **ec21e133** / **8832824e** packages historical. Product tear RBF also in `releases/Plex_vsync_tear_1441d409.rbf`. Package ≠ WIDE PASS.
- [x] misterplexd soak — **D-soak3/4/5 PASS** ok=6 (re-soak optional after next RBF)
- [x] Safe deploy only — **H-deploy-rcsum6 PROMOTE_OK|DEPLOY_OK** lab **LOADED `94bbfe43`** ONE menu (`/tmp/misterplex-agent-H-deploy-rcsum6.txt`). Prior **H-deploy-qsf2** **`ec21e133`** / **H-deploy-rcsum5** **`8832824e`**. Product present deploy **`1441d409`**. **Do not thrash-redeploy `8832824e`/`75da8bb1`/`4d6ee356` or second-menu `94bbfe43`.** Residual hard **PASS** via **H-gate-rcsum6** (separate).

## Phase 3 (decode / present)
| ID | Item | Status | Notes |
|----|------|--------|-------|
| P3-PRESENT | VSync page-flip + DMA hold-off (tear-free present) | **DONE** | git **`588e528`** RBF **`1441d409`**; holdoff2 0.00/s tears; user eyes-on OK. See `MILESTONE_VSYNC_PRESENT.md`. |
| P3-AVCAST | Product cast STREAM=0 + wall-48k A/V | **DONE** | blip24 ~−13 ms; Plex Web → MiSTerPlex HDMI/VGA. |
| P3-FBAR | Force bars O[9] visual = bars when pattern=grid | **DONE** (on ec21e133 + 8832824e + 75da8bb1 + 4d6ee356 + 4deaf6cc + dabdaeb0) | **H-fbar-ec21b / W-wide-gate-sf2 / H-gate-sf2 / H-gate-ec21 FBAR soft PASS** on **`ec21e133`**. Parked force bars. **FBAR ≠ hard residual PASS ≠ WIDE product PASS.** |
| P3-WIDE | Full-width DE HBlank@320 | **PARTIAL / FAIL open; WIDE still FAIL open Fix-2; Fix-3 HOLD FIT_GO=NO** | **W-wide-gate-sf2b WIDE FAIL** span=**0.605** **PILLAR_320_of_529** historical **`ec21e133`**; Fix-2 still WIDTH FAIL. **W-fix3-hold2 HOLD_OK FIT_GO=NO**. Exclusive FREE. **do not invent WIDE PASS / Fix-3 BUILD_OK**. |
| P3-DDR | DDR F1 kick reliable in product path | **HISTORICAL PASS / CURRENT UNSUBSTANTIATED** | Historical product **`1441d409`** / **B-ddr7** on **`ec21e133`** reported every-frame F1 / mean **16.5 ms**. Current silicon (`eeff4eee`, source-proven `b5c50c6`) accepts a YUV420P `PLXK` but leaves `PLXF=0/0` and `has_frame=0`; simulation reads `MAGIC_F=0x504c5846`. Reconfirm after fitting current post-`d803e4c`/`86558c4`/`97beb1d` source before marking current DDR present on-device green. |
| P3-3l0 | Host quant/IDCT golden | DONE | `2e2c2dc` |
| P3-3l1 | FPGA full 16 coeffs | **PARTIAL — R-csum6 BUILD_OK+DEPLOY lab `94bbfe43`; H-gate-rcsum6 IN_PROGRESS/PENDING (do not invent hard PASS); historical HARD_FAIL `ec21e133`+`8832824e`** | Host goldens 0x14 (**C-unit28 / C-unit27**). Lab **LOADED `94bbfe43`**. Soft-skip ≠ PASS. **3l2 BLOCKED** until sticky 0x14. Claim **c7a847f7/ca62d02b/904e9b2e** LOCK_OK. **BUILD_OK+DEPLOY ≠ residual PASS**. |
| P3-3l2 | Inv quant + IDCT first 4×4 | **EVIDENCE-BACKED MB0 + NATIVE INTRA GREEN** | W-REL: `h264_iq_idct_4x4.sv` + `test_p3_idct_reference_model`; MB0/block0 handoff is evidence-backed (`y00=73`, mean=62, coeff_csum=0x14; native-I420 scoreboard confirms `got=73 ref=73 abs=0`). W-CABAC native scorer now proves no-deblock intra frame exactness with loop-filter provenance: 624×480 `1170/1170`, 320×240 `300/300`, wcap fixture `300/300`, all MAE 0. |
| P3-3l3 | First full MB recon | **EVIDENCE-BACKED HOST FIXTURE / FPGA BLOCKED** | W-REL: checked-in `tests/fixtures/p3_host_recon/mb0_luma_v1.json` gives MB0 luma pred/dequant/post-IDCT/recon for RTL; source vector is fixed 6739 B. This supports MB0/first-MB handoff only; it is not a frame-wide product pass. |
| P3-3l4 | All MBs / frame mae | **NATIVE-I420 INTRA GREEN / INTER RED** | Retired claim 1: `tests/fixtures/p3_host_recon/frame_mae_v1.csv` / `test_p3_host_recon_vectors` reported `vector_bytes=6739 mb=300/300 frame=320x240 maeY=0.000000`, but that path was RGB565/presentation-contaminated. Retired claim 2: native-I420 ratchets reporting 624×480 `510/1170`, 320×240 `155/300`, and wcap `207/300` used a deblocked reference while RTL output was no-deblock. Current no-deblock native-I420 scorer is green for intra (`1170/1170`, `300/300`, `300/300`, MAE 0) and still expected-red for P frames until full inter reconstruction is wired. |
| P3-3l5 | Hybrid gate product | TODO | |
| P3-3m | Inter prediction scope + host goldens | **BASELINE MODEL DONE / PMS BASELINE FAILS** | W-REL: `docs/phase3-inter-prediction.md`; checked-in P16×16-only Baseline vector `plex_inter_p16_baseline_320x240_12f.264` (27653 B, md5 `fe5ba815…`), `pframe1_mb_v1.json`, `frame_mae_v1.csv`, and `test_p3_inter_pred_vectors` in `make unit`. Baseline Level 3.0 would make scope tractable: P/CAVLC only, no B/CABAC/weighted/interlace; Level 3.0 DPB max at 640x480 is 6 refs = 2.76 MB YUV420. **But W-A4 delivered-stream probe says PMS ignored Baseline request:** branch `feat/a4-sps-baseline` @ `b28e863` requested Baseline/L3.0 (`640x480`, 2500 kbps) but delivered High `profile_idc=100`, CABAC PPS, B-slices (`i=22 p=165 b=115` in 12s), 618×480, ~1344.3 kbps; mpegts target works, final mp4 target returned empty. Product must fail closed/fallback on B/CABAC/non-Baseline; unit now includes a generated High/CABAC/B unsupported probe. DDR3/YUV420 reference store required for any FPGA inter path; BRAM/SDRAM not viable. |
| P3-3n | Real PMS High/CABAC/B decoder sizing | **SCOPE DONE / A NOT SANE NEAR-TERM** | W-REL: `docs/phase3-high-cabac-scope.md` and `test_p3_high_cabac_scope.py` in `make unit`. Final W-A4 sweep says client-only Baseline forcing is impossible: delivered coded 624×480/display 618×480, 1170 macroblocks, 25 fps, High/CABAC/B, 4 refs. CABAC planning demand **8.775 Mbin/s** (300 bins/macroblock), stress **17.550 Mbin/s**. Current `clk_sys`/DDRAM path is 20 MHz, so 1 bin/cycle barely covers stress, 2 cycles/bin fails stress, 3 cycles/bin fails planning. 4 refs + current YUV420 = 2.25 MB; +present/reorder = 2.70 MB; DDR3 required, SDRAM/BRAM not viable. Existing 4×4 IQ/IDCT/recon survives below entropy, but CAVLC walker does not; High may require 8×8 transform detection/support. Verdict: decoding PMS as sent is a full High-profile decoder project; server-side Baseline XML or ARM/FFmpeg fallback is a hard requirement for a sane FPGA-offload path. |
| P3-3p | P-slice inter prediction / motion compensation | **PRODUCT DPB/MC + DEBLOCK SEAM LIVENESS GREEN / NATIVE INTER QUALITY RED** | W-REL: `docs/phase3-inter-rtl.md`; product RTL `h264_inter_pred.sv`, `h264_p_slice_modes.sv`, `h264_dpb.sv`, and `h264_deblock_writeback_ctrl` are in the stream-path liveness build. DPB budget closes at 987 B/MB = 1,154,790 B/frame = 28.87 MB/s at 25fps; DDR first, SDRAM escape hatch. `test_p3_dpb_mc_rtl_sim.sh` proves filtered I420 writeback, frame-boundary promotion, IDR invalidation, clamped 21x21/9x9 fetch, 16x16 MC, and partition masks for 16x8/8x16/8x8/8x4/4x8/4x4; red-checks clamp, MC arithmetic, early ref publication, and partition mask. `test_p3_inter_rtl_sim.sh` compares real RTL against `inter_mc_v1.json` (`mv_cases=6`, `partition_cases=10`, `frame_mv_cases=9090`) and red-checks bad interpolation rounding plus bad partition MV. `test_h264_p_slice_modes_rtl_sim.sh` covers P_Skip, P_L0_16x16, P_L0_16x8, P_L0_8x16, P_8x8/P_8x8ref0, sub-MB 8x8/8x4/4x4, intra-in-P and unsupported mode classification; red-check swaps 16x8 and fails. `slice_hdr_parser.sv` handles non-IDR ref-marking/ref-idx bits before QP and parses P `mb_skip_run` + first P MB type; shared multi-NAL raw `nalu=15 slice=11 idle_between_vcl=1 recon_sig_3b_cycles=39780 p_first_mb_seen=11 p_first_modes=8/2/1 p_first_bad=0`, with forced-`recon_sig=0` red-check. `decode_stub` routes IDR invalidation and reference promotion through product `h264_deblock_writeback_ctrl` (`filtered_sample_valid` before `filtered_mb_valid`, terminal commit then `frame_boundary`, `ref_ready_pulse` → DPB `frame_done`). `test_stream_path_full_frame_compare.sh` now emits both the old refused RGB565 diagnostic candidate and a native-I420 DPB/MC candidate with inter MB metadata; current 320×240 score is `intra=0/300 inter=0/3300`, first_bad_inter frame 1 MB0 Y `got=32 ref=80 abs=48`, `mb_type=P_L0_16x16`, `mv_l0=(0,0)`; 624×480 scorer self-check is `intra=0/1170 inter=0/12870`, first_bad_inter `got=32 ref=77 abs=45`; both use a declared diagnostic filtered reference state. Full native-I420 inter reconstruction, real decoded/deblocked reference pictures, parsed MV deltas, residual add, and all-P-MB traversal remain open. |
| P3-3q | In-loop deblocking | **NORMATIVE EDGE BLOCKS + MULTI-NAL GATE DONE / FRAME SCHEDULER PENDING** | W-A3: `h264_deblock.sv` covers bS derivation (intra, residual, ref/MV, idc=1/2 controls), alpha/beta/tC0 indexing with slice offsets/clipping, luma/chroma edge rules, and a registered edge pipe. `test_p3_deblock_rtl_sim.sh` now sources the ≥2-VCL `wcap_residual14_idr_plus_p` fixture rather than a single-NAL stream and red-checks edge order drift. `test_stream_path_deblock_integration.sh` proves multi-NAL parser handoff, stream QP/deblock controls, chroma bS4 short filtering, picture-boundary preservation, and that filtered samples feed the next reference value; red-checks trip for bS, threshold offsets, chroma-vs-luma behavior, picture boundaries, in-loop reference use, and slice controls. Full frame deblock scheduling/writeback to the DPB/reference store remains the consumer integration step with W-A4/W-REL. |


| P3-SPI | SPI F1 only ~9fps — retired; product F1 is DDR YUV420p-only | DONE | |

## Phase 4 (UX)
| ID | Item | Status | Notes |
|----|------|--------|-------|
| P4-SCRUB | Scrubber/playqueue edge cases | **DONE** | G-p4-dirty `ade6915`; C-unit14..21 reconfirm |
| P4-HZ | Match-source-Hz modeline (docs only OK) | DEFER | |
| P4-SUB | Subtitles burn-in plan | DEFER | |

## Phase 5 (release / lab)
| ID | Item | Status | Notes |
|----|------|--------|-------|
| P5-PKG | Package release tarball | **DONE for 94bbfe43 embed** | **F-prep-rcsum6 PACKAGE_OK** embeds **`94bbfe43`** (`/tmp/misterplex-agent-F-prep-rcsum6.txt`). Prior **ec21e133**/**8832824e** historical. Ship still blocked on **WIDE**. Package ≠ WIDE PASS. |
| P5-SOAK | WiFi soak multi-round | **DONE** | D-soak3/4/5 ok=6; optional re-soak after next RBF |
| P5-ETH | Eth vs wifi numbers | BLOCKED | no eth lab path |
| P5-CRT | CRT matrix checklist | **PARTIAL** | Physical CRT PENDING |

## RBF inventory (agent-J-backlog66: no Quartus; no deploy; no RTL thrash; evidence-only; R-csum6 BUILD_OK harvest)
| Path | md5 (prefix) | Notes |
|------|--------------|-------|
| `fpga/Plex_MiSTer/output_files/Plex.rbf` | **`94bbfe43`** | **R-csum6 NEW_RBF** full `94bbfe433feb562fabe0798e16b378c5` size **3506356**; BUILD_OK wall **438s** exit **0**; claim **c7a847f7/ca62d02b/904e9b2e** DIAG=ABSENT Rank1+2+3; ∉ ban |
| `misterfpga-dev/out/Plex_MiSTer/Plex.rbf` | **`94bbfe43`** | R-csum6 collect match output |
| `fpga/Plex_MiSTer/releases/Plex.rbf` | **`94bbfe43`** | R-csum6 promote MATCH full `94bbfe433feb562fabe0798e16b378c5` |
| `releases/Plex.rbf` (repo root) | **`94bbfe43`** | **PROMOTE_OK** H-deploy-rcsum6; MATCH lab LOADED |
| `releases/Plex_qsf2_ec21e133.rbf` | **`ec21e133`** | H-deploy sidecar copy-only |
| `releases/Plex_sf2_wide_ec21e133.rbf` | **`ec21e133`** | F-prep named snapshot |
| `dist/stage-misterplex/cores/Plex.rbf` | **`ec21e133`** | F-prep-qsf2 PACKAGE_OK stage embed |
| `dist/misterplex-3c43a66-dirty.tar.gz` (F-prep-qsf2) | embeds **`ec21e133`** | **PACKAGE_OK** ≠ product/WIDE PASS |
| Lab `/media/fat/_Utility/Plex.rbf` | **`94bbfe43`** | **LOADED** after **H-deploy-rcsum6 ONE menu** full `94bbfe433feb562fabe0798e16b378c5` CORENAME=Plex; cite user log + lab txt + `/tmp/misterplex-agent-H-deploy-rcsum6.txt`; **H-gate-rcsum6 IN_PROGRESS/PENDING** — do not invent hard PASS; prior **`ec21e133`** WIDE FAIL 0.605 historical open |
| Prior residual host/lab RBF (R-csum5; path superseded on lab) | **`8832824e`** | **BUILD_OK** + **DEPLOY_OK** + **H-gate-rcsum5d HARD_FAIL** MULTI_DRIVE; thrash **forbidden**; residual CLOSED FAIL historical |
| `releases/Plex_rcsum5_8832824e.rbf` | **`8832824e`** | local archive copy-only (R-csum5-build) |
| Prior host/lab RBF (R-csum4; path superseded) | **`75da8bb1`** | **BUILD_OK** + **DEPLOY_OK** + **H-gate HARD_FAIL**; thrash forbidden |
| Prior host/lab RBF (R-csum3b; path superseded) | **`4d6ee356`** | historical **HARD_FAIL** +0x53; **thrash forbidden** |
| Prior host/lab RBF (R-csum2) | **`4deaf6cc`** | superseded; PACK_FAIL stream24 |
| R-csum2 fit-start claim SRC | Plex **`9b97b792`** + slice **`eec44561`** | DIAG force-0x14 + multi-cycle |
| **R-csum3 claim @ fit start (historical; dead)** | Plex **`eb6b8541`** + slice **`6ce28d6e`** | **FIT_DEAD_MID**; not BUILD_OK |
| **R-csum3b freeze @ fit start (BUILD_OK product; HARD_FAIL lab; path superseded)** | Plex **`ce1ef26c`** + slice **`e45f98c4`** | **LOCK_OK**; DIAG ABSENT; failed product hard csum |
| **R-csum4 freeze @ claim (BUILD_OK; gate HARD_FAIL; DRIFT caveat)** | Plex **`94db41b7`** + slice **`9a2d10c5`** | DIAG PRESENT at claim; mid-fit **DRIFT_CRITICAL**; **PROVENANCE_UNTRUSTED**; silicon never sticky 0x14 |
| **R-csum5 claim@launch (BUILD_OK sole; product sticky map-era)** | Plex **`6422fb9a`** + slice **`8e6af3bb`** | sticky_pack PRESENT; **DIAG=ABSENT** product csum_acc; lock **13:31:58**; map-era class per midfit-rcsum5b; **PROVENANCE_UNTRUSTED** vs thrash |
| **R-csum5 live WT + claim overwrite mid-fit (DRIFT_CRITICAL)** | Plex **`6a5dcaaa`** + slice **`7d4a1d8b`** | DIAG PRESENT `residual_csum<=8'h14`; claim rewrite ~13:33:19 SUPERSEDES launch product; live matches overwrite; **PROVENANCE_UNTRUSTED**; map netlist ≠ this class |
| **Q-SF2 claim SRC_colorbars (wide Fix-2 BUILD_OK)** | colorbars **`f1d9666a`** full `f1d9666ada5347dbde7e7246bad345c8` | W-sf2-midfit **DRIFT: NO**; Fix-2 paint-full-DE; silicon still WIDTH FAIL 0.605 |
| **R-csum6 claim@fit (BUILD_OK+LOCK_OK wall 438s)** | Plex **`c7a847f7`** + slice **`ca62d02b`** + stream **`904e9b2e`** fulls `c7a847f743bace1e0df48f2d0571f513` / `ca62d02b7188f4dd9be5109dc4f2dd64` / `904e9b2ea3bc6f560cb10c65796f9fbc` | **FIT_GO=YES** Rank1+2+3 **DIAG=ABSENT**; live==claim **MATCH** **SRC_DRIFT: NO** **LOCK_OK**; lock **DONE BUILD_OK** NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` wall **438s**; mon **BUILD_OK**; **H-deploy ONE menu lab LOADED `94bbfe43`**; exclusive **FREE**; **H-gate-rcsum6 PENDING** — **BUILD_OK+DEPLOY ≠ residual PASS** |
| Prior lab FAIL baseline | **`dabdaeb0`** | Superseded |
| Prior tarballs | `820484a6` / `aa146c17` / `6db3a4d8` / F-prep-rcsum2/3b/4/5 era | Superseded by **ec21e133** package (F-prep-qsf2) |

### Quartus status (2026-07-24; **R-csum6 BUILD_OK DONE** NEW **`94bbfe43`** wall **438s** LOCK_OK; lab LOADED **`94bbfe43`**; exclusive **FREE**; H-gate PENDING; WIDE FAIL open Fix-2 — **J-backlog66**)
| Build | Log | Result |
|-------|-----|--------|
| Clean FBAR | `/tmp/plex_quartus_fbar_clean.log` | **BUILD_OK**; RBF `6db3a4d8` |
| **Q-3l1** | `/tmp/plex_quartus_3l1.log` | **BUILD_OK** → `aa146c17` (superseded) |
| Q-3l1b | (not started) | **ABORT** busy |
| **Q-SF1** | `/tmp/plex_quartus_sf1.log` | **BUILD_OK** → `820484a6` (superseded) |
| **R-csum1** | `/tmp/plex_quartus_rcsum1.log` | **BUILD_OK** → **`dabdaeb0`**; superseded |
| **R-csum2** | `/tmp/plex_quartus_rcsum2.log` | **BUILD_OK** → **`4deaf6cc`**; hard csum FAIL. Superseded. |
| **R-csum3** | **`/tmp/plex_quartus_rcsum3.log`** | **FIT_DEAD_MID** — **NOT BUILD_OK.** |
| **R-csum3b** | **`/tmp/plex_quartus_rcsum3b.log`** | **BUILD_OK** → **`4d6ee356`**. H-gate hard **HARD_FAIL**. Thrash forbidden. |
| **R-csum4** | **`/tmp/plex_quartus_rcsum4.log`** | **BUILD_OK ~13:22** → **`75da8bb1`** wall **421s**. **H-gate HARD_FAIL** MULTI_DRIVE; thrash forbidden. |
| **R-csum5** (residual serial **CLOSED FAIL** historical) | **`/tmp/plex_quartus_rcsum5.log`** | **BUILD_OK** wall **441s** → **`8832824e`**; **DEPLOY_OK**+**PACKAGE_OK**; **H-gate-rcsum5d HARD_FAIL** sticky0x14=0 +0x53 **16/69/bc/0f/62/b5/08**; res_dc PASS; **PROVENANCE_UNTRUSTED**; thrash **`8832824e` FORBIDDEN**. |
| **Q-SF2** (wide Fix-2 sole — **DONE**) | **`/tmp/plex_quartus_sf2.log`** | **BUILD_OK DONE** END **13:52:56**; Full Comp **0e/38w** wall **415s** exit **0**; NEW_RBF **`ec21e133`** full `ec21e1330ddd75ad7f39099e5abfad49`; mon **M-fitmon-qsf2d/qsf2b BUILD_OK**; lock was **DONE BUILD_OK** (H-deploy harvest) — **superseded by R-csum6 DONE BUILD_OK**. **H-deploy PROMOTE_OK \| ALREADY_DEPLOYED**; **F-prep-qsf2/sf2 PACKAGE_OK**; **W-wide-gate-sf2 WIDE FAIL** span=0.605 pillar; **H-gate-ec21 residual HARD_FAIL** on same RBF. **≠ residual PASS ≠ WIDE product PASS.** |
| **R-csum6** (residual multi-drive Rank1+2+3 product sticky — **DONE BUILD_OK + DEPLOY**) | **`/tmp/plex_quartus_rcsum6.log`** | **BUILD_OK DONE** END **14:10:09**; Full Comp **0e/40w** wall **438s** exit **0**; NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` size **3506356**; claim Rank1+2+3 **DIAG=ABSENT** **`c7a847f7`/`ca62d02b`/`904e9b2e`** **LOCK_OK** SRC_DRIFT **NO**; mon **M-fitmon-rcsum6 BUILD_OK**; lock **DONE BUILD_OK 14:10:26**; exclusive **FREE**. **H-deploy-rcsum6 ONE menu** lab **LOADED `94bbfe43`**; **H-gate-rcsum6 IN_PROGRESS/PENDING** — do **not** invent hard residual PASS. |

- Action this agent (**J-backlog66**): **refresh backlog only** (no Quartus; no lab thrash; no RTL) — **REFRESH_DONE**; cite **R-csum6 BUILD_OK** Full Comp **0e/40w** wall **438s** NEW_RBF **`94bbfe43`** LOCK_OK + **H-deploy ONE menu lab LOADED `94bbfe43`** (`/tmp/misterplex-H-deploy-rcsum6-user.log` + lab txt + agent report) + exclusive **FREE** + **H-gate-rcsum6 IN_PROGRESS/PENDING** (do not invent hard PASS) + **WIDE still FAIL open Fix-2** + **C-unit28/27 PASS** host + **3l2 BLOCKED** until sticky 0x14
- Sequence:
  1. ~~R-csum1 BUILD_OK + collect dabdaeb0~~ — **DONE**
  2. ~~Sole menu deploy dabdaeb0 + FBAR~~ — **DONE** H-deploy; hard csum **FAIL**
  3. ~~Postfail + rtl2 multi-cycle + diagrtl~~ — **DONE dirty**
  4. ~~**R-csum2 sole fit BUILD_OK**~~ — **DONE** → **`4deaf6cc`**
  5. ~~**Promote `4deaf6cc`**~~ — **DONE**
  6. ~~**One sole menu deploy `4deaf6cc` + FBAR + hard csum**~~ — **DONE deploy/FBAR PASS**; **hard csum FAIL**
  7. ~~**F-prep-rcsum2 package embeds 4deaf6cc`**~~ — **DONE PASS** (superseded)
  8. ~~**C-unit16..19 unit green**~~ — **DONE PASS**
  9. ~~**Pack-path RCA + R-csum-rtl3 dirty fix + LOCK_OK @ claim**~~ — **DONE**
  10. ~~**R-csum3 sole**~~ — **FIT_DEAD_MID** — **NOT BUILD_OK**
  10b. ~~**Mid-fit WT drift audit (R-csum3)**~~ — **DONE** → R-csum3b freeze
  11. ~~**R-csum3b sole fit BUILD_OK**~~ — **DONE** → **`4d6ee356`**
  11b. ~~**Promote + ONE menu (H-deploy-rcsum3b)**~~ — **DONE**
  11c. ~~**F-prep-rcsum3b package embeds 4d6ee356**~~ — **DONE** (superseded)
  11d..11d3. ~~**H-gate-rcsum3b / 3b2 / 3b3**~~ — **DONE HARD_FAIL** +0x53; thrash forbidden
  11e. ~~**C-unit21..27 unit green**~~ — **DONE PASS** (**C-unit27** host; ≠ lab PASS)
  11f. ~~**A-csum-probe7**~~ — **DONE HOST_PROBE_OK**
  11g. ~~**A-csum-map1 / map2**~~ — **DONE MAP_OK**
  11h. ~~Stale **R-csum-rtl4.txt @12:34**~~ — **ignore for FIT_GO**
  11i. ~~**L-csum-note21 DIAG pack bisect docs**~~ — **DONE**
  12. ~~**`R-csum-rtl4c` FIT_GO**~~ — **DONE** DIAG sticky 0x14; freeze **`94db41b7`/`9a2d10c5`**
  12b. ~~**`R-csum4` sole DIAG compile**~~ — **BUILD_OK ~13:22** → **`75da8bb1`** — **DRIFT / PROVENANCE_UNTRUSTED**
  12c. ~~**H-deploy-rcsum4 promote + ONE menu**~~ — **DONE** lab was LOADED (now superseded)
  12d. ~~**F-prep-rcsum4 package embeds 75da8bb1**~~ — **DONE PACKAGE_OK** (historical)
  12e. ~~**H-gate-rcsum4**~~ — **DONE HARD_FAIL** NOT PACK_PROVEN MULTI_DRIVE
  12e2. ~~**H-gate-rcsum4b reconfirm (no redeploy)**~~ — **DONE HARD_FAIL** +0x53 0x85→0xd8→0x2b
  12f. ~~**R-csum-rtl5** sticky pack product FIT_GO~~ — **DONE** claim **`6422fb9a`/`8e6af3bb`** DIAG=ABSENT
  12g. ~~**R-csum5 sole**~~ — **DONE BUILD_OK** lock **13:40:35** CDT; Full Comp **0e/37w** wall **441s** exit **0**; NEW_RBF **`8832824e`**; mid-fit **DRIFT_CRITICAL** launch **`6422fb9a`/`8e6af3bb`** → live/claim **`6a5dcaaa`/`7d4a1d8b`** DIAG PRESENT; map-era product sticky (**midfit-rcsum5b**); **PROVENANCE_UNTRUSTED**
  12h. ~~**H-deploy-rcsum5 ONE menu**~~ — **DONE DEPLOY_OK** lab was **LOADED `8832824e`** (path superseded)
  12i. ~~**PACKAGE_OK embed `8832824e`**~~ — **DONE** (superseded by F-prep-qsf2)
  12j. ~~**H-gate-rcsum5 / 5b / 5c / 5d**~~ — **DONE HARD_FAIL** residual serial **CLOSED FAIL** MULTI_DRIVE sticky0x14=0 +0x53 **16/69/bc/0f/62/b5/08**; res_dc PASS; FBAR soft; **NOT PACK_PROVEN**; thrash **`8832824e` forbidden** (**H-gate-rcsum5d** definitive)
  12k. ~~**Q-SF2 sole wide Fix-2 BUILD_OK**~~ — **DONE** END **13:52:56**; Full Comp **0e/38w** wall **415s** exit **0**; NEW_RBF **`ec21e133`**; mon M-fitmon-qsf2d/b **BUILD_OK**; lock was **DONE BUILD_OK** (superseded by R-csum6 DONE BUILD_OK)
  12l. ~~**H-deploy-qsf2 PROMOTE_OK | ALREADY_DEPLOYED**~~ — **DONE** lab was **`ec21e133`** (superseded by **`94bbfe43`**)
  12m. ~~**F-prep-qsf2 / F-prep-sf2 PACKAGE_OK embed `ec21e133`**~~ — **DONE** ≠ product/WIDE PASS
  12n. ~~**W-wide-gate-sf2 + H-gate-sf2 FBAR + WIDTH**~~ — **DONE FBAR soft PASS; WIDE/WIDTH FAIL** span=**0.605** pillar — **WIDE open**
  12o. ~~**H-gate-ec21 / H-res-ec21 residual hard on `ec21e133`**~~ — **DONE HARD_FAIL** sticky0x14=0 +0x53 **08/5b/ae/01**; res_dc PASS — residual **FAIL open on current lab**
  12p. ~~**C-unit-sf2 / C-unit27 / C-unit28 host unit**~~ — **DONE PASS** host ≠ lab residual PASS (**C-unit28** `/tmp/misterplex-agent-C-unit28.txt`)
  12p2. ~~**A-csum-host28 HOST_GOLDEN_OK**~~ — **DONE** host 0x14/−24 ≠ lab PASS
  12q. ~~**H-fbar-ec21b FBAR soft reconfirm**~~ — **DONE PASS** 7.0/82.9/94.4
  12r. ~~**W-wide-rca-sf2 + W-sf3-plan**~~ — **DONE** FIX2_INEFFECTIVE 0.605; Fix-3 READY; **FIT_GO_WIDE=NO** until residual hard path / parent Q-SF3 pick (exclusive FREE post BUILD_OK)
  13. ~~**R-csum6 sole**~~ — **DONE TERMINAL BUILD_OK** lock **14:10:26** wall **438s** exit **0** Full Comp **0e/40w**; NEW_RBF **`94bbfe43`**; mon **M-fitmon-rcsum6c BUILD_OK**; claim **MATCH** Rank1+2+3 DIAG=ABSENT **c7a847f7/ca62d02b/904e9b2e** — **BUILD_OK ≠ residual hard PASS**
  14. ~~**ONE H-deploy-rcsum6**~~ — **DONE PROMOTE_OK|DEPLOY_OK** lab **`94bbfe43`** (`/tmp/misterplex-agent-H-deploy-rcsum6.txt`) ≠ hard residual PASS
  14b. **H-gate-rcsum6 hard residual on `94bbfe43`** — **IN_PROGRESS / PENDING** sticky0x14 ≥2 reject +0x53 res_dc=-24 — **do not invent hard PASS**
  15. **Wide Q-SF3 after residual serial / free** — WIDTH FAIL open on **`ec21e133`**; **W-fix3-hold2 FIT_GO=NO** — **do not invent WIDE PASS / Fix-3 BUILD_OK**
  16. Soft-skip ≠ PASS; G-fpga WAIT; thrash banned residual FORBIDDEN; **3l2 BLOCKED**
- Do **not** invent hard-csum PASS / WIDE PASS / Fix-3 PASS / 3l2 UNBLOCK; soft-skip ≠ PASS; **DIAG ≠ product PASS**; **BUILD_OK+DEPLOY+PKG+FBAR ≠ residual PASS ≠ WIDE product PASS**; **3l2 BLOCKED** until sticky 0x14; thrash **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**; exclusive **FREE**; **H-gate-rcsum6 PENDING**; **WIDE still FAIL open Fix-2**

## Open (priority next workers)
1. ~~**R-csum6 sole LIVE**~~ — **DONE TERMINAL BUILD_OK** lock **`R-csum6 DONE BUILD_OK 2026-07-24T14:10:26-05:00 NEW_RBF=94bbfe433feb562fabe0798e16b378c5 wall=438s LOCK_OK`**; Full Comp **0e/40w** wall **438s** exit **0**; mon **M-fitmon-rcsum6c BUILD_OK**; claim **MATCH** Rank1+2+3 DIAG=ABSENT md5s **`c7a847f7`/`ca62d02b`/`904e9b2e`**; **SRC_DRIFT: NO**; exclusive **FREE** post-build — **BUILD_OK ≠ residual hard PASS**
2. ~~**ONE H-deploy-rcsum6**~~ — **DONE PROMOTE_OK|DEPLOY_OK** ONE menu lab **LOADED `94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` (`/tmp/misterplex-agent-H-deploy-rcsum6.txt`) — **DEPLOY_OK ≠ hard residual PASS** — do **not** invent hard sticky0x14 PASS
3. **H-gate-rcsum6** hard residual on lab **`94bbfe43`** (sticky0x14 ≥2; reject +0x53; res_dc=-24) — **IN_PROGRESS / PENDING** — **do not invent hard PASS / PACK_PROVEN**. Prior **ec21e133** **H-gate-ec21 HARD_FAIL** MULTI_DRIVE; historical **CLOSED FAIL** **`8832824e`**
4. **P3-WIDE / Q-SF3 Fix-3** — **WIDE still FAIL open Fix-2** historical **`ec21e133`** span=**0.605** pillar (**W-wide-gate-fix2b**); Fix-2 **CLOSED ineffective**. **W-fix3-hold2 HOLD_OK FIT_GO=NO** — **do not invent WIDE PASS / Fix-3 BUILD_OK**
5. **3l2 BLOCKED** — until hard product sticky **0x14** on non-DIAG product RBF (**H-gate-rcsum6 PENDING** on **`94bbfe43`**); historical FAIL **`ec21e133`** + **`8832824e`**; soft-skip ≠ PASS; thrash residual banned set FORBIDDEN
6. ~~**Q-SF2 sole**~~ — **DONE BUILD_OK** NEW_RBF **`ec21e133`** (superseded on lab path by residual **`94bbfe43`** post H-deploy-rcsum6)
7. ~~**H-deploy-qsf2**~~ — **DONE PROMOTE_OK | ALREADY_DEPLOYED** lab **LOADED `ec21e133`**
8. ~~**F-prep-qsf2 / F-prep-sf2**~~ — **DONE PACKAGE_OK** embeds **`ec21e133`**
9. ~~**W-wide-gate-fix2b / sf2 / H-gate-sf2**~~ — **DONE FBAR soft PASS; WIDE FAIL** 0.605 open (Fix-2 ineffective)
10. ~~**H-gate-ec21 / H-res-ec21**~~ — **DONE residual HARD_FAIL** on **`ec21e133`** sticky0x14=0 +0x53 MULTI_DRIVE; res_dc PASS
11. ~~**R-multidrive-rca14 + H-proto-rcsum6 + L-csum-note37**~~ — **DONE**
12. ~~**H-fbar-ec21b**~~ — **DONE FBAR soft PASS** 7.0/82.9/94.4
13. ~~**C-unit28 / C-unit27**~~ — **DONE PASS** host ≠ lab residual PASS
14. ~~**W-fix3-hold / W-fix3-hold2**~~ — **DONE HOLD_OK** FIT_GO Q-SF3=**NO**
15. ~~**H-gate-rcsum5d**~~ — residual serial **CLOSED FAIL** historical on **`8832824e`**
16. **P3-3l1 HW residual hard** — **H-gate-rcsum6 IN_PROGRESS / PENDING** on lab **`94bbfe43`**; historical FAIL **`ec21e133`** — **do not invent hard PASS**
17. **P3-3l2..3l5** — **3l2 BLOCKED** until hard csum sticky **0x14** on non-DIAG product; soft-skip ≠ PASS; **BUILD_OK+DEPLOY ≠ hard PASS**
18. **P5-CRT** — PARTIAL physical open
19. ~~**P5-PKG**~~ — **DONE** embeds **`94bbfe43`** (F-prep-rcsum6); prior ec21e133 historical
20. ~~**make unit C-unit14..28 + C-unit-sf2**~~ — DONE PASS host ≠ lab residual PASS
21. ~~**J-backlog16..65**~~ — DONE prior; this is **J-backlog66 REFRESH_DONE**
22. **G-fpga** — **WAIT** (no FPGA commit of thrash/DIAG; after hard green + intentional product SRC@fit match LOCK_OK)
23. ~~**R-csum6-midfit***~~ — **DONE** mid-fit DRIFT_OK / SRC_DRIFT NO through terminal

## Non-RBF always available
- (done **C-unit14..28 + C-unit-sf2**) unit GREEN host 0x14 — **C-unit28 PASS** (`/tmp/misterplex-agent-C-unit28.txt`) + **C-unit27 PASS** + **C-unit-sf2 PASS** + **C-unit26 PASS**; **host GREEN ≠ lab hard residual PASS**; soft-skip ≠ PASS
- (done **B-ddr7 optional**) DDR F1 on LOADED **`ec21e133`** mean **16.5 ms** PASS (`/tmp/misterplex-agent-B-ddr7.txt`) — DDR ≠ residual/WIDE PASS
- (done **Q-SF2 BUILD_OK**) Full Comp 0e/38w wall 415s exit 0; NEW_RBF **`ec21e133`**; mon M-fitmon-qsf2d/b; lock was DONE — then R-csum6 **TERMINAL BUILD_OK** NEW **`94bbfe43`**; exclusive FREE post-build
- (done **H-deploy-qsf2**) **PROMOTE_OK | ALREADY_DEPLOYED** lab was **`ec21e133`** (superseded by **`94bbfe43`**)
- (done **H-deploy-rcsum6 ONE menu**) **PROMOTE_OK | DEPLOY_OK** lab **LOADED `94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` (`/tmp/misterplex-agent-H-deploy-rcsum6.txt` + user log + lab txt) — **DEPLOY ≠ hard residual PASS**
- (in progress **H-gate-rcsum6**) hard residual on **`94bbfe43`** — **do not invent hard PASS**
- (done **F-prep-qsf2 / F-prep-sf2 PACKAGE_OK**) embeds **`ec21e133`** ≠ product/WIDE PASS; prior residual package **`8832824e`** / F-prep-rcsum4 **75da8bb1** historical
- (done **W-wide-gate-sf2**) **WIDE FAIL** span=0.605 **PILLAR_320_of_529**; FBAR soft PASS — WIDE open; ≠ residual PASS (`/tmp/misterplex-agent-W-wide-gate-sf2.txt`)
- (done **H-gate-sf2**) FBAR PASS; **WIDTH FAIL** span=0.605 pillar — WIDE open; ≠ residual PASS
- (done **H-fbar-ec21b**) FBAR soft PASS 7.0/82.9/94.4 on lab **`ec21e133`** — ≠ WIDE/residual PASS (`/tmp/misterplex-agent-H-fbar-ec21b.txt`)
- (done **H-gate-ec21 / H-res-ec21**) residual **HARD_FAIL** on LOADED **`ec21e133`**: sticky0x14=0 +0x53 **08/5b/ae/01**; res_dc PASS; MULTI_DRIVE; **3l2 BLOCKED** (`/tmp/misterplex-agent-H-gate-ec21.txt`, `/tmp/misterplex-agent-H-res-ec21.txt`)
- (done **H-deploy-rcsum5**) promote + one menu **DEPLOY_OK** lab was **`8832824e`** (path superseded)
- (done **H-gate-rcsum5 / 5b / 5c / 5d**) residual serial **CLOSED FAIL** historical **`8832824e`**: FBAR soft; res_dc PASS; sticky0x14=0 +0x53 **16/69/bc/0f/62/b5/08**; **MULTI_DRIVE**; **NOT PACK_PROVEN**; thrash **`8832824e` forbidden**; definitive **`H-gate-rcsum5d`**
- (done **H-deploy-rcsum4**) promote + one menu **DEPLOY_OK** lab was **75da8bb1** (path superseded)
- (done **H-gate-rcsum4 / 4b**) **HARD_FAIL** lab **75da8bb1**: FBAR PASS soft; res_dc PASS; res_csum +0x53 never 0x14; **MULTI_DRIVE/PACK_FAIL**; **NOT PACK_PROVEN**
- (done **H-gate-rcsum3b / 3b2 / 3b3**) hard residual **HARD_FAIL** on prior **4d6ee356** (path superseded; thrash forbidden)
- (done **A-csum-probe7**) HOST_PROBE_OK; ideal **`e8 14 53 1a` HARD_PASS** offline only
- (done **A-csum-map1 / map2**) **MAP_OK** — blame residual_csum value path / multi-drive
- (done **A-csum-host28**) **HOST_GOLDEN_OK** res_dc=−24/0xe8 res_csum=XOR **0x14** ideal **e8 14 xx** (`/tmp/misterplex-agent-A-csum-host28.txt`) — HOST GREEN ≠ lab residual PASS
- (done **L-csum-note2..38** + **H-proto-rcsum*** + **R-csum-rtl3/6-plan** + **R-csum-fold-audit2** + **R-csum-postfail8** + **R-multidrive-rca14**) docs + pack RCA + R-csum6 protocol
- (done **R-csum-midfit-rcsum4 / midfit-rcsum4b**) **DRIFT_CRITICAL** documented; STOP mid-fit RTL
- (done **R-csum-midfit-rcsum5 / midfit-rcsum5b / midfit-rcsum5c**) **DRIFT_CRITICAL** launch **`6422fb9a`/`8e6af3bb`** → **`6a5dcaaa`/`7d4a1d8b`** claim overwrite; map=`st_res_word_sticky` product-class **PROVENANCE_UNTRUSTED**
- (done **W-sf2-d2/d3** / **W-wide7/7b**) design READY; Fix-2 silicon still WIDTH FAIL 0.605 on **`ec21e133`**
- (done **R-csum6 sole TERMINAL BUILD_OK**) lock **DONE BUILD_OK 14:10:26** wall **438s** exit **0** Full Comp **0e/40w**; NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5`; claim freeze **MATCH** **`c7a847f7`/`ca62d02b`/`904e9b2e`** FIT_GO=YES DIAG=ABSENT Rank1+2+3; mon **M-fitmon-rcsum6c BUILD_OK**; mid-fit **SRC_DRIFT: NO**; exclusive **FREE**; lab **LOADED `94bbfe43`** after ONE menu
- soft-skip ≠ PASS; **3l2 BLOCKED** until hard product sticky **0x14**; **BUILD_OK / DEPLOY_OK / FBAR / PACKAGE ≠ hard residual PASS ≠ WIDE product PASS**; **DIAG ≠ product PASS**
- thrash **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**; exclusive **FREE**; **H-deploy-rcsum6 DONE** lab **LOADED `94bbfe43`**; **H-gate-rcsum6 IN_PROGRESS/PENDING** — do not invent hard PASS; **WIDE still FAIL open Fix-2**; **W-sf3-hold2 FIT_GO=NO**
- git docs **`3c43a66`**; FPGA committed **`7bee0a6`**; R-csum6 claim **MATCH** **`c7a847f7`/`ca62d02b`/`904e9b2e`** NEW host **`94bbfe43`**; prior thrash residual **`6a5dcaaa`/`7d4a1d8b`**; colorbars Fix-2 **`f1d9666a`**; lab LOADED **`94bbfe43`**; residual CLOSED FAIL historical **`8832824e`**; **WIDE FAIL 0.605** open; **C-unit28 PASS** host; G-fpga **WAIT**; stamp **J-backlog66**; **do not invent hard-csum PASS / WIDE PASS / Fix-3 PASS / 3l2 UNBLOCK**
