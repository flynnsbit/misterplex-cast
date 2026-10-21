# Hard residual gate runbook — post H-deploy-rcsum6

**Owner:** H-gate-rcsum6 (execute after deploy)  
**Prep:** H-gate-rcsum6-prep (this doc; **do not run gate mid-fit / pre-deploy**)  
**Lab:** `192.0.2.183` (set `MISTER_PASS`; no default)  
**Repo:** `$HOME/misterplex-cast`

This is the **product hard residual** acceptance procedure for the first lab session
after **ONE** `H-deploy-rcsum6` menu load of a **NEW product** RBF from sole **R-csum6**.

> **Never invent PASS.** soft residual EXIT=0 ≠ hard PASS. FBAR soft ≠ residual PASS.
> BUILD_OK + DEPLOY_OK + PACKAGE_OK ≠ hard residual PASS. DIAG ≠ product PASS.
> **3l2 stays BLOCKED** until **non-DIAG product** sticky `raw[13]==0x14` ≥2.

---

## 0) Preconditions (must all be true before this gate)

| # | Precondition | Evidence |
|---|--------------|----------|
| 1 | R-csum6 sole **BUILD_OK** Full Comp exit 0 | `/tmp/plex_quartus_rcsum6.log`, claim `build_result.txt` / `done.txt` |
| 2 | **LOCK_OK** live SRC == claim freeze | claim `final_lock.txt` / mid-fit audits; md5s match intentional freeze |
| 3 | **NEW_RBF** collected; prefix8 **∉ banned** | `releases/Plex_rcsum6_<prefix8>.rbf` + full md5 |
| 4 | **READY_TO_DEPLOY=YES** recorded by parent / H-proto | deploy-trust card (H-proto-rcsum6*) |
| 5 | **ONE** H-deploy-rcsum6 only: `DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh` | `/tmp/misterplex-agent-H-deploy-rcsum6*.txt` |
| 6 | Lab fat md5 **matches NEW full md5**; `CORENAME=Plex` | lab `md5sum /media/fat/_Utility/Plex.rbf` |
| 7 | Exclusive fit **idle** (no second Quartus) | `/tmp/plex_quartus.lock` not LIVE for another sole |

### Banned thrash redeploy (never luck-reload)

| prefix8 | Why banned |
|---------|------------|
| **`8832824e`** | H-gate-rcsum5d definitive MULTI_DRIVE HARD_FAIL |
| **`75da8bb1`** | H-gate-rcsum4 family MULTI_DRIVE HARD_FAIL |
| **`4d6ee356`** | H-gate-rcsum3b family MULTI_DRIVE HARD_FAIL |

Also: do **not** thrash-redeploy the same **NEW** product RBF a second time for luck
without a completed hard gate + parent decision. Do not use residual luck menu on
**`ec21e133`** (wide Fix-2; residual HARD_FAIL open — H-gate-ec21).

Claim freeze (R-csum6 product path — DIAG **ABSENT**):

| Source | Intentional freeze (prep-era) |
|--------|-------------------------------|
| `Plex.sv` | `c7a847f7…` |
| slice | `ca62d02b…` |
| stream | `904e9b2e…` |

Hard PASS on this path is **product sticky real XOR 0x14**, not DIAG force-pack.

---

## 1) Hard acceptance criteria (product)

| Gate | Expect | Fail if |
|------|--------|---------|
| Lab md5 | full match NEW product RBF | STALE / wrong prefix / banned |
| CORENAME | `Plex` | not Plex |
| **res_dc sticky** | **−24 / `raw[12]==0xe8`** on **all** hard probes | any hard probe ≠ −24 |
| **res_csum sticky** | **`raw[13]==0x14`** on **≥2 independent** hard F3 probes | sticky0x14 &lt; 2 |
| Ideal word | form **`e8 14 xx xx`** (best host map `e8 14 53 1a`) | never seen when required |
| **Reject +0x53/push** | adjacent raw[13] steps **must not** dominate +0x53 (mod 256) | plus53 family present |
| Reject pure alias | `e8 53 1a 00` is **not** PASS | mistaken stream-alias green |
| Control plane | `res_ok=1 res_tc=8 res_t1=3 mb0=0 qp=25` sps/pps/frame | broken F3 path |
| Soft residual | `tests/hw/test_f3_residual.sh` EXIT=0 on csum miss | **must not** be graded PASS |
| FBAR | optional soft only | **must not** invent residual PASS |
| **3l2** | remains **BLOCKED** until product sticky 0x14 ≥2 | invent UNBLOCK |
| Thrash | zero redeploy banned / luck second menu | thrash observed |

