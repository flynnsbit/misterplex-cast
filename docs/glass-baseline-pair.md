# Glass baseline pair — how old is “what’s running,” and what it is missing

**Last eyes-on PASS:** 2026-08-11 — HDMI USB content identity on B6 RealGlass  
**Contract:** `tests/fixtures/glass_baseline/pair.json`  
**Artifacts:** `release_artifacts/v0.2.0-glass-baseline/`  
**Live gate:** `make glass-baseline` → `tests/hw/test_glass_baseline_pair.sh`

---

## Short answer

| Question | Answer |
|----------|--------|
| What is live when glass PASS was proven? | **v0.2.0 release pair** (published **2026-07-26**) |
| How far back? | **~2+ weeks** from the Aug 8–11 freckle / H5 / v0.4.0 work; **before v0.3.0 and v0.4.0** |
| Does it include recent advancements? | **Almost none.** It is a **glass floor**, not the product tip |
| What must new pairs do? | **Match or beat** the behavioral gates below (not necessarily reuse these md5s) |

### Pair under test

| | md5 | Era |
|--|-----|-----|
| **Plex.rbf** | `dfebf2bf…` full `dfebf2bfd08dd70b473b587dd7e81848` | v0.2.0 / **G-VID1** edge-fix core (`docs/LESSONS.md` L28) |
| **misterplexd** | `7cd10b4d…` full `7cd10b4d438c714a9b8c4766dc982d59` | v0.2.0 companion |
| **Conf** | `PRESENT=fpga` (or `both`), **`DECODE=320x240`**, `STREAM=0`, `IDLE_SCREEN=logo` | RGB→F1 host present |

Present log class (old daemon): `STREAM=0 RGB→F1 PRESENT=fpga`, scale to **320×240**, `pfps ≈ 19` on B6.

---

## What this pair *does* have

These are real, shippable **v0.2.0** capabilities (still useful as a floor):

- Companion HTTP **`:3005`** + `playMedia` for **local MP4** paths  
- **PRESENT=fpga** host decode → **DDR frame store** (RGB path in this daemon)  
- **MrAudio** 48 kHz  
- Idle **Plex chevron** logo (two intentional colors: orange on dark)  
- Basic **PMS resolve / cast** plumbing  
- Stable **320×240** bank (not 640/720 multi-yuv FOAR)

Eyes-on 2026-08-11 gates that **passed**:

1. Idle chevron (not rainbow, not colorbars)  
2. B6 landscape + OCR overlay (`G`, `c=N`, `fps=24/1`) on USB grabber  
3. Frame-to-frame change + seek-to-seek change  
4. Protocol `frame_tx ok via DDR` with **pfps ≥ 8**

---

## What the last few days of work *added* (not in this pair)

Rough timeline vs this floor (repo `ship/v0.4.0` / lab Memory):

| Area | When (approx.) | In glass baseline? |
|------|----------------|--------------------|
| v0.3.0 playback-controls core `41adb98c` | late July package pin | **No** (core is older G-VID1) |
| v0.4.0 multi-res ladder, 720 bank, package | Aug 5–8 | **No** |
| Cast timeline token / scrubber fixes | Aug 8 | **No** |
| FOAR multi-yuv **624×480** geom + `ddrFrameGeometryForPresentedSize` | mid/late lab on modern daemon | **No** (this daemon uses RGB@320) |
| softc freckle UV ladder (softc10–24) | early Aug | **No** (and softc23@640 failed DDR on this lab day) |
| OSD local **`.mpx` → PLXP** path-play | feature branch, RBF unfitted | **No** |
| Fabric H.264 product / H5 residual sticky 0x14 | overnight product loop | **No** (still open; o106 fit-fail class) |
| STREAM hybrid / skip-RGB product path | v0.4.x | **No** (`STREAM=0` only here) |

So: **if you only run this pair, you are validating “glass still works like v0.2.0,” not “product tip is green.”**

---

## Known trap (why the pair is mandatory)

Deploying a **modern** FOAR-yuv daemon (e.g. `7b28268c`) on **`dfebf2bf`** at **640×480** can produce:

- Protocol **pfps > 0** (doorbell fires)  
- HDMI **rainbow / noise** (content identity FAIL)

That is a **mixed-pair** failure (`docs/LESSONS.md` L1-class: always deploy core + daemon as a matched set).  
The glass baseline gate **refuses** to treat “pfps alone” as PASS.

---

## Regression policy

1. **Floor pair** stays pinned under `release_artifacts/v0.2.0-glass-baseline/` with fixed md5s.  
2. **`make glass-baseline`** restores (optional) and runs idle + B6 eyes-on gates on hardware.  
3. **Candidate** pairs (`GLASS_CANDIDATE=1`): do **not** force these md5s; must still pass the **same behavioral gates** (or stronger).  
4. Do not declare a new default lab pair until candidate **PASS** is recorded under `Memory/lab/hdmi-proof/`.  
5. Product H5 / freckle work must **not thrash** the live RBF during a glass-baseline run; pin after overnight loops if glass breaks.

### Commands

```bash
# Offline policy + artifact md5 integrity
make glass-baseline-policy

# Live device (restores pair unless GLASS_CANDIDATE=1)
MISTER_HOST=<mister-ip> MISTER_PASS=REDACTED make glass-baseline

# Test whatever is already live (new RBF+daemon claim)
GLASS_CANDIDATE=1 make glass-baseline
```

---

## Evidence

- Verdict: `Memory/lab/status/hdmi-b6-validated.txt`  
- Captures: `tests/fixtures/glass_baseline/idle_chevron_mjpeg720.png`, `b6_play_sample_mjpeg720.jpg`  
- Parent note: v0.2.0 is the only published GitHub release as of the 720p verdict sweep; v0.3.0 was a tag/package pin, v0.4.0 is the multi-res ship branch.
