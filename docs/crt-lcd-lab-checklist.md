# CRT / LCD practical lab checklist (P5-CRT)

Practical acceptance rows for MiSTerPlex present paths.  
**Does not invent CRT PASS** without a physical 15 kHz set. Lab can complete most rows via **HDMI LCD + USB capture** and status telemetry.

| Related | Path |
|---------|------|
| High-level matrix | [crt-lcd-matrix.md](crt-lcd-matrix.md) |
| Cadence / match-Hz | [match-source-hz.md](match-source-hz.md) |
| Menu OSD captures | [captures/menu/CHECKLIST.md](../captures/menu/CHECKLIST.md) |
| Session fill-in sheet | [captures/menu/CRT_LCD_LAB.md](../captures/menu/CRT_LCD_LAB.md) |
| Automated driver | `tests/hw/run_menu_matrix.sh` |
| FBAR smoke | `tests/hw/test_fbar_fast.sh` |

**Honesty rules (non-negotiable)**

| Rule | Detail |
|------|--------|
| No false CRT PASS | HDMI / USB-capture success never promotes a **CRT** column to PASS |
| No Quartus for this checklist | Docs + existing captures + unit + optional status/soak only |
| No `load_core` thrash | Prefer `MENU_RELOAD=0` / `DEPLOY_LOAD=none`; only retest when md5 already matches |
| PARTIAL until CRT | P5-CRT stays **PARTIAL** while any physical-CRT row is open |

**Legend**

| Tag | Meaning |
|-----|---------|
| **LAB** | Verifiable on current lab (MiSTer + HDMI capture `/dev/video4`, no CRT) |
| **CRT** | Needs physical 15 kHz (or 31 kHz) CRT via VGA/DAC |
| **EITHER** | Pass criteria exist for both; CRT eyes-on is stronger |
| **PROXY** | Lab proxy only — approximates CRT behavior, not geometry/sync truth |

Status values: `PENDING` | `PASS` | `FAIL` | `SKIP` | `N/A (no CRT)`

---

## Practical tick matrix (LAB vs physical CRT)

Use this as the operator front page. Tick **LAB** only from HDMI/USB evidence; tick **CRT** only with a real CRT attached.

| # | Check | How (lab) | LAB | CRT |
|---|-------|-----------|-----|-----|
| 1 | Pattern Bars / Block / Grid / Ramp | `set_status` + capture mean≥20 | [x] **PASS** | [ ] PENDING |
| 2 | Force bars on non-Bars pattern | `test_fbar_fast.sh` (Δforce_grid ≫ 0) | [x] **PASS** `dabdaeb0` | [ ] PENDING |
| 3 | TV Mode NTSC | bit2=0 + non-black | [x] **PASS** | [ ] PENDING |
| 4 | TV Mode PAL (+ restore NTSC) | bit2=1; recover HDMI; do not soak on PAL | [x] **PASS** (PROXY) | [ ] PENDING (true 50 Hz) |
| 5 | Content FPS 12 / 24 / 30 / 60 bits | OSD / `set_status --fps` + capture | [x] **PASS** | [ ] PENDING eyes |
| 6 | Cadence math 3:2 / 2:2 | `./build/test_cadence` / `make unit` | [x] **PASS** | N/A (host unit) |
| 7 | Live film motion @ FPS 24 | play + eyes (optional multi-frame) | [ ] PENDING optional | [ ] PENDING |
| 8 | `PRESENT=fb0` cast picture+sound | Phase 2 / soak family | [x] **PASS** | [ ] PENDING |
| 9 | `PRESENT=both` STREAM=0/1 soak | `test_soak.sh` wifi; daemon stays up | [x] **PASS** wifi | [ ] PENDING |
| 10 | `PRESENT=fpga` frame store image | Video source = Frame store; non-black | [~] PARTIAL | [ ] PENDING |
| 11 | Aspect AR0–3 geometry | bits stick on HDMI; geometry weak on grab | [x] bits PASS | [ ] CRT eyes-on |
| 12 | Audio tone / MrAudio path | status bit LAB; hear on display | [x] bit PASS | [ ] hear analog |
| 13 | 15 kHz H-lock / no roll | modeline 320×240-class | N/A | [ ] **needs CRT** |
| 14 | Overscan / usable raster width | force bars fill usable area | N/A (HDMI ≠ proof) | [ ] **needs CRT** |
| 15 | Full-width DE (HBlank@320 / P3-WIDE) | `fw_*` / VGA eyes | [x] **FAIL** ~60.5% proxy (`820484a6`) | [ ] **needs CRT/VGA** |
| 16 | Match-source-Hz modeline | conf logged only today | [ ] TODO | [ ] TODO + switchres |
| 17 | Eth vs wifi soak compare | `SOAK_NET_LABEL=` | [~] wifi only; eth **BLOCKED** | — |