### Product hard PASS (all required)

1. Lab NEW md5 match + CORENAME=Plex  
2. res_dc sticky −24 / 0xe8 on **all** hard probes  
3. sticky **raw[13]==0x14** on **≥2** independent hard F3 Baseline pushes  
4. **No** dominating **+0x53/push** multi-drive family across the series  
5. Per-probe `tests/parse_res_csum_status.py` class **HARD_PASS** on those sticky probes  
6. Explicit: soft residual / FBAR EXIT=0 **not** used as product green  

### Product HARD_FAIL classes (historical)

| Class | Signature | Prior SoT |
|-------|-----------|-----------|
| **MULTI_DRIVE_OR_STILL_FAIL** | res_dc OK; raw[13] walks **+0x53** each push; never 0x14 | H-gate-rcsum5d (`8832824e` seq `16 69 bc 0f 62 b5 08`); H-gate-ec21 (`ec21e133` `5b ae 01`) |
| CSUM_FAIL_DC_OK (per probe) | res_dc=−24; res_csum≠20 | parse helper class |
| NOT PACK_PROVEN | sticky0x14=0 | all prior residual gates |

---

## 2) Scripts & goldens (cite)

| Path | Role |
|------|------|
| **`tests/parse_res_csum_status.py`** | **Hard grade SoT** — host goldens; decode `--status` / `--raw`; class HARD_PASS / CSUM_FAIL_DC_OK |
| **`tests/hw/test_f3_residual.sh`** | Soft residual — hard-gates res_dc=−24 + control plane; **soft-skips** res_csum≠20 with **EXIT=0** → **≠ hard PASS** |
| `tests/hw/test_fbar_fast.sh` | Optional visual soft; EXIT=0 **≠ residual PASS** |
| `scripts/gen_test_annexb_real.py` | Baseline AnnexB → `plex_real_baseline.264` (**6739** B; `6739 & 0xFF = 0x53`) |
| `scripts/deploy_plex_core.sh` | Safe deploy; gate worker uses **only if** preflight proves STALE and parent authorized — default **no second deploy** |
| `host/libmisterplex/h264_residual_gold.hpp` | Locked unit goldens `kDc=-24`, `kCsum8=0x14` |

### Host goldens (locked)

```
res_dc   = -24 = 0xE8   (sat8(coeff[0]))
res_csum =  20 = 0x14   (XOR sat8(full coeff[16]) — NOT arith sum -20 / 0xEC)
status map: raw[12]=res_dc  raw[13]=res_csum  raw[14:15]=stream_bytes LE
hard gate:  res_dc=-24 AND res_csum=20 (raw[13]=0x14)
```

Offline self-check (no lab):

```bash
python3 tests/parse_res_csum_status.py --self-test
python3 tests/parse_res_csum_status.py --goldens
python3 tests/parse_res_csum_status.py e8 14 2a 00   # HARD_PASS vector
```

### Soft residual caveat (explicit)

`tests/hw/test_f3_residual.sh` prints:

> soft-skip EXIT=0 is NOT hard PASS — re-gate after … deploy

When res_csum≠20 the script still exits 0 after control-plane + res_dc checks.
**H-gate-rcsum6 must grade hard from multi-probe parse + sticky tally**, not from that EXIT.

---

## 3) Probe style (match H-gate-ec21 / H-gate-rcsum5d)

