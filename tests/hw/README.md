# Hardware tests

Require a live MiSTer. Set `MISTER_HOST` and `MISTER_PASS`. There is no default host or password.

## HDMI capture (lab MacroSilicon)

Capture card: `/dev/video4` (UVC `534d:2109`), MJPG/YUYV **800×600**.

```bash
# Prefer: skip cold MJPG frames (first ~30 often solid black ~4KB — capture artifact)
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 800x600 -i /dev/video4 \
  -vf 'select=gte(n\,30)' -frames:v 1 -update 1 captures/mister_hdmi_latest.jpg

# Or YUYV (locks faster on this dongle)
ffmpeg -y -f v4l2 -input_format yuyv422 -video_size 800x600 -i /dev/video4 \
  -frames:v 1 -update 1 captures/mister_hdmi_yuyv.jpg
```

**Warm** capture shows color bars: SMPTE bars in **left ~content** region; **right-side black DE**
(Template **HBlank@529**, content painted only for `hc<320`). Cold single-frame MJPG is often
solid black (~4 KB) — a capture-card artifact, **not** proof the core is dead.

For **G-VID1 edge alignment**, use `scripts/check_edges.py` rather than an ad-hoc
grab. The checker forces `yuyv422`, discards 60 warm-up frames, and refuses to
grade unless the frame differs from a previous baseline (`--capture-only` before
the marker push, then `--previous` for the graded capture). Its `--source file`
and `--source synthetic` modes exercise the same grading/staleness logic without
opening the hardware grabber.

## Glass baseline floor (v0.2.0 pair)

**Floor pair** for eyes-on HDMI (not the product tip): RBF `dfebf2bf` + daemon `7cd10b4d` @ `DECODE=320x240`.  
Policy: `tests/fixtures/glass_baseline/pair.json` · docs: `docs/glass-baseline-pair.md` · artifacts: `release_artifacts/v0.2.0-glass-baseline/`.

```bash
# Offline md5/policy
make glass-baseline-policy

# Pin floor pair + idle chevron + B6 play (USB grabber)
MISTER_HOST=<mister-ip> MISTER_PASS=REDACTED make glass-baseline

# Prove a *new* misterplexd+Plex.rbf pair matches the floor (no pin)
GLASS_CANDIDATE=1 make glass-baseline
```

New pairs need **not** reuse the floor md5s; they **must** pass the same behavioral gates (or better) before replacing the lab default.

## Phase 1 / 2 (misterplexd)

| Script | What |
|--------|------|
| `test_glass_baseline_pair.sh` | HDMI glass floor (pair pin + chevron + B6) |
| `test_media_fb.sh` | play/pause/resume/stop via companion |
| `test_playqueue_bind.sh` | scrubber play-queue fields |
| `test_audio_mraudio.sh` | `/dev/MrAudio` PCM path |
| `test_single_process.sh` | one FFmpeg demux for A/V |
| `test_seek_kill.sh` | seek + kill/restart recovery |
| `test_soak.sh` | multi-title play/stop soak (Phase 5: auto conf + PMS discover) |

```bash
MISTER_HOST=<mister-ip> ./scripts/deploy_misterplexd.sh
./tests/hw/test_media_fb.sh
# Soak: loads conf from MiSTer (or MISTER_CONF), discovers PMS titles when token set
SOAK_HOLD_S=5 SOAK_ROUNDS=1 ./tests/hw/test_soak.sh
# Longer multi-round soak + Wi-Fi/Ethernet matrix label (logs net snapshot via ssh)
SOAK_HOLD_S=15 SOAK_ROUNDS=5 SOAK_PROGRESS=1 SOAK_NET_LABEL=wifi ./tests/hw/test_soak.sh
# Ethernet row when cable is default route:
# SOAK_NET_LABEL=eth SOAK_HOLD_S=15 SOAK_ROUNDS=5 ./tests/hw/test_soak.sh
```

## Phase 3.0 legacy SPI frame push (retired)

The old RGB565 F1/SPI push path is retired. Product and hardware helpers must
use the DDR YUV420p frame-store path below; non-YUV F1 sends are refused at the
tool/API boundary.

Continuous ARM→FPGA stream (misterplexd) is Phase 3.1.

## Phase 3.1b DDR bulk frame (beat SPI F1)

1. Deploy RBF that includes `rtl/ddram_frame_rd.sv` (DDRAM not tied to 0).
2. `python3 scripts/gen_edge_markers.py --format yuv420p build/plex_test_320x240.yuv420p`
3. On MiSTer: `push_frame --ddr --yuv420p 320x240 plex_test_320x240.yuv420p`
   - Or: `./tests/hw/test_ddr_frame.sh` (scp + push + status)
4. Expect wall time **≪ 100 ms** (SPI is ~200 ms) and `has_frame=1`.
5. misterplexd uses DDR YUV420p for F1 and does not fall back to RGB/SPI.
   Banks: `0x30000000` / `0x30040000`; kick = status[12], bank = status[13].