**Current lab hardware (2026-07-24, agent CRT3):** HDMI LCD + `/dev/video4` only — **no physical 15 kHz CRT**. Therefore every **CRT** column above remains unchecked / PENDING. Do not promote. Lab RBF **`dabdaeb0`** (H-deploy-rcsum1; FBAR reconfirm PASS; res_dc=-24 PASS; res_csum hard FAIL H-rcsum-gate; prior soak/DDR on `820484a6`; WIDE FAIL last on `820484a6` — orthogonal to CRT matrix).

---

## 0. Lab inventory (fill each session)

| Item | Expected | Lab 2026-07-24 |
|------|----------|----------------|
| MiSTer host | SSH `192.0.2.183` | wifi `wlan0` (eth NO-CARRIER) |
| Core | `CORENAME=Plex`, RBF md5 **`dabdaeb0…`** | lab LOADED (H-deploy-rcsum1; FBAR reconfirm; hard res_csum FAIL) |
| Capture | MJPEG **800×600** `/dev/video4` | recipe locked in menu driver |
| Tools | `set_status`, `push_frame` on device | `/media/fat/misterplex/bin/` |
| Display under test | HDMI LCD **and/or** CRT | **HDMI only** — no 15 kHz CRT attached |
| Conf | `/media/fat/misterplex/misterplex.conf` | `PRESENT=both` soak path exercised |

---

## 1. OSD / modes matrix (Plex.rbf)

Pass criteria are **LAB**-default: status bits stick + non-black capture (mean luma ≥ 20) unless noted.  
Screenshots live under `captures/menu/`.

| ID | Mode / control | Values | Pass criteria | Scope | Lab status | CRT status | Evidence / notes |
|----|----------------|--------|---------------|-------|------------|------------|------------------|
| BASE | Defaults after load / seed | NTSC, Bars, Force bars Yes | Non-black; `set_status --raw` shows seed | LAB | **PASS** | N/A | `baseline_forced` / seed path |
| PAT0 | Pattern | Bars | Distinct color bars; status pat=0 | LAB | **PASS** | **PENDING** (geometry on CRT) | `pat_bars.jpg` |
| PAT1 | Pattern | Bars+Block | Distinct vs pure bars (block may be small) | LAB | **PASS** | PENDING | `pat_bars_block.jpg` |
| PAT2 | Pattern | Grid | Checkerboard; MAD vs bars ≫ 0 | LAB | **PASS** | PENDING | `pat_grid.jpg` |
| PAT3 | Pattern | Ramp | Gradient; MAD vs bars ≫ 0 | LAB | **PASS** | PENDING | `pat_ramp.jpg` |
| FBAR | Force bars | Yes on pattern≠Bars | Visual = **bars**, not grid/ramp; MAD force vs grid_off ≫ 0; bits O[9]=1 | LAB | **PASS** on RBF `820484a6` | PENDING | H-gate-fix1 `test_fbar_fast` EXIT=0: grid_off=7.0 force=82.9 bars=94.4 (Δforce_grid=75.9). Prior also PASS on `6db3a4d8`/`aa146c17`. Re-confirm after next RBF. |
| FBAR0 | Force bars | No on pattern=Bars | Frame store or dark ok if has_frame path; no hang | LAB | **PASS** (recorded) | PENDING | `force_bars_no.jpg` |
| TV0 | TV Mode | NTSC | bit2=0; stable HDMI lock; non-black | LAB | **PASS** | PENDING | `tv_ntsc.jpg` |
| TV1 | TV Mode | PAL | bit2=1; core survives; **restore NTSC after** (LCD may desync) | LAB+PROXY | **PASS** (bit + capture) | PENDING (true 50 Hz CRT) | `tv_pal.jpg`; capture may blank — SKIP ok if restored |
| FPS0 | Content FPS | 24 | bits[5:4]=00; non-black static OK | LAB | **PASS** | PENDING (3:2 eyes-on) | `fps_24.jpg`; motion needs content |
| FPS1 | Content FPS | 30 | bits=01 | LAB | **PASS** | PENDING | `fps_30.jpg` |
| FPS2 | Content FPS | 60 | bits=10 | LAB | **PASS** | PENDING | `fps_60.jpg` |
| FPS3 | Content FPS | 12 | bits=11; stepped motion with moving content | LAB | **PASS** (static) | PENDING | `fps_12.jpg` |
| AUD0/1 | Audio tone | On / Off | Status bit sticks; no hang (no mic required for LAB) | LAB | **PASS** | CRT: hear tone on analog | status path only in lab |
| T10/T11 | Flush FIFOs | pulse | No hang; status still readable | LAB | **PASS** | same | pulses only |
| T0/R0 | Reset | pulse | Bars recoverable after reseed | LAB | **PASS** | same | `after_reset.jpg` |
| AR0–3 | Aspect | Original/Full/ARC1/ARC2 | AR bits stick; visual weak on USB scaler | LAB (bits) | **PASS** bits | **CRT** eyes-on geometry | captures subtle on HDMI grab |