Canonical prior evidence:

| Series | Report | Probes | Result |
|--------|--------|--------|--------|
| Product residual fail SoT | `/tmp/misterplex-agent-H-gate-rcsum5d.txt` | 12 hard | sticky0x14=**0**; +0x53; res_dc PASS |
| Primary 7-probe | `/tmp/misterplex-agent-H-gate-rcsum5.txt` + `…/H-gate-rcsum5-probes.txt` | 7 | seq `16 69 bc 0f 62 b5 08` |
| Wide residual reconfirm | `/tmp/misterplex-agent-H-gate-ec21.txt` + `…/H-gate-ec21-probes.txt` | 3 | +0x53 `5b ae 01` |

### Safe lab hygiene (gate worker)

- **NO Quartus** / no exclusive fit  
- **NO** thrash-redeploy banned prefixes; **NO** second menu for luck  
- Free SPI gently: `killall misterplexd` only — **never** kill-9 + load_core storms  
- If CORENAME already Plex and lab md5 == NEW: **do not** re-run deploy  
- Park bars at end (optional): force bars / leave FBAR end state  

### Recommended hard series size

| Mode | Hard F3 pushes | Notes |
|------|----------------|-------|
| Minimum accept | **≥3** independent `push_frame --index 3` | sticky0x14 count among hard probes; need **≥2** with raw[13]==0x14 |
| Canonical product (rcsum5 style) | **7** (series A×4 + B×3) or **3+** reconfirm | better +0x53 detection |
| Independent reconfirm | second worker 5× (rcsum5b style) | only if first series ambiguous |

**sticky0x14 ≥2** means at least two **hard** probes (after independent Baseline pushes)
observe `raw[13]==0x14` (and res_dc=−24). Prefetch/latched PRE without push may
show stale latch — count only post-push hard probes for sticky tally (record PRE separately).

---

## 4) Procedure (H-gate-rcsum6 execution checklist)

**Do not start this procedure while exclusive R-csum6 fit is LIVE or lab is still on
pre-deploy RBF (`ec21e133` / prior residual) unless parent explicitly re-gates a
named LOADED md5 (H-gate-ec21 style) — default path is post H-deploy-rcsum6 only.**

### Step A — Preflight identity (no thrash)

```bash
ROOT=$HOME/misterplex-cast
HOST=192.0.2.183
PASS=REDACTED
# NEW full md5 from deploy report / releases/*.md5
NEW_MD5=<from H-deploy-rcsum6 / new_rbf_md5.txt>
PREFIX8=${NEW_MD5:0:8}

sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST \
  'md5sum /media/fat/_Utility/Plex.rbf; ls -la /media/fat/_Utility/Plex.rbf; \
   cat /tmp/CORENAME 2>/dev/null; cat /tmp/CORENAME_FILE 2>/dev/null'

md5sum "$ROOT/fpga/Plex_MiSTer/output_files/Plex.rbf" \
       "$ROOT/releases/Plex_rcsum6_${PREFIX8}.rbf" 2>/dev/null
```

Record:

- `LAB_MD5` / match vs `NEW_MD5`  
- size / mtime  
- CORENAME=Plex  
- Reject if prefix ∈ `{8832824e,75da8bb1,4d6ee356}` as “new product” (banned thrash)  

Log path suggestion: `/tmp/misterplex-H-gate-rcsum6-probes.txt`

### Step B — Optional FBAR soft only

```bash
"$ROOT/tests/hw/test_fbar_fast.sh" | tee /tmp/misterplex-H-gate-rcsum6-fbar.log
# Expect fingerprint ~ grid_off 7.0 / force 82.9 / bars 94.4 EXIT=0 historically
# FBAR_EXIT=0 → record soft only; NEVER product residual PASS
```

### Step C — Gentle SPI free + ensure Plex

