# P3-WIDE RCA — HBlank@320 / DE / HDMI pillar

**Status:** formal WIDE gate **OPEN** (do **not** mark P3-WIDE **DONE** until hardened re-gate). Soft-skip ≠ PASS. FBAR soft ≠ WIDE PASS.  
**RCA:** **UPDATED** by **W-wide-rca-fix2** live remeasure (~14:08+ CDT).  
**Fix-1 silicon CLOSED (ineffective — not PASS):** state B (`aa146c17`) / state C (`820484a6`) still pillar ~60.5%. **No more HSync-only thrash.**  
**Fix-2:** Q-fix2 RBF **`ec21e133`** SRC **`f1d9666a`** map-absorbed (`lpm_divide` Div0). Early gate **W-wide-gate-fix2 / fix2b** captured **0.605** on `wq2_*` (~13:56). **Same RBF, fresh warm multi-frame remeasure** shows **frac≈0.998**, edges **~114 px (800/7)**, **7 bars + R/B**, R5≈75–79 — Fix-2 geometry **now observed**. Gate FAIL root class: **UVC/settle false pillar (c)**, not “paint missing from bitstream.” **Do not invent formal WIDE PASS** without hardened re-gate worker stamp.  
**Next:** **Fix-3-G gate harden / re-gate first** (no Quartus). Fix-3-C/P RTL **contingent** only if hardened re-gate reverts to 0.605. **`FIT_GO Q-fix3=NO`** until parent + exclusive free.  
**Reports:** `W-wide-gate-fix2*.txt` (early FAIL captures), **`W-wide-rca-fix2.txt` (RCA_OK + remeasure)**, `W-fix2-d3.txt`, `W-fix3-*.txt`  
**Lab RBF (current):** **`ec21e133`** — FBAR soft PASS; early WIDE FAIL artifacts on disk; **live remeasure PASS-class geometry** (not formal DONE).

## Symptom

HDMI UVC 800×600 captures with `force_bars=1` / Pattern=Bars show active content only on the **left ~60.4%** of the frame (x≈2–484), solid black on the right.

That ratio is **exactly** \(320/529 \approx 0.6049\): content painted for `hc < 320` inside a **Template DE of 529**.

## Multi-agent WIDE FAIL confirmation (locked)

| Worker | Lab RBF | Claim | span | verdict |
|--------|---------|-------|-----:|---------|
| W-wide / W-wide2 | `6db3a4d8` | Template A class | ~60.5% | `PILLAR_320_of_529` |
| W-wide3 / W-wide3b | `aa146c17` | Fix-1 state B (HBlank@320, HSync 544/590) | ~60.5% | `PILLAR_320_of_529` |
| **W-wide4** | **`820484a6`** | Fix-1 state C (HBlank@320, HSync 336/384, level HBlank) | **60.5%** | `PILLAR_320_of_529` |
| **W-wide5** | **`820484a6`** | same (independent re-eye) | **60.5%** | `PILLAR_320_of_529` |
| **W-wide6** | **`820484a6`** | same (third reconfirm) | **60.5%** | `PILLAR_320_of_529` |
| W-wide7 / W-wide7b | `dabdaeb0` | residual-era / pre-Fix-2 | ~60.5% | `PILLAR_320_of_529` |
| **W-wide-gate-fix2** | **`ec21e133`** | Fix-2 paint-full-DE claim `f1d9666a` | **60.5%** | `PILLAR_320_of_529` |
| **H-gate-fix2** | **`ec21e133`** | same (companion WIDTH) | **60.5%** | `PILLAR_320_of_529` |
| **W-wide-gate-fix2b** | **`ec21e133`** | independent reconfirm (no reload) | **60.5%** | `PILLAR_320_of_529` |

FBAR path is green on the same RBFs (bars force path OK). Failure class is **width/geometry**, not unlock/black and not force-bars mux.

**Fix-1 experiment closed as ineffective on silicon eyes-on.**  
**Fix-2 experiment closed as ineffective on silicon eyes-on** (map-absorbed, eyes still pillar). Prefer **Fix-3**.

## Timing (what the RTL is supposed to do)

| Parameter | Value | Role |
|-----------|------:|------|
| `H_CONTENT` | 320 | Product / frame_store width (stays 320 under Fix-2) |
| `H_LAST` | 637 | Line total 638 clocks |
| `H_BLANK_S` (Template A / pre-fix / **observed silicon**) | 529 | Template DE width |
| `H_BLANK_S` (state B / C source claim) | 320 | DE = content — **not observed** as full-width HDMI |
| `H_SYNC_S` .. `H_SYNC_E` (Template A) | 544 .. 590 | Proven FBAR-green class |
| `H_SYNC_S` .. `H_SYNC_E` (state C / `820484a6` source) | 336 .. 384 | Short FP after DE@320 — **WIDE still pillar** |

`VGA_DE = ~(HBlank | VBlank)` (`Plex.sv` ~L365). ascal measures DE and scales to HDMI. Default AR 4:3 matches 320×240 product content.

Bars (pre–Fix-2): `bar = px[8:6]` → 64-px slices; only bars 0–4 (W/Y/C/G/M) fit in 0..319. Red/blue never enter paint window.

## Why the pillar remains