### Force bars deep check (required before any CRT claim)

| Step | Command / action | Pass | Scope |
|------|------------------|------|-------|
| 1 | `set_status --pattern grid --force-bars 0` → capture | Grid or blackish frame store | LAB |
| 2 | `set_status --pattern grid --force-bars 1` → capture | **Vertical bars**, not grid | LAB |
| 3 | `set_status --pattern bars --force-bars 1` → capture | Bars reference | LAB |
| 4 | Compare mean/MAD: force ≈ bars, force ≠ grid | Δforce_grid large (e.g. >20) | LAB |
| 5 | Optional: eyes-on CRT same sequence | Sync stable; bars fill usable raster | **CRT** |

Automated: `tests/hw/test_fbar_fast.sh` (no `load_core` if md5+CORENAME already match).

### NTSC / PAL procedure (safe)

1. Start NTSC + Force bars Yes + Pattern Bars (or Grid).
2. Capture `tv_ntsc` (LAB).
3. Switch PAL briefly; capture `tv_pal` if possible; **immediately restore NTSC**.
4. LAB pass: status bit toggles; core answers SPI; HDMI recovers after NTSC restore.
5. CRT pass (when available): stable raster at ~50 Hz family; no permanent roll; note blanking.

Do **not** leave lab HDMI parked on PAL for long soaks.

---

## 2. Content FPS / cadence (fixed display Hz)

Unit math is always LAB: `content_index = floor(display_index * content_fps / display_hz)`  
(`host/libmisterplex/cadence.hpp`, `make unit` / `test_cadence`).

| Content | OSD Content FPS | Display | Expected | Scope | Lab status |
|---------|-----------------|---------|----------|-------|------------|
| Film ~24p | 24 | 60 Hz HDMI/NTSC | 3:2 density unique advances | LAB motion / **CRT** eyes | Unit **PASS**; static OSD **PASS**; live film motion **PENDING** eyes-on |
| Video ~30p | 30 | 60 Hz | 2:2 | same | Unit **PASS**; OSD **PASS** |
| Progressive 60 | 60 | 60 Hz | 1:1 | same | Unit **PASS**; OSD **PASS** |
| Debug | 12 | 60 Hz | Sparse steps | LAB easy to see | OSD **PASS** |
| PAL film | 24 | 50 Hz CRT | Cadence on 50 Hz | **CRT** | N/A no CRT |
| Match-Hz 24 native | n/a | modeline ~24 Hz | No 3:2; true 24 | **CRT** + switchres **TODO** | conf logged only |

**LAB motion proxy (no CRT):** play local `test.mp4` or testsrc with `PRESENT=fb0|both`, set Content FPS 12 vs 60, confirm stepped vs smooth unique presents (eyes or multi-frame capture). Do not force FFmpeg to 60 unique RGB/s.

---

## 3. PRESENT / STREAM × display