```bash
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST \
  'killall misterplexd 2>/dev/null || true; killall -CONT MiSTer 2>/dev/null || true; \
   rm -f /tmp/misterplex_spi.lock'
# Only if CORENAME not Plex AND parent authorized: DEPLOY_LOAD=menu (rare mid-gate)
# Default: do NOT deploy
```

### Step D — Baseline AnnexB on lab

```bash
mkdir -p "$ROOT/build"
python3 "$ROOT/scripts/gen_test_annexb_real.py" "$ROOT/build/plex_real_baseline.264"
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$ROOT/build/plex_real_baseline.264" \
  root@$HOST:/media/fat/plex_real_baseline.264
# Prefer also misterplex path if present:
# /media/fat/misterplex/plex_real_baseline.264  size 6739
```

### Step E — PRE status (optional latch note)

```bash
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST \
  '/media/fat/misterplex/bin/push_frame --status; \
   /media/fat/misterplex/bin/push_frame --raw'
# Pipe status/raw into parser; do NOT count PRE toward sticky0x14 ≥2 unless policy says so
```

### Step F — Hard probes (repeat N≥3)

For each probe `P1..PN`:

```bash
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST \
  '/media/fat/misterplex/bin/push_frame --index 3 /media/fat/plex_real_baseline.264'
sleep 0.5
ST=$(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST \
  '/media/fat/misterplex/bin/push_frame --status')
RAW=$(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST \
  '/media/fat/misterplex/bin/push_frame --raw')
echo "$ST"
echo "$RAW"
echo "$ST" | python3 "$ROOT/tests/parse_res_csum_status.py" -
echo "$RAW" | python3 "$ROOT/tests/parse_res_csum_status.py" -
```

Per probe record:

| Field | Source |
|-------|--------|
| res_dc / res_csum / bytes_in | `--status` |
| raw[12..15] | `--raw` line `raw[0]: …` bytes 12–15 |
| hard class | parse helper |
| control plane | res_ok, res_tc, res_t1, mb0, qp, sps/pps |

### Step G — Soft residual (optional; for soft-skip proof only)

```bash
"$ROOT/tests/hw/test_f3_residual.sh" | tee /tmp/misterplex-H-gate-rcsum6-soft-residual.log
# If csum miss: EXIT=0 soft-skip → document "soft EXIT=0 ≠ hard PASS"
# If csum==20: note soft green; hard still requires multi-probe sticky tally
```

### Step H — Tally + classify

```
hard_probes=N
res_dc_pass=X/N
sticky0x14=Y          # count raw[13]==0x14 among hard probes
plus53_steps=Z        # adjacent deltas == 0x53 (mod 256)
raw13 sequence: ...
bytes_in sequence: ...
hard: PASS|FAIL per probe
CLASS=...
LAB_MD5_FINAL=...
CORENAME_FINAL=Plex
```

**Delta rule:** for consecutive hard raw[13] values `a → b`,  
`delta = (b - a) & 0xFF`; if `delta == 0x53` → multi-drive step.  
Annex length lo byte **0x53** is the historical multi-drive fingerprint.

### Step I — Verdict table (write report)

| Field | PASS requires |
|-------|----------------|
| product hard residual | sticky0x14≥2 **and** res_dc all PASS **and** not +0x53-dominated |
| PACK_PROVEN / product green | only if product DIAG-absent path sticky 0x14 ≥2 (this sole is DIAG ABSENT) |
| FBAR | soft only |
| soft residual | soft only |
| 3l2 | **BLOCKED** until product sticky proven; may **consider** unblock only on hard PASS |
| thrash this worker | **NONE** |

### Step J — Park + leave lab

- Leave CORENAME=Plex; lab md5 unchanged (no thrash)  
- Optional park bars  
- Write `/tmp/misterplex-agent-H-gate-rcsum6.txt` with evidence paths  

---

## 5) Decision branches (post-gate)