## Phase 3.2 audio FIFO

1. Deploy latest `Plex.rbf`.
2. `python3 scripts/gen_test_pcm.py /tmp/pcm.s16le`
3. `push_frame --index 2 /tmp/pcm.s16le` on MiSTer (F2).
4. Core should play tone from FIFO (~1s) instead of OSD square wave.
5. LED blinks faster while `has_audio`.
6. Append + continuous: `./tests/hw/test_f2_append.sh` (multi-chunk SPI + live f2==bytes).

## Phase 3.3 elementary bitstream (F3)

1. Deploy RBF with `stream_path` / F3 menu entry + status readback.
2. `python3 scripts/gen_test_annexb.py build/plex_test_annexb.264`
3. `./tests/hw/test_f3_bitstream.sh` — SPI index 3 + `push_frame --status` asserts `nalu≥4`, `has_stream=1`.
4. Manual: `push_frame --status` on MiSTer dumps has_frame/audio/stream + nalu_count.

## Phase 3.3b decode_stub (F3 → pixels)

1. Deploy latest RBF (`decode_stub` wired into `frame_store`).
2. `./tests/hw/test_f3_decode_stub.sh` — asserts `has_idr`, `stub_frames≥1`, `has_frame`.
3. Display shows green-border diagnostic frame after F3 push (not color bars alone).
4. Continuous product path: `STREAM=1` in `misterplex.conf` (+ `PRESENT=fpga|both`)
   demuxes annex-B → F3 while playing (decode_stub until real H.264 IP).

## Phase 3.3i/k host I-slice recon → F1 (product STREAM)

1. Conf: `PRESENT=both` (or `fpga`) and `STREAM=1` in `/media/fat/misterplex/misterplex.conf`.
2. Deploy: `MISTER_HOST=<mister-ip> ./scripts/deploy_misterplexd.sh`.
3. Play Baseline 320×240 annex-B or direct H.264 Part (STREAM prefers direct H.264 over CABAC universal).
4. Log should show:
   - `STREAM=1 host I-slice recon →F1 +F3`
   - `recon frame ok #1 320x240 mb=300 …` (multi-IDR increments `idr=`)
   - session `recon=N` (N≥1 after first IDR); CABAC sticky from PPS entropy flag (not cleared by in-band SPS)
5. Frame store shows reconstructed I-frame (not only decode_stub green border).
6. Fallback: if recon fails, FFmpeg RGB still drives F1 until first recon success (`PRESENT=both`).
7. Seek/stop: both FFmpeg groups killed; seek restarts demux at offset.
8. Optional: `STREAM_SKIP_RGB=auto` + `PRESENT=fpga` drops heavy RGB (audio kept).
9. Smoke: `./tests/hw/test_stream_recon.sh` (local Baseline + companion play).

## Phase 3.3c SPS parse (real Baseline)

1. Deploy RBF with `sps_parser`.
2. `./tests/hw/test_f3_sps.sh` — ffmpeg Baseline 320×240 annex-B → `sps_valid=1 sps=320x240`.
3. Unit: `make unit` includes `test_sps_parse`.

## Phase 3.3d PPS + I-slice header

1. Deploy RBF with `pps_parser` + `slice_hdr_parser`.
2. `./tests/hw/test_f3_slice_hdr.sh` — `pps_valid=1 slice_type=7` (I/IDR) + `sps=320x240`.
3. Unit: `test_slice_hdr`. Display: MB grid diagnostic on VCL.

## Phase 3.3e first MB type + slice QP

1. Deploy RBF with extended slice_hdr (qp/deblock/mb0).
2. `./tests/hw/test_f3_mb0.sh` — real Baseline IDR (IDR marking fixed): **`mb0=0` (I_NxN)** **`qp=25`**.
   Older notes claiming `mb0=7` / `qp=14` were pre-fix goldens; do not use them.
3. Unit: `test_slice_hdr` asserts mb0+qp.

## Phase 3.3f/j/k residual probe + hybrid present

1. Deploy RBF with residual levels/runs + 3.3j hybrid mux (`host_owns_fs`).
2. `./tests/hw/test_f3_residual.sh` — F3-only golden (same Baseline IDR as 3.3e):
   `res_ok=1 res_tc=8 res_t1=3 res_dc=-24` (I_NxN first MB full CAVLC),
   **`mb0=0 qp=25`** `has_frame=1` (stub MB0 gray from residual DC; no F1 so host_owns_fs clear).
   Soft: `res_csum=20` (XOR sat8 full-16 = **0x14**) — soft-skip EXIT=0 is **not** hard PASS.
3. Unit: `test_cavlc_dc` (host CAVLC + bit-exact recon maeY=U=V=0); `test_idct_quant` locks csum **0x14**.
4. STREAM hybrid: host recon F1 owns product present; F3 residual status must not wipe F1.

### Residual csum RCA (host-only, post-R-csum1 re-gate)