Lab bar **edges** land at capture x ≈ \(hc \times 800/529\) for `hc ∈ {0,64,128,192,256,320}` → ends at **x≈484**, then black. That is black **inside DE**, not an ascal letterbox outside content.

| If bitstream had… | Expected HDMI | Lab |
|-------------------|---------------|-----|
| HBlank@529 + paint `hc<320` + `bar=px[8:6]` | ~60.4% span, 5 bars + black right, edges ~97 cap-px | **Observed** on `6db3a4d8`…`820484a6`, `dabdaeb0`, **and Fix-2 `ec21e133`** |
| HBlank@320 + paint fills DE | ≥~95% span, bars across full width | **Never observed** (Fix-1 B/C) |
| HBlank@529 + paint fills DE 0..528 + bar×7/529 | ≥~95% span, 7 bars, edges ~114 cap-px | **Fix-2 target — map-absorbed, NOT observed on `ec21e133`** |

So either:

1. **H1 reinforced (dominant):** silicon still **behaves as HBlank@529 + paint hc<320** despite source claims of HBlank@320 (states B and C). Exact 320/529 fingerprint across three RBFs.  
2. **H2 secondary:** HBlank@320 live but something else recreates the same geometry — still fails the full-width gate; Fix-2 does not depend on resolving H1 vs H2 because it paints the Template-wide DE class that eyes already measure.

`present_core` only forwards colorbars blanking; `mycore.v` (still HBlank@529) is not on the present path.

## Closed experiments

### State B — Q-3l1 / `aa146c17`

| Step | Result |
|-----:|--------|
| BUILD_OK + deploy | md5 **aa146c17…** |
| FBAR | PASS |
| Residual hard | res_dc=-24 PASS; **res_csum FAIL** (separate) |
| WIDE (W-wide3/3b) | **FAIL** frac≈0.605 `PILLAR_320_of_529` |

HBlank@320 + Template-late HSync@544: **ineffective** on silicon eyes-on.

### State C / Fix-1 — Q-fix1 / `820484a6`  ★ CLOSED FAIL (ineffective — not PASS)

Source claim (absorbed into Q-fix1; **current tree `colorbars.sv` still matches**):

```text
H_BLANK_S = H_CONTENT          // 320
H_SYNC_S  = 336, H_SYNC_E = 384
HBlank   <= (hc >= H_BLANK_S)  // level
```

| Worker | Result |
|--------|--------|
| W-wide4 | FAIL span=60.5% R5%=0 |
| W-wide5 | FAIL span=60.5% R5%=0 |
| W-wide6 | FAIL span=60.5% R5%=0 |

Indistinguishable from Template A fingerprint. **Do not redeploy `820484a6` expecting WIDE green.** **Do not thrash more HSync-only variants.** **Fix-1 is closed as ineffective (experiment FAIL), not as WIDE PASS.**

## Fix-2 design (LOCKED — W-fix2-d3) — paint-full-DE@529 — ★ SILICON CLOSED FAIL

**Silicon status:** Q-fix2 **BUILD_OK** RBF **`ec21e133`**, SRC **`f1d9666a`**, Full Compilation, map shows `colorbars` changed + `lpm_divide:Div0` at L124. Deploy + FBAR soft PASS. **W-wide-gate-fix2 / H-gate-fix2: WIDE FAIL span=0.605** — same class as pre-Fix-2. Bar edges still **~97 cap-px** (64-px hc class), **not** ~114 (7-bar class). R/B never enter. **Fix-2 CLOSED as ineffective on eyes-on (not PASS).** Do not redeploy `ec21e133` expecting WIDE green.

**Intent (historical):** Keep **Template A timing** (FBAR-proven DE-class) and **paint the entire DE** so force-bars HDMI span ≥95%. Product `frame_store` stays 320×240 left-aligned until a later width ticket.

### Timing constants (Template A / mycore-class)

| Symbol | Value | Notes |
|--------|------:|-------|
| `H_CONTENT` | `10'd320` | Product / frame_store only; **not** DE width under Fix-2 |
| `H_DE` / `H_BLANK_S` | `10'd529` | Active DE width; blank starts at hc≥529 |
| `H_LAST` | `10'd637` | 638 clocks/line (leave alone; FPS) |
| `H_SYNC_S` | `10'd544` | Template |
| `H_SYNC_E` | `10'd590` | Pulse width 46 clocks (Template) |
| HBlank style | edge **or** level | Prefer level `HBlank <= (hc >= 529)` (clear); edge Template OK |

VBlank/VSync remain keyed off HSync start as today (NTSC/PAL scandouble tables unchanged).

### Paint bounds

| Signal | Pre–Fix-2 (pillar) | Fix-2 |
|--------|-------------------|-------|
| `in_content` (colorbar paint) | `hc < H_CONTENT` (320) | `hc < H_DE` (**529**) i.e. hc ∈ **0..528** |
| Vertical | `py < 240` | unchanged |
| Blank gate | `~HBlank && ~VBlank` | unchanged |
| `bar` (pattern 0/1) | `px[8:6]` (64-px; 5 bars) | **stretch 7 bars across DE** |

**Bar stretch (concrete):**

```text
// Integer scale: 7 equal slices over DE width 529
// bar ∈ {0..6} for hc ∈ {0..528}
wire [12:0] bar_prod = hc * 4'd7;     // 0 .. 3696  (< 8192)
wire [2:0]  bar      = bar_prod / H_DE; // 0..6 (hc=528 → 3696/529=6)
```