| Branch | Trigger | Next |
|--------|---------|------|
| **A HARD_PASS product** | sticky raw[13]==0x14 ≥2; res_dc PASS; +0x53 rejected | May consider **3l2** unblock (parent); package residual NEW; no thrash |
| **B MULTI_DRIVE still** | never 0x14; +0x53/push | **HARD_FAIL**; RCA value-path — **not** thrash redeploy NEW / banned |
| **C CSUM wrong stable** | sticky wrong constant ≠0x14; no +0x53 | HARD_FAIL pack/value; RTL sole next — no luck menu |
| **D lab STALE** | lab md5 ≠ NEW after claimed deploy | re-run **H-deploy-rcsum6 once** only if parent confirms; else stop |
| **E soft-only green** | soft residual EXIT=0 / FBAR EXIT=0 but sticky0x14&lt;2 | **FAIL** hard — soft ≠ PASS |

**Never:** second menu for luck; kill-9 storms; redeploy `8832824e`/`75da8bb1`/`4d6ee356`;
invent 3l2 UNBLOCK; invent HARD_PASS from single lucky probe without ≥2 sticky.

---

## 6) Report template (H-gate-rcsum6)

Write: `/tmp/misterplex-agent-H-gate-rcsum6.txt`

```
# H-gate-rcsum6 — hard residual gate on NEW product RBF <prefix8>

Worker / time / lab / NEW full md5 / DIAG=ABSENT product path

## VERDICT: HARD_PASS | HARD_FAIL
class: PRODUCT_STICKY_OK | MULTI_DRIVE_OR_STILL_FAIL | …

| Field | Result |
| lab md5 match | |
| res_dc sticky −24 / 0xe8 | |
| sticky raw[13]==0x14 | Y/N hard probes |
| +0x53/push family | YES/NO |
| FBAR | soft EXIT=? ≠ residual PASS |
| soft residual | EXIT=? ≠ hard PASS |
| 3l2 | BLOCKED | may-consider-unblock |
| thrash / redeploy | NONE |

## Probe table + Δ raw[13]
## GATE TALLY
## Evidence paths
## Backlog status suggestion
## Blockers
```

Probe log: `/tmp/misterplex-H-gate-rcsum6-probes.txt`  
Summary (optional): `/tmp/misterplex-H-gate-rcsum6-summary.txt`

---

## 7) Explicit non-goals for prep / gate workers

| Action | Prep (this doc) | Gate (H-gate-rcsum6) |
|--------|-----------------|----------------------|
| Run hard probes on lab | **NO** (lab may still be `ec21e133`; fit may be LIVE) | YES post ONE menu NEW |
| Deploy / menu | **NO** | only if STALE + parent — default NO second |
| Quartus | **NO** | **NO** |
| Invent BUILD_OK / HARD_PASS | **NO** | **NO** |
| Unblock 3l2 without sticky | **NO** | **NO** |
| Thrash banned RBF | **NO** | **NO** |

---

## 8) Cross-links

| Doc / card | Role |
|------------|------|
| `docs/AGENT_ORCHESTRATION.md` § Post–R-csum6 deploy-trust | READY / thrash / hard expect |
| `docs/phase3-3l-idct.md` § Post–R-csum6 | serial T7 handoff H-gate-rcsum6 |
| `docs/PHASE_BACKLOG.md` | living status; 3l2 BLOCKED |
| `/tmp/misterplex-agent-H-proto-rcsum6.txt` | deploy-trust protocol card |
| `/tmp/misterplex-agent-H-gate-rcsum5d.txt` | definitive prior HARD_FAIL style |
| `/tmp/misterplex-agent-H-gate-ec21.txt` | probe-style reconfirm on LOADED RBF |
| `/tmp/misterplex-agent-H-gate-rcsum6-prep.txt` | this prep worker report |

---

## 9) One-liner

**After ONE H-deploy-rcsum6 menu of NEW product RBF (∉ banned): multi-probe F3 hard
gate — res_dc sticky −24; sticky raw[13]==0x14 ≥2; reject +0x53/push; soft residual /
FBAR EXIT=0 ≠ PASS; 3l2 BLOCKED until product sticky proven; no thrash redeploy.**