Host golden locked: `res_dc=-24` (`raw[12]=0xe8`) + `res_csum=20` (`raw[13]=0x14`).
Do **not** thrash lab with residual push storms while R-csum1 fit/re-gate is in flight —
capture one `--status`/`--raw` line and decode offline.

```bash
# print goldens + status map (no lab)
python3 tests/parse_res_csum_status.py
python3 tests/parse_res_csum_status.py --self-test

# decode a captured status / raw line → EXPECTED vs ACTUAL + GATE
echo 'status ... res_dc=-24 res_csum=20 ...' | python3 tests/parse_res_csum_status.py -
python3 tests/parse_res_csum_status.py e8 14 3b 53   # raw[12..15]
# class CSUM_FAIL_DC_OK / STREAM_BYTES_ALIAS / STALE_ARITH_SUM_FOLD
```

| raw | field | hard expect | ARM decode |
|-----|-------|-------------|------------|
| `[12]` | residual_dc | `0xe8` (−24) | `parseCoreStatus` → `residual_dc = (int8_t)raw[12]` |
| `[13]` | residual_csum8 | `0x14` (20) XOR — never arith `0xEC` | `residual_csum = raw[13]` |
| `[14]` | recon_sig8 | `0x3b` (59) after P3-3l2 | `recon_sig = raw[14]` |
| `[15]` | recon_dbg flags | P3-3l2 RCA: usable bits `0x01/0x08/0x10/0x20/0x40/0x80` (AR may mask bits `[2:1]`) | `recon_dbg = raw[15]` |

Sources: `arm/misterplexd/fpga_spi.cpp` `parseCoreStatus`; `host/libmisterplex/h264_residual_gold.hpp` (`kCsum8==0x14`); `tools/push_frame.cpp` `res_csum=%u`.

ARM on lab: `push_frame --status` prints `res_csum=`; `push_frame --raw` dumps hex.
Hard gate after F3 push: `res_dc=-24` **and** `res_csum=20` (soft-skip EXIT=0 is **not** hard PASS).

### Post–R-csum1 sole-deploy + hard-gate (ONE agent)

Full numbered protocol: [`docs/phase3-3l-idct.md`](../../docs/phase3-3l-idct.md) § *Post–R-csum1 sole-deploy + hard-gate protocol*.

**R-csum1 BUILD_OK** md5 **`dabdaeb0`**; sole deploy + hard-gate **DONE** (**H-deploy-rcsum1** / **H-rcsum-gate**): FBAR PASS; res_dc=-24 PASS; **res_csum HARD FAIL** (raw[13] unstable 139/222/49 ≠0x14). Soft-skip ≠ PASS. **Do not invent hard PASS.** Do not thrash-redeploy `dabdaeb0` expecting green.

Historical ONE-agent checklist (already run; re-use after *next* residual-fix BUILD_OK only; new RBF md5 **≠** `dabdaeb0` / **≠** `820484a6`):

1. **Collect md5** of `output_files/Plex.rbf` (abort if still prior FAIL md5).
2. **Promote** same bitfile → `fpga/Plex_MiSTer/releases/Plex.rbf` + `releases/Plex.rbf` (all three md5s match).
3. **Sole menu deploy once:** `DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh` — never scp+load_core thrash.
4. Lab confirm: remote md5 match + `CORENAME=Plex`.
5. **FBAR:** `./tests/hw/test_fbar_fast.sh` EXIT=0 (m1/m2 ≥15).
6. **Hard residual:** `res_dc=-24` (`raw[12]=0xe8`) **and** `res_csum=20` (`raw[13]=0x14`) stable ≥2 re-pushes.
   Soft-skip EXIT=0 from `test_f3_residual.sh` is **not** hard PASS.
   Decode: `python3 tests/parse_res_csum_status.py` (A-csum-host2); lab print: `push_frame --status`/`--raw` (A-arm-csum).
7. **Park bars:** `set_status --pattern bars --force-bars 1 --tv ntsc --fps 60`.
8. **Report:** `/tmp/misterplex-agent-H-rcsum-gate.txt` (or successor).

On hard PASS only → unlock 3.3l-2 paint. On FAIL (current dabdaeb0) → RCA contingency (a–g: status/lev/pack/dc-only/dc-regress/FBAR/tools) — measure first, no thrash. Branch **a** first for dabdaeb0 (unstable csum, res_dc stable).

## Safe core deploy (lab DE10)

Do **not** `scp` over a live `Plex.rbf` and immediately `echo load_core … > /dev/MiSTer_cmd`
while SPI companions are active — that path has locked the unit (WiFi/SSH die until power cycle).

Use:

```bash
# copy only (safest while watching VGA)
DEPLOY_LOAD=none ./scripts/deploy_plex_core.sh

# soft switch Menu → Plex when a reload is required
DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh

# already on Menu core
DEPLOY_LOAD=core ./scripts/deploy_plex_core.sh
```

HW tests prefer **skip reload** when `CORENAME` is already Plex.