Expected slice starts (floor):  
hc boundaries ≈ 0, 76, 151, 227, 302, 378, 453, 529  
→ colors W / Y / C / G / M / **R** / **B** all enter the paint window (lab today stops at magenta).

Threshold form (no divider, optional implementer choice):

```text
// approx equal ~75–76 px slices (same visual intent)
// 0:[0..75] 1:[76..151] 2:[152..226] 3:[227..302]
// 4:[303..377] 5:[378..453] 6:[454..528]
```

Grid (`px[3]^py[3]`) and ramp (`px[7:0]`) automatically fill DE once `in_content` uses hc&lt;529.

Moving block (pattern 1): leave position math on `content_index`; still valid inside expanded paint window.

### Concrete RTL touch points (implementer checklist)

**Source of truth for line numbers:** tree at design lock time (Fix-1 state C still on disk while R-csum1 fits residual). Lines may shift ± a few after other commits — match by **symbol/comment**, not only line #.

#### 1. PRIMARY — `fpga/Plex_MiSTer/rtl/colorbars.sv`

| Region (approx L#) | Current (state C / Fix-1) | Fix-2 action |
|--------------------|---------------------------|--------------|
| **L1–6** header | Claims “Do NOT use Template HBlank@529 with only 320px paint” | Rewrite: Fix-2 keeps HBlank@529 **and paints full DE**; note Fix-1 `820484a6` WIDE FAIL closed |
| **L29–39** localparams | `H_CONTENT=320`; `H_FP=16`; `H_SYNC=48`; `H_BLANK_S=H_CONTENT` (320); `H_SYNC_S=336`; `H_SYNC_E=384` | Replace with Template: `H_CONTENT=320` **keep**; add `H_DE=529`; `H_BLANK_S=H_DE`; `H_SYNC_S=544`; `H_SYNC_E=590`; drop/leave-unused `H_FP`/`H_SYNC` derived path |
| **L33** `H_LAST` | `10'd637` | **Leave alone** (FPS / 638 clocks) |
| **L79–87** HBlank | comment “Level HBlank so DE never stretches past H_CONTENT”; `HBlank <= (hc >= H_BLANK_S)` | Keep level form; now `H_BLANK_S=529`. Update comment → DE width 529 |
| **L89–92** HSync edges | fire at `H_SYNC_S`/`H_SYNC_E` | Unchanged pattern; constants become 544/590 |
| **L94–117** VBlank/VSync | keyed off `hc == H_SYNC_S` | **Leave tables alone**; auto-follow new H_SYNC_S=544 |
| **L121–127** paint | `bar = px[8:6]`; `in_content = (hc < H_CONTENT) && …` | `bar = (hc * 7) / H_DE`; `in_content = (hc < H_DE) && (py < 240) && ~HBlank && ~VBlank` |
| **L129–142** `case(bar)` colors | 0..6 W/Y/C/G/M/R/B | **Unchanged** (R/B become reachable) |
| **L144–148** moving block | `bx`/`by` from `content_index` | **Leave** |
| **L150** grid | `px[3]^py[3]` | **Leave** (auto-fills DE once paint expands) |
| **L152–178** pattern mux | uses `in_content` | **Leave body**; inherits expanded window |

**Diff-intent sketch (paste reference only — do not apply mid-fit):**

```systemverilog
// Fix-2: Template HBlank@529 + HSync 544/590 (FBAR-proven) with paint
// across full DE (hc 0..528). Fix-1 HBlank@320 / HSync 336/384 was
// ineffective on silicon eyes-on (still PILLAR_320_of_529 on 820484a6).
// frame_store remains 320; present_core active window stays hc<320.

localparam H_CONTENT = 10'd320;  // product / frame_store only
localparam H_DE      = 10'd529;  // Template active DE width
localparam H_LAST    = 10'd637;  // 638 clocks/line — UNCHANGED
localparam H_BLANK_S = H_DE;     // 529
localparam H_SYNC_S  = 10'd544;  // Template
localparam H_SYNC_E  = 10'd590;  // Template

// blanking block (level preferred):
HBlank <= (hc >= H_BLANK_S);

// paint:
wire [9:0] px = hc;
wire [9:0] py = scandouble ? (vc >> 1) : vc;
wire [12:0] bar_prod = hc * 4'd7;
wire [2:0]  bar      = bar_prod / H_DE;
wire in_content = (hc < H_DE) && (py < 10'd240) && ~HBlank && ~VBlank;
// case(bar) / pattern mux / block / grid / ramp — unchanged
```

#### 2. COMMENTS ONLY — `fpga/Plex_MiSTer/rtl/present_core.sv`

| Region (approx L#) | Current | Fix-2 action |
|--------------------|---------|--------------|
| **L90–108** hc/vc reconstruct | matches colorbars `H_LAST=637` | **No functional change** |
| **L110–111** `active` | `hc < 10'd320` | **KEEP** — frame_store read window stays 320 left-aligned |
| **L120–123** `frame_store` WIDTH/HEIGHT | 320 / 240 | **KEEP** |
| **L146–150** `force_bars` / RGB mux | O[9] / pattern force | **DO NOT TOUCH** |
| **L152–157** blanking assign | pass-through colorbars | **KEEP** |

Optional comment near L110:

```text
// colorbars paint window is H_DE (529) under Fix-2; store remains 320 left-aligned.
// WIDE gate is measured under force_bars=1 (colorbar path fills DE).
```

#### 3. DO NOT TOUCH (this ticket)

| File / region | Why |
|---------------|-----|
| `fpga/Plex_MiSTer/Plex.sv` (O[9] mux ~L330–335, `VGA_DE` ~L365) | **R-csum1 owns residual mid-fit**; O[9]/DE out of WIDE scope |
| `fpga/Plex_MiSTer/rtl/mycore.v` (HBlank@529 leftover) | Not on present path |
| `frame_store.sv` WIDTH | Later product-width ticket |
| `sys/ascal` / AR / video_freak | Not root cause; FBAR green |
| Any mid-fit write under `fpga/Plex_MiSTer/` while quartus_fit LIVE | Race residual rebuild |

### FBAR / lock risk

| Change | Risk |
|--------|------|
| Fix-2 paint-full-DE @ HBlank@529 + HSync 544/590 | **Low** — timing matches FBAR-green Template class |
| Further HSync-only variants after Fix-1 FAIL | **Avoid** — wasted; lock risk without geometry win |
| O[9] force_bars mux edits | **High** — out of scope |
| Expanding frame_store to 529 | **Out of scope** — later width ticket |

Always: one deploy → **FBAR green** → WIDE span check → park bars force=1 NTSC 60.

### Acceptance (WIDE) — do not invent PASS

PASS only if **all** of:

- `force_bars=1` bars NTSC 60  
- mid-band active span **frac ≥ 0.95** of capture width  
- R5% mid-band luma **> 15**  
- verdict **≠** `PILLAR_320_of_529`  
- FBAR still EXIT=0 on the **same** RBF  

FAIL if still ~60.5% (paint not absorbed / wrong RBF) or unlock/black (unexpected for Fix-2 timing).

Capture method (W-wide3): MacroSilicon UVC `/dev/video4`; warm frames (discard cold mean≈7 stubs); MJPG 800×600 + YUYV 800×600 + MJPG 1920×1080 cross-check.

Expected PASS signature under Fix-2:

- span ≳ 95% (full DE painted; ascal scales full DE)  
- **7** SMPTE-style bars visible (R and B enter window)  
- R5% ≫ 15 (right edge is bar, not black pillar)  
- bar edges every ~114 capture-px (800/7), not every ~97 (800×64/529)

Diagnostic bonus (not hard gate): all 7 bars + red/blue enter paint → proves paint past hc=320 on 529-class DE.

**Post-Fix-2 (corrected):** early gate on `ec21e133` logged `PILLAR_320_of_529` (`wq2_*`). Live remeasure (W-wide-rca-fix2) on **same RBF** shows Fix-2 full-DE 7-bar. Prefer **hardened re-gate** before any Fix-3 RTL. Not Fix-1 thrash.

### Sequencing (orchestrator) — Fix-2 path + correction

1. ~~R-csum1 BUILD_OK~~ done.  
2. ~~Fix-2 RTL + Q-fix2 BUILD_OK `ec21e133`~~ done.  
3. ~~Deploy + FBAR~~ done soft PASS.  
4. Early WIDE eyes-on **FAIL** 0.605 (`wq2_*` @ ~13:56) — artifact retained.  
5. **W-wide-rca-fix2 remeasure** — Fix-2 geometry **live** (frac 0.998, 7 bars).  
6. **Next:** Fix-3-G hardened formal re-gate → if PASS, DONE; if FAIL 0.605, then Fix-3-C/P + FIT_GO.

**While residual sole fit LIVE:** **zero** mid-fit RTL thrash under `fpga/Plex_MiSTer/`. Docs-only OK. **FIT_GO_WIDE / Q-fix3 = NO** until parent authorizes after exclusive free.

---

## Fix-2 silicon FAIL analysis (ec21e133 / W-wide-gate-fix2)

| Evidence | Value |
|----------|------:|
| Lab RBF | `ec21e1330ddd75ad7f39099e5abfad49` |
| SRC_colorbars claim / live | `f1d9666ada5347dbde7e7246bad345c8` |
| Map absorption | **YES** — `Info (293027) colorbars.sv has changed`; elaborate `colorbars:bars`; **`lpm_divide:Div0`** L124 (81 LEs combo) |
| FBAR soft | **PASS** 7.0 / 82.9 / 94.4 EXIT=0 |
| WIDE span | **0.605** (x≈2..485 on 800) |
| R5% | **0.0** |
| Bar edges (capture) | ≈ {2, 98, 195, 292, 389, 485} → **800×64/529** class |
| Expected Fix-2 edges | ≈ {0, 113, 228, 342, 457, 570, 685, 800} → **800/7** class |
| Bars visible | 5 (W/Y/C/G/M); **R/B absent** |
| vs prior dabdaeb0 / 820484a6 | **SAME FAIL class** |

### W-wide-rca-SF2 independent re-measure (captures only, no redeploy)

Re-analyzed `captures/menu/wq2_{mjpg,yuyv,1080}_best.jpg` (lab RBF **ec21e133**, force_bars=1):

| Capture | live x | frac | peaks (cap-px) | peak spacing | R5% |
|---------|--------|-----:|----------------|-------------:|----:|
| wq2_mjpg_best 800×600 | 2..485 | **0.605** | 97,195,291,387,484 | **96.75** | **0.0** |
| wq2_yuyv_best 800×600 | 2..485 | **0.605** | same | 96.75 | **0.0** |
| wq2_1080_best 1920×1080 | 3..1166 | **0.606** | 234,463,699,929,1162 | ~232 (×2.4) | **0.0** |

- Expected **OLD** edges `hc×800/529` for `{0,64,128,192,256,320}` = **{0,97,194,290,387,484}** — **exact match**.  
- Expected **Fix-2** edges `{0,76,151,227,302,378,453,529}×800/529` = **{0,115,228,343,457,572,685,800}** — **not observed**.  
- Segment RGB order: white → yellow → cyan → green → magenta → **solid black** (no red/blue).  
- Left-aligned black (not centered) rules out AR letterbox as primary.

### force_bars path trace (is Fix-2 paint on the HDMI path UVC sees?)

| Stage | Behavior under force_bars=1 | Conclusion |
|-------|----------------------------|------------|
| `Plex.sv` O[9] | `use_frame_store=status[9]`; pattern forced 0 | force live |
| `present_core` | `force_bars = use_frame_store \| (pattern!=0)` → **1** | |
| `use_ext` | `has_frame && !force_bars` → **0** | **not** frame_store |
| RGB mux (pre–Fix-3) | `r/g/b = br/bg/bb` (colorbars) | colorbars RGB **is** HDMI RGB |
| `active` (hc\<320) | only `frame_store.rd_active` | **does not** gate force_bars RGB |
| Blanking | pass-through colorbars HBlank@529 | DE class 529 |
| `Plex.sv` | `VGA_DE=~(HBlank\|VBlank)`; `VGA_R/G/B=r/g/b` | direct |
| FBAR soft green | same force path unlocks bars | mux alive |

**Verdict:** Fix-2 colorbars paint **is** on the HDMI path UVC measures. Pillar is therefore **functional paint identity ≠ Fix-2 intent** (eyes-on SoT), not “wrong module / wrong mux.” `present_core.active@320` is **not** the force_bars crop. ascal crop **secondary** (left-aligned exact 320/529 + 64-px bar fingerprint is source-side).

### Interpretation (ranked) — W-wide-rca-SF2

1. **H_func_old ★ (dominant):** HDMI RGB still matches **pre-Fix-2** paint identity (`bar=px[8:6]`, paint ends hc≈320) even though map shows Fix-2 netlist pieces (`colorbars:bars|lpm_divide:Div0` 81 LEs). Gate treats **eyes-on as SoT** — not map alone. Even if bar-div failed, `in_content=hc<H_DE` should non-black right DE; clean 320 cutoff + 64-px edges = full old identity.  
2. **H_synth_paint_mismatch (tied to ★):** map proves divider elaborated in colorbars; eyes prove paint window still 320. Prefer Fix-3-P dual identity over re-thrashing colorbars-only math.  
3. **H_path_split (weak / closed for mux):** force_bars **does** select colorbars RGB (trace above + FBAR + SMPTE palette). Residual doubt is not “wrong mux” but “wrong functional paint inside colorbars silicon.”  
4. **H_downstream ascal/sys (secondary):** DE is 529-class; black is **inside** DE. Escalate only if Fix-3-P still pillars with **identical** 64-px fingerprint after proven absorption.  
5. **H_metric_false (rejected by SF2 on `wq2_*` only):** early triple-format gate agreed on OLD fingerprint.

**Do not:** Fix-1 HSync thrash; residual RBF thrash; invent WIDE PASS from BUILD_OK+DEPLOY+FBAR alone; start Q-fix3 while residual exclusive LIVE without parent GO.

---

## W-wide-rca-fix2 live remeasure correction (ec21e133, ~14:08+ CDT)

**Worker:** W-wide-rca-fix2. **No redeploy.** Lab md5 still `ec21e133…`. Measure-only `set_status` + host UVC `/dev/video4`.

| Evidence set | Result |
|--------------|--------|
| Historical `wq2_{mjpg,yuyv,1080}_best.jpg` | Still **frac 0.605** OLD 64-px / 5 bars (gate files unchanged on disk) |
| Fresh warm MJPG **n=29** | **All** frac **0.998** live 2..799 R5≈79; edges {3,116,230,345,459,573,687} |
| Fresh warm YUYV **n=8** | **All** frac **0.998**; same Fix-2 edge class; R+B present |
| Grid / ramp diags | frac **0.998** full DE (proves `in_content` H_DE in silicon) |
| Cold stubs | mean≈7 still exist — discard required |

**Ranked root cause of early gate FAIL (corrected):**

1. **(c) UVC / settle / stale-frame false pillar — DOMINANT** — same RBF; early `wq2_*` = 0.605; post-settle warm stream = Fix-2 PASS-class.  
2. **(a) paint not in bitstream — REJECTED** — map Div0 + live 7-bar + grid full DE.  
3. **(b) ascal crop 320 — REJECTED** — full span after settle.  
4. **(d) HBlank@320 — REJECTED** — DE remains 529-class with full paint.  
5. **(e) divider timing hygiene** — STA −17 ns; optional Fix-3-C threshold rewrite only.

**Formal WIDE still OPEN** until a hardened re-gate worker stamps PASS (this RCA does **not** DONE-flip). Captures: `captures/menu/wq2_{grid_diag,ramp_diag,bars_warm,rca_bars_best}.jpg`. Report: `/tmp/misterplex-agent-W-wide-rca-fix2.txt`.

---

## Fix-3 direction (corrected priority)

| Rank | ID | Action | Quartus? |
|-----:|----|--------|----------|
| **1 ★** | **Fix-3-G** | Harden eyes-on: ≥20 warm stable frames; edge-class 800/7; R+B check; optional pattern-toggle UVC flush; then formal re-gate on `ec21e133` | **NO** |
| 2 | Fix-3-C | colorbars threshold bars (no `lpm_divide`); drop `~HBlank` from `in_content` | only if re-gate fails |
| 3 | Fix-3-P | present_core local DE paint dual path | last resort if C fails with identical OLD fingerprint |

**FIT_GO Q-fix3 = NO** until parent authorizes and exclusive free. Prefer **no RTL** if Fix-3-G re-gate PASSes.

---

## Fix-3 HOLD (this tick — W-fix3-hold2)

| Item | Status |
|------|--------|
| **FIT_GO_WIDE** | **NO** |
| **FIT_GO Q-fix3** | **NO** |
| Exclusive | **R-csum6 LIVE** lock `R-csum6 LIVE 2026-07-24T14:02:42-05:00` (`/tmp/plex_quartus.lock`) — `quartus_fit` in flight |
| Design freeze | **Fix-3-P LOCKED / READY** (docs only; RTL **not** applied) |
| P3-WIDE | **OPEN** — early gate 0.605 on `wq2_*`; live remeasure PASS-class on same **`ec21e133`** (not formal DONE) |
| WIDE PASS invented? | **NO** (needs hardened re-gate stamp) |
| RTL under `fpga/Plex_MiSTer/` | **FORBIDDEN this tick** — present_core / colorbars owned **after** residual free only |
| Mid-fit thrash residual + wide | **FORBIDDEN** |

**Cite (locked evidence — not invented):**

| Source | Fact |
|--------|------|
| **W-wide-rca-fix2** | **FIX2_INEFFECTIVE** — root class paint still content320-in-DE529; map Div0 absorbed; eyes still pillar |
| **W-fix3-plan** | Preferred **Fix-3-P** dual-path `present_core` DE paint + colorbars threshold hygiene; **FIT_GO Q-fix3=NO** while exclusive LIVE |
| **W-wide-gate-fix2b** | **WIDTH FAIL** span=**0.605** x≈2..485 R5%=0 FBAR soft 7.0/82.9/94.4 — **≠ WIDE PASS** |
| Exclusive **R-csum6** | LIVE mid-fit — second exclusive / Q-fix3 / wide RTL **FORBIDDEN** |

**After residual sole frees (parent serial — one sole only):** harvest residual gate **or** set **FIT_GO Q-fix3=YES** and apply Fix-3-P under Q-fix3. Never concurrent. FIT_GO ≠ BUILD_OK ≠ WIDE PASS.

Reports: `/tmp/misterplex-agent-W-fix3-hold2.txt` (HOLD_OK), `W-fix3-hold.txt`, `W-fix3-plan.txt`, `W-wide-rca-fix2.txt`, `W-wide-gate-fix2b.txt`.

---

## Fix-3 design (LOCKED — W-wide-rca-fix2b / Fix-3-P) — dual-path force_bars DE paint

**Status:** design **LOCKED / READY (frozen)**. RTL **not applied** (docs-first). **`FIT_GO Q-fix3 = NO` while R-csum6 exclusive LIVE** — design READY ≠ auto-start. Kick only when exclusive free **and** parent authorizes sole **Q-fix3**. Do **not** mark P3-WIDE DONE. Full report: `/tmp/misterplex-agent-W-wide-rca-fix2b.txt` + plan `/tmp/misterplex-agent-W-fix3-plan.txt`.

**Intent:** Prove and deliver full-DE paint under `force_bars=1` by a path that is **independent of the Fix-2 colorbars-only change**, while cleaning colorbars to remove `lpm_divide` and gate paint without registered-HBlank skew. Template A timing stays (FBAR-proven). `frame_store` stays 320 left-aligned.

### Candidates ranked

| Rank | ID | Summary | Pros | Cons / risk |
|-----:|----|---------|------|-------------|
| **1 ★** | **Fix-3-P** | **`present_core` force_bars local DE paint** + colorbars threshold cleanup | Dual identity; if still pillar → root is downstream of present; if green → WIDE PASS without ascal thrash | Touches `present_core.sv` (wide-safe; not residual `Plex.sv`) |
| 2 | Fix-3-D | colorbars-only **solid red full DE** diagnostic | Instant absorption test (red right half vs black) | Not product 7-bar PASS alone; needs follow-up paint |
| 3 | Fix-3-C | colorbars-only threshold 7-bar (no divider) + drop `~HBlank` from `in_content` | Smallest RTL surface | May **repeat Fix-2 FAIL** if root is not colorbars math |
| 4 | Fix-3-A | ascal / AR Full Screen / `VGA_SCALER` | Explores scaler class | Weak: bar fingerprint is source-side 64-px; FBAR already green |
| 5 | Fix-3-H | HBlank@320 product retry | — | **CLOSED** Fix-1 ineffective; do not thrash |

### ★ Preferred: Fix-3-P (lock)

#### A. PRIMARY — `fpga/Plex_MiSTer/rtl/present_core.sv`

Under `force_bars`, **do not** take RGB from colorbars `br/bg/bb`. Paint from **local reconstructed `hc`/`vc`** across full Template DE.

| Region (symbol) | Current | Fix-3-P action |
|-----------------|---------|----------------|
| Header / L90–108 hc reconstruct | matches H_LAST=637 | **KEEP** functional |
| L110–111 `active` | `hc < 10'd320` | **KEEP** — frame_store read window only |
| L120–123 `frame_store` 320×240 | product size | **KEEP** |
| L146–147 `force_bars` / `use_ext` | as today | **KEEP** definitions |
| **L148–150 RGB mux** | `r = use_ext ? fr : br` | **CHANGE** — see sketch |
| L152–157 blanking | pass-through colorbars | **KEEP** (Template DE from colorbars HBlank@529) |

**Diff-intent sketch (implementer reference — apply only under Q-fix3 sole, exclusive free):**

```systemverilog
// Fix-3-P: force_bars paints FULL Template DE from local hc (0..528).
// Fix-2 colorbars-only paint was map-absorbed on ec21e133 but eyes-on still
// PILLAR_320_of_529 with 64-px bar fingerprint. Local path is the WIDE SoT
// under force_bars; frame_store remains 320 left-aligned when !force_bars.

localparam WIDE_DE = 10'd529;

// 7-bar thresholds (~75–76 px); NO divider / no lpm_divide
wire [2:0] wide_bar =
	(hc < 10'd76)  ? 3'd0 :
	(hc < 10'd151) ? 3'd1 :
	(hc < 10'd227) ? 3'd2 :
	(hc < 10'd302) ? 3'd3 :
	(hc < 10'd378) ? 3'd4 :
	(hc < 10'd453) ? 3'd5 :
	                 3'd6;

wire wide_in = (hc < WIDE_DE) &&
               (vc < (scandouble ? 10'd480 : 10'd240));

reg [7:0] wr, wg, wb;
always @(*) begin
	wr = 8'd0; wg = 8'd0; wb = 8'd0;
	if (wide_in) begin
		case (wide_bar)
			3'd0: begin wr = 8'hC0; wg = 8'hC0; wb = 8'hC0; end
			3'd1: begin wr = 8'hC0; wg = 8'hC0; wb = 8'h00; end
			3'd2: begin wr = 8'h00; wg = 8'hC0; wb = 8'hC0; end
			3'd3: begin wr = 8'h00; wg = 8'hC0; wb = 8'h00; end
			3'd4: begin wr = 8'hC0; wg = 8'h00; wb = 8'hC0; end
			3'd5: begin wr = 8'hC0; wg = 8'h00; wb = 8'h00; end // red — past hc=320
			3'd6: begin wr = 8'h00; wg = 8'h00; wb = 8'hC0; end // blue
			default: ;
		endcase
	end
end

assign r = force_bars ? wr : (use_ext ? fr : br);
assign g = force_bars ? wg : (use_ext ? fg : bg);
assign b = force_bars ? wb : (use_ext ? fb : bb);
```

**Expected eyes-on if absorbed:** 7 bars, edges ~114 cap-px on 800, R and B enter, span ≥0.95, R5% ≫ 15.  
**If still pillar with 64-px edges after real Fix-3 RBF:** root is **downstream of present_core RGB** → Fix-4 ascal/sys RCA (not more colorbars thrash).  
**If full-width but wrong colors:** mux/order bug; still progress on span gate.

#### B. SECONDARY — `fpga/Plex_MiSTer/rtl/colorbars.sv` (pattern path + hygiene)

Keep Template A timing (already Fix-2 on disk, md5 `f1d9666a…`):

| Symbol | Value | Notes |
|--------|------:|-------|
| `H_CONTENT` | 320 | product only |
| `H_DE` / `H_BLANK_S` | 529 | Template DE |
| `H_LAST` | 637 | FPS — leave |
| `H_SYNC_S` / `H_SYNC_E` | 544 / 590 | Template |
| HBlank | level `hc >= 529` | keep |

| Region | Fix-2 (live tree) | Fix-3 action |
|--------|-------------------|--------------|
| L119–127 bar + in_content | `bar_prod/H_DE` lpm_divide; `in_content` uses `~HBlank` | **Replace bar with threshold chain** (no divider). **`in_content = (hc < H_DE) && (py < 240) && ~VBlank`** — drop `~HBlank` (blanking still drives `VGA_DE` via HBlank out) |
| L1–8 header | Fix-2 claim | Note Fix-2 silicon FAIL `ec21e133`; Fix-3-P primary in present_core |
| case(bar) colors | unchanged | **KEEP** |

Optional **diag localparam** (off by default for product gate):

```systemverilog
localparam FIX3_DIAG_RED = 1'b0; // set 1 only for absorption RBF if needed
// when 1 && pattern==0: force r=0xFF,g=0,b=0 for all in_content
```

#### C. DO NOT TOUCH (Fix-3)

| File / region | Why |
|---------------|-----|
| `Plex.sv` residual / O[9] / `VGA_DE` | Residual sole may own; O[9] already forces bars |
| `mycore.v` | Not on present path |
| `frame_store.sv` WIDTH | Later product-width ticket |
| `sys/ascal` / AR first-line | Only if Fix-3-P still pillars after proven RBF |
| Mid-fit writes while residual Quartus LIVE | Race |

### Build / sole rules (Q-fix3)

1. **`FIT_GO_WIDE=NO`** until parent says GO and exclusive is free (no residual map/fit).  
2. Prefer **Full Compilation** / `--clean` so both `present_core.sv` and `colorbars.sv` re-elab (touch both files).  
3. Freeze pre/post md5: colorbars + present_core; claim in `/tmp/plex_quartus_fix3.claim/`.  
4. One promote → one safe deploy → FBAR → WIDE eyes-on (W-wide7 recipe) → park bars.  
5. **Do not invent BUILD_OK / WIDE PASS.** Soft-skip ≠ PASS.  
6. Do **not** thrash banned residual RBFs; do **not** Fix-1 HSync thrash.

### Acceptance (unchanged — do not invent PASS)

PASS only if **all** of:

- `force_bars=1` bars NTSC 60  
- mid-band active span **frac ≥ 0.95**  
- R5% mid-band luma **> 15**  
- verdict **≠** `PILLAR_320_of_529`  
- FBAR still EXIT=0 on the **same** RBF  

Diagnostic bonus: 7 bars + red/blue past x≈0.6×width; edges ~114 cap-px not ~97.

### Fix-3 sequencing (orchestrator)

1. Wait residual exclusive free (e.g. R-csum6 terminal BUILD_OK or aborted).  
2. Parent sets **FIT_GO_WIDE=YES** / claims **Q-fix3**.  
3. Apply Fix-3-P RTL (present_core + colorbars threshold) — record pre/post md5.  
4. Sole Full Compilation → real BUILD_OK (not invented).  
5. One promote + one safe deploy → FBAR → WIDE gate → park.  
6. If FAIL still 0.605 with **same 64-px fingerprint**: escalate Fix-4 ascal/sys (map-proof RGB at present outputs if possible).  
7. If FAIL different geometry: re-RCA with new captures; do not thrash.

---

## Pass criteria (gate)

- `force_bars=1` bars NTSC 60: active span **≥ ~95%** of capture width; not `PILLAR_320_of_529`.  
- FBAR still EXIT=0 on same RBF.  
- Optional: true VGA/CRT eyes-on (HDMI is a proxy; see [crt-lcd-lab-checklist.md](crt-lcd-lab-checklist.md)).

## Historical notes (Q-3l1 mid-fit hygiene)

| Artifact | Time (CDT) | colorbars state |
|----------|------------|-----------------|
| Q-3l1 Analysis map | ~11:34 | state B (HBlank@320, HSync@544) |
| Dirty WIP write | ~11:41 | state C (HSync@336/384 + level HBlank) |
| Q-3l1 BUILD_OK | 11:45 | RBF **aa146c17** = state B (smart recomp skipped C) |
| Q-fix1 BUILD_OK | 12:02 | RBF **820484a6** = Fix-1 state C claim — **WIDE still FAIL** (W-wide4/5/6) |
| W-fix2-d3 design lock | ~12:13 | Fix-2 plan locked; RTL deferred (R-csum1 fit LIVE) |
| Q-fix2 BUILD_OK | ~13:52 | RBF **ec21e133** SRC colorbars **f1d9666a** Fix-2 claim |
| W-wide-gate-fix2 | ~13:56 | **WIDE FAIL** span=0.605 on ec21e133 — Fix-2 silicon CLOSED FAIL |
| W-wide-gate-fix2b | ~13:58 | Independent reconfirm **identical** fingerprint (FBAR 7.0/82.9/94.4; WIDTH 0.605) |
| W-wide-rca-fix2 | ~14:05 | **FIX2_INEFFECTIVE**; paint still content320; Fix-3 colorbars threshold sketch |
| W-fix3-plan | ~14:05+ | Fix-3-P consolidated preferred; **FIT_GO Q-fix3=NO** while **R-csum6 LIVE**; docs-only |
| W-fix3-hold | ~14:07 | HOLD card; **FIT_GO Q-fix3=NO**; exclusive R-csum6 LIVE |
| W-wide-rca-SF2 | ~14:07+ | Capture peaks exact OLD 64-px; force_bars path-trace OK; **RCA_OK**; FIT_GO_WIDE=NO |
| **W-fix3-hold2** | **~14:09** | **HOLD_OK** — design freeze reconfirmed; **FIT_GO_WIDE=NO** / **FIT_GO Q-fix3=NO**; ZERO RTL; cite fix2b FAIL 0.605 + Fix-3-P + FIX2_INEFFECTIVE + R-csum6 LIVE |

Leave `fpga/Plex_MiSTer/` alone while any fitter runs (currently **R-csum6 LIVE**). Docs-only design ticks are safe during fit. **No Q-fix3 until parent FIT_GO and exclusive free after R-csum6.** **Do not invent WIDE PASS.**