| Conf | HDMI LCD (LAB) | 15 kHz CRT | Pass criteria |
|------|----------------|------------|---------------|
| `PRESENT=fb0` `STREAM=0` | **PASS** (Phase 2 cast path / soak family) | PENDING | Picture + sound; single FFmpeg |
| `PRESENT=both` `STREAM=0` | **PASS** / exercised | PENDING | fb0 primary; no regression |
| `PRESENT=both` `STREAM=1` | **PASS** soak (wifi) | PENDING | Daemon stays up; log recon path |
| `PRESENT=fpga` `STREAM=1` | PARTIAL (frame store / F1 path) | PENDING | Needs Video source = Frame store; image non-black |

Soak evidence (LAB, not CRT): **D-soak4** on RBF `820484a6`, `SOAK_HOLD_S=6 ROUNDS=2 SOAK_NET_LABEL=wifi` → **ok=6 fail=0** (`PRESENT=both`; no load_core). Prior D-soak3 same recipe on `aa146c17` also green. Eth comparison **BLOCKED** (no carrier).

**Related lab gates (not CRT columns):** DDR F1 **B-ddr5 PASS** on `820484a6` (push_frame --ddr ×5 mean≈18.0 ms, has_frame=1). **P3-WIDE FAIL** ~60.5% `PILLAR_320_of_529` (W-wide4/5/6; Fix-1 dead). **res_csum hard FAIL** on same RBF (live raw[13] ≠0x14; res_dc=-24 PASS). These do not invent CRT PASS and do not block LAB HDMI OSD rows.

---

## 4. Display hardware matrix — what lab can vs cannot claim

| Output | Available in current lab? | What LAB can verify | What needs physical CRT |
|--------|---------------------------|---------------------|-------------------------|
| **HDMI LCD 60 Hz** | **Yes** | All OSD rows, FBAR, soak, fb0 cast, audio path (HDMI), capture golden | — |
| **HDMI LCD 50 Hz / PAL** | Partial (OSD TV Mode) | Status bit + brief capture; recovery to NTSC | Stable 50 Hz panel lock if TV supports |
| **VGA → 15 kHz CRT** | **No** (not attached) | — | Sync, geometry, underscan, roll, 15 kHz modelines, analog audio |
| **VGA → 31 kHz / PC CRT** | **No** | — | 480p-class eyes-on |
| **Y/C / component** | **No** | — | Board-dependent path |
| **Full-width DE (HBlank@320)** | PROXY only — **LAB FAIL** ~60.5% pillar on `820484a6` | HDMI proxy shows content320/DE529 (not full-width) | **CRT/VGA eyes-on** still required for true geometry (P3-WIDE FAIL open) |

### CRT-only pass criteria (when hardware arrives)

| Check | Pass |
|-------|------|
| 15 kHz lock | Stable horizontal (no tear/roll) at NTSC 60 family modeline |
| Force bars | Full usable width; no severe cut-off (note overscan) |
| Content FPS 24 @ 60 | Film cadence without audio drop |
| PAL 50 | Optional; restore NTSC after |
| `PRESENT=fb0` cast | ascal/fb stable on CRT modeline |
| `PRESENT=fpga` | Frame store readable on CRT |
| Match-Hz | **SKIP** until switchres lands; use fixed Hz + Content FPS |

Record in sign-off table: date, CRT type, modeline/ini, PRESENT/STREAM, FPS, result, notes.

---

## 5. Operator procedures

### A. HDMI-only session (no CRT) — full LAB green path

1. Confirm MiSTer up; `CORENAME=Plex`; RBF md5 matches `fpga/Plex_MiSTer/releases/Plex.rbf`.
2. **No** Quartus; **no** `load_core` thrash — use `DEPLOY_LOAD=none` or skip deploy if md5+CORE match.
3. Run FBAR: `tests/hw/test_fbar_fast.sh` → EXIT 0.
4. Optional full OSD matrix: `MENU_RELOAD=0 tests/hw/run_menu_matrix.sh` (settles fast; parks on bars/NTSC).
5. Soak: `SOAK_HOLD_S=6 SOAK_ROUNDS=2 SOAK_NET_LABEL=wifi ./tests/hw/test_soak.sh`.
6. Unit cadence: `make unit` (includes `test_cadence`).
7. Fill §6 LAB column only. Leave CRT columns `N/A (no CRT)` or `PENDING`.

### B. CRT session (when available)

1. Same seed as LAB: NTSC, Force bars Yes, Pattern Bars.
2. MiSTer video menu / ini: 15 kHz-friendly modeline (320×240-class; long VBlank).
3. Walk §1 FBAR + patterns + FPS with eyes-on; do not rely on USB capture alone.
4. Play one library (or local) title at Content FPS 24 and 30.
5. Optional brief PAL; restore NTSC.
6. Fill CRT columns + sign-off in [crt-lcd-matrix.md](crt-lcd-matrix.md).

### C. Forbidden / avoid

- Do not mark CRT rows PASS from HDMI-only evidence.
- Do not thrash `load_core` for matrix retests.
- Do not leave Force bars No + empty frame store as “failure” without checking has_frame.
- Do not start Quartus for this checklist.

---

## 6. Session rollup (2026-07-24)

| Area | LAB (HDMI) | Physical CRT 15 kHz |
|------|------------|---------------------|
| Pattern modes | **PASS** (menu captures) | **PENDING** (no CRT) |
| Force bars visual | **PASS** RBF **`dabdaeb0`** (H-deploy-rcsum1) | **PENDING** |
| NTSC / PAL OSD | **PASS** (PAL restored) | **PENDING** (true 50 Hz CRT) |
| Content FPS bits 12/24/30/60 | **PASS** | **PENDING** eyes-on |
| Cadence unit tests | **PASS** (`test_cadence` / `make unit`; CRT2 recheck) | N/A |
| DDR F1 push path | **PASS** B-ddr5 mean≈18.0 ms on `820484a6` | **PENDING** (frame store on CRT) |
| PRESENT=both soak wifi | **PASS** D-soak4 ok=6 fail=0 on `820484a6` | **PENDING** |
| PRESENT=fb0 cast | **PASS** (prior Phase 2 / soak family) | **PENDING** |
| Live film 3:2 eyes-on | PENDING (optional lab) | **PENDING** |
| Full-width DE / P3-WIDE | **FAIL** ~60.5% pillar (W-wide4/5/6) — PROXY not green | **PENDING — needs CRT/VGA** (lab already FAIL open) |
| Residual hard res_csum | **FAIL** on `dabdaeb0` (raw[13] unstable ≠0x14; res_dc=-24 OK; soft-skip ≠ PASS) | N/A (decode gate, not CRT) |
| 15 kHz geometry / sync | N/A (cannot claim from HDMI) | **PENDING — needs hardware** |
| Overscan / usable raster | N/A (HDMI ≠ proof) | **PENDING — needs hardware** |
| Match-source-Hz modeline | TODO (docs only) | TODO |
| Eth vs wifi | wifi only; eth **BLOCKED** | — |

| Agent | What changed | Quartus / load_core |
|-------|--------------|---------------------|
| CRT | Wrote checklist + matrix pointer + session sheet | **none** |
| CRT2 | Practical tick matrix (LAB vs CRT), honesty rules, rollup dual columns, backlog note | **none** |
| CRT3 | Polished LAB ticks from evidence on **`820484a6`**: FBAR PASS, soak PASS, DDR PASS, cadence PASS; WIDE FAIL + res_csum hard FAIL noted; physical CRT still **PENDING** | **none** |

**P5-CRT verdict:** checklist **exists, dual-scoped (LAB vs CRT), and actionable without CRT**. LAB HDMI rows green on current RBF **`820484a6`** where evidence exists. Physical CRT 15 kHz rows remain **PENDING** (no CRT attached) — **no false PASS**. Overall item status → **PARTIAL** (not DONE).

---

## 7. Quick command cheat sheet

```bash
# Host
export MISTER_HOST=<mister-ip> MISTER_PASS=REDACTED

# Status / modes (on MiSTer)
/media/fat/misterplex/bin/set_status --raw
/media/fat/misterplex/bin/set_status --pattern grid --force-bars 1 --tv ntsc --fps 24 --raw

# FBAR smoke (host)
./tests/hw/test_fbar_fast.sh

# Full menu matrix without reload if already good
MENU_RELOAD=0 ./tests/hw/run_menu_matrix.sh

# Soak (no core reload)
SOAK_HOLD_S=6 SOAK_ROUNDS=2 SOAK_NET_LABEL=wifi ./tests/hw/test_soak.sh

# Capture recipe (host)
ffmpeg -y -f v4l2 -input_format mjpeg -video_size 800x600 -framerate 30 \
  -i /dev/video4 -frames:v 8 /tmp/grab_%02d.jpg
# Prefer brightest frame; reject mean_luma < 20
```
