# Phase 3.3l — Inv quant + 4×4 IDCT + Intra pred (FPGA)

**Status:** 3.3l-0 done; 3.3l-1 **host goldens locked** (`res_csum` XOR sat8 full-16 = **0x14** / 20); **R-csum1 BUILD_OK** RBF **`dabdaeb0`** still FAIL reference. **R-csum2 BUILD_OK** → lab **`4deaf6cc` HARD FAIL** **stream24 pack alias** (expect green **`e8 14 53 1a`**; observed **+0x53 family** / **`e8 53 1a 00`**; FBAR/res_dc PASS; DIAG force no stick). **R-csum-rtl3 pack fix:** 128b **`status_telem_r`** + multi-cycle; claim **`eb6b8541…`/`6ce28d6e…` LOCK_OK@claim**. **R-csum3 FIT_DEAD_MID** (SIGTERM ~12:49; no NEW_RBF). Freeze **`ce1ef26c`/`e45f98c4` LOCK_OK**. **R-csum3b BUILD_OK ~13:00** wall **~438s** → **NEW_RBF `4d6ee356`**. **H-deploy-rcsum3b** one menu **DEPLOY_OK** lab **`4d6ee356`**. **Post-`4d6ee356` HARD_FAIL reconfirm (H-gate-rcsum3b / 3b2 / 3b3 + L-csum-note21/22):** **FBAR PASS** (7.0/82.9/94.4); **res_dc=-24 PASS**; **hard `res_csum` HARD_FAIL** — **raw[13] additive +0x53/push** (**0x53 = 6739 lo** = annex-B size&0xFF), **never sticky 0x14**. Series: 3b **`e8 55…`/`e8 a8…`/`e8 fb…`** (85→168→251); 3b2 **`e8 4e…`/`e8 a1…`/`e8 f4…`** (78→161→244); **3b3** **`e8 47…`/`e8 9a…`/`e8 ed…`** (71→154→237). Class **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM**. **MAP_OK** (A-csum-map1/map2 — host raw[13]=status[111:104]; blame FPGA value, not host remap). Soft sample `e8 02 d7 01` ≠ hard SoT. **Do not invent hard PASS**; **soft-skip ≠ hard PASS**; **3l2 BLOCKED**; **no thrash `4d6ee356`**. **R-csum4 BUILD_OK (L-csum-note24):** claim freeze DIAG **`94db41b7`/`9a2d10c5`** (`residual_csum<=8'h14`); Full Compilation **exit 0** wall **421s**; **NEW_RBF `75da8bb1`** full `75da8bb10e36b3e068d66a9ed053cd2c` (∉ banned); promote `releases/Plex_rcsum4_diag14_75da8bb1.rbf`; log `/tmp/plex_quartus_rcsum4.log`. **MID-FIT DRIFT_CRITICAL caveat:** live WT thrashed under docker `-v` during fit (≠ claim) — RBF intended inputs = claim md5s; sticky-0x14 pack-proof may need parent **accept-ambiguous** or re-freeze re-sole. **H-deploy-rcsum4** one menu **DEPLOY_OK** → lab **LOADED `75da8bb1`**. **F-prep-rcsum4 PACKAGE_OK**. **H-gate-rcsum4 HARD_FAIL (L-csum-note25):** **NOT PACK_PROVEN**; **DIAG force did not stick** (sticky raw[13]==0x14 count **0/7**); series A **0x40→0x93→0xe6** (**+0x53/push**); series B **0x39→0x8c→0xdf→0x32** (wrap +0x53); ideal **`e8 14 xx` NEVER**; **FBAR PASS** (7.0/82.9/94.4); res_dc=−24 PASS; class **MULTI_DRIVE_OR_STILL_FAIL** / **CSUM_UNSTABLE_ACCUM**. **R-csum-rtl5 FIT_GO:** **`st_res_word_sticky`** latches `{residual_csum,residual_dc}` **only on `residual_ok_rise`**; status residual half from sticky only; **product `residual_csum<=csum_acc`** at ST_PLACE; **DIAG stripped** after silicon fail on **`75da8bb1`**. Claim@launch freeze **`6422fb9a`/`8e6af3bb`**. **R-csum5 sole LIVE (L-csum-note26…note29):** sticky-pack **product** sole started **`2026-07-24T13:31:58-05:00`**; log `/tmp/plex_quartus_rcsum5.log`; claim dir `/tmp/plex_quartus_rcsum5.claim`; **CLAIM LAUNCH md5s AUTHORITATIVE for map-era RBF:** **Plex `6422fb9a…` / slice `8e6af3bb…`** product sticky **DIAG=ABSENT** at launch (R-csum5-build). **NOT BUILD_OK** (fit LIVE — do not invent). **L-csum-note28/29 mid-fit claim overwrite (DRIFT_CRITICAL → PROVENANCE_UNTRUSTED):** ~**13:32:27** live WT thrashed to **`6a5dcaaa`/`7d4a1d8b`**; ~**13:33:19** claim-dir rewrite **DIAG=PRESENT** residual_csum<=**8'h14** + **`res_pair_sticky`** note *prior product claim SUPERSEDED* — **CONTAMINATED** (not valid sole re-claim under live fit). map.rpt has **`st_res_word_sticky` count=33** / **`res_pair_sticky` count=0** → fit places **map-era PRODUCT** netlist, **not** live DIAG thrash. Docker **`-v`** thrash = **PROVENANCE_UNTRUSTED** until map/fit SRC match intentional freeze. **STOP further mid-fit RTL** — no `Plex.sv`/`slice_hdr_parser.sv` edits under docker thrash. **L-csum-note31 — R-csum5 BUILD_OK ~13:40:35 CDT** wall **441s**; **NEW_RBF `8832824e`** full `8832824e483cf6613f82ee3ba3e592b3` (∉ banned); exclusive **IDLE**. **L-csum-note32 — parent accept-ambiguous** names **`8832824e`** → **ONE promote + ONE menu**. **H-deploy-rcsum5** **DEPLOY_OK** lab **LOADED `8832824e`**. **L-csum-note33 / H-gate-rcsum5 HARD_FAIL on `8832824e`:** sticky0x14=**0**/7; **+0x53 multi-drive continues after sticky-pack sole**; series A **0x16→0x69→0xbc→0x0f**; series B **0x62→0xb5→0x08**; ideal **`e8 14 xx` NEVER**; **res_dc PASS** (−24 / 0xe8 7/7); **FBAR soft** 7.0/82.9/94.4; class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN**; **BUILD_OK+DEPLOY_OK ≠ hard residual PASS**; soft-skip ≠ PASS; thrash **`8832824e`/`75da8bb1`/`4d6ee356` forbidden**; **next = RCA not redeploy**; **3l2 BLOCKED**. **L-csum-note34 (docs stamp):** (1) **H-gate HARD_FAIL `8832824e` multi-drive** reconfirm (sticky0x14=0; +0x53/push; **NOT PACK_PROVEN**); (2) exclusive **Q-SF2 LIVE** lock (alias) — **residual RTL FREEZE** while wide Fix-2 fits (shared docker `-v` mount); (3) **thrash FORBIDDEN** incl. **mid-fit residual edits during Q-fix2** (`Plex.sv`/`slice_hdr_parser.sv` ZERO; no residual re-sole; no thrash-redeploy **`8832824e`/`75da8bb1`/`4d6ee356`**). soft-skip ≠ PASS; **3l2 BLOCKED**. **L-csum-note35 (docs stamp):** (1) **HARD_FAIL `8832824e` DEFINITIVE** — **H-gate-rcsum5d** consolidates H-gate-rcsum5+5b → class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN**; sticky0x14=**0**/12 (never); +0x53/push family confirmed; ideal **`e8 14 xx` NEVER**; **res_dc PASS**; FBAR soft ≠ product PASS; **BUILD_OK+DEPLOY_OK+PACKAGE_OK ≠ hard residual PASS**; (2) exclusive **Q-fix2 LIVE** lock **`Q-fix2 2026-07-24T13:45:49-05:00`** (`/tmp/plex_quartus.lock`) — **hold residual RTL** (FREEZE; no residual re-sole / second exclusive while wide Fix-2 holds slot); (3) **thrash FORBIDDEN** (`8832824e`/`75da8bb1`/`4d6ee356`; mid-fit residual edits under docker `-v` ZERO); (4) **3l2 BLOCKED** until non-DIAG product sticky 0x14 ≥2. **L-csum-note37 (docs stamp; this tick — dual-track post Q-SF2 BUILD_OK+deploy+WIDE FAIL):** exclusive **FREE** at post-Q-SF2 harvest (lock was `Q-SF2 DONE BUILD_OK 2026-07-24T13:52:56-05:00 NEW_RBF=ec21e133…`; parent may later claim **Q-SF3 OR R-csum6**). **W:** Q-SF2 **BUILD_OK** wall **415s** → NEW_RBF **`ec21e133`** full `ec21e1330ddd75ad7f39099e5abfad49` (SRC colorbars **`f1d9666a`**); **ONE** promote/menu (**H-deploy-qSF2**); lab **LOADED `ec21e133`**; FBAR soft PASS 7.0/82.9/94.4; **WIDE FAIL** span=**0.605** **PILLAR_320_of_529** (**W-wide-gate-SF2/SF2b**) — FAIL open. **R residual HARD_FAIL on both:** (1) product **`8832824e`** **H-gate-rcsum5d** sticky0x14=0/12 +0x53 **16/69/bc/0f/62/b5/08** **NOT PACK_PROVEN**; (2) LOADED wide **`ec21e133`** **H-gate-ec21 / H-res-ec21** sticky0x14=0 +0x53 seq **54/a7/fa/4d/a0/f3/46** (res_dc PASS). **READY_TO_DEPLOY residual=NO** (do not invent YES). **Competing next soles (parent picks ONE):** **Q-SF3** (wide Fix-3 after design) **OR** **R-csum6** (residual multi-drive RCA) — one at a time; **ZERO second Quartus**; mid-fit thrash **FORBIDDEN**; next sole → **ONE** menu only after BUILD_OK. Thrash ban: residual **`8832824e`/`75da8bb1`/`4d6ee356`** + **no thrash-redeploy luck on `ec21e133` for residual green**. **WIDE gate ≠ residual PASS ≠ 3l2**; soft-skip ≠ PASS; hard expect sticky **raw[13]==0x14 ≥2** reject **+0x53**; **3l2 BLOCKED**. **L-csum-note36 (docs stamp; this tick — mid-fit residual HOLD reconfirm):** (1) lab **`8832824e` HARD_FAIL DEFINITIVE** unchanged (**H-gate-rcsum5d** SoT; sticky0x14=**0**/12; +0x53 seq **16/69/bc/0f/62/b5/08**; **NOT PACK_PROVEN** / **MULTI_DRIVE**); (2) exclusive **Q-SF2 still LIVE mid-fit** lock **`Q-SF2 2026-07-24T13:45:49-05:00`** — **do not invent Q-SF2 BUILD_OK / NEW_RBF**; host residual RBF remains stale **`8832824e`**; (3) **HOLD residual RTL mid-fit** — ZERO residual edits / re-sole / second exclusive while wide Fix-2 owns docker `-v` mount; (4) thrash-redeploy **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**; soft-skip ≠ PASS; **3l2 BLOCKED**; next residual path = multi-drive RCA **after** Q-SF2 frees (not luck redeploy).

**Depends on:** 3.3k residual levels/runs → `residual_dc` (HW-green); **3.3l-1 hard `res_csum=20` on lab** (still **HARD_FAIL** after R-csum1 **`dabdaeb0`**, R-csum2 **`4deaf6cc`**, and R-csum3b lab **`4d6ee356`** sole menu + H-gate 3b/3b2/**3b3** — **CSUM_UNSTABLE_ACCUM** / **pack-likely-fixed** + **MAP_OK**, raw[13] additive +0x53/push not sticky 0x14)  
**Product rule:** hybrid host recon → F1 still owns present until FPGA mae is competitive.  
**No Quartus for 3.3l-0 / host 3.3l-1 / host 3.3l-2 / L-3l2-rtl / L-csum-note…note41 / L-3l2-gate / L-3l2-gate2** — residual product hard gate closed **HARD_PASS** on lab **`94bbfe43`** (L-csum-note41); paint fit / SV wire-up / `files.qip` may proceed under parent sole for **3l2 product IDCT** (this docs agent: **NO Quartus / NO deploy / NO RTL**). **No mid-RCA FPGA commit thrash.** **Dual-track:** residual thrash ban remains for historical FAIL RBFs; WIDE Fix-3 separate. **No invent residual DEPLOY_OK / residual hard PASS / residual BUILD_OK** without residual-track lab evidence.  
**Hard unblock (MET L-csum-note41):** sticky `raw[13]==0x14` **AND** `res_dc=-24` stable ≥2 on **non-DIAG product** lab **`94bbfe43`** — sticky0x14=**7/7** plus53=**0** ideal **e8 14 xx**. DIAG force-0x14 is pack-bisect only — **not** product PASS. Soft-skip EXIT=0 is **NOT** enough (**soft-skip ≠ hard PASS**). **3l2 residual hard-block LIFTED**; **≠ WIDE PASS** (0.605); **≠ full 3l2 product DONE**. **No thrash-redeploy `94bbfe43` / `8832824e` / `4d6ee356` / `75da8bb1` / luck `ec21e133`.**  

**Contingency ACTIVE (L-csum-note40 mid-fit reconfirm; L-csum-note38 parent FIT_GO=YES R-csum6 product Rank1+2+3 place_pulse sticky sole LIVE; L-csum-note37 dual-track post Q-SF2 BUILD_OK+deploy+WIDE FAIL; residual HARD_FAIL both `8832824e` + `ec21e133`; H-gate-rcsum5d DEFINITIVE; R-multidrive-rca14 RCA_OK; L-csum-note36 mid-fit HOLD historical; L-csum-note35; prior H-gate-rcsum5/5b / L-csum-note33/34; H-gate-rcsum4/4b / note25→note32; 3b/3b2/3b3):** Exclusive **R-csum6 LIVE** **`2026-07-24T14:02:42-05:00`** (parent **FIT_GO=YES** product Rank1+2+3; **ZERO invent residual BUILD_OK**). Lab currently **LOADED wide Fix-2 `ec21e133`** (H-deploy-qSF2 ONE menu) until residual NEW after BUILD_OK. **W:** Q-SF2 **BUILD_OK** wall **415s** NEW_RBF **`ec21e133`** full `ec21e1330ddd75ad7f39099e5abfad49` SRC **`f1d9666a`**; FBAR soft PASS; **WIDE FAIL OPEN** span=**0.605** **PILLAR_320_of_529** (**W-wide-gate-fix2b** / W-wide-gate-fix2) — **orthogonal**; **WIDE Fix-2 CLOSED ineffective** (not PASS). **R residual HARD_FAIL both RBFs:** product sticky-pack sole **`8832824e`** (**H-gate-rcsum5d** sticky0x14=0/12 +0x53 **16/69/bc/0f/62/b5/08** **NOT PACK_PROVEN**) **and** LOADED wide **`ec21e133`** (**H-gate-ec21** sticky0x14=0 +0x53 **08/5b/ae/01**; H-res-ec21 **54/a7/fa/4d/a0/f3/46** res_dc PASS) — same **MULTI_DRIVE_OR_STILL_FAIL** class. Ideal **`e8 14 xx` NEVER**. **BUILD_OK+DEPLOY_OK+PACKAGE_OK+FBAR ≠ hard residual PASS**; **WIDE FAIL ≠ residual PASS ≠ 3l2**. **READY_TO_DEPLOY residual=NO** (do not invent YES). **soft-skip ≠ PASS**. Thrash **FORBIDDEN**: residual banned **`8832824e`/`75da8bb1`/`4d6ee356`** + **no thrash-redeploy luck on `ec21e133` for residual green** + mid-fit residual RTL under docker `-v` **ZERO** while R-csum6 sole. After residual **BUILD_OK+LOCK_OK** → **ONE** `DEPLOY_LOAD=menu` only (H-deploy-rcsum6). Hard residual expect sticky **raw[13]==0x14 ≥2** reject **+0x53**. **3l2 BLOCKED** until **non-DIAG product** sticky green. Probes: `/tmp/misterplex-agent-L-csum-note40.txt`, `/tmp/misterplex-agent-L-csum-note38.txt`, `/tmp/misterplex-agent-H-gate-ec21.txt`, `/tmp/misterplex-agent-W-wide-gate-fix2b.txt`, `/tmp/misterplex-agent-R-multidrive-rca14.txt`, `/tmp/misterplex-agent-H-gate-rcsum5d.txt`, `/tmp/misterplex-agent-H-res-ec21.txt`, `/tmp/misterplex-agent-W-wide-gate-fix2.txt`, `/tmp/misterplex-agent-H-deploy-qSF2.txt`, `/tmp/misterplex-agent-M-fitmon-qSF2d.txt`, `/tmp/plex_quartus.lock`, `/tmp/plex_quartus_rcsum6.log`, `/tmp/plex_quartus_rcsum6.claim/`, `/tmp/plex_quartus_sf2.log`, `/tmp/misterplex-agent-H-gate-rcsum5.txt`, `/tmp/misterplex-agent-H-gate-rcsum5b.txt`, `/tmp/misterplex-H-gate-rcsum5-probes.txt`, `/tmp/misterplex-H-gate-rcsum5-summary.txt`, `/tmp/misterplex-agent-H-deploy-rcsum5.txt`, `/tmp/misterplex-accept-ambiguous-rcsum5.txt`, `/tmp/plex_quartus_rcsum5.log`, `/tmp/misterplex-agent-L-csum-note21.txt`…`note40.txt`, `/tmp/misterplex-agent-H-gate-rcsum4.txt`…`4b-probes.txt`.


**L-csum-note40 (docs stamp; post-terminal BUILD_OK):** **R-csum6 DONE BUILD_OK** wall **438s** NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` (∉ banned); **LOCK_OK** live==claim **`c7a847f7`/`ca62d02b`/`904e9b2e` DIAG=ABSENT** Rank1+2+3; exclusive **IDLE**. **BUILD_OK ≠ residual PASS**. Next: **ONE** menu **H-deploy-rcsum6** only → hard gate sticky **raw[13]==0x14 ≥2** reject **+0x53**. thrash banned **FORBIDDEN** (`8832824e`/`75da8bb1`/`4d6ee356`; no luck `ec21e133`); **3l2 BLOCKED** until product sticky; **WIDE** orthogonal FAIL hold 0.605. soft-skip ≠ PASS. Report: `/tmp/misterplex-agent-L-csum-note40.txt` (mon: M-fitmon-rcsum6e; lock DONE BUILD_OK).

**L-csum-note41 (docs stamp; post H-gate-rcsum6 HARD_PASS):** Lab LOADED **`94bbfe43`** product residual **HARD_PASS** / **CLASS=HARD_PASS_PRODUCT_STICKY** / **PACK_PROVEN=YES** (DIAG=**ABSENT** sticky **0x14**). sticky0x14=**7/7** plus53=**0** res_dc **PASS 7/7** ideal **e8 14 xx** 7/7. Claim freeze **c7a847f7/ca62d02b/904e9b2e** LOCK_OK; DEPLOY_OK ONE menu. **3l2 residual hard-block LIFTED** (may start 3.3l-2 product IDCT wire-up / gate). **≠ WIDE PASS** (still FAIL **0.605** on historical **ec21e133**); **≠ full 3l2 product DONE**. soft-skip ≠ PASS. Do not re-run residual gate / thrash redeploy **94bbfe43**. Next: **3l2 product IDCT wire-up / gate**; **WIDE Fix-3** separate. Report: `/tmp/misterplex-agent-L-csum-note41.txt`. Evidence: `/tmp/misterplex-agent-H-gate-rcsum6.txt`, `/tmp/misterplex-H-gate-rcsum6-summary.txt`, `/tmp/misterplex-agent-H-deploy-rcsum6.txt`.

**R-multidrive-rca14 (docs stamp; 2026-07-24):** dual-gate multi-drive RCA **RCA_OK** (recipe-era **FIT_GO=NO** until parent). Root class **MULTI_DRIVE wire ownership of status[111:104]/raw[13]** (annex_len_lo **+0x53** = 6739&0xFF). Evidence: **H-gate-rcsum5d** lab **`8832824e`** sticky0x14=**0/12** seq **16/69/bc/0f/62/b5/08**; **H-gate-ec21** lab **`ec21e133`** sticky0x14=**0/3** series **08→5b→ae→01** (3/3 +0x53; PRE latch continuity from B3). res_dc PASS both; ideal `e8 14 xx` NEVER; **NOT PACK_PROVEN**. WIDE Fix-2 ≠ residual fix. **Superseded for FIT_GO by L-csum-note38:** parent **FIT_GO=YES** product **Rank1+2+3 place_pulse sticky** exclusive sole **R-csum6 LIVE** — **ZERO invent BUILD_OK**. Hard expect sticky raw[13]==**0x14 ≥2** reject +0x53 res_dc=−24. Thrash **FORBIDDEN** `8832824e`/`75da8bb1`/`4d6ee356`/`ec21e133`. soft-skip ≠ PASS; DIAG ≠ product PASS; **3l2 BLOCKED**. Report: `/tmp/misterplex-agent-R-multidrive-rca14.txt`. Plan: `/tmp/misterplex-agent-R-csum-rtl6-plan.txt`.


**Current P3-3l2 ABI/work item (W-CAP 2026-07-26):** product `decode_stub.sv`
now instantiates the shared `h264_iq_idct_4x4.sv` modules copied from
`feat/p3-idct-sim` for first-4×4 inverse quant + H.264 integer IDCT from
`residual_coeff[0:15]` and `slice_qp`; it paints the reconstructed top-left 4×4
and publishes `raw[14]=recon_sig=0x3b` (XOR of MB0 block0 reconstructed Y) while
preserving `raw[12]=res_dc` and `raw[13]=res_csum`. `raw[15]` is now P3-3l2
silicon RCA debug (`recon_dbg` usable bits avoid AR-masked bits [2:1]:
coeff/dequant/IDCT/recon non-zero plus residual-wait flags). Gate
before fit: 6739B baseline and 6776B padded vectors must both read
`res_csum=0x14`, `recon_sig=0x3b`, and `recon_dbg&~0x06=0xf9`; the existing
`nalu_count`/raw[4] move is the liveness witness. Fixture
source of truth: `tests/fixtures/p3_host_recon/mb0_luma_v1.json`. Real Verilator
behavioral sim now gates both the shared IQ/IDCT modules and product `decode_stub.sv`:
`make rtl-sim` must show `OK real RTL sim`, `OK decode_stub RTL sim: ... recon_sig=0x3b`,
the pred-only red-check `recon_sig got 0x0 want 0x3b`, and
`OK stream_path integrated sim: ... recon_sig=0x3b` before any Quartus/device gate.

Silicon RCA for `fe7673bc` showed `raw[13]=0x14` but `raw[14]=0x00`, matching
sixteen pred-only `0x80` samples XORed together. The root cause was an integration
race: `slice_hdr_parser` kept `residual_csum` sticky but cleared transient
`residual_coeff[]`/`residual_ok` on the next capture, while `decode_stub` could
latch before/after the real place-time payload. The fix extends the existing
`ST_PLACE` pattern with sticky place-time payload latches (`residual_place_*`) and
has `decode_stub` latch only on `residual_place_pulse`, not on `residual_ok` or
slice-valid edges.


## Goal

Move from **coeff probe** (`residual_dc = scan coeff[0]`) to **real recon pixels** on FPGA:

1. Inverse quantize first residual 4×4  
2. 4×4 integer IDCT (FFmpeg `ff_h264_idct_add`)  
3. Intra prediction (first MB, then all MBs)  
4. Write reconstructed samples into `frame_store` (F3 diagnostic path)

Host reference (bit-exact vs FFmpeg no-deblock):  
`host/libmisterplex/h264_recon.hpp` — `dequant4x4`, `idct4x4_add`, `predI4` / I16 / chroma.

## Where we are (3.3k baseline)

| Piece | Location | State |
|-------|----------|--------|
| Slice header + mb_type + QP | `slice_hdr_parser.sv` | done |
| CAVLC first residual (nC=0) | same: levels, total_zeros, run_before, ST_PLACE | done |
| Export | `residual_ok`, `residual_tc`, `residual_t1`, `residual_dc` | golden `-24` |
| Paint | `decode_stub.sv` MB0 gray = clamp(128+dc) | diagnostic only |
| Capture | nalu_scanner + MAXB=**48 B** RBSP | first residual ~17 B |
| Product present | host `reconISlice` → F1; `host_owns_fs` | unchanged |
| Fit (5CSEBA6, post-3.3k / current `Plex.fit.summary`) | ALM **22%** (9.4k/41.9k), M10K **74%** (407/553), bits 56%, DSP 33% | headroom: **ALM**, **DSP**; **M10K is tight** |

Golden Baseline F3 vector (`scripts/gen_test_annexb_real.py`):  
`mb0=0` (I_NxN), `qp=25`, first residual `tc=8 t1=3 coeff[0]=-24`.

---

## Architecture

### Dataflow target

```text
bitstream_fifo → nalu_scanner
                    ├─ SPS/PPS parsers (unchanged)
                    └─ slice path:
                         headers + first MB meta (existing bit-walk)
                         → residual CAVLC (extend ST_PLACE → full coeff[16])
                         → inv_quant 4×4
                         → idct4x4_add onto pred
                         → intra pred (I4 / later I16+chroma)
                         → pixel pack RGB565
                         → frame_store wr_en (F3; gated by host_owns_fs)
```

### Module split (new / grow)

| Module | Role | BRAM? |
|--------|------|-------|
| `slice_hdr_parser` | Keep header + CAVLC; **export full `coeff[0:15]`** after ST_PLACE | no (regs) |
| `inv_quant4` (new) | LevelScale × shift; FFmpeg `(level*qmul+32)>>6` | no |
| `idct4x4` (new) | Multi-cycle butterfly = `ff_h264_idct_add` | no |
| `intra_pred4` (new) | Modes 0–8 + DC avail; later I16/chroma | no |
| `mb_recon` (new or in stub) | Seq: pred → idct-add → MB buffer → RGB | first MB: regs only |
| `decode_stub` → evolve | Consume recon pixels instead of gray DC | no extra |
| Neighbour store (all-MB only) | Top row Y/UV + TC for nC | **yes, small** |

**Do not** store full YUV frame in M10K: write RGB565 into existing dual-bank `frame_store` as MBs finish.

---

## What stays logic-only (no new M10K)

Safe for 3.3l first-MB and most of all-MB math:

| Item | Why logic |
|------|-----------|
| `lev[0:15]`, `runv[0:15]`, `coeff[0:15]` | Already regs; 16×9 + 16×4 + 16×16 ≈ few hundred flops |
| Zigzag + LevelScale tables | 6×3 mf + scan — `case` / const ROM → LUT |
| Inv quant multiply | One 16×16 MAC; use **1 DSP** or shift-add by qp%6 |
| 4×4 IDCT | Pure add/shift butterflies; multi-cycle FSM (~8–16 cycles) |
| Intra4×4 modes 0–8 | Spec formulas; DC with hasA/hasL |
| I16 DC Hadamard | Same as host `invQuantHadamardDc4x4` (TRANSPOSE layout!) |
| Chroma 2×2 Hadamard | Small; host `invChromaDc2x2` |
| Within-MB neighbours | Left/above 4×4 of **same** MB = MB Y regs (256 B → MLAB or flops, not M10K) |
| First-MB unavailable | Outside: hasA=hasL=0 → DC=128 (no top-row RAM) |

### Must use BRAM (only for all-MB)

| Buffer | Size | M10K est | Notes |
|--------|------|----------|-------|
| Top luma row | 320 × 8 b | **1** | after each MB row |
| Top chroma U/V | 160 × 8 × 2 | **1** | 4:2:0 |
| TotalCoeff cache (nC) | ~ mb_w×4×5b × 2 rows | **≤1** | or MLAB |
| Live slice stream | **0 new** | 0 | keep reading `bitstream_fifo`; **do not** grow MAXB to whole IDR |
| Optional MB scratch | 16×16×8 | 0 | prefer regs/MLAB |

**Forbidden BRAM growth:** full-slice RBSP capture, dual YUV frame buffers, large coeff FIFOs.

---

## BRAM budget (Cyclone V 5CSEBA6)

Device: **553 M10K**, **5.66 Mbit**. Current fit: **407 / 553 (74%)**, **~3.17 Mbit (56%)**.

### Existing major consumers (approx)

| Block | Bits | ~M10K |
|-------|------|-------|
| `frame_store` 320×240×16×2 | 2,457,600 | ~240 |
| `bitstream_fifo` 32 KiB | 262,144 | ~26 |
| `audio_fifo` 2048×32 | 65,536 | ~7 |
| sys / ascal / misc | rest → 407 total | ~134 |

### Headroom

| Resource | Free now | 3.3l plan use | After 3.3l (est) |
|----------|----------|---------------|------------------|
| M10K | **146** | +0 first-MB; +2…4 all-MB | **~74–76%** |
| Block bits | ~2.5 Mbit | negligible for tables | still ≪ 100% |
| ALM | **77% free** | IDCT+pred+CAVLC walk ~1–3k ALM | ~25–30% |
| DSP | **75 free** | 0–2 for quant mul | ~33–35% |

### Risk controls if M10K creeps

1. Prefer MLAB for ≤2 Kib scratch.  
2. Never enlarge `bitstream_fifo` for decode.  
3. If neighbour rows push fit: drop chroma top-row first (luma-only paint for lab).  
4. Last resort: shrink audio DEPTH 2048→1024 (~3 M10K) — product impact, avoid.  
5. Do **not** dual-buffer YUV; RGB into existing banks only.

---

## Implementation phases

### 3.3l-0 — Host golden stubs (no FPGA) ✅

Lock math before RTL. **Done** in `tests/unit/test_idct_quant.cpp` (`make unit`).

- `dequant4x4` + `idct4x4_add` on synthetic DC and real first residual  
- First I_NxN 4×4: pred=128 (unavailable), qp=25, host coeffs → pixel golden  
- Hard-locked golden + printable dump: coeffs, dequant block, meanY, y[0..3][0..3]  
- Reuse `probeFirstI16Dc` / recon path; no new algorithm

**Locked golden (Baseline clip, first residual):**

| Field | Value |
|-------|-------|
| tc / t1 / qp | 8 / 3 / 25 |
| coeff scan | `-24 4 4 0 -4 0 -1 0 0 -1 1 0 1 0 0 0` |
| synth DC-only y | 62 (uniform; coeff0=-24) |
| y00 / mean4×4 | 73 / 62 |
| y 4×4 | `73 72 76 76` / `72 74 71 73` / `76 71 32 27` / `76 73 27 24` |

**Exit:** `make unit` includes quant/IDCT check; coeffs and 4×4 pixels known for golden clip.

### 3.3l-1 — Full first residual coeffs on FPGA (logic-only)

**Host prep (done, no Quartus):** goldens in `host/libmisterplex/h264_residual_gold.hpp`;
helpers in `h264_cavlc.hpp`; locked by `test_idct_quant`.

| Host / RTL | Value |
|------------|-------|
| coeff scan | `-24 4 4 0 -4 0 -1 0 0 -1 1 0 1 0 0 0` |
| `residualCsum8` | **20 / 0x14** = XOR of `sat_s8(coeff[i])` (host + ST_PLACE RTL) |
| Ports | `residual_coeff[0:15]` signed 9b through `stream_path`; `residual_csum[7:0]` |
| Status pack | `[103:96]=dc` `[111:104]=csum` `[127:112]=bytes[15:0]` (`Plex.sv`) |
| Host status | `push_frame` prints `res_csum=`; `CoreStatus::residual_csum` |
| Unit | `test_idct_quant` locks full-16 + csum; prints `FPGA_GOLD` lines |
| MAXB / M10K | **48** unchanged; **no new M10K** |
| RBF | **R-csum1** **`dabdaeb0`** FAIL ref; **R-csum2** lab **`4deaf6cc` HARD FAIL** **stream24 PACK_FAIL**. **R-csum3b** lab **`4d6ee356` HARD_FAIL** (+0x53/push; pack-likely-fixed + MAP_OK). **R-csum4** lab **`75da8bb1` HARD_FAIL MULTI_DRIVE** (L-csum-note25/28); sticky0x14=**0**; **NOT PACK_PROVEN**. **R-csum5 BUILD_OK** wall **441s** **NEW_RBF `8832824e`** full `8832824e483cf6613f82ee3ba3e592b3`; **PROVENANCE_UNTRUSTED**; **H-deploy-rcsum5 DEPLOY_OK**. **H-gate-rcsum5 HARD_FAIL (L-csum-note33); H-gate-rcsum5d DEFINITIVE (L-csum-note35):** lab **LOADED `8832824e`**; sticky0x14=**0**/12; **+0x53 multi-drive continues after sticky-pack sole** (A **0x16→0x69→0xbc→0x0f**; B **0x62→0xb5→0x08**); **res_dc PASS**; **NOT PACK_PROVEN**; thrash **`8832824e`/`75da8bb1`/`4d6ee356` forbidden**; soft-skip ≠ PASS; **Q-fix2 LIVE hold residual RTL**; **next RCA after Q-fix2 frees (not redeploy)**; **3l2 BLOCKED** |

Helpers: `satS8`, `residualCsum8`, `dumpResidualCoeffs`, `hostToFpgaResidualExpose`,
`residualCoeffsMatch`. Goldens: `residual_gold::{kCoeffScan,kCsum8,kDc,kY}`.

- Keep `residual_dc = satS8(coeff[0])` = **-24** for regression  
- Still **one** residual block; nC=0; MAXB=48 unchanged  

**Exit unit:** host dump == `residual_gold` (csum **0x14**, full-16). ✅  
**Exit HW (hard):** `test_f3_residual.sh` `res_dc=-24` + **hard** `res_csum=20` (soft-skip ≠ PASS).

#### HW evidence — residual csum (L-csum-note → note9…note20 → **L-csum-note21**, 2026-07-24)

| Item | Result |
|------|--------|
| Host unit | XOR sat8(full-16) = **0x14** / **20** locked (`test_idct_quant`, `residual_gold::kCsum8`) |
| Host progressive XOR (**A-csum-probe3**, reconfirm probe2) | Progressive sat8 XOR of residual_gold ends **0x14**; unique intermediates `{0xe8,0xec,0x14,0xeb,0x15}`. Lab **0x53 / 0xa6 / 0xf9 / 0x4c / 0x8b / 0xde / 0x31 ∉** residual XOR intermediates (nor final). Annex-B F3 size **6739** (`0x001A53`); low8 **0x53** is stream-size class, not residual-class. Report `/tmp/misterplex-agent-A-csum-probe3.txt` |
| Prior RBF `aa146c17` | `res_dc=-24` PASS; `res_csum` FAIL (raw[13]=`0x53` ≈ stream_bytes residue) |
| Prior RBF **`820484a6`** (Q-fix1) | FBAR green; res_dc=-24 OK; hard csum FAIL (raw[13] unstable 232/59/142) — **superseded** |
| **R-csum1 APPLIED** | **BUILD_OK** 12:17:04 exit=0; running XOR + ST_PLACE one-cycle `lev[]` combo recompute + preserve; RTL commit **`7bee0a6`**; log `/tmp/plex_quartus_rcsum1.log`; RBF full md5 **`dabdaeb0c5ae708c4fdbba388ba275b6`** |
| Lab RBF **`dabdaeb0`** | **H-deploy-rcsum1** one `DEPLOY_LOAD=menu`; **H-gate-rcsum1** independent reconfirm (no thrash); CORENAME=Plex; **FBAR PASS** (m1=82.9 m2=94.4); **`res_dc=-24` PASS** stable `0xE8`; **hard `res_csum=20` FAIL** |
| raw[13] **H-gate-rcsum1** (canonical) | **0x53 → 0xa6 → 0xf9** (dec 83/166/249); soft re-push → **0x4c** (76). **Δ = +0x53 per F3 Baseline push** (u8 wrap). Class **CSUM_UNSTABLE_ACCUM** |
| raw[13] H-deploy session | **0x8b → 0xde → 0x31** (139/222/49) — **same +0x53 step**, mid-stream base (bytes_in 391/417/444); and/or same FAIL class as H-gate |
| Soft-skip | `test_f3_residual.sh` EXIT=0 on mismatch (got 76/49/… want 20) — **still NOT hard PASS** |
| **stream[15:0]** | **OK independent of csum** — live pack `[127:112]` / raw[14:15]; advances with push; **≠** raw[13] on every probe |
| Not residual XOR partial | **A-csum-probe3** (reconfirm probe2): lab 0x53-class values ∉ progressive residual XOR table; final golden remains **0x14** |
| Not dc-only / arith | raw[13] never stuck at `0xE8`; never stale arith fold `0xEC` as final sticky; res_dc pack live |
| **Corrected RCA (R-csum-rtl3 / L-csum-note9) — stream24 LE alias** | Lab **+0x53/push on raw[13]** is **NOT** residual XOR intermediate. Algebra: `stream24 = raw[13]\|(raw[14]<<8)\|(raw[15]<<16)`. H-gate **dabdaeb0** clean probes: p1 **`e8 53 1a 00`** → stream24 **0x001A53 = 6739 = 1×** annex-B; p2 **`e8 a6 34 00`** → **0x0034A6 = 2×6739**; p3 **`e8 f9 4e 00`** → **0x004EF9 = 3×6739**. So `(N×6739)&0xFF = (N×0x53)&0xFF` → observed “accum +0x53”. Silicon pack is **pre-3.3l-1 layout**: `[103:96]=residual_dc`, stream occupies the csum byte (`residual_csum` **ABSENT** from status). Prior mis-class compared csum to host stream16&0xFF with wrong slice of raw. Host (**A-csum-probe3**): progressive residual XOR ends **0x14**; lab 0x53-class **∉** residual intermediates — orthogonal confirmation. Report: `/tmp/misterplex-agent-R-csum-rtl3.txt` |
| **RCA (R-csum-rtl2 compute class — secondary)** | One-cycle combinatorial XOR over a reg array is fragile on Q17 Lite (tmpc[] / lev[]). `residual_dc` green = scalar path. Multi-cycle scalar RMW is the compute fix **once pack is live**. |
| **Fix direction (R-csum-rtl3 APPLIED dirty; LOCK_OK@claim)** | **(1) Pack PRIMARY:** one `(*preserve*) reg [127:0] status_telem_r` filled every clk — `[103:96]<=residual_dc`, **`[111:104]<=residual_csum`**, `[127:112]<=stream_bytes_in[15:0]`; `status_in` from full vector. **(2) Compute:** multi-cycle RMW after place (`residual_csum ^= sat8(lev[csum_i])`); **DIAG force stripped** (no constant 0x14). Claim md5: Plex **`eb6b85410d4b13e8005e7afb01fea063`**, slice **`6ce28d6e4adde5fa8f9070d972f8ebdd`**. **Do not** re-open combo array XOR first. |
| **R-csum2 sole fit (historical)** | **BUILD_OK** START **12:30:36** / **BUILD END 12:37:32 exit=0** (claim SRC multi-cycle + **DIAG force-0x14** Plex `9b97b792` slice `eec44561` — **≠** rtl3 final). Host+lab RBF **`4deaf6cc`**. **Lab HARD FAIL** stream24 pack alias (not invent PASS). |
| **Lab RBF `4deaf6cc` (H-gate-rcsum2) — HARD FAIL stream24 pack alias** | Sole menu deploy done; **FBAR PASS**; **`res_dc=-24` PASS**; **hard `res_csum=20` FAIL**; expect **`e8 14 53 1a`**; observed **+0x53 family** / **`e8 53 1a 00`** class; **DIAG force-0x14 did NOT stick** → **PACK_FAIL**. Independent: **0xd5 → 0x7b → 0x21** (+0xA6=2×0x53); prior sole **0x89 → 0xdc → 0x2f** (+0x53/push). **Never sticky 0x14**. **zero thrash redeploy** |
| **DIAG force on 4deaf6cc** | Claim SRC had force-0x14; if pack OK → sticky raw[13]=0x14. Observed never 0x14 → pack path still broken (stream24 alias class). **Not** product PASS; **not** 3l2 unblock |
| **R-csum3 sole fit → FIT_DEAD_MID (L-csum-note12)** | START **2026-07-24T12:44:02** claim SRC **`eb6b8541`/`6ce28d6e`** (map OK); was LIVE mid phys-synth; **docker SIGTERM ~12:49** (**~5m** wall) → log **frozen** mid phys-synth; QPROCS empty; **no** Full Compilation / BUILD END; **no NEW_RBF** (still **`4deaf6cc`**). **NOT BUILD_OK**. Class **FIT_DEAD_MID** / INCOMPLETE. **Caution for 3b: avoid wrapper timeout kill.** |
| **MID-FIT WT DRIFT → intentional freeze (note10–14)** | Claim (SoT `/tmp/plex_quartus_rcsum3.claim/src_md5.txt`): **`eb6b8541…`/`6ce28d6e…`** LOCK_OK@claim. Live drifted: Plex **`ce1ef26c…`** / slice **`e45f98c4…`** — still **128b pack + multi-cycle; DIAG ABSENT**. **Stop thrash.** Drift tree **frozen intentional** = R-csum3b claim SRC. |
| **R-csum3b BUILD_OK (L-csum-note16)** | **BUILD_OK ~13:00** wall **~438s** Full Compilation **0e/33w**; freeze **`ce1ef26c`/`e45f98c4`**; **NEW_RBF `4d6ee356`** full `4d6ee35645c399f5036db220c3d4b07b`; log `/tmp/plex_quartus_rcsum3b.log`. BUILD_OK ≠ hard residual PASS. |
| **Lab RBF `4d6ee356` (H-deploy-rcsum3b / H-gate-rcsum3b / 3b2 / 3b3 / L-csum-note21)** | **HARD_FAIL reconfirm**. Sole menu deploy OK; lab md5 match; CORENAME=Plex; **FBAR PASS** (grid_off=7.0 force=82.9 bars=94.4); **`res_dc=-24` PASS**; **hard `res_csum=20` FAIL**. **Hard-gate SoT series:** (1) 3b raw **`e8 55 f1 01` → `e8 a8 0b 00` → `e8 fb 25 00`** csum **85→168→251**; (2) 3b2 (**no redeploy**) **`e8 4e 40 00` → `e8 a1 5a 00` → `e8 f4 74 00`** csum **78→161→244**; (3) **3b3** (**no redeploy**) **`e8 47 8f 00` → `e8 9a a9 00` → `e8 ed c3 00`** csum **71→154→237**. All **raw[13] additive +0x53/push** (**0x53 = 6739 lo**); **never sticky 0x14**. Class **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** (vs pure stream24: `raw[13] ≠ stream[7:0]`; not exact `e8 53 1a 00`) **but csum still wrong/unstable**. Soft residual mid-stream sample `e8 02 d7 01` (csum 0x02) = note17 soft path only — **not** hard-gate SoT. **soft-skip ≠ hard PASS**. **3l2 BLOCKED**. **no thrash**. |
| **MAP_OK (A-csum-map1 / map2 — L-csum-note21)** | Host/ARM parse of raw[12..15] matches `Plex.sv` status_telem: raw[12]=dc=`[103:96]`, raw[13]=csum=`[111:104]`, raw[14:15]=stream LE16=`[127:112]`. **Not** a host remap bug after 128b pack. Lab +0x53/push on raw[13] is the **FPGA residual_csum slot value** — **MAP_OK → blame FPGA** (compute / multi-drive / pack drive). |
| **R-csum4 BUILD_OK + H-gate-rcsum4 HARD_FAIL (L-csum-note24→note25)** | **BUILD_OK** claim **`94db41b7`/`9a2d10c5`** DIAG; wall **421s**; **NEW_RBF `75da8bb1`** full `75da8bb10e36b3e068d66a9ed053cd2c`; H-deploy **DEPLOY_OK**; F-prep **PACKAGE_OK**. **H-gate-rcsum4 HARD_FAIL:** sticky 0x14 **0/7**; series A **`e8 40 de 00` → `e8 93 f8 00` → `e8 e6 12 01`** (csum **0x40→0x93→0xe6**); series B **0x39→0x8c→0xdf→0x32** (wrap +0x53); **FBAR PASS** 7.0/82.9/94.4; res_dc=−24 PASS; ideal **`e8 14 xx` NEVER**. Class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN** — **DIAG force did not stick** (same as historical DIAG on **`4deaf6cc`**). Mid-fit **DRIFT_CRITICAL** caveat. **DIAG ≠ product PASS** / **≠ 3l2 unblock**. Evidence: `/tmp/misterplex-agent-H-gate-rcsum4.txt`, `/tmp/misterplex-H-gate-rcsum4-probes.txt`. |
| **R-csum-rtl5 FIT_GO + R-csum5 sole LIVE (L-csum-note26)** | **PRODUCT sticky pack** claimed at sole launch (docs agent: no Quartus / no RTL). **`Plex.sv`:** `st_res_word_sticky` latches `{residual_csum,residual_dc}` **only on `residual_ok_rise`**; status residual half `[111:96]` from sticky ONLY; stream ONLY in `[127:112]`. **`slice_hdr_parser.sv`:** **product `residual_csum <= csum_acc`** at ST_PLACE; **DIAG `8'h14` stripped** (failed silicon proof on **`75da8bb1`**). **Claim@launch** full md5 **Plex `6422fb9a829f3a9c70c6e39daeb69278`** / **slice `8e6af3bbbc95021cb4b1f2986db8b537`** (**DIAG=ABSENT**). **R-csum5 sole LIVE:** lock **`2026-07-24T13:31:58-05:00`**; log `/tmp/plex_quartus_rcsum5.log`; claim `/tmp/plex_quartus_rcsum5.claim`; build-core PID live; output RBF still **`75da8bb1`** stale until NEW. **NOT BUILD_OK** — do not invent Full Compilation success / NEW_RBF. Mid-fit **no RTL policy** (ZERO thrash under docker `-v`). Mid-fit **DRIFT_CRITICAL observed** (live WT ≠ claim; claim file may be overwritten) → provenance may be untrusted even if exit 0 later. residual_dc path unchanged (sat8(dcv)=-24). **Does NOT claim hard residual PASS / PACK_PROVEN / 3l2 unblock.** **After BUILD_OK (serial, not this agent):** NEW_RBF ∉ banned (incl **`75da8bb1`**) → ONE menu only → hard sticky **`raw[13]==0x14`** product (ideal **`e8 14 53 1a`**); reject **+0x53**; soft-skip ≠ PASS. Lab SoT **`75da8bb1` HARD_FAIL multi-drive** until that deploy. Evidence: `/tmp/misterplex-agent-R-csum-rtl5.txt`, `/tmp/misterplex-agent-R-csum5-build.txt`, `/tmp/plex_quartus_rcsum5.log`, `/tmp/plex_quartus_rcsum5.claim/src_md5.txt`, `/tmp/misterplex-agent-M-fitmon-rcsum5b.txt`, `/tmp/misterplex-agent-L-csum-note26.txt`. |
| **Mid-fit RTL thrash policy (L-csum-note23 → note29; R-csum5 LIVE)** | **STOP mid-fit RTL** — **ZERO** further `Plex.sv` / `slice_hdr_parser.sv` edits while exclusive LIVE (docker `-v …/Plex_MiSTer:/build` = poison). **Claim LAUNCH product sticky AUTHORITATIVE for map-era intent:** Plex **`6422fb9a…`** / slice **`8e6af3bb…` DIAG=ABSENT** @13:31:58. **Claim freeze now (overwritten mid-fit):** **`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT** — launch pack **overwritten** → **PROVENANCE_UNTRUSTED** until proven map/fit SRC match intentional freeze. **After BUILD_OK:** trust branch (LOCK_OK / accept-ambiguous / refreeze+re-sole) **before ONE menu**; thrash **`75da8bb1` forbidden**; soft-skip ≠ PASS; DIAG ≠ product; **3l2 BLOCKED**. Full card: § *STOP mid-fit RTL + R-csum5 LIVE provenance (L-csum-note29)*. Evidence: `/tmp/misterplex-agent-L-csum-note28.txt`…`note29.txt`, `/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt`, `/tmp/misterplex-agent-R-csum5-build.txt`, `/tmp/plex_quartus_rcsum5.claim/`, `/tmp/plex_quartus_rcsum5.log`. |
| **H-gate-rcsum4b HARD_FAIL reconfirm (L-csum-note28)** | Lab **`75da8bb1`** (NO redeploy) reconfirm **HARD residual_csum FAIL** class **MULTI_DRIVE_OR_STILL_FAIL**. sticky raw[13]==0x14 = **0/3** (never); res_dc=−24 PASS; series **0x85→0xd8→0x2b** (**+0x53/push** wrap); class match H-gate-rcsum4. **NOT PACK_PROVEN**. thrash-redeploy **FORBIDDEN**. Evidence: `/tmp/misterplex-agent-H-gate-rcsum4b.txt`, `/tmp/misterplex-H-gate-rcsum4b-probes.txt`. |
| **R-csum5 mid-fit claim overwrite DRIFT_CRITICAL (L-csum-note28)** | Sole **R-csum5** lock **13:31:58** product claim **`6422fb9a`/`8e6af3bb` DIAG=ABSENT**. Timeline: **13:32:27** live thrash **`6a5dcaaa`/`7d4a1d8b`**; **13:32:57** map SUCCESS (`st_res_word_sticky`=33, `res_pair_sticky`=0); **13:33:19** claim dir rewrite owner=R-csum-rtl5 **DIAG=PRESENT** residual_csum<=**8'h14** + **`res_pair_sticky`** note *prior product SUPERSEDED* — **CONTAMINATED** (not valid sole re-claim). Docker **`-v`** → **PROVENANCE risk** (map vs fit/disk sources). Any NEW_RBF attribute to **map-era PRODUCT** claim, **not** thrash DIAG md5s. **STOP further mid-fit RTL**; no kill Quartus; no deploy thrash **`75da8bb1`**. soft-skip ≠ PASS; DIAG sticky alone ≠ product PASS; **3l2 BLOCKED** until non-DIAG product sticky 0x14. Evidence: `/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt`, `/tmp/plex_quartus_rcsum5.claim/claim_meta.txt`, `/tmp/plex_quartus_rcsum5.claim/src_md5.txt`, `/tmp/misterplex-agent-R-csum5-build.txt`. |
| **Expected green probe** | Still want sticky raw[12..15] = **`e8 14 53 1a`** (dc + csum=0x14 + stream LE). Offline (**A-csum-probe4**): `e8 14 53 1a` HARD_PASS; `e8 53 1a 00` stream24 alias FAIL; lab hard SoT = additive +0x53/push with pack-likely-fixed slots. |
| **H-gate-rcsum5 HARD_FAIL on lab `8832824e` (L-csum-note33 / note34; 5d DEFINITIVE note35)** | **H-deploy-rcsum5** ONE menu **DEPLOY_OK** → lab md5 **`8832824e483cf6613f82ee3ba3e592b3`** match; CORENAME=Plex. **H-gate-rcsum5** (+5b) + **H-gate-rcsum5d DEFINITIVE**: **HARD residual_csum FAIL** class **MULTI_DRIVE_OR_STILL_FAIL**. sticky0x14=**0**/12 (**never**); ideal **`e8 14 xx` NEVER**. Series A **`e8 16…`→`e8 69…`→`e8 bc…`→`e8 0f…`** (0x16→0x69→0xbc→0x0f); series B **0x62→0xb5→0x08** — **+0x53/push each step** (annex len lo **0x53**). **res_dc PASS** (−24 / 0xe8 all hard). **FBAR soft** 7.0/82.9/94.4 ≠ product PASS. **NOT PACK_PROVEN** — sticky-pack sole **did not** latch host XOR **0x14**; multi-drive continues. **BUILD_OK+DEPLOY_OK+PACKAGE_OK ≠ hard residual PASS**. soft-skip ≠ PASS. thrash-redeploy **`8832824e` FORBIDDEN**. **Next = multi-drive RCA after Q-fix2 frees (not redeploy)**. **3l2 BLOCKED**. Evidence: `/tmp/misterplex-agent-H-gate-rcsum5d.txt` (DEFINITIVE), `/tmp/misterplex-agent-H-gate-rcsum5.txt`, `/tmp/misterplex-agent-H-gate-rcsum5b.txt`, `/tmp/misterplex-H-gate-rcsum5-probes.txt`, `/tmp/misterplex-H-gate-rcsum5-summary.txt`, `/tmp/misterplex-agent-H-deploy-rcsum5.txt`. |
| **L-csum-note34 — Q-fix2 LIVE residual freeze + thrash ban** | Exclusive **Q-fix2 LIVE** lock **`Q-fix2 2026-07-24T13:45:49-05:00`** (wide Fix-2 sole; lock file authoritative). **Residual RTL FREEZE** while wide fits. **Thrash FORBIDDEN** including: thrash-redeploy **`8832824e`/`75da8bb1`/`4d6ee356`**; **mid-fit residual edits during Q-fix2** (`Plex.sv`/`slice_hdr_parser.sv` ZERO under docker `-v`); residual re-sole / second exclusive while Q-fix2 holds slot. H-gate **`8832824e` HARD_FAIL multi-drive** remains SoT. soft-skip ≠ PASS; **3l2 BLOCKED**. Evidence: `/tmp/plex_quartus.lock`, `/tmp/misterplex-agent-L-csum-note34.txt`, `/tmp/misterplex-agent-H-gate-rcsum5.txt`. |
| **L-csum-note35 — HARD_FAIL 8832824e DEFINITIVE (H-gate-rcsum5d) + Q-fix2 hold residual RTL** | **H-gate-rcsum5d DEFINITIVE HARD_FAIL** consolidates 5+5b: sticky0x14=**0**/12; +0x53 family confirmed; **NOT PACK_PROVEN** / **MULTI_DRIVE_OR_STILL_FAIL**; res_dc PASS; FBAR soft ≠ PASS; **BUILD_OK+DEPLOY+PKG ≠ residual PASS**. Exclusive **Q-fix2 LIVE** **`Q-fix2 2026-07-24T13:45:49-05:00`** — **hold residual RTL** (no residual re-sole / mid-fit residual edits). **Thrash FORBIDDEN**. **3l2 BLOCKED**. Evidence: `/tmp/misterplex-agent-H-gate-rcsum5d.txt`, `/tmp/plex_quartus.lock`, `/tmp/misterplex-agent-L-csum-note35.txt`. |
| **L-csum-note36 — HARD_FAIL 8832824e + Q-SF2 mid-fit residual HOLD** | **Reconfirm (docs only; NO Quartus/deploy/RTL):** lab **`8832824e` HARD_FAIL DEFINITIVE** (H-gate-rcsum5d; sticky0x14=0/12; +0x53 **16/69/bc/0f/62/b5/08**; **NOT PACK_PROVEN**). Exclusive **Q-SF2 still LIVE mid-fit** lock **`Q-SF2 2026-07-24T13:45:49-05:00`** — **do not invent residual BUILD_OK / residual NEW_RBF**. **HOLD residual RTL mid-fit** (ZERO `Plex.sv`/`slice_hdr_parser.sv` thrash under docker `-v`; no residual re-sole). Host residual RBF stale **`8832824e`** until intentional residual sole. Thrash-redeploy **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**. soft-skip ≠ PASS; **3l2 BLOCKED**. Next residual = multi-drive RCA after exclusive free. Evidence: `/tmp/plex_quartus.lock`, `/tmp/misterplex-agent-H-gate-rcsum5d.txt`, `/tmp/misterplex-agent-L-csum-note36.txt`. |
| **L-csum-note37 — dual-track post Q-SF2 BUILD_OK+deploy+WIDE FAIL (exclusive FREE)** | **W:** Q-SF2 **BUILD_OK** NEW_RBF **`ec21e133`** wall **415s** SRC **`f1d9666a`**; lab LOADED; FBAR soft PASS; **WIDE FAIL 0.605** **PILLAR_320_of_529** (W-wide-gate-SF2/SF2b). PACKAGE_OK embeds ec21e133 ≠ PASS. **R residual HARD_FAIL both:** **`8832824e`** (H-gate-rcsum5d sticky0x14=0/12 +0x53) **and** LOADED **`ec21e133`** (H-gate-ec21 / H-res-ec21 sticky0x14=0 +0x53 **54…46**). **READY_TO_DEPLOY residual=NO**. Competing next soles parent picks **ONE:** **Q-SF3** (wide Fix-3) **OR** **R-csum6** (residual multi-drive). **ZERO second Quartus**; mid-fit thrash **FORBIDDEN**; ONE menu after BUILD_OK only; thrash ban residual banned + **no luck redeploy `ec21e133` for residual green**; WIDE ≠ residual PASS; hard expect sticky raw[13]==0x14 ≥2 reject +0x53; soft-skip ≠ PASS; **3l2 BLOCKED**. Evidence: `/tmp/misterplex-agent-L-csum-note37.txt`, H-gate-ec21, H-res-ec21, W-wide-gate-SF2/b, H-gate-rcsum5d, H-deploy-qSF2, M-fitmon-qSF2d. |
| **L-csum-note38 — parent FIT_GO=YES R-csum6 product Rank1+2+3 sole LIVE** | **H-gate-ec21 HARD_FAIL** lab **`ec21e133`** sticky0x14=0 +0x53 **08/5b/ae/01**. **W-wide-gate-fix2b FAIL** span=**0.605** **PILLAR_320_of_529** orthogonal OPEN. **R-multidrive-rca14 RCA_OK** (MULTI_DRIVE). Parent **FIT_GO=YES** product **Rank1+2+3 place_pulse sticky** exclusive sole **R-csum6 LIVE** — **ZERO invent BUILD_OK**. READY residual **NO**. **3l2 BLOCKED**. Evidence: L-csum-note38, H-gate-ec21, W-wide-gate-fix2b, R-multidrive-rca14, claim FIT_GO. |
| **L-csum-note40 — R-csum6 BUILD_OK NEW 94bbfe43 LOCK_OK (docs stamp)** | **R-csum6 DONE BUILD_OK** wall **438s** NEW_RBF **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` (∉ banned); **LOCK_OK** live==claim **`c7a847f7`/`ca62d02b`/`904e9b2e` DIAG=ABSENT**. Exclusive **IDLE**. **BUILD_OK≠PASS**. Next **ONE** menu **H-deploy-rcsum6** → hard sticky **raw[13]==0x14 ≥2** reject **+0x53**. thrash banned **FORBIDDEN**; **3l2 BLOCKED** until gate; **WIDE** orthogonal FAIL hold 0.605. Evidence: `/tmp/misterplex-agent-L-csum-note40.txt`, M-fitmon-rcsum6e, lock, claim. |
| **L-csum-note41 — H-gate-rcsum6 HARD_PASS product sticky 94bbfe43 (docs stamp)** | Lab LOADED **`94bbfe43`** **HARD_PASS** / **HARD_PASS_PRODUCT_STICKY** / **PACK_PROVEN**. sticky0x14=**7/7** plus53=**0** res_dc **7/7** ideal **e8 14 xx** 7/7; DIAG=**ABSENT** product XOR; DEPLOY_OK ONE menu; claim **c7a847f7/ca62d02b/904e9b2e** LOCK_OK. **3l2 residual hard-block LIFTED** (≠ WIDE PASS 0.605 ≠ full 3l2 product DONE). Next: 3l2 IDCT wire-up/gate; WIDE Fix-3 separate. Evidence: H-gate-rcsum6, H-gate-rcsum6-summary, H-deploy-rcsum6, L-csum-note41. |
| **L-csum-note39 — R-csum6 LIVE claim freeze Rank1+2+3 DIAG strip** | Exclusive **R-csum6 LIVE** lock **`2026-07-24T14:02:42-05:00`**. Claim **FIT_GO=YES DIAG=ABSENT** pack **Rank1+2+3** (`st_res_word_sticky`+`residual_place_pulse`+`status_telem_masked`). Freeze md5 **Plex `c7a847f7…` / slice `ca62d02b…` / stream `904e9b2e…`**. **mid-fit thrash FORBIDDEN**. **READY_TO_DEPLOY=NO** until **BUILD_OK+LOCK_OK**. After: **ONE** menu **H-deploy-rcsum6**. Thrash banned **`8832824e`/`75da8bb1`/`4d6ee356`**; NEW must **≠ `ec21e133`**. soft-skip ≠ PASS; **3l2 BLOCKED**. Do not invent BUILD_OK. Evidence: `/tmp/misterplex-agent-L-csum-note39.txt`, `/tmp/plex_quartus_rcsum6.claim/`, `/tmp/plex_quartus.lock`, `/tmp/misterplex-loop-status.txt`. |
| **3.3l-2 paint** | **Residual hard-block LIFTED** on product lab **`94bbfe43`** (L-csum-note41 / H-gate-rcsum6 **HARD_PASS** sticky0x14=7/7 plus53=0 res_dc PASS ideal e8 14). May start **3.3l-2 product IDCT wire-up / gate**. **≠ full 3l2 product DONE** (paint/fit still open). **≠ WIDE PASS** (historical **ec21e133** span=**0.605** still FAIL). Historical residual FAIL refs: **`8832824e`/`75da8bb1`/`4d6ee356`/`ec21e133`** thrash **FORBIDDEN**. soft-skip ≠ PASS. |
| **Do not** | Thrash-redeploy **`8832824e`** / **`4d6ee356`** / **`75da8bb1`** / **`4deaf6cc`** / **`dabdaeb0`**; invent hard PASS / BUILD_OK / **DEPLOY_OK**; treat soft-skip or DIAG-force as product PASS; start paint / `files.qip` while csum red; second menu for luck; redeploy for residual luck after HARD_FAIL; **mid-fit residual RTL edits while exclusive Q-fix2 LIVE** |

**Post-deploy interpretation (H-gate-rcsum4/4b HARD_FAIL / L-csum-note25→note28):** Lab **LOADED DIAG `75da8bb1` HARD_FAIL multi-drive SoT until new deploy**. Sole menu **DEPLOY_OK** (H-deploy-rcsum4); **F-prep PACKAGE_OK**; **FBAR PASS**; res_dc=−24 PASS; **hard `res_csum` FAIL** — **raw[13] additive +0x53/push** (**0x40→0x93→0xe6**; series B wrap **0x39…**; **H-gate-rcsum4b reconfirm 0x85→0xd8→0x2b**); **never sticky 0x14** (0/7 + 0/3); ideal **`e8 14 xx` NEVER**. Class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN** — **DIAG force did not stick**. Prior product **`4d6ee356` HARD_FAIL** same +0x53 family (pack-likely-fixed + MAP_OK). **soft-skip ≠ hard PASS**; **DIAG ≠ product PASS**; **3l2 BLOCKED**; **no thrash `75da8bb1`/`4d6ee356`**. **R-csum5 sole LIVE (note28):** started **13:31:58** product claim **`6422fb9a`/`8e6af3bb` DIAG=ABSENT**; mid-fit claim overwrite ~**13:33** → **`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT** SUPERSEDES stamp (**CONTAMINATED** / **PROVENANCE risk** under docker `-v`); **not BUILD_OK**; **STOP mid-fit RTL**; after BUILD_OK **evaluate trust** then **ONE menu** only. Evidence: `/tmp/misterplex-agent-H-gate-rcsum4.txt`, `/tmp/misterplex-agent-H-gate-rcsum4b.txt`, `/tmp/misterplex-H-gate-rcsum4b-probes.txt`, `/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt`, `/tmp/misterplex-agent-R-csum5-build.txt`, `/tmp/plex_quartus_rcsum5.claim/`, `/tmp/plex_quartus_rcsum5.log`, `/tmp/misterplex-agent-L-csum-note24.txt`…`note28.txt`.

##### L-csum-note28 — mid-fit R-csum5 claim overwrite + lab HARD_FAIL stamp (docs only)

**When:** 2026-07-24 (CDT). **Agent:** L-csum-note28. **No Quartus / no deploy / no RTL.**

| Fact | Evidence |
|------|----------|
| Lab **`75da8bb1`** H-gate-rcsum4 + **4b HARD_FAIL MULTI_DRIVE** | sticky0x14=**0**; **+0x53/push**; **NOT PACK_PROVEN** |
| R-csum5 sole start **13:31:58** | product claim **Plex `6422fb9a…` / slice `8e6af3bb…` DIAG=ABSENT** |
| Live thrash **~13:32:27** | WT **`6a5dcaaa`/`7d4a1d8b`** |
| Claim rewrite **~13:33:19** | **DIAG=PRESENT** residual_csum<=**8'h14** + **`res_pair_sticky`**; note *prior product SUPERSEDED* — **CONTAMINATED** |
| Map-era identity | map.rpt **`st_res_word_sticky`=33**, **`res_pair_sticky`=0** → fit places **PRODUCT** sticky, not thrash DIAG |
| Docker `-v` | mid-fit host thrash poisons container `/build` → **PROVENANCE risk** (map SRC ≠ disk SRC) |

**Policy (frozen):**
1. **STOP** further mid-fit RTL while exclusive LIVE.
2. After **BUILD_OK**: **evaluate trust** (attribute NEW_RBF to map-era product claim **or** re-sole clean freeze) **before ONE menu**.
3. Thrash-redeploy banned RBF (**`75da8bb1`**, **`4d6ee356`**, …) **forbidden**.
4. **soft-skip ≠ PASS**; **DIAG sticky alone ≠ product PASS**.
5. **3l2 BLOCKED** until **non-DIAG product sticky 0x14**.

Primary audit: `/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt`. Lab reconfirm: `/tmp/misterplex-agent-H-gate-rcsum4b.txt`.

##### +0x53/push table (H-gate-rcsum3b + 3b2 + 3b3 lab **`4d6ee356` HARD_FAIL reconfirm** — L-csum-note21)

| Probe | res_dc | res_csum | raw[13] | stream[15:0] (raw[14:15] LE) | bytes_in | Δ csum | hard |
|-------|--------|----------|---------|------------------------------|----------|--------|------|
| H-gate-rcsum3b 1 | −24 / `0xE8` | 85 | **`0x55`** | **`0x01f1`** OK (≠ raw[13]) | 497 | — | **FAIL** |
| H-gate-rcsum3b 2 | −24 / `0xE8` | 168 | **`0xa8`** | **`0x000b`** OK | 11 | **+0x53** | **FAIL** |
| H-gate-rcsum3b 3 | −24 / `0xE8` | 251 | **`0xfb`** | **`0x0025`** OK | 37 | **+0x53** | **FAIL** |
| H-gate-rcsum3b2 1 | −24 / `0xE8` | 78 | **`0x4e`** | **`0x0040`** OK (≠ raw[13]) | 64 | — | **FAIL** |
| H-gate-rcsum3b2 2 | −24 / `0xE8` | 161 | **`0xa1`** | **`0x005a`** OK | 90 | **+0x53** | **FAIL** |
| H-gate-rcsum3b2 3 | −24 / `0xE8` | 244 | **`0xf4`** | **`0x0074`** OK | 116 | **+0x53** | **FAIL** |
| H-gate-rcsum3b3 1 | −24 / `0xE8` | 71 | **`0x47`** | **`0x008f`** OK (≠ raw[13]) | 143 | — | **FAIL** |
| H-gate-rcsum3b3 2 | −24 / `0xE8` | 154 | **`0x9a`** | **`0x00a9`** OK | 169 | **+0x53** | **FAIL** |
| H-gate-rcsum3b3 3 | −24 / `0xE8` | 237 | **`0xed`** | **`0x00c3`** OK | 195 | **+0x53** | **FAIL** |

**Algebra (L-csum-note21 reconfirm):** Δ = **+0x53/push** where **0x53 = 6739 & 0xFF** (F3 Baseline annex-B size lo). Hard SoT series: 3b **`e8 55 f1 01` / `e8 a8 0b 00` / `e8 fb 25 00`** → 3b2 **`e8 4e 40 00` / `e8 a1 5a 00` / `e8 f4 74 00`** → 3b3 **`e8 47 8f 00` / `e8 9a a9 00` / `e8 ed c3 00`**. **Never sticky `raw[13]==0x14`**. **pack-likely-fixed** vs pure stream24 (**raw[13] ≠ stream[7:0]**; not exact `e8 53 1a 00`) — residual_csum slot live but **wrong + additive**. **MAP_OK**. Class **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM**. EXPECT green **`e8 14 53 1a`**. **soft-skip ≠ hard PASS**. **3l2 BLOCKED**. **Next: `R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute** (DIAG on raw[13] → pack proven; else multi-drive). **No thrash-redeploy `4d6ee356`**.

**H-gate-rcsum1 probes (canonical silicon SoT for dabdaeb0 FAIL — independent, no redeploy):**
- Report: `/tmp/misterplex-agent-H-gate-rcsum1.txt`
- Probes file: `/tmp/misterplex-H-gate-rcsum1-probes.txt`
- PROBE1: `res_dc=-24 res_csum=83` raw `e8 53 1a 00` (bytes_in=26) → HARD_FAIL
- PROBE2: `res_dc=-24 res_csum=166` raw `e8 a6 34 00` (bytes_in=52) → HARD_FAIL
- PROBE3: `res_dc=-24 res_csum=249` raw `e8 f9 4e 00` (bytes_in=78) → HARD_FAIL
- Soft: `res_csum=76` soft-skip EXIT=0 (**not** hard PASS) — 0xf9+0x53 → 0x14c → u8 0x4c
- Sequence: **0x53 / 0xa6 / 0xf9** (+**0x53**/push)

##### +0x53/push table (H-gate dabdaeb0 FAIL — L-csum-note4)

| Probe | res_dc | res_csum | raw[13] | stream[15:0] (raw[14:15] LE) | bytes_in | Δ csum | hard |
|-------|--------|----------|---------|------------------------------|----------|--------|------|
| H-gate 1 | −24 / `0xE8` | 83 | **`0x53`** | **`0x001a`** OK | 26 | — | **FAIL** |
| H-gate 2 | −24 / `0xE8` | 166 | **`0xa6`** | **`0x0034`** OK | 52 | **+0x53** | **FAIL** |
| H-gate 3 | −24 / `0xE8` | 249 | **`0xf9`** | **`0x004e`** OK | 78 | **+0x53** | **FAIL** |
| H-gate soft | −24 / `0xE8` | 76 | **`0x4c`** | advances OK | (soft) | **+0x53** wrap | soft-skip ≠ PASS |
| H-deploy 1 | −24 / `0xE8` | 139 | **`0x8b`** | **`0x0187`** OK | 391 | — | **FAIL** |
| H-deploy 2 | −24 / `0xE8` | 222 | **`0xde`** | **`0x01a1`** OK | 417 | **+0x53** | **FAIL** |
| H-deploy soft | −24 / `0xE8` | 49 | **`0x31`** | **`0x01bc`** OK | 444 | **+0x53** wrap | soft-skip ≠ PASS |

**Corrected stream24 LE alias (L-csum-note9 / R-csum-rtl3):** host parsers that only read `stream16` from raw[14:15] saw “stream OK ≠ raw[13],” but **raw[13:15] LE24 == N×6739** exactly on clean dabdaeb0 probes — residual_csum is **absent** and stream occupies the csum slot (pre-3.3l-1 layout). +0x53/push is **stream-size class**, **not** residual XOR intermediate. Expected product green: **`e8 14 53 1a`**.

##### +0x53/push table (H-gate-rcsum2 lab **`4deaf6cc`** — L-csum-note7)

| Probe | res_dc | res_csum | raw[13] | stream[15:0] | bytes_in | Δ csum | hard |
|-------|--------|----------|---------|--------------|----------|--------|------|
| H-gate-rcsum2 indep 1 | −24 / `0xE8` | 213 | **`0xd5`** | OK LE | 510 | — | **FAIL** |
| H-gate-rcsum2 indep 2 | −24 / `0xE8` | 123 | **`0x7b`** | OK LE | 51 | **+0xA6** (2×0x53) | **FAIL** |
| H-gate-rcsum2 indep 3 | −24 / `0xE8` | 33 | **`0x21`** | OK LE | 104 | **+0xA6** | **FAIL** |
| Prior sole same RBF | −24 | … | **`0x89 → 0xdc → 0x2f`** | OK | … | **+0x53**/push | **FAIL** |

**DIAG force-0x14 no stick → PACK_FAIL / stream24 alias.** Expected sticky `raw[13]==0x14` if pack OK; observed never 0x14. **res_dc PASS**; **FBAR PASS**. **soft-skip ≠ hard PASS**. **3l2 BLOCKED**. Historical: **R-csum3 FIT_DEAD_MID ~5m**; **R-csum3b BUILD_OK** → **`4d6ee356`**.

**(Canonical hard-probe table for lab `4d6ee356` is above — L-csum-note21: raw[13] additive +0x53/push; 0x53=6739 lo; pack-likely-fixed + MAP_OK; not sticky 0x14; HARD_FAIL; 3l2 BLOCKED; soft-skip ≠ PASS; next R-csum4 DIAG sticky-0x14 pack-vs-compute.)**

**H-deploy-rcsum1 probes (earlier same RBF; and/or unstable 0x8b/de/31 class):**
- Reports: `/tmp/misterplex-agent-H-deploy-rcsum1.txt`, `/tmp/misterplex-agent-H-rcsum-gate.txt`
- Probes file: `/tmp/misterplex-H-deploy-rcsum1-probes.txt`
- PROBE1: `res_dc=-24 res_csum=139` raw `e8 8b 87 01` (bytes_in=391) → HARD_FAIL
- PROBE2/3: `res_dc=-24 res_csum=222` raw `e8 de a1 01` (bytes_in=417)
- PROBE4 soft: `res_csum=49` soft-skip EXIT=0 (**not** hard PASS)
- Soft log: `/tmp/misterplex-H-deploy-rcsum1-residual-soft.log`

**Host progressive XOR (A-csum-probe3 — reconfirm probe2; offline, no SPI thrash):**
- Reports: `/tmp/misterplex-agent-A-csum-probe3.txt` (primary), `/tmp/misterplex-agent-A-csum-probe2.txt`
- Final kCsum8 = **0x14** (host XOR→**0x14**); unique `{0xe8,0xec,0x14,0xeb,0x15}`
- Lab +0x53 class **0x53/0xa6/0xf9/0x4c** and H-deploy **0x8b/0xde/0x31 ∉** residual progressive intermediates
- Ideal post-fix raw[12..15] after one 6739 B push: **`e8 14 53 1a`** (lab dabdaeb0 / 4deaf6cc show `e8 53 1a 00` stream24-alias class instead)

**RTL RCA + fix (R-csum-rtl3 APPLIED dirty — L-csum-note9):**
- Report: `/tmp/misterplex-agent-R-csum-rtl3.txt` (pack RCA HIGH confidence)
- **Pack PRIMARY:** full 128b `status_telem_r` — force residual_csum into `[111:104]` same registered vector as stream
- **Compute:** multi-cycle scalar RMW after place; DIAG force **removed**
- Fingerprints: Plex `eb6b85410d4b13e8005e7afb01fea063`, slice `6ce28d6e4adde5fa8f9070d972f8ebdd`
- Prior rtl2 note (combo XOR): `/tmp/misterplex-agent-R-csum-rtl2.txt`

##### stream24 LE alias algebra (H-gate dabdaeb0 SoT — L-csum-note9)

| Probe | raw[12..15] | stream24 LE (`raw[13:15]`) | = N×6739? | residual_csum live? |
|-------|-------------|----------------------------|-----------|---------------------|
| 1 | `e8 53 1a 00` | **0x001A53 = 6739** | **1×** | **NO** (stream lo8 in csum slot) |
| 2 | `e8 a6 34 00` | **0x0034A6 = 13478** | **2×** | **NO** |
| 3 | `e8 f9 4e 00` | **0x004EF9 = 20217** | **3×** | **NO** |
| **expect green** | **`e8 14 53 1a`** | stream LE16 in raw[14:15]=`53 1a`; **raw[13]=0x14** sticky | N/A | **YES** |

**4deaf6cc HARD FAIL same class** (H-gate-rcsum2): never sticky 0x14; +0x53 family / +0xA6; DIAG force no stick → PACK_FAIL / stream24 alias still owns raw[13].

Peer reports: `/tmp/misterplex-agent-H-gate-rcsum2.txt`, `/tmp/misterplex-H-gate-rcsum2-probes.txt`,
`/tmp/misterplex-agent-H-gate-rcsum1.txt`, `/tmp/misterplex-agent-R-csum-rtl3.txt`,
`/tmp/misterplex-agent-R-csum-rtl2.txt`, `/tmp/misterplex-agent-A-csum-probe3.txt`,
`/tmp/plex_quartus_rcsum3.log`, `/tmp/misterplex-agent-L-csum-note7.txt`,
`/tmp/misterplex-agent-L-csum-note8.txt`, `/tmp/misterplex-agent-L-csum-note9.txt`,
`/tmp/misterplex-agent-L-csum-note11.txt`, `/tmp/misterplex-agent-L-csum-note12.txt`,
`/tmp/misterplex-agent-L-csum-note13.txt`, `/tmp/misterplex-agent-L-csum-note14.txt`,
`/tmp/misterplex-agent-L-csum-note17.txt`, `/tmp/misterplex-agent-L-csum-note18.txt`,
`/tmp/misterplex-agent-L-csum-note19.txt`, `/tmp/misterplex-agent-L-csum-note20.txt`,
`/tmp/misterplex-agent-L-csum-note21.txt`,
`/tmp/misterplex-agent-L-csum-note22.txt` (R-csum4 LIVE + bisect + mid-fit drift),
`/tmp/misterplex-agent-L-csum-note23.txt` (mid-fit thrash policy + ZERO RTL while exclusive LIVE),
`/tmp/misterplex-agent-L-csum-note24.txt` (R-csum4 BUILD_OK 75da8bb1; lab LOADED; F-prep PACKAGE_OK),
`/tmp/misterplex-agent-L-csum-note25.txt` (H-gate-rcsum4 HARD_FAIL 75da8bb1; NOT PACK_PROVEN; +0x53 0x40/0x93/0xe6; FBAR PASS; DIAG no stick; multi-drive; next sticky residual-pair latch + sole R-csum5; 3l2 BLOCKED),
`/tmp/misterplex-agent-L-csum-note26.txt` (R-csum5 sole LIVE sticky pack product claim@launch 6422fb9a/8e6af3bb DIAG=ABSENT; not BUILD_OK; mid-fit no RTL; after BUILD_OK ONE menu expect sticky 0x14 product; lab 75da8bb1 HARD_FAIL multi-drive SoT; 3l2 BLOCKED; thrash forbidden; soft-skip≠PASS),
`/tmp/misterplex-agent-L-csum-note27.txt` (ZERO mid-fit RTL while R-csum5 LIVE; claim launch 6422fb9a/8e6af3bb AUTHORITATIVE; concurrent DIAG re-inject DRIFT_CRITICAL; post-fit gate once or refreeze; no thrash 75da8bb1; soft-skip ≠ PASS),
`/tmp/misterplex-agent-L-csum-note28.txt` (mid-fit claim overwrite + H-gate-rcsum4b HARD_FAIL MULTI_DRIVE; 13:31:58 product 6422fb9a/8e6af3bb DIAG=ABSENT; ~13:33 DIAG PRESENT 6a5dcaaa/7d4a1d8b SUPERSEDES CONTAMINATED; docker -v PROVENANCE; STOP mid-fit RTL; evaluate trust before ONE menu; 3l2 BLOCKED),
`/tmp/misterplex-agent-L-csum-note29.txt` (STOP mid-fit RTL + PROVENANCE_UNTRUSTED; claim now 6a5dcaaa/7d4a1d8b DIAG; launch product overwritten; no further Plex/slice edits until exclusive free; trust before ONE menu; thrash 75da8bb1 forbidden; soft-skip≠PASS; DIAG≠product; 3l2 BLOCKED),
`/tmp/misterplex-agent-L-csum-note30.txt` (R-csum5 still LIVE ~fit historical; claim/live 6a5dcaaa/7d4a1d8b DIAG PRESENT; PROVENANCE_UNTRUSTED vs launch; STOP mid-fit RTL; thrash 75da8bb1 forbidden; soft-skip≠PASS; 3l2 BLOCKED),
`/tmp/misterplex-agent-L-csum-note31.txt` (R-csum5 BUILD_OK NEW_RBF 8832824e; PROVENANCE_UNTRUSTED),
`/tmp/misterplex-agent-L-csum-note32.txt` (accept-ambiguous one-menu serial historical),
`/tmp/misterplex-agent-L-csum-note33.txt` (H-gate-rcsum5 HARD_FAIL 8832824e; sticky0x14=0; +0x53 multi-drive after sticky-pack sole; res_dc PASS; NOT PACK_PROVEN; thrash forbidden; next RCA not redeploy; soft-skip≠PASS; 3l2 BLOCKED),
`/tmp/misterplex-agent-L-csum-note34.txt` (H-gate HARD_FAIL 8832824e multi-drive reconfirm; exclusive Q-fix2 LIVE residual RTL FREEZE; thrash forbidden incl mid-fit residual edits during Q-fix2; soft-skip≠PASS; 3l2 BLOCKED),
`/tmp/misterplex-agent-L-csum-note35.txt` (HARD_FAIL 8832824e DEFINITIVE H-gate-rcsum5d; Q-fix2 LIVE hold residual RTL; thrash forbidden; 3l2 BLOCKED),
`/tmp/misterplex-agent-H-gate-rcsum5d.txt` (DEFINITIVE HARD_FAIL MULTI_DRIVE 8832824e sticky0x14=0/12 +0x53 family NOT PACK_PROVEN),
`/tmp/misterplex-agent-H-gate-rcsum5.txt` (HARD_FAIL MULTI_DRIVE 8832824e sticky0x14=0/7 series A 0x16→0x69→0xbc→0x0f B 0x62→0xb5→0x08),
`/tmp/misterplex-agent-H-gate-rcsum5b.txt` (HARD_FAIL reconfirm 8832824e),
`/tmp/misterplex-H-gate-rcsum5-probes.txt`, `/tmp/misterplex-H-gate-rcsum5-summary.txt`,
`/tmp/misterplex-agent-H-deploy-rcsum5.txt` (DEPLOY_OK 8832824e ONE menu),
`/tmp/misterplex-agent-M-fitmon-rcsum5d.txt` (BUILD_OK Full Compilation wall 441s NEW_RBF 8832824e),
`/tmp/plex_quartus.lock` (Q-fix2 LIVE 2026-07-24T13:45:49-05:00 — hold residual RTL),
`/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt` (DRIFT_CRITICAL mid-fit audit; map-era product sticky AUTHORITATIVE),
`/tmp/misterplex-agent-H-gate-rcsum4b.txt` (HARD_FAIL reconfirm 75da8bb1 MULTI_DRIVE sticky0x14=0 +0x53/push NOT PACK_PROVEN),
`/tmp/misterplex-agent-R-csum-rtl5.txt` (FIT_GO product sticky pack; DIAG ABSENT),
`/tmp/misterplex-agent-R-csum5-build.txt` (sole LIVE; launch claim product 6422fb9a/8e6af3bb),
`/tmp/misterplex-agent-H-gate-rcsum4.txt`, `/tmp/misterplex-H-gate-rcsum4-probes.txt`,
`/tmp/misterplex-H-gate-rcsum4-fbar.log`, `/tmp/misterplex-agent-H-deploy-rcsum4.txt`,
`/tmp/misterplex-agent-R-csum-midfit-rcsum4b.txt` (DRIFT_CRITICAL; docker `-v` poison),
`/tmp/plex_quartus_rcsum3b.log`, `/tmp/plex_quartus_rcsum4.log`, `/tmp/plex_quartus_rcsum5.log`,
`/tmp/misterplex-agent-R-csum-rtl4c.txt`, `/tmp/misterplex-agent-R-csum4-build.txt`,
`/tmp/misterplex-agent-A-csum-probe4.txt`,
`/tmp/misterplex-agent-A-csum-map1.txt`, `/tmp/misterplex-agent-A-csum-map2.txt`,
`/tmp/misterplex-agent-H-gate-rcsum3b.txt`, `/tmp/misterplex-H-gate-rcsum3b-probes.txt`,
`/tmp/misterplex-agent-H-gate-rcsum3b2.txt`, `/tmp/misterplex-H-gate-rcsum3b2-probes.txt`,
`/tmp/misterplex-agent-H-gate-rcsum3b3.txt`, `/tmp/misterplex-H-gate-rcsum3b3-probes.txt`.

#### Post–R-csum4 READY_TO_DEPLOY + sole-deploy protocol (H-proto-rcsum4f)

> **READY_TO_DEPLOY=NO** while **R-csum4 LIVE** and/or **DRIFT_CRITICAL**.
> Evidence card: `/tmp/misterplex-agent-H-proto-rcsum4f.txt` (supersedes arming cards for gate semantics).
>
> **Write-time harvest (~13:22 CDT 2026-07-24):** log `/tmp/plex_quartus_rcsum4.log` shows
> **Full Compilation successful** (0e/35w) + **Compile wall 421s (exit 0)** + docker/qprocs idle
> → **BUILD_OK evidence present**. NEW_RBF **`75da8bb1`** full `75da8bb10e36b3e068d66a9ed053cd2c`
> (**∉ banned**). Claim freeze **`94db41b7`/`9a2d10c5`** DIAG PRESENT; **live WT ≠ claim**
> (DRIFT_CRITICAL — multi mid-fit thrash under docker `-v`) → **LOCK_OK fail**.
> Therefore **READY_TO_DEPLOY=NO** until parent **accept-ambiguous** for this bitstream **or**
> proven LOCK_OK provenance. **NO thrash-redeploy `4d6ee356`**. Lab product SoT still
> **`4d6ee356` HARD_FAIL** until a sole menu of a non-banned NEW RBF is authorized.
> **DIAG ≠ product PASS**; **3l2 BLOCKED**.

**Hard NO (READY_TO_DEPLOY stays NO):**

| Condition | Meaning |
|-----------|---------|
| **R-csum4 LIVE** | `quartus_*` or docker compile still up; no Full Compilation + exit 0 yet |
| **DRIFT_CRITICAL** | live `Plex.sv` / `slice_hdr_parser.sv` md5 ≠ claim `/tmp/plex_quartus_rcsum4.claim/src_md5.txt` **and** no parent **accept-ambiguous** flag |
| NEW_RBF ∈ banned | `{4d6ee356,4deaf6cc,dabdaeb0,820484a6,6db3a4d8,aa146c17}` |
| Invented BUILD_OK | partial log / process liveness without Full Compilation success + exit 0 |

**After BUILD_OK (required before READY_TO_DEPLOY=YES):**

1. Full Compilation successful + exit 0 + exclusive idle (log `/tmp/plex_quartus_rcsum4.log`).
2. **NEW_RBF ∉ banned** (record full md5 + prefix8). Example OK: **`75da8bb1…`**.
3. Prefer claim **LOCK_OK** (end-of-fit live == claim). If AMBIG/DRIFT_CRITICAL → require
   explicit parent **`accept-ambiguous`** note naming the NEW md5; else READY_TO_DEPLOY=**NO**.
4. DIAG PRESENT expected for this bisect (`residual_csum <= 8'h14`); record intent.
5. **ONE promote** of the same NEW bitfile → `releases/Plex.rbf` + `fpga/Plex_MiSTer/releases/Plex.rbf`
   (H-deploy-rcsum3b pattern). Abort promote if banned.
6. **ONE** safe deploy only:
   ```bash
   DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh
   # log: /tmp/misterplex-H-deploy-rcsum4-deploy.log
   ```
   - **Never** `killall -9` + load_core thrash loops
   - **Never** thrash-redeploy **`4d6ee356`** (or any banned md5)
   - **Never** second menu same NEW md5 for residual luck
7. Lab verify: remote md5 == HOST_OUT; CORENAME=Plex.
8. FBAR then hard residual ≥2 probes (owner **H-gate-rcsum4**):
   - **EXPECT DIAG sticky:** `raw[13]==0x14` ≥2 → **`e8 14 xx`** class (ideal product-shape
     `e8 14 53 1a` when stream LE tracks 6739)
   - **REJECT:** +0x53/push family (lab 4d6ee356 SoT); pure `e8 53 1a 00` stream24 alias
9. **DIAG ≠ product PASS** — sticky 0x14 under force = **PACK_PROVEN** only; strip DIAG +
   restore `csum_acc` on a **new product sole** before claiming product residual green.
10. **3l2 BLOCKED** until **non-DIAG product** sticky 0x14 ≥2 + FBAR + res_dc=-24.
    soft-skip EXIT=0 ≠ hard PASS; BUILD_OK+DEPLOY_OK ≠ hard residual PASS.

**Owners:** promote+menu = sole **H-deploy-rcsum4**; gate = sole **H-gate-rcsum4**.
Protocol agents do **not** deploy/fit/edit RTL.

#### Post–lab 75da8bb1 branch protocol (H-proto-rcsum4g)

> **STATUS 2026-07-24 ~13:26 CDT:** Lab **LOADED `75da8bb1`** (full
> `75da8bb10e36b3e068d66a9ed053cd2c`) — **H-deploy-rcsum4 PROMOTE_OK | DEPLOY_OK**
> (ONE menu). Card: `/tmp/misterplex-agent-H-proto-rcsum4g.txt`.
>
> **H-gate-rcsum4 class (probes):** FBAR **PASS** (7.0/82.9/94.4); res_dc=−24 **PASS**;
> hard `res_csum` **HARD_FAIL** — **never sticky 0x14**; raw[13] additive **+0x53/push**
> series **0x40 → 0x93 → 0xe6** (Δ=+0x53/+0x53; 0x53=6739&0xFF); preflight idle **0xed**;
> pack-likely-fixed (csum ≠ stream lo). Evidence:
> `/tmp/misterplex-H-gate-rcsum4-probes.txt`, `/tmp/misterplex-H-gate-rcsum4-fbar.log`.
>
> **ACTIVE BRANCH B — still +0x53 / NOT PACK_PROVEN.** **R-csum-rtl5 FIT_GO** sticky residual-pair
> latch applied; **R-csum5 sole LIVE (L-csum-note26)** claim@launch product **`6422fb9a`/`8e6af3bb`**
> DIAG=ABSENT — **not BUILD_OK** yet; mid-fit **no RTL**. After BUILD_OK → ONE menu → hard sticky
> 0x14 product — **not** re-DIAG thrash; **no thrash-redeploy** same RBF **75da8bb1**
> or banned **4d6ee356**; **no re-menu storm**. **soft-skip ≠ PASS**; **DIAG ≠ product PASS**;
> **3l2 BLOCKED**. Lab SoT **75da8bb1 HARD_FAIL multi-drive** until new deploy.

**Branch A — if H-gate sticky 0x14 PACK_PROVEN** (not observed on 75da8bb1 this tick):

1. **Strip DIAG in RTL** — restore product `residual_csum <= csum_acc` (remove force
   `residual_csum <= 8'h14` at ST_PLACE in `slice_hdr_parser.sv`).
2. **New freeze md5s** — claim both `Plex.sv` + `slice_hdr_parser.sv`; require **LOCK_OK**;
   zero mid-fit RTL under docker `-v`.
3. **Sole R-csum5 product** — Full Compilation + exit 0; NEW_RBF ∉ banned; DIAG ABSENT.
4. **ONE promote + ONE menu** only (never killall-9 thrash; never second menu for luck).
5. **Hard gate product sticky 0x14** — EXPECT `e8 14 xx` (ideal `e8 14 53 1a`); reject
   +0x53/push; soft-skip ≠ PASS.
6. **Only then** may **3l2** consider unblock (non-DIAG product sticky 0x14 ≥2 + FBAR +
   res_dc=−24). **DIAG sticky alone never unblocks 3l2.**

**Branch B — if still +0x53** (**ACTIVE** on lab 75da8bb1 / L-csum-note25):

1. **Sticky residual-pair latch** — `Plex.sv` `status_telem_r` residual pair
   (status[111:96] = {csum,dc} / raw[13:12]) must **hold** post-ST_PLACE against
   multi-drive / stream-byte bleed (+0x53 = annex len lo); host MAP_OK → blame FPGA.
2. **Not re-DIAG thrash** — do not stack more force-0x14 variants without LOCK_OK sole;
   do not thrash-redeploy **75da8bb1** for residual luck.
3. **Never thrash 4d6ee356** or re-menu storm / killall-9 + load_core loops.
4. **R-csum5 sole LIVE** (L-csum-note26) claim@launch **`6422fb9a`/`8e6af3bb`** — wait
   **BUILD_OK** (do not invent); then one promote → one menu → **H-gate-rcsum5**
   (expect sticky raw[13]==0x14 ≥2 on intentional product fix RBF; reject +0x53).
5. **3l2 BLOCKED** until non-DIAG product sticky 0x14 ≥2.

**Always:** soft-skip ≠ PASS; DIAG ≠ product PASS; BUILD_OK+DEPLOY_OK+FBAR ≠ hard residual
PASS; invent nothing without evidence paths.

#### Post–HARD_FAIL 75da8bb1 R-csum5 serial protocol (H-proto-rcsum5)

> **STATUS 2026-07-24 ~13:28 CDT:** H-gate-rcsum4 final **MULTI_DRIVE_OR_STILL_FAIL**
> on lab **`75da8bb1`** — **NOT PACK_PROVEN** (sticky0x14=0/7; +0x53/push continues;
> DIAG `e8 14` never). Card: `/tmp/misterplex-agent-H-proto-rcsum5.txt`.
> **READY_TO_DEPLOY=NO** until real **BUILD_OK** of a **NEW** RBF. Protocol agents do
> **not** deploy / fit / edit RTL.

**Hard rules (post HARD_FAIL):**

1. **No thrash-redeploy** **`75da8bb1`** or **`4d6ee356`** (also never thrash
   `4deaf6cc` / `dabdaeb0`). Zero re-menu storm / killall-9 / load_core loops.
2. **Intentional RTL sticky residual-pair latch** owned by **R-csum-rtl5** → must emit
   **FIT_GO** with **new freeze md5s** (`Plex.sv` + `slice_hdr_parser.sv`) under
   `/tmp/plex_quartus_rcsum5.claim/` (or equivalent). FIT_GO without new md5s is invalid.
3. **Sole R-csum5** (exclusive only): wrapper wall expect **>420s**; log
   `/tmp/plex_quartus_rcsum5.log`; BUILD_OK = Full Compilation success + exit 0 + idle.
   **NEW_RBF prefix8 ∉** `{75da8bb1,4d6ee356,4deaf6cc,dabdaeb0}` (also reject
   `820484a6`/`6db3a4d8`/`aa146c17`). Prefer claim **LOCK_OK**; mid-fit DRIFT without
   parent accept-ambiguous → READY_TO_DEPLOY stays **NO**. Do **not invent BUILD_OK**.
4. **ONE menu only** after READY_TO_DEPLOY=YES (`DEPLOY_LOAD=menu`; owner
   **H-deploy-rcsum5**). Never second menu for luck; never thrash banned RBFs.
5. **Hard gate** (**H-gate-rcsum5**): expect sticky **raw[13]==0x14** ≥2 (class
   `e8 14 xx`; ideal `e8 14 53 1a`). **Reject +0x53/push**. soft-skip ≠ PASS;
   FBAR ≠ hard residual PASS. Product or proven-pack only with observed sticky 0x14.
6. **3l2 BLOCKED** until **non-DIAG product** sticky 0x14 ≥2. DIAG sticky alone never
   unblocks 3l2.
7. **READY_TO_DEPLOY=NO** until exclusive idle + BUILD_OK new + NEW_RBF ∉ banned +
   LOCK_OK (or parent accept-ambiguous naming that NEW md5). Soft-skip / partial log /
   reusing failed bitstreams never flip YES.

**Serial path (L-csum-note28 stamp):** product launch claim **`6422fb9a`/`8e6af3bb` DIAG=ABSENT** @ **13:31:58** →
**R-csum5 sole LIVE** (log `/tmp/plex_quartus_rcsum5.log`; **not BUILD_OK**; **STOP mid-fit RTL**) →
mid-fit claim rewrite ~**13:33** **`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT** = **CONTAMINATED** (map-era product sticky AUTHORITATIVE for this fit) →
BUILD_OK + NEW_RBF ∉ banned → **evaluate provenance trust** (map-era product vs re-sole clean freeze) →
READY_TO_DEPLOY check → **ONE menu only** → hard sticky **0x14** product →
only then may 3l2 consider unblock (**non-DIAG product sticky 0x14**). Lab remains **`75da8bb1` HARD_FAIL multi-drive SoT** (H-gate-rcsum4/4b) until that deploy.

#### Post–R-csum5 BUILD_OK trust + single safe deploy (H-proto-rcsum5c)

> **STATUS 2026-07-24 ~13:36–13:40 CDT:** Sole **R-csum5 LIVE** (map product sticky
> **DIAG=ABSENT** launch **`6422fb9a`/`8e6af3bb`**; mid-fit thrash live+claim
> **`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT**). Card:
> `/tmp/misterplex-agent-H-proto-rcsum5c.txt`. **READY_TO_DEPLOY=NO** while LIVE and
> until terminal **BUILD_OK + trust**. Protocol agents do **not** deploy / fit / edit RTL.
> Lab **`75da8bb1` HARD_FAIL multi-drive** — thrash **FORBIDDEN**. soft-skip ≠ PASS;
> never kill-9 storms; **DIAG ≠ product PASS**; **3l2 BLOCKED** until non-DIAG product sticky 0x14.

**Trust identities (mid-fit thrash):**

| ID | md5 prefix | DIAG | Notes |
|----|------------|------|-------|
| MAP/LAUNCH (authoritative for in-flight netlist) | **`6422fb9a`/`8e6af3bb`** | ABSENT product sticky | map.rpt: `st_res_word_sticky` |
| LIVE WT + claim rewrite (~13:33) | **`6a5dcaaa`/`7d4a1d8b`** | PRESENT `8'h14` | CONTAMINATED; thrash evidence only |

**LOCK_OK strict** = live == claim == **map/launch** (triple equality). Live matching thrash claim alone is **not** LOCK_OK when map is launch product → **PROVENANCE_UNTRUSTED**.

| Branch | Trigger | Next |
|--------|---------|------|
| **A** | BUILD_OK + LOCK_OK (live==claim==map) + NEW_RBF ∉ banned | ONE promote + ONE `DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh` → **H-gate-rcsum5** sticky **raw[13]==0x14** ≥2; reject **+0x53/push**; res_dc=-24 |
| **B** | BUILD_OK + DRIFT / PROVENANCE_UNTRUSTED (expected after thrash) | **NO auto-deploy**; parent **accept-ambiguous** naming NEW md5 + attribution **or** abort + refreeze intentional tree + re-sole |
| **C** | FIT_DEAD / FAIL | free exclusive; refreeze intentional tree; re-sole only; READY_TO_DEPLOY stays NO |
| **D** (after gate) | sticky 0x14 on **DIAG only** | **PACK_PROVEN maybe**; still **strip DIAG** for product sole before **3l2**; product sticky 0x14 required to unblock 3l2 |

**Always:** NEW_RBF ∉ `{75da8bb1,4d6ee356,4deaf6cc,dabdaeb0,…}`; never thrash **`75da8bb1`/`4d6ee356`**; never kill-9 storms; soft-skip ≠ PASS; DIAG ≠ product PASS; do not invent BUILD_OK.

**Default after observed thrash:** Branch **B** (prefer B2 clean re-sole of intentional product freeze unless parent B1 accept-ambiguous for map-era product RBF).

#### R-csum5 endgame deploy-trust protocol (H-proto-rcsum5d)

> **STATUS 2026-07-24 ~13:38 CDT:** Sole **R-csum5 LIVE** (quartus_fit; log
> `/tmp/plex_quartus_rcsum5.log`; **not BUILD_OK**). Card:
> `/tmp/misterplex-agent-H-proto-rcsum5d.txt`. **READY_TO_DEPLOY=NO** while fit LIVE
> or until **BUILD_OK + trust**. Protocol agents do **not** deploy / fit / edit RTL.
> Lab **`75da8bb1`** RO probes only; thrash **FORBIDDEN**. soft-skip ≠ PASS;
> never kill-9 thrash; **DIAG ≠ product PASS**; **3l2 BLOCKED** until non-DIAG product sticky 0x14.

**NEW_RBF banned (prefix8):** `{75da8bb1,4d6ee356,4deaf6cc,dabdaeb0}` (+ older).

**Intentional freeze (LOCK_OK):** **`6a5dcaaa`/`7d4a1d8b`** DIAG PRESENT.
**Provenance risk:** map may have seen launch **`6422fb9a`/`8e6af3bb`**.

| Branch | Trigger | Next |
|--------|---------|------|
| **A** | BUILD_OK + LOCK_OK (live==**6a5dcaaa/7d4a1d8b**) + clean NEW_RBF | ONE promote + ONE `DEPLOY_LOAD=menu` → H-gate-rcsum5 sticky **raw[13]==0x14** ≥2; reject **+0x53/push** |
| **B** | BUILD_OK + PROVENANCE_UNTRUSTED | **NO auto-deploy**; parent accept-ambiguous **or** abort re-sole frozen tree |
| **C** | FAIL / FIT_DEAD | free lock; refreeze; re-sole only |

**READY_TO_DEPLOY=YES** only under Branch A (or B after explicit parent accept-ambiguous
naming the NEW md5). Never invent YES. DIAG sticky 0x14 never unblocks 3l2.

**POST-TERMINAL ~13:40:35 CDT BUILD_OK → ~13:42 DEPLOY → ~13:44 HARD_FAIL (L-csum-note33; DEFINITIVE L-csum-note35 / H-gate-rcsum5d):**
R-csum5 **BUILD_OK** wall **441s**; **NEW_RBF `8832824e`** full
`8832824e483cf6613f82ee3ba3e592b3`; mid-fit **PROVENANCE_UNTRUSTED**; parent
accept-ambiguous → **H-deploy-rcsum5 DEPLOY_OK** lab **LOADED `8832824e`**.
**H-gate-rcsum5d DEFINITIVE HARD_FAIL MULTI_DRIVE** — sticky0x14=**0**/12; **+0x53 multi-drive
continues after sticky-pack sole**; **res_dc PASS**; **NOT PACK_PROVEN**;
**BUILD_OK+DEPLOY_OK+PACKAGE_OK ≠ hard residual PASS**. thrash **`8832824e`/`75da8bb1`/`4d6ee356`
forbidden**. **Q-fix2 LIVE hold residual RTL**. **Next = multi-drive RCA after Q-fix2 frees (not redeploy)**. soft-skip ≠ PASS; **3l2 BLOCKED**.
Cards: `/tmp/misterplex-agent-H-gate-rcsum5d.txt` (DEFINITIVE), `/tmp/misterplex-agent-H-gate-rcsum5.txt`, `/tmp/misterplex-agent-H-gate-rcsum5b.txt`,
`/tmp/misterplex-agent-H-deploy-rcsum5.txt`, `/tmp/misterplex-agent-L-csum-note33.txt`,
`/tmp/misterplex-agent-L-csum-note35.txt`, `/tmp/plex_quartus.lock`,
`/tmp/misterplex-accept-ambiguous-rcsum5.txt`.

#### L-csum-note31 stamp — R-csum5 BUILD_OK NEW_RBF 8832824e (docs only; historical)

> **R-csum5 BUILD_OK ~13:40:35 CDT.** Wall **441s**. **NEW_RBF `8832824e`** full
> `8832824e483cf6613f82ee3ba3e592b3` (∉ banned). Mid-fit provenance: launch product
> **`6422fb9a`/`8e6af3bb`** → claim/live DIAG **`6a5dcaaa`/`7d4a1d8b`**; map-before-thrash
> ⇒ product sticky **may** be in silicon — **PROVENANCE caution**. Superseded for serial
> path by **L-csum-note32 accept-ambiguous**. Agent:
> `/tmp/misterplex-agent-L-csum-note31.txt`. **No Quartus / no deploy / no RTL.**

#### L-csum-note32 stamp — accept-ambiguous one-menu serial + 3l2 BLOCKED (docs only; historical)

> **R-csum5 BUILD_OK** wall **441s** exit 0; **NEW_RBF `8832824e`** full
> `8832824e483cf6613f82ee3ba3e592b3` (∉ banned). Evidence:
> `/tmp/misterplex-agent-R-csum5-build.txt`, `/tmp/plex_quartus_rcsum5.log`.
>
> **PROVENANCE_UNTRUSTED:** launch **`6422fb9a`/`8e6af3bb` DIAG=ABSENT** → mid-fit
> thrash **`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT** (docker `-v`); not auto LOCK_OK.
>
> **Parent accept-ambiguous (Branch B1):**
> `/tmp/misterplex-accept-ambiguous-rcsum5.txt` names **`8832824e…`** → authorizes
> **ONE promote + ONE `DEPLOY_LOAD=menu` only**. **H-gate expect** sticky
> **raw[13]==0x14** ≥2; reject **+0x53/push**. thrash forbidden; soft-skip ≠ PASS;
> **DIAG ≠ product**; **3l2 BLOCKED**. This stamp did **not** invent DEPLOY_OK /
> hard PASS (authorized serial only). Gate outcome supersession: **L-csum-note33**.
>
> Agent: `/tmp/misterplex-agent-L-csum-note32.txt`. **No Quartus / no deploy / no RTL.**

#### L-csum-note33 stamp — H-gate-rcsum5 HARD_FAIL on lab `8832824e` (docs only)

> **Lab LOADED sticky-pack sole `8832824e` HARD_FAIL multi-drive SoT (canonical).**
>
> | Fact | Value |
> |------|--------|
> | RBF full | **`8832824e483cf6613f82ee3ba3e592b3`** size **3510568** |
> | Deploy | **H-deploy-rcsum5** ONE menu **DEPLOY_OK** (~13:42 CDT); lab md5 MATCH; CORENAME=Plex |
> | Gate | **H-gate-rcsum5** (+ **H-gate-rcsum5b**) **HARD residual_csum FAIL** |
> | Class | **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN** |
> | sticky0x14 | **0**/7 (never); ideal **`e8 14 xx` NEVER** |
> | +0x53 multi-drive | **continues after sticky-pack sole** — series A **0x16→0x69→0xbc→0x0f**; B **0x62→0xb5→0x08** (each step **+0x53** mod 256; annex len lo **0x53**) |
> | res_dc | **PASS** (−24 / raw[12]=**0xe8** sticky 7/7) |
> | FBAR | soft EXIT=0 (7.0/82.9/94.4) — **≠ product residual PASS** |
> | BUILD_OK+DEPLOY | **≠ hard residual PASS** (explicit) |
> | soft-skip | **≠ PASS** |
> | thrash | **`8832824e` / `75da8bb1` / `4d6ee356` FORBIDDEN** (no second menu for luck) |
> | next | **multi-drive RCA (not redeploy)** — sticky latch / telem multi-drive of raw[13] still live |
> | 3l2 | **BLOCKED** until non-DIAG product sticky **raw[13]==0x14** ≥2 |
>
> Sticky-pack sole (`st_res_word_sticky` product-intent) **did not** stop annex-size
> bleed into residual_csum slot. Same +0x53 family as **`75da8bb1`** / **`4d6ee356`**.
> **Do not invent hard PASS / PACK_PROVEN / 3l2 UNBLOCK.**
>
> Evidence: `/tmp/misterplex-agent-H-gate-rcsum5.txt`,
> `/tmp/misterplex-agent-H-gate-rcsum5b.txt`,
> `/tmp/misterplex-H-gate-rcsum5-probes.txt`,
> `/tmp/misterplex-H-gate-rcsum5-summary.txt`,
> `/tmp/misterplex-agent-H-deploy-rcsum5.txt`,
> `/tmp/misterplex-H-deploy-rcsum5-deploy.log`.
> Agent: `/tmp/misterplex-agent-L-csum-note33.txt`. **No Quartus / no deploy / no RTL.**

#### L-csum-note34 stamp — H-gate HARD_FAIL 8832824e multi-drive + Q-SF2 LIVE residual freeze (docs only)

> **Three stamps (L-csum-note34; docs only; NO Quartus / NO deploy / NO RTL):**
>
> 1. **H-gate HARD_FAIL `8832824e` multi-drive (reconfirm SoT)**  
>    Lab **LOADED** sticky-pack sole **`8832824e`** (full `8832824e483cf6613f82ee3ba3e592b3`).  
>    **H-gate-rcsum5** (+5b) **HARD residual_csum FAIL** class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN**.  
>    sticky0x14=**0**/7; **+0x53/push multi-drive continues** after sticky-pack sole  
>    (A **0x16→0x69→0xbc→0x0f**; B **0x62→0xb5→0x08**); ideal **`e8 14 xx` NEVER**.  
>    **res_dc PASS**; **FBAR soft ≠ residual PASS**. **BUILD_OK+DEPLOY_OK ≠ hard residual PASS**.  
>    soft-skip ≠ PASS. **3l2 BLOCKED**. Next residual path = **multi-drive RCA** (not redeploy).
>
> 2. **Exclusive Q-SF2 LIVE — residual RTL FREEZE while wide fits**  
>    Lock `/tmp/plex_quartus.lock` = **`Q-SF2 2026-07-24T13:45:49-05:00`** (alias; lock file text is **`Q-fix2 …`**).  
>    Wide Fix-2 sole owns the exclusive Quartus slot. Residual agents must **not**  
>    start a second exclusive / residual re-sole / residual fit. Residual RTL sources  
>    under the shared docker mount are **FROZEN** for the duration of Q-SF2 / Q-fix2.
>
> 3. **Thrash FORBIDDEN — includes mid-fit residual edits during Q-SF2**  
>    - thrash-redeploy **`8832824e` / `75da8bb1` / `4d6ee356`** (no second menu for luck)  
>    - **ZERO** mid-fit residual edits to `Plex.sv` / `slice_hdr_parser.sv` (or other  
>      residual path) while Q-SF2 LIVE — docker `-v` thrash poisons the sole  
>    - no residual re-claim / re-sole under live exclusive  
>    - no invent hard PASS / PACK_PROVEN / 3l2 UNBLOCK
>
> Evidence: `/tmp/misterplex-agent-H-gate-rcsum5.txt`,
> `/tmp/misterplex-agent-H-gate-rcsum5b.txt`,
> `/tmp/misterplex-H-gate-rcsum5-probes.txt`,
> `/tmp/misterplex-H-gate-rcsum5-summary.txt`,
> `/tmp/plex_quartus.lock`,
> `/tmp/misterplex-agent-L-csum-note34.txt`.
> Agent: `/tmp/misterplex-agent-L-csum-note34.txt`. **No Quartus / no deploy / no RTL.**
> Superseded for definitive gate wording by **L-csum-note35** (H-gate-rcsum5d).

#### L-csum-note35 stamp — HARD_FAIL 8832824e DEFINITIVE (H-gate-rcsum5d) + Q-fix2 hold residual RTL (docs only)

> **Four stamps (L-csum-note35; docs only; NO Quartus / NO deploy / NO RTL):**
>
> 1. **HARD_FAIL `8832824e` DEFINITIVE — H-gate-rcsum5d**  
>    Lab **LOADED** sticky-pack sole **`8832824e`** full `8832824e483cf6613f82ee3ba3e592b3` size **3510568**.  
>    **H-gate-rcsum5d** consolidates **H-gate-rcsum5** + **H-gate-rcsum5b** (report-only; no lab thrash) →  
>    **HARD residual_csum FAIL** class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN**.  
>    sticky raw[13]==0x14 = **0**/12 (**never**); ideal **`e8 14 xx` NEVER**.  
>    +0x53/push family confirmed (canonical A/B **6/6** adjacent; 5b reconfirm same family).  
>    Series SoT: A **0x16→0x69→0xbc→0x0f**; B **0x62→0xb5→0x08** (annex len lo **0x53**).  
>    **res_dc PASS** (−24 / 0xe8 all hard probes). **FBAR soft** EXIT=0 ≠ product PASS.  
>    **BUILD_OK + DEPLOY_OK + PACKAGE_OK ≠ hard residual PASS**. soft-skip ≠ PASS.  
>    Sticky-pack sole **did not** latch host XOR **0x14**; multi-drive continues after sole.
>
> 2. **Exclusive Q-fix2 LIVE — hold residual RTL**  
>    Lock `/tmp/plex_quartus.lock` = **`Q-fix2 2026-07-24T13:45:49-05:00`** (wide Fix-2 sole).  
>    Residual agents **HOLD**: no residual re-sole, no second exclusive, no residual fit,  
>    no mid-fit residual RTL edits under shared docker `-v` while Q-fix2 owns the slot.
>
> 3. **Thrash FORBIDDEN**  
>    - thrash-redeploy **`8832824e` / `75da8bb1` / `4d6ee356`** (no second menu for luck)  
>    - **ZERO** mid-fit residual edits (`Plex.sv` / `slice_hdr_parser.sv` / residual path)  
>    - no residual re-claim under live exclusive  
>    - no invent hard PASS / PACK_PROVEN / 3l2 UNBLOCK / Q-fix2 BUILD_OK
>
> 4. **3l2 BLOCKED** until **non-DIAG product** sticky **raw[13]==0x14** stable ≥2  
>    on a *new* post-fix RBF (≠ **`8832824e`** / ≠ banned set). Soft-skip ≠ hard PASS.
>
> **Next residual path:** multi-drive telem RCA **after Q-fix2 frees** — not redeploy /  
> not residual re-sole under live exclusive / not luck menu.
>
> Evidence: `/tmp/misterplex-agent-H-gate-rcsum5d.txt` (**DEFINITIVE**),
> `/tmp/misterplex-agent-H-gate-rcsum5.txt`,
> `/tmp/misterplex-agent-H-gate-rcsum5b.txt`,
> `/tmp/misterplex-H-gate-rcsum5-probes.txt`,
> `/tmp/misterplex-H-gate-rcsum5-summary.txt`,
> `/tmp/misterplex-agent-H-deploy-rcsum5.txt`,
> `/tmp/plex_quartus.lock`,
> `/tmp/misterplex-agent-L-csum-note35.txt`.
> Agent: `/tmp/misterplex-agent-L-csum-note35.txt`. **No Quartus / no deploy / no RTL.**

#### L-csum-note36 stamp — HARD_FAIL 8832824e + Q-SF2 LIVE mid-fit residual HOLD (docs only)

> **Four stamps (L-csum-note36; docs only; NO Quartus / NO deploy / NO RTL):**
>
> 1. **HARD_FAIL `8832824e` DEFINITIVE — unchanged SoT**  
>    Lab **LOADED** sticky-pack sole **`8832824e`** full `8832824e483cf6613f82ee3ba3e592b3` size **3510568**.  
>    **H-gate-rcsum5d DEFINITIVE** (consolidates 5+5b+5c): class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN**.  
>    sticky raw[13]==0x14 = **0**/12 (**never**); ideal **`e8 14 xx` NEVER**.  
>    +0x53/push family: A **0x16→0x69→0xbc→0x0f**; B **0x62→0xb5→0x08** (annex len lo **0x53**).  
>    **res_dc PASS** (−24 / 0xe8). **FBAR soft** ≠ product PASS.  
>    **BUILD_OK + DEPLOY_OK + PACKAGE_OK ≠ hard residual PASS**. soft-skip ≠ PASS.
>
> 2. **Exclusive Q-SF2 (wide Fix-2) — HOLD residual RTL**  
>    At note36 write: lock was **`Q-SF2 2026-07-24T13:45:49-05:00`** LIVE mid-fit (residual HOLD).  
>    Later lock harvest may show Q-SF2 **DONE** / wide NEW_RBF (e.g. `ec21e133…`) — that is **wide Fix-2**,  
>    **not** residual hard green. **WIDE ≠ residual PASS ≠ 3l2 unblock**.  
>    Host residual lab SoT remains **`8832824e` HARD_FAIL**. Residual agents **HOLD**: no residual  
>    thrash-redeploy, no residual re-sole without intentional LOCK_OK freeze, no residual RTL thrash.
>
> 3. **Thrash FORBIDDEN (mid-fit residual)**  
>    - thrash-redeploy **`8832824e` / `75da8bb1` / `4d6ee356`** (no second menu for luck)  
>    - **ZERO** mid-fit residual edits (`Plex.sv` / `slice_hdr_parser.sv` / residual path) while Q-SF2 LIVE  
>    - no residual re-claim under live exclusive  
>    - no invent hard PASS / PACK_PROVEN / 3l2 UNBLOCK / Q-SF2 BUILD_OK
>
> 4. **3l2 BLOCKED** until **non-DIAG product** sticky **raw[13]==0x14** stable ≥2  
>    on a *new* post-SF RBF (≠ **`8832824e`** / ≠ banned set). Soft-skip ≠ hard PASS.
>
> **Next residual path:** multi-drive telem RCA **after Q-SF2 frees** — not redeploy /  
> not residual re-sole under live exclusive / not luck menu / not mid-fit residual thrash.
>
> Evidence: `/tmp/misterplex-agent-H-gate-rcsum5d.txt` (**DEFINITIVE**),
> `/tmp/misterplex-agent-H-gate-rcsum5.txt`,
> `/tmp/misterplex-agent-H-gate-rcsum5b.txt`,
> `/tmp/misterplex-H-gate-rcsum5-probes.txt`,
> `/tmp/misterplex-H-gate-rcsum5-summary.txt`,
> `/tmp/misterplex-agent-H-deploy-rcsum5.txt`,
> `/tmp/plex_quartus.lock` (reconfirm **`Q-SF2 2026-07-24T13:45:49-05:00`**),
> `/tmp/misterplex-agent-L-csum-note35.txt`,
> `/tmp/misterplex-agent-L-csum-note36.txt`.
> Agent: `/tmp/misterplex-agent-L-csum-note36.txt`. **No Quartus / no deploy / no RTL.**
> Superseded for dual-track post-deploy wording by **L-csum-note37** (BUILD_OK+deploy+WIDE FAIL `ec21e133` 0.605; residual HARD_FAIL both `8832824e`+`ec21e133`; exclusive FREE; competing Q-SF3 OR R-csum6).

#### L-csum-note37 — dual-track post Q-SF2 BUILD_OK+deploy+WIDE FAIL (docs only)

> **POST-DEPLOY DUAL-TRACK (L-csum-note37; docs only; NO Quartus / NO deploy / NO RTL).**
> Supersedes prior note37 mid-fit “approaching BUILD” wording and incomplete residual-only card.
> Exclusive **FREE** (`/tmp/plex_quartus.lock` = `Q-SF2 DONE BUILD_OK 2026-07-24T13:52:56-05:00 NEW_RBF=ec21e133…`).

| Track | State | Gate / READY |
|-------|-------|--------------|
| **W** wide Fix-2 | Q-SF2 **BUILD_OK** NEW_RBF **`ec21e133`** full `ec21e1330ddd75ad7f39099e5abfad49`; SRC colorbars **`f1d9666a`**; wall **415s** exit 0; ONE promote/menu (**H-deploy-qSF2**); lab **LOADED `ec21e133`** | FBAR soft **PASS** 7.0/82.9/94.4; **WIDE FAIL** span=**0.605** pillar **PILLAR_320_of_529** (**W-wide-gate-SF2 / SF2b**) — same ~60.5% fingerprint. WIDE still **FAIL open**. |
| **R** residual **both RBFs** | (1) product sticky-pack sole **`8832824e`** **H-gate-rcsum5d** DEFINITIVE: sticky0x14=**0**/12; +0x53 seq **16/69/bc/0f/62/b5/08**; **NOT PACK_PROVEN**. (2) LOADED wide **`ec21e133`** **H-gate-ec21 / H-res-ec21**: sticky0x14=**0**; +0x53 seq **54/a7/fa/4d/a0/f3/46**; res_dc PASS; same **MULTI_DRIVE_OR_STILL_FAIL** class | **HARD_FAIL** on **both**. **READY_TO_DEPLOY residual=NO** (do not invent YES). |

**PACKAGE_OK** (F-prep-qSF2 / F-prep-SF2) embeds **`ec21e133`** ≠ product PASS ≠ wide visual PASS ≠ residual PASS.  
**R-csum-rtl6-plan `FIT_GO=NO`** until parent authorize intentional multi-drive freeze + sole.  
**READY_TO_DEPLOY residual = NO** until intentional **LOCK_OK** product sole **BUILD_OK** (not luck redeploy of banned / wide RBFs). soft-skip ≠ PASS; **3l2 BLOCKED**.

**Serial rules (exclusive FREE now; parent picks ONE sole at a time):**

1. **Competing next soles:** **Q-SF3** (wide Fix-3 after design) **OR** **R-csum6** (residual multi-drive RCA) — **one at a time**; parent authorizes.
2. **ZERO second Quartus** while any sole LIVE (shared docker `-v` = PROVENANCE poison).
3. **Mid-fit thrash FORBIDDEN** — ZERO residual / colorbars / `Plex.sv` / `slice_hdr_parser.sv` edits under live exclusive.
4. After next **BUILD_OK** only: **ONE** promote + **ONE** `DEPLOY_LOAD=menu` — never menu storms / kill-9 thrash.
5. Thrash ban: residual **`8832824e` / `75da8bb1` / `4d6ee356`** (+ older fails) **and** **no thrash-redeploy luck on `ec21e133` for residual green**.
6. **WIDE gate ≠ residual PASS ≠ 3l2 unblock**. **BUILD_OK+DEPLOY+PACKAGE_OK+FBAR ≠ hard residual PASS**.
7. Hard residual expect: sticky **raw[13]==0x14 ≥2** reject **+0x53/push**; ideal green probe **`e8 14 53 1a`**.

Evidence: `/tmp/misterplex-agent-L-csum-note37.txt`, `/tmp/plex_quartus.lock` (`Q-SF2 DONE BUILD_OK … NEW_RBF=ec21e133…`), `/tmp/plex_quartus_sf2.log`, `/tmp/misterplex-agent-M-fitmon-qSF2d.txt` (BUILD_OK wall 415s), `/tmp/misterplex-agent-H-deploy-qSF2.txt`, `/tmp/misterplex-agent-W-wide-gate-SF2.txt`, `/tmp/misterplex-agent-W-wide-gate-SF2b.txt` (WIDE FAIL 0.605), `/tmp/misterplex-agent-H-gate-ec21.txt`, `/tmp/misterplex-agent-H-res-ec21.txt` (residual HARD_FAIL on ec21e133), `/tmp/misterplex-agent-H-gate-rcsum5d.txt` (residual HARD_FAIL on 8832824e), `/tmp/misterplex-agent-R-csum-rtl6-plan.txt` (FIT_GO=NO), `/tmp/misterplex-agent-H-proto-dual1.txt`. **No Quartus / no deploy / no RTL this agent.**


#### Post–HARD_FAIL 8832824e branch protocol (H-proto-rcsum6)

> **STATUS 2026-07-24 ~13:46 CDT (+ L-csum-note35):** Lab **LOADED `8832824e`** after
> H-deploy-rcsum5 ONE menu. **H-gate-rcsum5d DEFINITIVE HARD_FAIL** (consolidates 5+5b)
> class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN** (sticky0x14=0/12; +0x53×6
> family; series `16 69 bc 0f 62 b5 08`; ideal `e8 14` never; res_dc PASS; FBAR soft only).
> Card: `/tmp/misterplex-agent-H-proto-rcsum6.txt`; definitive gate
> `/tmp/misterplex-agent-H-gate-rcsum5d.txt`. **READY_TO_DEPLOY=NO** until new
> intentional **BUILD_OK + LOCK_OK**. Protocol agents do **not** deploy / fit / edit RTL.
> **Q-fix2 LIVE** — hold residual RTL. Thrash **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**.
> soft-skip ≠ PASS; **DIAG ≠ product PASS**; **3l2 BLOCKED** until non-DIAG product sticky 0x14.

**Hard rules (canonical after HARD_FAIL 8832824e):**

| Rule | Value |
|------|--------|
| Thrash-redeploy | **FORBIDDEN** for **`8832824e`**, **`75da8bb1`**, **`4d6ee356`** |
| READY_TO_DEPLOY | **NO** until new intentional **BUILD_OK + LOCK_OK** |
| Next RTL | Multi-drive telem isolation RCA → intentional freeze md5s → sole only; **mid-fit thrash FORBIDDEN** |
| Deploy after next sole | **ONE** menu only (`DEPLOY_LOAD=menu`) |
| Hard expect | sticky **raw[13]==0x14** ≥2; reject **+0x53/push** |
| 3l2 | **BLOCKED** until non-DIAG **product** sticky 0x14 |
| soft-skip | ≠ PASS |
| NEW_RBF banned | `{8832824e,75da8bb1,4d6ee356,4deaf6cc,dabdaeb0,…}` |

| Branch | Trigger | Next (serial) |
|--------|---------|----------------|
| **A PACK_PROVEN** | sticky raw[13]==0x14 ≥2 | **Not this silicon** (never on 8832824e). If future RBF: strip DIAG → freeze LOCK_OK → sole product → ONE menu → product sticky 0x14 — only then 3l2 may unblock |
| **B still multi-drive** (**ACTIVE**) | never 0x14; +0x53/push | Multi-drive telem isolation RCA (`Plex.sv` / hps_io status); **no thrash-redeploy** 8832824e/75da8bb1/4d6ee356; no re-menu storm |
| **C re-sole clean** | intentional multi-drive RTL freeze ready + exclusive FREE | New freeze md5s LOCK_OK → sole **R-csum6** (no mid-fit thrash) → NEW_RBF ∉ banned → ONE menu → H-gate sticky 0x14 ≥2 |

**Always:** soft-skip ≠ PASS; DIAG ≠ product PASS; BUILD_OK+DEPLOY_OK+FBAR ≠ hard residual
PASS; never invent READY_TO_DEPLOY=YES. Evidence: H-gate-rcsum5/5b, H-deploy-rcsum5,
L-csum-note33, this card.

#### Post–R-csum6 deploy-trust protocol (H-proto-rcsum6 … → H-proto-rcsum6e)

> **STATUS 2026-07-24 post BUILD_OK+DEPLOY (H-proto-rcsum6e; PROTO_OK):** Branch **A TAKEN**.
> R-csum6 **BUILD_OK** wall **438s** Full Comp 0e/40w exit 0; NEW_RBF **`94bbfe43`**
> full `94bbfe433feb562fabe0798e16b378c5` ∉ banned; **LOCK_OK** live==claim
> **c7a847f7 / ca62d02b / 904e9b2e DIAG=ABSENT**. **READY_TO_DEPLOY=YES** path: **ONE**
> menu (waiter / H-deploy) → lab **LOADED `94bbfe43`**. Serial now = **H-gate-rcsum6**
> hard residual (sticky **raw[13]==0x14 ≥2** reject **+0x53**; res_dc=−24 **PENDING**).
> **BUILD_OK+DEPLOY_OK ≠ hard residual PASS.** Thrash banned **FORBIDDEN**; soft-skip ≠ PASS;
> **3l2 BLOCKED** until product sticky 0x14. If multi-drive HARD_FAIL → **RCA not thrash**.
> **WIDE Q-SF3** only after residual gate or parent prioritizes. Canonical card:
> `/tmp/misterplex-agent-H-proto-rcsum6e.txt`. Prior: `H-proto-rcsum6d` checklist ·
> `H-proto-rcsum6c` WAIT · authorize `H-proto-rcsum6`. Protocol agents do **not** deploy /
> fit / edit RTL.

**Residual HARD_FAIL evidence (same MULTI_DRIVE +0x53 class):**

| Bitfile | Gate | sticky0x14 | +0x53 | res_dc |
|---------|------|------------|-------|--------|
| **`8832824e`** residual sole (historical) | **H-gate-rcsum5d** DEFINITIVE | 0/12 never | **16/69/bc/0f/62/b5/08** | PASS −24 |
| **`ec21e133`** wide LOADED (reconfirm) | **H-res-ec21** / **H-gate-ec21** | 0/7 · 0/3 | **54…46** / **5b→ae→01** | PASS −24 |

Wide **WIDTH FAIL 0.605** (**H-gate-sf2**) is **orthogonal** — do not conflate with residual.
Lab **LOADED residual NEW `94bbfe43`** after ONE waiter menu (was `ec21e133` pre-deploy).

#### Post BUILD_OK+DEPLOY `94bbfe43` serial (H-proto-rcsum6e)

> **PROTO stamp (H-proto-rcsum6e; ~14:11 CDT; ZERO Quartus / ZERO deploy / ZERO RTL).**  
> Card: `/tmp/misterplex-agent-H-proto-rcsum6e.txt`.

| Step | Status / action |
|------|-----------------|
| **T0–T2** | **DONE** BUILD_OK + LOCK_OK + NEW `94bbfe43` ∉ banned |
| **T3–T6** | **DONE** READY=YES → ONE waiter menu → lab LOADED `94bbfe43` CORENAME=Plex |
| **T7 now** | **DONE HARD_PASS** (H-gate-rcsum6 / L-csum-note41) sticky0x14=**7/7** plus53=**0** res_dc **7/7** ideal **e8 14 xx** — do **not** re-run residual gate |
| **T8** | HARD_FAIL multi-drive path **closed** on product **`94bbfe43`** (no thrash redeploy) |
| **T9** | **3l2 residual hard-block LIFTED** — next 3l2 product IDCT wire-up/gate; WIDE orthogonal FAIL 0.605; Q-SF3 separate |

| Branch | Trigger | Next |
|--------|---------|------|
| **A TAKEN** | BUILD_OK + LOCK_OK + NEW `94bbfe43` + ONE menu | **H-gate-rcsum6** sticky 0x14 ≥2 reject +0x53; res_dc=−24 |
| **D after gate** | **TAKEN** post H-gate-rcsum6 **HARD_PASS** | product sticky 0x14 **7/7** → **3l2 residual hard-block LIFTED**; ≠ WIDE PASS; ≠ full 3l2 product DONE |

**Hard rules:** thrash banned **FORBIDDEN** (`8832824e`/`75da8bb1`/`4d6ee356`; no luck
`ec21e133` / second-menu `94bbfe43`); soft-skip ≠ PASS; FBAR EXIT=0 ≠ residual PASS;
**BUILD_OK+DEPLOY_OK+FBAR ≠ hard residual PASS** (gate now closed **HARD_PASS** separately);
**3l2 residual hard-block LIFTED** on product sticky (L-csum-note41); **≠ WIDE PASS**;
**Q-SF3** separate track. Protocol agents do not deploy/fit/edit RTL. **PROTO_OK** + **L-csum-note41**.

Evidence: `/tmp/misterplex-agent-H-proto-rcsum6e.txt`, `/tmp/misterplex-agent-H-gate-rcsum6.txt`,
`/tmp/misterplex-H-gate-rcsum6-summary.txt`, `/tmp/misterplex-agent-H-deploy-rcsum6.txt`,
`/tmp/misterplex-agent-L-csum-note41.txt`, loop status, lock DONE BUILD_OK,
`/tmp/misterplex-agent-M-fitmon-rcsum6e.txt`, `/tmp/misterplex-agent-R-csum6-sole.txt`,
claim `/tmp/plex_quartus_rcsum6.claim/`, log `/tmp/plex_quartus_rcsum6.log`,
releases `Plex_rcsum6_94bbfe43.rbf`.

#### L-csum-note41 — H-gate-rcsum6 HARD_PASS product sticky `94bbfe43` + 3l2 residual unblock (docs only)

> **DOCS stamp (L-csum-note41; ZERO Quartus / ZERO deploy / ZERO lab probes / ZERO RTL).**  
> Evidence only from existing gate/deploy reports — **do not re-run residual gate**.  
> **residual HARD_PASS ≠ WIDE PASS ≠ full 3l2 product DONE.** soft-skip ≠ PASS.

| Fact | Value (lab LOADED **`94bbfe43`**) |
|------|-----------------------------------|
| product hard residual | **HARD_PASS** |
| CLASS | **HARD_PASS_PRODUCT_STICKY** |
| PACK_PROVEN | **YES** (product sticky XOR 0x14, not DIAG force) |
| sticky raw[13]==0x14 | **7/7** hard probes (gate ≥2; extended series full sticky) |
| plus53 / multi-drive | **0** steps — **REJECTED** |
| res_dc sticky −24 / 0xe8 | **PASS 7/7** |
| ideal class e8 14 xx | **YES 7/7** (stream free LE varies; raw[13] sticky) |
| DIAG | **ABSENT** (claim + live product `residual_csum <= cs`) |
| DEPLOY | **DEPLOY_OK** ONE menu (H-deploy-rcsum6) — no thrash |
| BUILD / LOCK | **BUILD_OK** NEW_RBF **94bbfe43** wall 438s; **LOCK_OK** freeze **c7a847f7 / ca62d02b / 904e9b2e** |
| FBAR soft | EXIT=0 separate (**≠** hard residual PASS; already HARD_PASS independently) |
| Historical FAIL contrast | **8832824e** sticky0x14=0/12 +0x53; **ec21e133** sticky0x14=0 +0x53 (MULTI_DRIVE) |
| WIDE orthogonal | still **FAIL 0.605** on historical **ec21e133** — do not invent WIDE PASS |
| **3l2 residual hard-block** | **LIFTED** — product sticky 0x14 hard PASS met |
| 3l2 product DONE | **NO** — paint/IDCT wire-up/gate still open after residual unblock |
| Do not | re-run residual gate; thrash-redeploy **94bbfe43**/banned; invent WIDE PASS / full 3l2 DONE |

**Next (parent serial):**
1. **3l2 product IDCT wire-up / gate** (inv_quant / IDCT / `files.qip` path per P3-3l2 after residual green)
2. **WIDE Fix-3** separate (pillar 0.605 open; FIT_GO only when parent prioritizes)
3. Optional package embed residual NEW; **no** residual luck menu

Evidence: `/tmp/misterplex-agent-H-gate-rcsum6.txt` (VERDICT HARD_PASS sticky0x14 7/7 plus53=0 res_dc PASS ideal e8 14),
`/tmp/misterplex-H-gate-rcsum6-summary.txt` (CLASS=HARD_PASS_PRODUCT_STICKY),
`/tmp/misterplex-agent-H-deploy-rcsum6.txt` (DEPLOY_OK),
lock DONE BUILD_OK NEW_RBF 94bbfe43 LOCK_OK claim c7a847f7/ca62d02b/904e9b2e DIAG=ABSENT,
`/tmp/misterplex-agent-L-csum-note41.txt`. **DOCS_OK.**

#### Post-terminal H-deploy-rcsum6 ONE-menu checklist (H-proto-rcsum6d — historical LIVE-era)

> **Historical PROTO stamp (H-proto-rcsum6d; ~14:08 CDT LIVE fit).** Superseded for
> post-OK state by **H-proto-rcsum6e**. Card: `/tmp/misterplex-agent-H-proto-rcsum6d.txt`.

Serial steps (T0–T7) were the pre-terminal checklist; **T0–T6 now DONE** on `94bbfe43`
(waiter ONE menu). Remaining = **T7 H-gate-rcsum6**. soft-skip ≠ PASS;
BUILD_OK+DEPLOY_OK ≠ hard residual PASS; **3l2 BLOCKED** until product sticky 0x14 ≥2.

#### L-csum-note38 — parent authorize R-csum6 FIT_GO=YES product Rank1+2+3 (docs only)

> **DOCS stamp (L-csum-note38; ZERO Quartus / ZERO deploy / ZERO RTL).**  
> **ZERO invent BUILD_OK** — exclusive sole **R-csum6 LIVE** (fit in flight); authorization ≠ Full Comp success.

| Cite | Result |
|------|--------|
| **H-gate-ec21** | **HARD_FAIL** lab **`ec21e133`** sticky0x14=**0/3**; +0x53 **08→5b→ae→01**; res_dc PASS; class **MULTI_DRIVE_OR_STILL_FAIL** |
| **W-wide-gate-fix2b** | **FAIL OPEN** span=**0.605** **PILLAR_320_of_529** (FBAR soft PASS ≠ WIDE); **orthogonal** to residual |
| **R-multidrive-rca14** | **RCA_OK** root **MULTI_DRIVE** status[111:104]/raw[13] (+0x53=6739&0xFF); recipe Rank1+2+3; plan-era **FIT_GO=NO** until parent |
| **Parent authorize** | **FIT_GO=YES** product **Rank1+2+3 place_pulse sticky**; exclusive sole **R-csum6** start **`2026-07-24T14:02:42-05:00`** |

**Product sole composition (claim):** Rank1 `st_res_word_sticky` on **place_pulse / residual_ok_rise** + Rank2 status residual mask + Rank3 place doublebuf/`residual_place_pulse`; **DIAG=ABSENT** (product `residual_csum<=cs`); freeze md5 match live Plex **`c7a847f7…`** / slice **`ca62d02b…`** / stream **`904e9b2e…`**. Ban NEW_RBF includes **`ec21e133`** + residual fails.

| Gate | Value |
|------|--------|
| Exclusive | **R-csum6 LIVE** sole (`/tmp/plex_quartus.lock`) — **no second Quartus** |
| BUILD_OK | **NOT claimed** (do not invent; await Full Comp + exit 0 + idle + LOCK_OK) |
| READY_TO_DEPLOY residual | **NO** until BUILD_OK+LOCK_OK + NEW_RBF ∉ banned |
| Hard expect (post ONE menu) | sticky **raw[13]==0x14 ≥2**; reject **+0x53**; res_dc=−24 |
| Thrash | **FORBIDDEN** `8832824e`/`75da8bb1`/`4d6ee356`/`ec21e133` luck |
| WIDE | **FAIL OPEN 0.605** orthogonal — Fix-3 after residual exclusive frees |
| 3l2 | **BLOCKED** until non-DIAG **product** sticky 0x14 |
| This agent | **No** Quartus / deploy / RTL |

Evidence: `/tmp/misterplex-agent-L-csum-note38.txt`, `/tmp/misterplex-agent-H-gate-ec21.txt`, `/tmp/misterplex-agent-W-wide-gate-fix2b.txt`, `/tmp/misterplex-agent-R-multidrive-rca14.txt`, `/tmp/misterplex-agent-R-csum-rtl6-plan.txt`, `/tmp/plex_quartus_rcsum6.claim/claim_meta.txt`, `/tmp/plex_quartus.lock`, `/tmp/misterplex-loop-status.txt`. **DOCS_OK.**


#### L-csum-note40 — R-csum6 BUILD_OK NEW 94bbfe43 LOCK_OK post-terminal (docs only)

> **PROTO stamp (L-csum-note40; ZERO Quartus / ZERO deploy / ZERO RTL).**  
> Supersedes mid-fit LIVE wording. Terminal harvest **~14:10 CDT**.

| Fact | Value |
|------|--------|
| Exclusive | **R-csum6 DONE BUILD_OK** `2026-07-24T14:10:26-05:00` wall **438s** — **IDLE** |
| NEW_RBF | **`94bbfe43`** full `94bbfe433feb562fabe0798e16b378c5` (∉ banned; ≠ `ec21e133`) |
| LOCK_OK | **YES** live==claim Plex **`c7a847f7…`** / slice **`ca62d02b…`** / stream **`904e9b2e…` DIAG=ABSENT** Rank1+2+3 |
| **BUILD_OK ≠ PASS** | Full Comp + NEW + LOCK_OK **≠** residual hard PASS; soft-skip ≠ PASS |
| Deploy | **ONE** menu only — owner **H-deploy-rcsum6** `DEPLOY_LOAD=menu` (not this agent) |
| Hard gate | sticky **raw[13]==0x14 ≥2**; reject **+0x53/push**; res_dc=−24 (**H-gate-rcsum6**) |
| Thrash | **FORBIDDEN** `8832824e`/`75da8bb1`/`4d6ee356` (+ no luck `ec21e133` residual) |
| 3l2 | **BLOCKED** until non-DIAG **product** sticky 0x14 |
| WIDE | orthogonal **FAIL hold** span=**0.605** — do not conflate with residual |

Evidence: `/tmp/misterplex-agent-L-csum-note40.txt`, `/tmp/misterplex-agent-M-fitmon-rcsum6e.txt`,
`/tmp/plex_quartus.lock`, `/tmp/plex_quartus_rcsum6.log`, claim `/tmp/plex_quartus_rcsum6.claim/`,
H-proto-rcsum6d checklist. **PROTO_OK.**  
**Superseded for hard residual outcome by L-csum-note41** (H-gate-rcsum6 **HARD_PASS** on lab **`94bbfe43`**).


#### L-csum-note39 — R-csum6 LIVE claim freeze Rank1+2+3 DIAG strip (docs only)

> **PROTO stamp (L-csum-note39; docs only; ZERO Quartus / ZERO deploy / ZERO RTL).**  
> Exclusive **R-csum6 LIVE** `2026-07-24T14:02:42-05:00`. **Do not invent BUILD_OK.** Mid-fit thrash **FORBIDDEN**.

| Claim | Value |
|-------|--------|
| FIT_GO / DIAG | **YES** / **ABSENT** (product strip — no force `8'h14`) |
| Pack | **Rank1+2+3**: `st_res_word_sticky` + `residual_place_pulse` + `status_telem_masked` |
| Freeze md5 | Plex **`c7a847f7…`** / slice **`ca62d02b…`** / stream **`904e9b2e…`** |
| READY_TO_DEPLOY | **NO** until **BUILD_OK + LOCK_OK** (live==claim) |
| After BUILD_OK | **ONE** menu **H-deploy-rcsum6** only |
| NEW_RBF ban | **`8832824e`/`75da8bb1`/`4d6ee356`/…** and must **≠ `ec21e133`** |
| soft-skip / 3l2 | ≠ PASS; **3l2 BLOCKED** until non-DIAG product sticky 0x14 |

Evidence: `/tmp/misterplex-agent-L-csum-note39.txt`, `/tmp/plex_quartus_rcsum6.claim/`, `/tmp/plex_quartus.lock`, `/tmp/misterplex-loop-status.txt`, `/tmp/misterplex-agent-H-proto-rcsum6.txt`. **PROTO_OK.**

#### Dual-track protocol (H-proto-dual1)

> **STATUS 2026-07-24 ~13:51 CDT:** Dual-track SoT card
> `/tmp/misterplex-agent-H-proto-dual1.txt`. Exclusive **Q-fix2 LIVE** (wide Fix-2;
> lock `Q-fix2 2026-07-24T13:45:49-05:00`; log `/tmp/plex_quartus_fix2.log`;
> `quartus_fit` active; host RBF still prior **8832824e** — **NOT BUILD_OK**, do not invent).
> Lab residual **LOADED `8832824e` HARD_FAIL** multi-drive (sticky0x14=0; +0x53
> `16/69/bc/0f/62/b5/08`; res_dc PASS; NOT PACK_PROVEN). Protocol agents do **not**
> deploy / fit / edit RTL. soft-skip ≠ PASS; **DIAG ≠ product PASS**; **3l2 BLOCKED**.

**Dual model:** TRACK_W = Q-fix2 wide sole (active exclusive). TRACK_R = residual parked
(CLOSED FAIL on 8832824e). Gates must not be conflated.

| Rule (while LIVE) | Value |
|-------------------|--------|
| Second Quartus | **ZERO** |
| Mid-fit RTL thrash | **ZERO** (docker `-v`) |
| Residual thrash-redeploy | **FORBIDDEN** **`8832824e`/`75da8bb1`/`4d6ee356`** |
| R-csum6 | **BLOCKED** until FREE + multi-drive LOCK_OK + FIT_GO |
| After wide BUILD_OK | ONE menu **wide only** if NEW_RBF clean — separate residual gate |
| soft-skip / 3l2 | ≠ PASS; **3l2 BLOCKED** |

Peers: H-proto-qfix2 (wide detail), H-proto-rcsum6 (residual branch). Evidence: this card,
H-gate-rcsum5d, M-fitmon-qfix2*, PHASE_BACKLOG dual-track stamp.

**POST-TERMINAL (~13:52:56):** Q-fix2 **BUILD_OK** NEW_RBF **`ec21e133`** clean;
colorbars **f1d9666a** LOCK_OK. Wide: ONE menu only (H-deploy-fix2). Residual thrash
**8832824e FORBIDDEN**; R-csum6 only after FREE + multi-drive LOCK_OK + FIT_GO.
soft-skip ≠ PASS; **3l2 BLOCKED**. Amend: dual1 card §10.

**+ L-csum-note37 (dual-track post BUILD_OK+deploy+WIDE FAIL; exclusive FREE):**
Q-SF2 **BUILD_OK** NEW_RBF **`ec21e133`** full `ec21e1330ddd75ad7f39099e5abfad49`;
ONE menu done; **WIDE FAIL** span=**0.605** **PILLAR_320_of_529**. Residual **HARD_FAIL
on both `8832824e` (H-gate-rcsum5d) and LOADED `ec21e133` (H-gate-ec21 / H-res-ec21)**
same MULTI_DRIVE +0x53 class. **READY_TO_DEPLOY residual=NO**. Competing next soles
parent picks **ONE:** **Q-SF3** (wide Fix-3) **OR** **R-csum6** (residual multi-drive).
**ZERO second Quartus**; mid-fit thrash **FORBIDDEN**; ONE menu after BUILD_OK only;
thrash ban residual banned + **no luck redeploy `ec21e133` for residual green**.
**WIDE ≠ residual ≠ 3l2**; soft-skip ≠ PASS; hard expect sticky raw[13]==0x14 ≥2
reject +0x53; **3l2 BLOCKED**. Report: `/tmp/misterplex-agent-L-csum-note37.txt`.


#### Q-fix2 wide track dual-path protocol (H-proto-qfix2)

> **STATUS 2026-07-24 ~13:49 CDT:** Exclusive **Q-fix2 LIVE** (wide Fix-2 sole; lock
> `Q-fix2 2026-07-24T13:45:49-05:00`; log `/tmp/plex_quartus_fix2.log`; claim
> `worker=Q-fix2-wide SRC_colorbars=f1d9666ada5347dbde7e7246bad345c8`). Lab residual
> still **LOADED `8832824e` HARD_FAIL** multi-drive (H-gate-rcsum5). Canonical card:
> `/tmp/misterplex-agent-H-proto-qfix2.txt`. Protocol agents do **not** deploy / fit /
> edit RTL. **Do not invent BUILD_OK.**

**While LIVE (hard):**

| Rule | Value |
|------|--------|
| Second Quartus | **ZERO** |
| Mid-fit RTL thrash | **ZERO** (docker `-v` shared mount) |
| Residual thrash-redeploy | **FORBIDDEN** **`8832824e`/`75da8bb1`/`4d6ee356`** |
| Residual re-sole / R-csum6 | **BLOCKED** until exclusive FREE + intentional multi-drive LOCK_OK |
| soft-skip | ≠ PASS |
| 3l2 | **BLOCKED** until non-DIAG product sticky 0x14 |

**After Q-fix2 BUILD_OK (wide only):** NEW_RBF ∉ banned `{8832824e,75da8bb1,4d6ee356,…}`
→ **ONE** promote + **ONE** menu → **wide eyes-on gate separate** from residual hard
gate (frac ≥ 0.95; ≠ PILLAR_320_of_529; FBAR still green). **WIDE PASS ≠ residual PASS**.

**Residual R-csum6:** serial only after Q-fix2 frees + intentional multi-drive RTL freeze
LOCK_OK — **not** luck redeploy of **8832824e**. Hard expect sticky raw[13]==0x14 ≥2;
reject +0x53/push. Evidence: this card, H-proto-rcsum6, L-csum-note34, M-fitmon-qfix2,
PHASE_BACKLOG dual-track stamp.

#### STOP mid-fit RTL + R-csum5 LIVE provenance (L-csum-note29)

> **ACTIVE while exclusive R-csum5 LIVE** (log `/tmp/plex_quartus_rcsum5.log`; lock
> `2026-07-24T13:31:58-05:00`). **NOT BUILD_OK** — do not invent. Docs agents only.

| Fact | Value |
|------|--------|
| Sole | **R-csum5** exclusive **LIVE** (only one Quartus) |
| Claim@launch (product sticky) | Plex **`6422fb9a…`** / slice **`8e6af3bb…`** **DIAG=ABSENT** |
| Claim freeze **now** (overwritten mid-fit) | Plex **`6a5dcaaa…`** / slice **`7d4a1d8b…`** **DIAG=PRESENT** (`residual_csum<=8'h14`) |
| Class | **DRIFT_CRITICAL** → **PROVENANCE_UNTRUSTED** until map/fit SRC proven to match an **intentional** freeze |

**Launch product pack was overwritten mid-fit.** Claim-dir rewrite (~13:33) and live WT thrash under docker `-v` do **not** re-authorize the sole; product claim **`6422fb9a`/`8e6af3bb`** remains map-era intent, but disk/claim now show DIAG thrash → treat any exit-0 RBF as **PROVENANCE_UNTRUSTED** until parent proves map-era / fit inputs == intentional freeze (or accepts-ambiguous / refreezes + re-soles).

**Forbidden now (until exclusive FREE):**
- Any further edits to `fpga/Plex_MiSTer/Plex.sv` or `slice_hdr_parser.sv` (or other residual RTL under the docker bind)
- Second Quartus / re-claim while fit LIVE
- Thrash-redeploy lab **`75da8bb1`** (or **`4d6ee356`**)

**After BUILD_OK (serial only):**
1. **Trust branch first** — LOCK_OK intentional product freeze **or** parent accept-ambiguous **or** free exclusive → refreeze intentional tree → re-sole clean. Do **not** promote untrusted DIAG thrash as product.
2. Only then **ONE** menu (`H-deploy-rcsum5`); never second for luck.
3. **Thrash `75da8bb1` forbidden.** soft-skip ≠ PASS; **DIAG ≠ product PASS**; **3l2 BLOCKED** until **non-DIAG product** sticky `raw[13]==0x14` ≥2.

Evidence: `/tmp/misterplex-agent-R-csum5-build.txt`, `/tmp/plex_quartus_rcsum5.claim/`,
`/tmp/plex_quartus_rcsum5.log`, `/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt`,
`/tmp/misterplex-agent-L-csum-note27.txt`…`note32.txt`, `/tmp/misterplex-agent-H-gate-rcsum4.txt`,
`/tmp/misterplex-agent-M-fitmon-rcsum5d.txt`, `/tmp/misterplex-accept-ambiguous-rcsum5.txt`.

#### L-csum-note30 stamp — R-csum5 still LIVE ~fit (docs only; historical mid-fit)

> **R-csum5 still LIVE ~fit** (superseded by **L-csum-note31 BUILD_OK**). Claim/live
> **`6a5dcaaa`/`7d4a1d8b` DIAG PRESENT**. **PROVENANCE_UNTRUSTED** vs launch product.
> **STOP mid-fit RTL**. **READY_TO_DEPLOY=NO**. Thrash **`75da8bb1` forbidden**.
> soft-skip ≠ PASS; **3l2 BLOCKED**. Agent: `/tmp/misterplex-agent-L-csum-note30.txt`.

#### Post–R-csum3 sole-deploy + hard-gate protocol (ONE agent after BUILD_OK)

> **STATUS 2026-07-24 (H-proto-rcsum4f / L-csum-note22):** **R-csum3b** lab **`4d6ee356` HARD_FAIL**;
> sole menu **DEPLOY_OK**; **FBAR PASS**; **res_dc PASS**; **hard csum FAIL** — **raw[13] additive +0x53/push**
> (**0x53=6739 lo**); **not sticky 0x14**. Class **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM**
> + **MAP_OK**. H-gate 3b2+**3b3** reconfirm. **3l2 BLOCKED**; **no thrash `4d6ee356`**.
> **R-csum4:** claim freeze **`94db41b7`/`9a2d10c5`** DIAG sticky-0x14; log
> `/tmp/plex_quartus_rcsum4.log` — **BUILD_OK evidence ~13:22** wall **421s** exit **0**;
> NEW_RBF **`75da8bb1`** ∉ banned. **DRIFT_CRITICAL** live ≠ claim (accept-ambiguous path used for one-menu bisect).
> **H-deploy-rcsum4 DEPLOY_OK** lab **LOADED `75da8bb1`**; **F-prep PACKAGE_OK**. See § *Post–R-csum4* above.
> **Gate expect** sticky raw[13]==0x14 = **pack proven** else **+0x53=multi-drive** (probe read never 0x14). **DIAG ≠ product PASS**.
> **DIAG ≠ product PASS**. soft-skip ≠ hard PASS. Expect product green still **`e8 14 53 1a`** (non-DIAG).
> **DO NOT edit RTL mid-fit** (docker `-v`); **DO NOT thrash 4d6ee356**.
>
> **Historical runbook** below still shapes sole post-BUILD_OK owners (done for `4d6ee356` → HARD FAIL).
> Prefer product (non-force) RBF for 3l2 unblock. DIAG bisect ≠ product PASS. One menu deploy only.
> Do **not** invent BUILD_OK / hard PASS. Soft-skip ≠ PASS.

**Preconditions (all required):**

1. R-csum1 Full Compilation **successful** (`BUILD_OK` in `/tmp/plex_quartus_rcsum1.log`; exit 0).
2. New RBF md5 **≠** `820484a686dc6b744954e3c8ef8df3f4` (prefix **≠** `820484a6`).
3. Build inputs were the dirty-fix tree (or post-commit equivalent): running XOR +
   ST_PLACE `lev[]` recompute + `Plex.sv` `st_res_*` preserve barrier — **not** the
   34bf755 tmpc-fold-only path that produced `820484a6`.
4. No parallel residual fit; no mid-fit RTL thrash of `Plex.sv` / `slice_hdr_parser.sv`.
5. **Single owner** for promote → deploy → gate → park (no concurrent `load_core`).

##### ONE-agent checklist (after BUILD_OK only)

Copy-paste order for the sole post-BUILD_OK agent. **Historical checklist** (already run for dabdaeb0; hard FAIL). Do not invent PASS.

1. **Confirm BUILD_OK** — log shows successful Full Compilation; exit 0; do **not** invent.
2. **Collect md5** — `md5sum fpga/Plex_MiSTer/output_files/Plex.rbf` → record full hash + 8-char prefix; **abort** if prefix still `820484a6`.
3. **Promote paths if needed** — copy *same* bitfile to:
   - `fpga/Plex_MiSTer/releases/Plex.rbf`
   - `releases/Plex.rbf`
   Re-`md5sum` all three; all **identical** and **≠ 820484a6**.
4. **Sole menu deploy (ONE only)** —
   ```bash
   DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh
   # or explicit promoted path:
   DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh releases/Plex.rbf
   ```
   Never raw `scp` + `echo load_core … > /dev/MiSTer_cmd` thrash. Never second deploy same tick.
5. **Lab confirm** — remote `/media/fat/_Utility/Plex.rbf` md5 == local new; `/tmp/CORENAME` = **Plex**. If already match after the one deploy, **do not** redeploy.
6. **FBAR** — `./tests/hw/test_fbar_fast.sh` → EXIT=0; hard ok m1/m2 ≥15 (expect PASS; Fix-1 colorbars still in tree).
7. **Hard residual gate** (soft-skip is **NOT** PASS) —
   - Control: `res_ok=1 res_tc=8 res_t1=3 mb0=0 qp=25`
   - **Hard:** `res_dc=-24` (`raw[12]=0xE8`) **and** `res_csum=20` (`raw[13]=0x14`)
   - **Stable** across ≥2 re-pushes after F3 Baseline push
   - Tools: `tests/hw/test_f3_residual.sh` for push/print; then **agent-enforced** hard check via
     `python3 tests/parse_res_csum_status.py` (**A-csum-host2**) on captured status/raw lines;
     lab print path: `push_frame --status` / `--raw` (**A-arm-csum** READY on lab)
8. **Park bars** — `set_status --pattern bars --force-bars 1 --tv ntsc --fps 60`
9. **Report** — write `/tmp/misterplex-agent-H-rcsum-gate.txt` (md5s, FBAR scores, ≥2 raw probes, hard PASS/FAIL, soft-skip note).

##### Numbered steps (detail table)

| # | Action | Pass when / command |
|---|--------|---------------------|
| 1 | **Confirm BUILD_OK** | Log shows successful compile; `output_files/Plex.rbf` mtime/md5 post-build |
| 2 | **Collect RBF md5** | `md5sum fpga/Plex_MiSTer/output_files/Plex.rbf` → record full + 8-char prefix |
| 3 | **Promote RBF** (host paths only) | Copy *same* bitfile to both: `fpga/Plex_MiSTer/releases/Plex.rbf` **and** `releases/Plex.rbf`. All three md5s **identical** and **≠ 820484a6** |
| 4 | **ONE menu deploy** | `DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh` (optional path arg: promoted RBF). **Never** raw `scp` + `echo load_core … > /dev/MiSTer_cmd` thrash |
| 5 | **Lab confirm** | Remote `/media/fat/_Utility/Plex.rbf` md5 == local new; `/tmp/CORENAME` = **Plex**. If already match after deploy, **do not** redeploy |
| 6 | **FBAR** | `./tests/hw/test_fbar_fast.sh` → EXIT=0; hard ok m1/m2 ≥15 |
| 7 | **Hard residual gate** | See hard criteria + tool recipe below — **NOT** soft-skip |
| 8 | **Stability** | ≥2 extra `push_frame` re-pushes: `res_dc` and `res_csum` **stable** at goldens |
| 9 | **Park bars** | `set_status --pattern bars --force-bars 1 --tv ntsc --fps 60` |
| 10 | **Report** | `/tmp/misterplex-agent-H-rcsum-gate.txt`: md5, FBAR, raw probes, PASS/FAIL |

**Hard residual criteria (agent must enforce; script soft-skip is insufficient):**

| Field | Status / raw | Hard expect |
|-------|--------------|-------------|
| `res_ok` / `res_tc` / `res_t1` | status line | `1` / `8` / `3` |
| `mb0` / `qp` | status line | `0` / `25` |
| `res_dc` | `raw[12]` | **`-24` / `0xE8`** stable |
| `res_csum` | `raw[13]` | **`20` / `0x14`** stable across ≥2 re-pushes |
| stream_bytes | `raw[14:15]` LE | live counter — **must not** equal csum (aa146c17 pack alias) |

```bash
# 1) Push Baseline F3 residual (script may soft-skip csum — ignore soft PASS):
./tests/hw/test_f3_residual.sh
# Soft-skip EXIT=0 on res_csum mismatch is NOT hard PASS.

# 2) Capture status / raw (lab ARM — A-arm-csum tools READY):
#   push_frame --status   → must show res_dc=… res_csum=…
#   push_frame --raw      → 16B hex; indices 12..15 = residual pack

# 3) Offline hard decode (host — A-csum-host2; no SPI thrash):
python3 tests/parse_res_csum_status.py              # goldens + map
python3 tests/parse_res_csum_status.py --self-test  # offline OK
# pipe a captured line, or pass raw[12..15] hex:
echo '<status or raw line>' | python3 tests/parse_res_csum_status.py -
python3 tests/parse_res_csum_status.py e8 14 2a 00  # expect HARD_PASS
# Require GATE hard=PASS and class HARD_PASS on ≥2 independent probes.
```

Host goldens (locked unit — never invent other values):

| Field | Value | Notes |
|-------|-------|-------|
| `res_dc` | −24 / `0xE8` | `sat8(coeff[0])` |
| `res_csum` | 20 / `0x14` | XOR sat8 full-16 — **never** arith sum −20 / `0xEC` |

**Hard PASS only if:** FBAR green **and** `res_dc=-24` **and** `res_csum=20` (`raw[13]==0x14`) stable **and** agent did **not** treat soft-skip as PASS.  
**Then (only):** unlock 3.3l-2 paint path (inv_quant/IDCT / `files.qip` per *P3-3l2 UNBLOCK GATE*).  
**Else (post-deploy FAIL):** keep 3l2 **BLOCKED**; follow failure branches below (measure first — no thrash).  
WIDE re-eyes (W-proto7) orthogonal, not a csum blocker.

**NO-GO (do not do):**

- Redeploy **`820484a6`** expecting csum green (H-deploy-fix1 / H-gate-fix1 already FAIL: 232/59/142 class)
- Kill / restart / second Quartus while any residual fit is LIVE
- Second / thrash deploy in the same tick (one `DEPLOY_LOAD=menu` only)
- Count `test_f3_residual.sh` soft-skip EXIT=0 as hard csum PASS
- Invent hard PASS without post-deploy `raw[13]==0x14` evidence
- Start 3.3l-2 paint fit / half-wire `files.qip` before hard csum PASS
- Pre-write further RTL “fixes” before measuring the new RBF
- Residual push storm / thrash loops while diagnosing (capture ≥2 probes, park, report)

**Failure branches (only after post–R-csum1 sole deploy still hard-fails — measure first):**

From R-csum-rca3/4 contingency. Capture ≥2 `push_frame --status`/`--raw` probes, classify with
`parse_res_csum_status.py`, park bars, write report — **then** decide next RTL owner.
Do **not** re-open the tmpc-fold theory as first guess (that is the pre-R-csum1 RCA).

| # | Symptom after **new** RBF | Class (RCA contingency) | Next |
|---|---------------------------|-------------------------|------|
| a | `res_csum` **+0x53 per push** / stream24 LE — dabdaeb0 **`e8 53 1a 00`…** (`raw[13:15]==N×6739`); **`4deaf6cc` HARD FAIL same class** (0x89/0xdc/0x2f; 0xd5/0x7b/0x21; DIAG force no stick) | **STREAM24_LE_ALIAS / PACK_FAIL** (R-csum-rtl3 corrected RCA) — residual_csum absent; stream occupies csum byte. **Not** residual XOR intermediate | Historical: **R-csum-rtl3** → **R-csum3b** → lab **`4d6ee356`**. soft-skip ≠ PASS; zero thrash |
| a′ | lab **`4d6ee356` HARD_FAIL** hard probes **raw[13] additive +0x53/push** (**0x53=6739 lo**; 85→168→251; 3b2 78→161→244; **3b3** 71→154→237) but **raw[13] ≠ stream lo8** (pack slots distinct); **MAP_OK** | **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** (L-csum-note21) — pack-likely-fixed but csum still wrong/unstable | **Next: `R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute** (DIAG on raw[13] → pack proven; else multi-drive). **no thrash-redeploy `4d6ee356`**. **3l2 BLOCKED**; soft-skip ≠ PASS |
| a″ | future **DIAG force-0x14** sole (R-csum4 bisect class) | **PACK_PROOF vs MULTI_DRIVE** (L-csum-note21) | **If sticky raw[13]==0x14** → pack proven → residual is **compute** on product path. **If force does not stick** → **multi-drive** / pack still broken (status path fights force). **DIAG ≠ product PASS**; **≠ 3l2 unblock** |
| b | `res_csum` **live/stable** but ≠ `0x14` (incl. soft mid-stream sample raw **`e8 02 d7 01`** csum **`0x02`**; stale arith `0xEC`) | **CSUM_VALUE_FAIL** / pack-likely-fixed (note17 soft-path subclass of **a′**) — level fold / sat8 / wrong residual multiset | Check `lev[]` write order, clear-before-signs, multi-cycle XOR vs host; reject arith-sum fold; **no thrash 4d6ee356**; hard SoT still **a′** |
| c | `res_csum` == stream_bytes low again | pack regression (aa146c17 class) | Re-check `Plex.sv` pack `[111:104]` vs `[127:112]`; helper class `STREAM_BYTES_ALIAS` |
| d | `res_csum` == `0xE8` stable (== dc) | ST_PLACE still dc-only collapse | Confirm lev[] recompute actually in **bitstream** (source/md5 mismatch vs R-csum1 inputs?) |
| e | `res_dc` broken (≠ −24) | scalar regression vs R-csum1 delta | **STOP**; residual_dc must stay green — bisect vs dirty fix; no paint, no thrash |
| f | FBAR FAIL | video/pattern path | Fix FBAR first; do **not** residual thrash loop |
| g | Lab ARM lacks `res_csum=` print | tools lag (pre A-arm-csum) | Use `push_frame --raw` / `set_status --raw` + host parse helper; optionally re-push A-arm bins only (**no** RBF thrash) |

Residual risk (**L-csum-note21** lab SoT): **`4d6ee356` HARD_FAIL** class **PACK_LIKELY_FIXED +
CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK** — **raw[13] additive +0x53/push** (**0x53=6739 lo**); **not sticky 0x14**.
Pack path **likely fixed** vs pure stream24 **PACK_FAIL** on **`4deaf6cc`/`dabdaeb0`** (csum slot ≠ stream lo8)
but **csum still wrong/unstable**. res_dc green. **3l2 BLOCKED**. **Next: `R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute**
(DIAG on raw[13] → pack proven; else multi-drive). **no thrash-redeploy `4d6ee356`**. Expected product green: **`e8 14 53 1a`**. soft-skip ≠ PASS.

**Deploy command reference (only path allowed for this gate):**

```bash
# After promote (checklist step 3):
DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh
# or explicit:
DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh releases/Plex.rbf
# Env: MISTER_HOST (set MISTER_HOST), MISTER_PASS, DEPLOY_WAIT_S=5
# DEPLOY_LOAD=none = copy only (not enough alone for this gate if CORE still old)
# DEPLOY_LOAD=core = Plex-only reload (only if already on Menu)
```

**Tools inventory (ready before BUILD_OK):**

| Tool | Owner / path | Role |
|------|--------------|------|
| Host parse helper | `tests/parse_res_csum_status.py` (A-csum-host2) | Offline EXPECTED vs ACTUAL + GATE; no SPI |
| Lab status print | `push_frame --status` / `--raw` (A-arm-csum READY) | Live `res_csum=` + raw[13] |
| Residual push | `tests/hw/test_f3_residual.sh` | F3 Baseline push; soft-skip ≠ hard PASS |
| FBAR | `tests/hw/test_fbar_fast.sh` | Visual/pattern gate before residual trust |
| Safe deploy | `scripts/deploy_plex_core.sh` `DEPLOY_LOAD=menu` | Sole load path |

Report template peers: H-deploy-fix1 / H-gate-fix1 scorecards.  
RCA SoT: R-csum-rca3 + R-csum-rca4. Protocol authors: H-rcsum-proto / H-rcsum-proto2.

### 3.3l-2 — Inv quant + IDCT on first 4×4 (logic-only)

**Host prep (done, no Quartus):** `h264_residual_gold.hpp` locks dequant + recon paint;
`test_idct_quant` prints `FPGA_GOLD deq4x4` / `recon_y4x4` / `recon_y00` / `recon_mean`.

New small FSM after residual place (in parser or `mb_recon`):

1. `dequant4x4(coeff, max=16, qp=slice_qp±mb_qp_delta)`  
2. pred block = 128 (MB0 4×4, no neighbours)  
3. `idct4x4_add` → 4×4 recon Y  
4. Status: mean of 16 samples or y00 for eyes-on  
5. `decode_stub`: paint **top-left 4×4** (or full 16×16 if only one block) with true recon gray/RGB

**Bit-exact target vs host** for that 4×4 (mae=0 on block).

#### Locked host paint vector (Baseline first residual)

Source of truth: `host/libmisterplex/h264_residual_gold.hpp` + `tests/unit/test_idct_quant.cpp`
(`3l2-table` table-only + `3l2-real` annex-B). `make unit` locks both.

| Step | Value |
|------|-------|
| coeff scan | `-24 4 4 0 -4 0 -1 0 0 -1 1 0 1 0 0 0` |
| qp / pred | 25 / **128** (unavailable neighbours → DC) |
| residual_csum | **20 / 0x14** = XOR sat8(coeff[i]) — **not** stale arith sum **−20 / 0xEC** |
| dequant 4×4 (row-major) | `-4224 896 0 -224` / `896 -1152 0 288` / `0 0 0 0` / `-224 288 0 0` |
| recon Y 4×4 | `73 72 76 76` / `72 74 71 73` / `76 71 32 27` / `76 73 27 24` |
| y00 / mean4×4 | **73** / **62** = `(sum+8)/16` |
| paint y00 RGB565 | **0x4A49** (`grayRgb565(73)`) |
| 3.3k stub contrast | MB0 gray = `128+dc` = **104** (RGB565 **0x6B4D**) — **not** true recon |
| frame_store addrs @W=320 | `0..3, 320..323, 640..643, 960..963` |

#### `decode_stub` top-left 4×4 sketch (no Quartus this fire)

Today (`decode_stub.sv` 3.3k): when `residual_ok`, **entire MB0 16×16** is painted
`clamp(128 + residual_dc)` → gray **104**, packed RGB565 `{R[7:3],G[7:2],B[7:3]}` with R=G=B.

**3.3l-2 target paint** (logic-only, no new M10K):

```text
// After inv_quant + idct_add onto pred=128 → recon_y[0:3][0:3] = residual_gold::kY
// Sequential paint (existing wr_en / wr_pixel / wr_reset_ptr path):
//   for y in 0..239:
//     for x in 0..319:
//       if (x < 4 && y < 4 && lat_res_ok && recon_ready)
//         wr_pixel = gray_rgb565(recon_y[y][x]);   // true 4×4
//       else if (mb0 && lat_res_ok)
//         // optional: leave rest of MB0 as diagnostic grid / stub 104 / or 128
//         ...
// frame_store linear addrs for top-left 4×4 @ W=320:
//   0,1,2,3, 320,321,322,323, 640..643, 960..963
// y00 RGB565 = grayRgb565(73)  (host residual_gold::kPaintY00Rgb565)
```

**Minimal RTL add** (prefer regs only):

| Item | Notes |
|------|-------|
| Hold `coeff[0:15]` after ST_PLACE | already 3.3l-1 tree |
| `inv_quant4` + `idct4x4` FSM | multi-cycle; match host butterflies |
| `recon_y[0:3][0:3]` 16×8b regs | paint source |
| `recon_ready` sticky | gate paint vs incomplete IDCT |
| Status soft | `recon_y00=73`, `recon_mean=62`; **keep `res_dc=-24`** |

Do **not** require full MB0 true recon in 3.3l-2 — only first coded 4×4 (scan block 0).
Rest of frame may stay strip/MB-grid diagnostic.

**Exit unit:** `test_idct_quant` locks deq + y[][] + paint contract (stub 104 ≠ y00 73).  
**Exit HW:** `tests/hw/test_f3_idct_mb0.sh` — residual gates + recon mean/y00 match host golden.

---

### P3-3l2 UNBLOCK GATE (L-3l2-gate / L-3l2-gate2 — exact checklist)

> **Residual hard-block LIFTED (L-csum-note41 / H-gate-rcsum6):** lab product **`94bbfe43`** met
> **non-DIAG product sticky `raw[13]==0x14`** (sticky0x14=**7/7** plus53=**0** res_dc PASS ideal **e8 14**).
> **3l2 residual hard-block LIFTED** — may start **3.3l-2 product IDCT wire-up / gate**.
> **≠ full 3l2 product DONE** (paint/fit still open). **≠ WIDE PASS** (0.605 historical).
> soft-skip ≠ PASS. Do **not** re-run residual gate or thrash-redeploy **94bbfe43**.
> Historical FAIL refs (thrash FORBIDDEN): DIAG **`75da8bb1`**, product **`4d6ee356`/`8832824e`**, wide residual **`ec21e133`**.
> **DIAG sticky alone ≠ product PASS**. Full §A criteria table retained below as SoT formula.

#### A) Hard residual PASS criteria (ALL required — no soft-skip)

| # | Criterion | Exact pass value | How to read |
|---|-----------|------------------|-------------|
| **A0** | **Post residual-fix sole deploy only** | Candidate **product** (non-DIAG / non-force) fix RBF lab-loaded via one `DEPLOY_LOAD=menu` (lab **`4d6ee356`** HARD FAIL +0x53/push; next must be **≠ 4d6ee356** / **≠ 4deaf6cc** / **≠ dabdaeb0**) | Do **not** invent PASS; **do not** thrash-redeploy **`4d6ee356`**; DIAG force-0x14 RBF ≠ product unblock |
| A1 | `res_dc` | **−24** | `push_frame --status` → `res_dc=-24`; **`raw[12]==0xE8`** |
| A2 | `res_csum` | **20** | `res_csum=20`; **`raw[13]==0x14`** (XOR sat8 full-16 — **never** arith −20 / 0xEC); must be **real fold**, not DIAG force |
| A3 | No soft-skip | Hard only | `test_f3_residual.sh` soft EXIT=0 on csum mismatch is **NOT** hard PASS and **NOT** unblock (**soft-skip ≠ hard PASS**) |
| A4 | Stability | ≥2 re-pushes | A1+A2 hold across ≥2 Baseline F3 pushes (not one lucky sample) |
| A5 | Control plane | green | `res_ok=1 res_tc=8 res_t1=3 mb0=0 qp=25`; FBAR still green on *new* RBF |
| A6 | Host unit still locked | 0x14 | `make unit` / `test_idct_quant` — do not re-derive goldens |
| A7 | Non-DIAG product | policy | Unblock 3l2 only on **non-force product** bitstream; DIAG force-0x14 pack bisect is evidence class only |

**One-line hard PASS formula (L-3l2-gate2):**

```text
HARD_PASS = FBAR green on candidate residual-fix **product** RBF (non-DIAG / non-force)
         && res_dc  = -24  (raw[12] == 0xE8)  // A1
         && res_csum = 20  (raw[13] == 0x14)  // A2 — real fold, not DIAG force
         && stable across ≥2 re-pushes        // A4
         && NOT soft-skip-as-pass             // A3: script EXIT=0 on mismatch ≠ PASS
// 4deaf6cc (R-csum2 multi-cycle + DIAG force BUILD_OK): A1 PASS; A2+A4 FAIL → PACK_FAIL → NOT HARD_PASS
// dabdaeb0 (R-csum1 XOR+lev): A1 PASS; A2+A4 FAIL → still FAIL reference → NOT HARD_PASS
// → 3.3l-2 paint remains BLOCKED (soft-skip ≠ hard PASS; DIAG force ≠ product PASS; force no stick)
```

**NOT hard PASS / NOT unblock:**
- `test_f3_residual.sh` EXIT=0 with soft-skip on `res_csum` mismatch (**soft-skip ≠ hard PASS**)
- host unit XOR `0x14` alone (already locked — HW must match)
- thrash-redeploying **`4deaf6cc`** or **`dabdaeb0`** or prior **`820484a6`** expecting csum green
- inventing PASS without post-deploy **`raw[13]==0x14`** evidence
- A1 alone (res_dc green) without A2 — **both** required (4deaf6cc and dabdaeb0 are this case)
- treating soft-skip EXIT=0 as unblock
- treating **DIAG force-0x14** pack-bisect RBF as product hard PASS / 3l2 unblock (and on 4deaf6cc force **did not stick**)

**Lab now (blocked evidence — L-csum-note21 / H-gate-rcsum3b + 3b2 + 3b3):** RBF **`4d6ee356` HARD_FAIL**
(R-csum3b BUILD_OK product; sole menu **DEPLOY_OK**; lab md5 match). A1 PASS (stable 0xE8);
**FBAR PASS** (7.0/82.9/94.4); **A2 FAIL** (raw[13] **0x55→0xa8→0xfb** / **0x4e→0xa1→0xf4** / **0x47→0x9a→0xed** — never 0x14;
**additive +0x53/push**; **0x53=6739 lo**); class **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK**;
pack-likely-fixed vs pure stream24 alias; A3 soft-skip EXIT=0 is **not** unblock; A4 FAIL (unstable). Contingency §D **ACTIVE**: **no 3l2**.
**no thrash-redeploy `4d6ee356`**. **Next: `R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute**
(DIAG on raw[13] → pack proven; else multi-drive). Historical FAIL refs: `4deaf6cc` PACK_FAIL; `dabdaeb0` +0x53.
Evidence: `/tmp/misterplex-agent-H-gate-rcsum3b.txt`, `/tmp/misterplex-agent-H-gate-rcsum3b2.txt`,
`/tmp/misterplex-agent-H-gate-rcsum3b3.txt`, probes `/tmp/misterplex-H-gate-rcsum3b*-probes.txt`,
FBAR `/tmp/misterplex-H-gate-rcsum3b-fbar.log`, MAP `/tmp/misterplex-agent-A-csum-map1.txt`.

```bash
# After sole deploy of NEW RBF only (A0):
./tests/hw/test_f3_residual.sh                 # status print; soft EXIT=0 ≠ PASS
# Agent hard-check (status + raw):
#   res_dc=-24  res_csum=20
#   raw[12]=e8  raw[13]=14
python3 tests/parse_res_csum_status.py e8 14 .. ..   # expect dc=-24 csum=20
# ≥2 extra push_frame re-pushes → both fields STABLE
```

Helper: `python3 tests/parse_res_csum_status.py` (or pipe status line through `-`).

#### B) Pre-unblock sequence (R-csum path — 3l2 must wait)

Hard residual measurements (§A) **only count after B1**. No parallel paint work during B0–B3.

| Step | Owner | Action | Gate / result (2026-07-24) |
|------|-------|--------|----------------------------|
| B0 | R-csum1 | BUILD_OK new RBF md5 **≠** `820484a6` | **DONE** — → **`dabdaeb0`** (still FAIL ref) |
| B0′ | R-csum2 | BUILD_OK new RBF md5 **≠** dabdaeb0 | **DONE** — → **`4deaf6cc`** (PACK_FAIL) |
| B0″ | R-csum3b | BUILD_OK product pack RBF | **DONE** — → **`4d6ee356`** (freeze `ce1ef26c`/`e45f98c4`) |
| B1 | Deploy | **One** sole menu deploy of *new* RBF | **DONE** — lab==host **`4d6ee356`**; CORENAME=Plex (H-deploy-rcsum3b) |
| B2 | FBAR | `test_fbar_fast` reconfirm on **new** RBF | **PASS** on **`4d6ee356`** (7.0/82.9/94.4) |
| B3 | Hard residual | §A **A0–A5** | **FAIL reconfirm** — res_dc=-24 OK; raw[13] **+0x53/push** never 0x14; class **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK**; soft-skip ≠ PASS (H-gate-rcsum3b + **3b2** + **3b3**) |
| B4 | Unblock 3l2 | Only if B3 hard PASS | **NOT unlocked** — 3.3l-2 **remains BLOCKED** (contingency §D) |

Report for R-csum3b B0″–B3: `/tmp/misterplex-agent-H-gate-rcsum3b.txt`, `/tmp/misterplex-agent-H-gate-rcsum3b2.txt`, `/tmp/misterplex-agent-H-gate-rcsum3b3.txt`, `/tmp/misterplex-H-gate-rcsum3b-fbar.log`.
Next after B3 FAIL: **`R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute** (DIAG on raw[13] → pack proven; else multi-drive; no thrash-redeploy **`4d6ee356`**; no paint).

#### C) After hard PASS only — SV wire-up order (logic-only, **no new M10K**)

> **Execute C0–C9 only after §A HARD_PASS on post–R-csum1 lab RBF.** Docs-only until then.
> Do **not** half-wire. Prefer full modules at sole paint-fit start; **no orphan SV** in
> tree / `files.qip` before C1 authorized. Do **not** check in SV while gate is open.
> Order is strict (C1→C2 before C3; C7 only after modules exist; C8 sole fit).

| Step | Action | File(s) | Verify |
|------|--------|---------|--------|
| **C0** | Confirm §A hard residual green on **new** lab RBF | H-rcsum-gate report | **`raw[13]==0x14` AND `res_dc=-24`** stable; no soft-skip; md5 ≠ `820484a6` |
| **C1** | Add `inv_quant4` — match host `dequant4x4` / `kDeq` (deq00=**-4224**) | `fpga/Plex_MiSTer/rtl/inv_quant4.sv` | table vs `residual_gold::kDeq` |
| **C2** | Add `idct4x4_add` — pred=128 → `kY` (y00=**73** mean=**62**) | `fpga/Plex_MiSTer/rtl/idct4x4_add.sv` | host butterflies; **no new M10K** |
| **C3** | Instantiate both in **`stream_path.sv`** next to `decode_stub` (**not** inside `slice_hdr_parser`) | `fpga/Plex_MiSTer/rtl/stream_path.sv` | residual bus untouched; no csum recompute |
| **C4** | Feed `residual_coeff[0:15]` + `slice_qp`/`sl_qp`(=25); start on `residual_ok` / `sl_res_ok` rise | same | coeffs stable post ST_PLACE |
| **C5** | Extend `decode_stub`: paint **top-left 4×4 only** when `recon_ready`; rest MB0 may stay stub 104 | `fpga/Plex_MiSTer/rtl/decode_stub.sv` | addrs @W=320: `0..3,320..323,640..643,960..963`; y00 RGB565 **0x4a49** |
| **C6** | Soft telem optional: `recon_y00=73` / `recon_mean=62` | spare status bytes only | **hard keep** res_dc/csum/ok/tc/t1 |
| **C7** | `files.qip` add both SV modules **only** at sole paint fit start | `fpga/Plex_MiSTer/files.qip` (or equiv) | no orphan modules; `NUM_PARALLEL=2` |
| **C8** | Sole Quartus → one `DEPLOY_LOAD=menu` → FBAR green | paint RBF | BUILD_OK; residual still hard green |
| **C9** | `test_f3_idct_mb0.sh` residual hard + recon soft→hard | HW | y00=73 mean=62 eyes-on ≠104 |

**Do not touch from paint path:** `Plex.sv` residual status pack (`[103:96]` dc / `[111:104]` csum / `[127:112]` bytes); `slice_hdr_parser` ST_PLACE csum/dc compute (R-csum1 owner until green, then leave alone).

Detail interfaces: *3.3l-2 concrete RTL plug sketch* below (L-3l2-rtl).

#### D) Contingency — **ACTIVE** after lab **`4d6ee356` HARD_FAIL** (**no 3l2**; **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK**)

Aligned with **H-deploy-rcsum3b**, **H-gate-rcsum3b/3b2/3b3**, **L-csum-note21**, **A-csum-map1 MAP_OK**, prior **R-csum-rtl3** pack RCA, and *Failure branches*:

- **Lab now:** R-csum3b product RBF **`4d6ee356` HARD_FAIL**; sole menu **DEPLOY_OK**; **FBAR PASS**; res_dc=-24 PASS; **hard csum FAIL**.
- **Hard-gate SoT series:** 3b **`e8 55…`/`e8 a8…`/`e8 fb…`** (85→168→251); 3b2 **`e8 4e…`/`e8 a1…`/`e8 f4…`** (78→161→244); **3b3** **`e8 47…`/`e8 9a…`/`e8 ed…`** (71→154→237) — **raw[13] additive +0x53/push** where **0x53 = 6739 lo**; **never sticky 0x14**.
- **Class (L-csum-note21):** **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK** — pack path **likely fixed** vs stream24 **PACK_FAIL** on **`4deaf6cc`** (csum slot ≠ stream lo8) **but csum still wrong/unstable**; host map correct → **blame FPGA**.
- Soft mid-stream sample `e8 02 d7 01` (note17) is **not** hard-gate SoT.
- **Still BLOCKED:** **P3-3l2 paint** until **non-DIAG sticky `raw[13]==0x14`** stable ≥2. Soft-skip ≠ hard PASS.
- **Historical FAIL refs:** `4deaf6cc` PACK_FAIL; `dabdaeb0` +0x53/push; DIAG force no stick on 4deaf6cc.
- **R-csum3b BUILD_OK** freeze **`ce1ef26c`/`e45f98c4`** (128b pack + multi-cycle; DIAG ABSENT) — BUILD_OK ≠ hard residual PASS.
- **R-csum4 BUILD_OK (L-csum-note24) + H-gate HARD_FAIL (L-csum-note25):** claim **`94db41b7`/`9a2d10c5`** DIAG; wall **421s**; **NEW_RBF `75da8bb1`**; H-deploy **DEPLOY_OK**; F-prep **PACKAGE_OK**. **H-gate-rcsum4:** sticky 0x14 **0/7**; **0x40→0x93→0xe6 +0x53/push**; **FBAR PASS**; **NOT PACK_PROVEN**; **DIAG force did not stick**; class **MULTI_DRIVE_OR_STILL_FAIL**. **DIAG ≠ product PASS**. **no thrash-redeploy** of **`75da8bb1`**, **`4d6ee356`**, **`4deaf6cc`**, **`dabdaeb0`**.
- **R-csum-rtl5 FIT_GO + R-csum5 sole LIVE (L-csum-note26):** `st_res_word_sticky` on `residual_ok_rise`; product `residual_csum<=csum_acc`; DIAG stripped; claim@launch freeze **`6422fb9a`/`8e6af3bb`** DIAG=ABSENT. Lock **`R-csum5 2026-07-24T13:31:58-05:00`**; log `/tmp/plex_quartus_rcsum5.log`. **Not** hard residual PASS / **not** BUILD_OK invent (fit LIVE). **After BUILD_OK:** ONE menu → hard sticky **0x14** product; lab **`75da8bb1` HARD_FAIL multi-drive SoT** until new deploy.
- **R-csum5 LIVE + mid-fit claim overwrite (L-csum-note28→note29):** exclusive lock **13:31:58**; **LAUNCH product claim** **`6422fb9a`/`8e6af3bb` DIAG=ABSENT** (map-era intent). **Claim freeze now** **`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT** — launch product pack **overwritten mid-fit** → **PROVENANCE_UNTRUSTED** until map/fit SRC match intentional freeze. **Forbidden now:** further `Plex.sv` / `slice_hdr_parser.sv` edits until exclusive free. **After BUILD_OK:** trust branches before **ONE** menu; thrash **`75da8bb1` forbidden**; soft-skip ≠ PASS; DIAG ≠ product; **3l2 BLOCKED**. See § *STOP mid-fit RTL + R-csum5 LIVE provenance (L-csum-note29)*.
- **R-csum5 still LIVE ~fit (L-csum-note30 stamp; historical):** claim/live **`6a5dcaaa`/`7d4a1d8b` DIAG PRESENT**; **PROVENANCE_UNTRUSTED** vs launch; **STOP mid-fit RTL**; thrash **`75da8bb1` forbidden**.
- **R-csum5 BUILD_OK (L-csum-note31 stamp):** ~**13:40:35 CDT** wall **441s**; **NEW_RBF `8832824e`** full `8832824e483cf6613f82ee3ba3e592b3`; launch product **`6422fb9a`/`8e6af3bb`** → claim/live DIAG **`6a5dcaaa`/`7d4a1d8b`**; map-before-thrash ⇒ product sticky may be in silicon — **PROVENANCE_UNTRUSTED**.
- **Parent accept-ambiguous one-menu serial (L-csum-note32 stamp; historical):** `/tmp/misterplex-accept-ambiguous-rcsum5.txt` names **`8832824e`** → Branch **B1** authorized **ONE promote + ONE menu** (**H-deploy-rcsum5**).
- **H-deploy-rcsum5 DEPLOY_OK + H-gate-rcsum5 HARD_FAIL (L-csum-note33 / note34):** lab **LOADED `8832824e`**; sticky0x14=**0**/7; **+0x53 multi-drive continues after sticky-pack sole** (A **0x16→0x69→0xbc→0x0f**; B **0x62→0xb5→0x08**); **res_dc PASS**; **FBAR soft only**; class **MULTI_DRIVE_OR_STILL_FAIL** / **NOT PACK_PROVEN**; **BUILD_OK+DEPLOY_OK ≠ hard residual PASS**. thrash **`8832824e`/`75da8bb1`/`4d6ee356` forbidden**. soft-skip ≠ PASS. **3l2 BLOCKED**.
- **H-gate-rcsum5d DEFINITIVE + Q-fix2 hold (L-csum-note35):** consolidates 5+5b → sticky0x14=**0**/12; **NOT PACK_PROVEN** / **MULTI_DRIVE_OR_STILL_FAIL** definitive; exclusive **Q-fix2 LIVE** lock **`Q-fix2 2026-07-24T13:45:49-05:00`** — **hold residual RTL**; thrash forbidden; soft-skip ≠ PASS; **3l2 BLOCKED**. **Next residual = multi-drive RCA after Q-fix2 frees** (not redeploy / not residual re-sole under live exclusive).
- **L-csum-note34 — exclusive Q-SF2/Q-fix2 LIVE residual FREEZE (historical alias):** lock `/tmp/plex_quartus.lock` = **`Q-fix2 2026-07-24T13:45:49-05:00`** (wide Fix-2 sole; lock file authoritative). **Residual RTL FREEZE while wide fits.** Thrash FORBIDDEN **including mid-fit residual edits** (`Plex.sv`/`slice_hdr_parser.sv` ZERO under docker `-v`; no residual re-sole under live exclusive). Superseded for definitive wording by note35/36.
- **L-csum-note36 — residual HOLD reconfirm:** lab **`8832824e` HARD_FAIL DEFINITIVE** (H-gate-rcsum5d); residual RTL **HOLD** during/after Q-SF2; thrash **`8832824e`/`75da8bb1`/`4d6ee356` FORBIDDEN**; **WIDE Fix-2 NEW_RBF ≠ residual hard PASS ≠ 3l2 unblock**; soft-skip ≠ PASS; **3l2 BLOCKED**; next residual = multi-drive RCA (not luck residual redeploy).
- **L-csum-note37 — dual-track post Q-SF2 BUILD_OK+deploy+WIDE FAIL:** exclusive **FREE**; **W** **`ec21e133`** WIDE FAIL **0.605**; residual **HARD_FAIL both `8832824e` + `ec21e133`**; READY residual **NO**; competing **Q-SF3 OR R-csum6** one sole; thrash ban residual + no luck **`ec21e133`** residual green; soft-skip ≠ PASS; **3l2 BLOCKED**.
- **H-gate-rcsum4b** reconfirm lab **`75da8bb1` HARD_FAIL MULTI_DRIVE** (historical prior SoT; sticky0x14=0; **+0x53/push** 0x85→0xd8→0x2b); **NOT PACK_PROVEN**.
- Branch **B ACTIVE** (multi-drive on lab **`8832824e`** and LOADED wide **`ec21e133`**; same family as 75da8bb1/4d6ee356); Branch **A PACK_PROVEN** not observed. Exclusive slot: **FREE** (post Q-SF2). Competing next sole: **Q-SF3 OR R-csum6** (parent one).
- **3l2 BLOCKED** until **non-DIAG product sticky 0x14**.
- Probe SoT: `/tmp/misterplex-agent-H-gate-rcsum5d.txt` (DEFINITIVE), `/tmp/misterplex-agent-H-gate-rcsum5.txt`, `/tmp/misterplex-agent-H-gate-rcsum5b.txt`,
  `/tmp/misterplex-H-gate-rcsum5-probes.txt`, `/tmp/misterplex-H-gate-rcsum5-summary.txt`,
  `/tmp/misterplex-agent-H-deploy-rcsum5.txt`, `/tmp/misterplex-agent-H-gate-rcsum4.txt`,
  `/tmp/misterplex-agent-H-gate-rcsum4b.txt`, `/tmp/misterplex-H-gate-rcsum4b-probes.txt`,
  `/tmp/plex_quartus_rcsum5.log`, `/tmp/misterplex-accept-ambiguous-rcsum5.txt`,
  `/tmp/plex_quartus.lock`,
  `/tmp/misterplex-agent-L-csum-note24.txt`…`note37.txt`, `/tmp/misterplex-agent-H-gate-ec21.txt`, `/tmp/misterplex-agent-H-res-ec21.txt`, `/tmp/misterplex-agent-W-wide-gate-SF2.txt`, `/tmp/misterplex-agent-W-wide-gate-SF2b.txt`, `/tmp/misterplex-agent-H-deploy-qSF2.txt`.

#### Handoffs / goldens (read, do not re-derive)

| Item | Location |
|------|----------|
| L-3l2e host goldens DONE | `/tmp/misterplex-agent-L-3l2e.txt` |
| L-3l2-rtl plug sketch DONE (docs only, no SV) | `/tmp/misterplex-agent-L-3l2-rtl.txt` + section below |
| L-csum-note HW evidence (pre–R-csum1) | `/tmp/misterplex-agent-L-csum-note.txt` |
| L-csum-note2 dabdaeb0 H-deploy FAIL | `/tmp/misterplex-agent-L-csum-note2.txt` |
| **L-csum-note3** H-gate +0x53/push | `/tmp/misterplex-agent-L-csum-note3.txt` |
| **L-csum-note4** dabdaeb0 FAIL + probe2 + rtl2 RCA + R-csum2 in flight | `/tmp/misterplex-agent-L-csum-note4.txt` |
| **L-csum-note5** R-csum2 LIVE (multi-cycle+DIAG force 0x14); 3l2 BLOCKED non-DIAG product | `/tmp/misterplex-agent-L-csum-note5.txt` |
| **L-csum-note6** R-csum2 LIVE~12:30+; log BUILD END→`4deaf6cc`; DIAG pack bisect; 3l2 BLOCKED | `/tmp/misterplex-agent-L-csum-note6.txt` |
| **L-csum-note7** R-csum2 BUILD_OK + lab `4deaf6cc` HARD FAIL (DIAG force no stick → PACK_FAIL); 3l2 BLOCKED | `/tmp/misterplex-agent-L-csum-note7.txt` |
| **L-csum-note8** stream24 RCA first lock + R-csum3 LIVE banner | `/tmp/misterplex-agent-L-csum-note8.txt` |
| **L-csum-note9** reconfirm stream24 RCA; 4deaf6cc FAIL; R-csum3 LIVE on rtl3 128b pack; expect green `e8 14 53 1a`; 3l2 BLOCKED | `/tmp/misterplex-agent-L-csum-note9.txt` |
| **L-csum-note10** claim 128b pack + stream24 RCA + host `e8 14 53 1a`; **4deaf6cc FAIL**; **3l2 BLOCKED**; **R-csum3 LIVE not BUILD_OK**; **MID-FIT DRIFT** live `ce1ef26c…`/`e45f98c4…` ≠ claim | `/tmp/misterplex-agent-L-csum-note10.txt` (may be absent; note11 supersedes) |
| **L-csum-note11** reconfirm: R-csum3 sole LIVE (then superseded); stream24 RCA; mid-fit drift; 3l2 BLOCKED | `/tmp/misterplex-agent-L-csum-note11.txt` |
| **L-csum-note12** R-csum2 `4deaf6cc` HARD FAIL stream24 alias; rtl3 claim LOCK_OK@claim; **R-csum3 FIT_DEAD_MID** SIGTERM ~12:49 no NEW_RBF; WT DRIFT; next **R-csum3b**; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note12.txt` |
| **L-csum-note13** R-csum3b sole LIVE BUILD_STARTED 12:53:16 on freeze `ce1ef26c`/`e45f98c4`; map OK; fit LIVE; RBF still `4deaf6cc`; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note13.txt` |
| **L-csum-note14** **R-csum3b LIVE sole** on freeze **`ce1ef26c`/`e45f98c4`** (historical mid-fit note) | `/tmp/misterplex-agent-L-csum-note14.txt` |
| **L-csum-note15** R-csum3b still LIVE past ~5m; not BUILD_OK yet | `/tmp/misterplex-agent-L-csum-note15.txt` |
| **L-csum-note16** R-csum3b BUILD_OK NEW_RBF `4d6ee356`; next sole menu + hard gate | `/tmp/misterplex-agent-L-csum-note16.txt` |
| **L-csum-note17** lab **`4d6ee356`** deploy+FBAR; soft-path sample `e8 02 d7 01` / pack-likely-fixed note (superseded class by note18 hard SoT) | `/tmp/misterplex-agent-L-csum-note17.txt` |
| **L-csum-note18** lab **`4d6ee356` DEPLOY_OK**; **FBAR PASS**; hard FAIL **+0x53/push** (not sticky 0x14) → **CSUM_UNSTABLE_ACCUM** / **CSUM_FAIL_DC_OK**; pack improved but csum still wrong; **3l2 BLOCKED**; **no thrash**; next RCA → `R-csum4` | `/tmp/misterplex-agent-L-csum-note18.txt` |
| **L-csum-note19** lab **`4d6ee356` HARD_FAIL**; **pack-likely-fixed**; **raw[13] additive +0x53/push** (**0x53=6739 lo**); **not sticky 0x14**; next **rtl4/RCA → `R-csum4`**; **3l2 BLOCKED**; soft-skip ≠ PASS | `/tmp/misterplex-agent-L-csum-note19.txt` |
| **L-csum-note20** lab **`4d6ee356` HARD_FAIL reconfirm** (H-gate-rcsum3b + **3b2**); **+0x53/push series**; **pack-likely-fixed** | `/tmp/misterplex-agent-L-csum-note20.txt` |
| **L-csum-note21** post-**`4d6ee356` HARD_FAIL** (+0x53 additive, **MAP_OK**); next **R-csum4 DIAG sticky-0x14** silicon pack proof vs compute (DIAG on raw[13] → pack proven; else multi-drive); **3l2 BLOCKED**; soft-skip ≠ PASS | `/tmp/misterplex-agent-L-csum-note21.txt` |
| **L-csum-note22** **R-csum4 DIAG sole LIVE** (rtl4c FIT_GO freeze **`94db41b7`/`9a2d10c5`**; log `/tmp/plex_quartus_rcsum4.log`; **not BUILD_OK** then); bisect sticky 0x14=pack proven else multi-drive; **DIAG ≠ product PASS**; **3l2 BLOCKED**; thrash forbidden; soft-skip ≠ PASS; **MID-FIT DRIFT OBSERVED** | `/tmp/misterplex-agent-L-csum-note22.txt` |
| **L-csum-note23** mid-fit thrash policy ZERO RTL while exclusive LIVE | `/tmp/misterplex-agent-L-csum-note23.txt` |
| **L-csum-note24** **R-csum4 BUILD_OK** **`75da8bb1`** wall **421s** claim DIAG **`94db41b7`/`9a2d10c5`**; mid-fit **DRIFT_CRITICAL**; **lab LOADED** (H-deploy-rcsum4); **F-prep PACKAGE_OK** | `/tmp/misterplex-agent-L-csum-note24.txt` |
| **L-csum-note25** **H-gate-rcsum4 HARD_FAIL** lab **`75da8bb1`**; **NOT PACK_PROVEN**; **DIAG force did not stick**; **+0x53/push 0x40→0x93→0xe6** (+ series B wrap); **FBAR PASS**; class **MULTI_DRIVE_OR_STILL_FAIL**; next **sticky residual-pair latch + sole R-csum5**; **3l2 BLOCKED**; soft-skip ≠ PASS; thrash forbidden | `/tmp/misterplex-agent-L-csum-note25.txt` |
| **L-csum-note26** **R-csum5 sole LIVE** sticky-pack **product**; claim@launch **`6422fb9a`/`8e6af3bb`** DIAG=ABSENT; log `/tmp/plex_quartus_rcsum5.log`; **not BUILD_OK**; mid-fit **no RTL**; after BUILD_OK ONE menu expect sticky **0x14** product; lab **`75da8bb1` HARD_FAIL multi-drive SoT** until new deploy; **3l2 BLOCKED**; soft-skip ≠ PASS; thrash forbidden; **no invent BUILD_OK/hard PASS** | `/tmp/misterplex-agent-L-csum-note26.txt` |
| **L-csum-note27** **ZERO mid-fit RTL policy** while **R-csum5 LIVE** (docker `-v` poison); **claim launch `6422fb9a`/`8e6af3bb` AUTHORITATIVE** for this RBF; concurrent DIAG re-inject live **`6a5dcaaa`/`7d4a1d8b`** → **DRIFT_CRITICAL**; after fit **no thrash lab `75da8bb1`** — gate NEW_RBF once or refreeze if untrusted; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note27.txt` |
| **L-csum-note28** mid-fit **R-csum5** claim overwrite stamp: lab **`75da8bb1`** H-gate-rcsum4/4b **HARD_FAIL MULTI_DRIVE** sticky0x14=0 **+0x53/push NOT PACK_PROVEN**; sole start **13:31:58** product **`6422fb9a`/`8e6af3bb` DIAG=ABSENT**; ~**13:33** claim→**`6a5dcaaa`/`7d4a1d8b` DIAG=PRESENT** 8'h14 + **`res_pair_sticky` SUPERSEDES** (**CONTAMINATED**); docker `-v` **PROVENANCE risk** (map vs disk); **STOP mid-fit RTL**; after BUILD_OK evaluate trust before ONE menu; thrash banned RBF forbidden; soft-skip ≠ PASS; DIAG sticky alone ≠ product PASS; **3l2 BLOCKED** until non-DIAG product sticky 0x14 | `/tmp/misterplex-agent-L-csum-note28.txt` |
| **L-csum-note29** **STOP mid-fit RTL** + **PROVENANCE_UNTRUSTED** card while R-csum5 LIVE; claim freeze now DIAG **`6a5dcaaa`/`7d4a1d8b`**; launch product **`6422fb9a`/`8e6af3bb`** overwritten mid-fit; **forbidden** further `Plex.sv`/`slice_hdr_parser.sv` until exclusive free; after BUILD_OK trust branches before ONE menu; thrash **`75da8bb1` forbidden**; soft-skip ≠ PASS; DIAG ≠ product; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note29.txt` |
| **L-csum-note30** **R-csum5 still LIVE ~fit** (historical); claim/live **`6a5dcaaa`/`7d4a1d8b` DIAG PRESENT**; **PROVENANCE_UNTRUSTED** vs launch; **STOP mid-fit RTL**; thrash **`75da8bb1` forbidden**; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note30.txt` |
| **L-csum-note31** **R-csum5 BUILD_OK ~13:40:35 CDT** wall **441s**; **NEW_RBF `8832824e`** full `8832824e483cf6613f82ee3ba3e592b3`; launch product **`6422fb9a`/`8e6af3bb`** → claim/live DIAG **`6a5dcaaa`/`7d4a1d8b`**; **PROVENANCE_UNTRUSTED**; no invent hard PASS / DEPLOY_OK | `/tmp/misterplex-agent-L-csum-note31.txt` |
| **L-csum-note32** **accept-ambiguous one-menu serial** parent names **`8832824e`** Branch **B1**; historical pre-gate; superseded by note33 | `/tmp/misterplex-agent-L-csum-note32.txt` |
| **L-csum-note33** **H-gate-rcsum5 HARD_FAIL** lab **`8832824e`**; sticky0x14=**0**; **+0x53 multi-drive continues after sticky-pack sole**; **res_dc PASS**; **NOT PACK_PROVEN**; thrash forbidden; **next RCA not redeploy**; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note33.txt` |
| **L-csum-note34** (1) **H-gate HARD_FAIL `8832824e` multi-drive** reconfirm; (2) exclusive **Q-fix2 LIVE** residual RTL FREEZE while wide fits; (3) thrash FORBIDDEN incl. mid-fit residual edits; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note34.txt` |
| **L-csum-note35** **HARD_FAIL `8832824e` DEFINITIVE (H-gate-rcsum5d)**; **Q-fix2 LIVE hold residual RTL**; thrash forbidden; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note35.txt` |
| **L-csum-note36** **HARD_FAIL `8832824e`** + residual HOLD (Q-SF2 mid-fit era); thrash **`8832824e`/`75da8bb1`/`4d6ee356` forbidden**; WIDE ≠ residual PASS; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note36.txt` |
| **L-csum-note37** dual-track **post Q-SF2 BUILD_OK+deploy+WIDE FAIL**: exclusive **FREE**; **W** **`ec21e133`** WIDE FAIL **0.605**; **R HARD_FAIL both `8832824e` + `ec21e133`**; READY residual **NO**; competing next sole **Q-SF3 OR R-csum6** (parent one); thrash ban residual + no luck **`ec21e133`** residual green; soft-skip ≠ PASS; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note37.txt` |
| **L-csum-note38** parent **FIT_GO=YES** product **Rank1+2+3 place_pulse sticky**; exclusive sole **R-csum6 LIVE**; cite **H-gate-ec21 HARD_FAIL**, **W-wide-gate-fix2b FAIL 0.605** orthogonal OPEN, **R-multidrive-rca14**; **ZERO invent BUILD_OK**; READY residual **NO**; **3l2 BLOCKED** | `/tmp/misterplex-agent-L-csum-note38.txt` |
| **H-gate-rcsum5d** **DEFINITIVE HARD_FAIL** MULTI_DRIVE lab **`8832824e`** sticky0x14=0/12 +0x53 family **NOT PACK_PROVEN** | `/tmp/misterplex-agent-H-gate-rcsum5d.txt` |
| **H-gate-rcsum5** HARD_FAIL MULTI_DRIVE lab **`8832824e`** sticky0x14=0/7 +0x53 series A/B | `/tmp/misterplex-agent-H-gate-rcsum5.txt`, `/tmp/misterplex-H-gate-rcsum5-probes.txt`, `/tmp/misterplex-H-gate-rcsum5-summary.txt` |
| **H-gate-rcsum5b** HARD_FAIL reconfirm lab **`8832824e`** MULTI_DRIVE | `/tmp/misterplex-agent-H-gate-rcsum5b.txt` |
| **H-deploy-rcsum5** PROMOTE_OK \| DEPLOY_OK lab **`8832824e`** ONE menu | `/tmp/misterplex-agent-H-deploy-rcsum5.txt` |
| **R-csum-midfit-rcsum5** DRIFT_CRITICAL audit (map-era product sticky vs thrash DIAG; claim CONTAMINATED) | `/tmp/misterplex-agent-R-csum-midfit-rcsum5.txt` |
| **M-fitmon-rcsum5d** **BUILD_OK** Full Compilation wall **441s** NEW_RBF **`8832824e`** | `/tmp/misterplex-agent-M-fitmon-rcsum5d.txt` |
| **H-gate-rcsum4b** HARD_FAIL reconfirm lab **`75da8bb1`** MULTI_DRIVE (+0x53 0x85→0xd8→0x2b; sticky0x14=0/3) | `/tmp/misterplex-agent-H-gate-rcsum4b.txt`, `/tmp/misterplex-H-gate-rcsum4b-probes.txt` |
| **R-csum-rtl5** FIT_GO sticky residual pack + product csum_acc; DIAG ABSENT; freeze **`6422fb9a`/`8e6af3bb`** | `/tmp/misterplex-agent-R-csum-rtl5.txt` |
| **R-csum5-build** exclusive LIVE sole Full Compilation (launch claim product sticky DIAG=ABSENT) | `/tmp/misterplex-agent-R-csum5-build.txt` |
| **R-csum-rtl4c** FIT_GO DIAG sticky-0x14 freeze **`94db41b7`/`9a2d10c5`** | `/tmp/misterplex-agent-R-csum-rtl4c.txt` |
| **R-csum4-build** **BUILD_OK** NEW_RBF **`75da8bb1`** wall 421s (claim DIAG; mid-fit drift note) | `/tmp/misterplex-agent-R-csum4-build.txt` |
| **H-deploy-rcsum4** promote + one menu **`75da8bb1` DEPLOY_OK** lab LOADED | `/tmp/misterplex-agent-H-deploy-rcsum4.txt` |
| **F-prep-rcsum4** **PACKAGE_OK** tarball embeds **`75da8bb1`** | `/tmp/misterplex-agent-F-prep-rcsum4.txt` |
| **H-gate-rcsum4** FBAR PASS; hard residual **HARD_FAIL** on `75da8bb1` (+0x53/push; sticky 0x14=0/7; MULTI_DRIVE) | `/tmp/misterplex-agent-H-gate-rcsum4.txt`, `/tmp/misterplex-H-gate-rcsum4-probes.txt`, `/tmp/misterplex-H-gate-rcsum4-fbar.log` |
| **H-deploy-rcsum3b** promote + one menu **`4d6ee356`** DEPLOY_OK | `/tmp/misterplex-agent-H-deploy-rcsum3b.txt` |
| **H-gate-rcsum3b** FBAR PASS; hard residual **HARD_FAIL** on `4d6ee356` (+0x53/push 85/168/251; 0x53=6739 lo) | `/tmp/misterplex-agent-H-gate-rcsum3b.txt`, `/tmp/misterplex-H-gate-rcsum3b-probes.txt` |
| **H-gate-rcsum3b2** HARD_FAIL reconfirm no redeploy (+0x53/push 78/161/244; pack-likely-fixed) | `/tmp/misterplex-agent-H-gate-rcsum3b2.txt`, `/tmp/misterplex-H-gate-rcsum3b2-probes.txt` |
| **H-gate-rcsum3b3** HARD_FAIL residual reconfirm no redeploy (+0x53/push 71/154/237; same class) | `/tmp/misterplex-agent-H-gate-rcsum3b3.txt`, `/tmp/misterplex-H-gate-rcsum3b3-probes.txt` |
| **A-csum-map1 / map2** **MAP_OK** (host raw[13]=status[111:104]; blame FPGA) | `/tmp/misterplex-agent-A-csum-map1.txt`, `/tmp/misterplex-agent-A-csum-map2.txt` |
| **R-csum-rtl3** stream24 RCA + full 128b `status_telem_r` + multi-cycle (no DIAG); FIT_GO fingerprints | `/tmp/misterplex-agent-R-csum-rtl3.txt` |
| **R-csum-rtl3-lock** claim==tree LOCK_OK @12:44 (pre-drift) | `/tmp/misterplex-agent-R-csum-rtl3-lock.txt` |
| **A-csum-probe4** host classify V1 `e8 14 53 1a` HARD_PASS; V2 `e8 53 1a 00` stream24 alias | `/tmp/misterplex-agent-A-csum-probe4.txt` |
| **H-gate-rcsum2** lab 4deaf6cc HARD FAIL PACK_FAIL; res_dc PASS; FBAR prior PASS | `/tmp/misterplex-agent-H-gate-rcsum2.txt`, `/tmp/misterplex-H-gate-rcsum2-probes.txt` |
| **G-fpga-rcsum2** WAIT commit (SRC@fit≠now; DIAG-era RBF) | `/tmp/misterplex-agent-G-fpga-rcsum2.txt` |
| **H-gate-rcsum1** probes 0x53/0xa6/0xf9 | `/tmp/misterplex-agent-H-gate-rcsum1.txt`, `/tmp/misterplex-H-gate-rcsum1-probes.txt` |
| **A-csum-probe3** progressive XOR→0x14; lab +0x53 class ∉ residual intermediates | `/tmp/misterplex-agent-A-csum-probe3.txt` |
| **A-csum-probe2** progressive XOR ends 0x14; 0x53 ≠ residual intermediate | `/tmp/misterplex-agent-A-csum-probe2.txt` |
| **A-csum-probe** residual partial-XOR ruled out | `/tmp/misterplex-agent-A-csum-probe.txt` |
| **R-csum-rtl2** combo array XOR RCA + multi-cycle RMW / st_res_word | `/tmp/misterplex-agent-R-csum-rtl2.txt` |
| **R-csum-diagrtl** DIAG force-0x14 pack bisect (dirty; under R-csum2 fit) | `/tmp/misterplex-agent-R-csum-diagrtl.txt` |
| **H-deploy-rcsum1** mid-stream 0x8b/de/31 | `/tmp/misterplex-agent-H-deploy-rcsum1.txt`, `/tmp/misterplex-H-deploy-rcsum1-probes.txt` |
| L-3l2-gate / gate2 unblock checklist | `/tmp/misterplex-agent-L-3l2-gate.txt`, `/tmp/misterplex-agent-L-3l2-gate2.txt` |
| H-rcsum-proto sole-deploy protocol | `/tmp/misterplex-agent-H-rcsum-proto.txt` |
| R-csum-rca3 go/no-go + contingency | `/tmp/misterplex-agent-R-csum-rca3.txt` |
| SoT coeffs/csum/deq/Y | `host/libmisterplex/h264_residual_gold.hpp` |
| Unit lock | `tests/unit/test_idct_quant.cpp` (`FPGA_GOLD recon_*`) |
| Tree residual inputs (keep) | `residual_coeff[0:15]` signed 9b ST_PLACE → `stream_path` |

**Host paint goldens (mae=0 target after C9):**

| Field | Value |
|-------|-------|
| `residual_coeff` scan | `-24 4 4 0 -4 0 -1 0 0 -1 1 0 1 0 0 0` |
| res_csum | **20 / 0x14** (XOR sat8) |
| pred / qp | **128** / **25** |
| y00 / mean4×4 | **73** / **62** |
| paint y00 RGB565 | **0x4a49** |
| stub contrast | y=104 RGB565 **0x6B4D** (128+dc — not true recon) |

SoT: `host/libmisterplex/h264_residual_gold.hpp` + `tests/unit/test_idct_quant.cpp`.

#### HW gates after paint RBF (only after §A green + §C complete)

| Step | Gate | Pass when |
|------|------|-----------|
| Residual | `test_f3_residual.sh` | hard `res_dc=-24` + hard `res_csum=20` (no soft-skip) |
| IDCT/paint | `test_f3_idct_mb0.sh` | residual hard; soft→hard y00=73 / mean=62; eyes-on ≠104 |
| FBAR | `test_fbar_fast` | still green |

#### Non-goals while gate open (and paint non-goals)

- Full MB0 / all MBs (3.3l-3/4) · MAXB/FIFO/dual-YUV BRAM · hybrid present  
- Half-wiring inv_quant/idct into `files.qip` **before** §A HARD_PASS  
- Any paint Quartus while R-csum1 (or residual fit) is LIVE  
- Checking in orphan paint SV before C1 authorized  
- Treating `test_f3_residual.sh` soft-skip EXIT=0 as unblock (**soft-skip ≠ hard PASS**)  
- Treating DIAG force-0x14 pack-bisect RBF as product hard PASS / 3l2 unblock  
- Thrash-redeploying lab **`4deaf6cc`** or **`dabdaeb0`** or prior **`820484a6`** expecting csum green  
- Starting §C SV wire-up while residual still FAIL (contingency §D **ACTIVE**)  
- Touching `Plex.sv` residual status pack from paint work  
- Mid-fit / ad-hoc RTL thrash while planning **R-csum3b** (freeze intentional WT first)  
- Inventing hard PASS / BUILD_OK without log / 3l2 unblock on DIAG force or soft-skip (**soft-skip ≠ hard PASS**)  

---

### 3.3l-2 concrete RTL plug sketch (L-3l2-rtl — docs only; no fit)

**Gate still open (do not start paint Quartus / SV wire-up):** lab RBF **`4deaf6cc`**
(R-csum2 **BUILD_OK** + sole deploy + **H-gate-rcsum2**) has `res_dc=-24` PASS +
**FBAR PASS** but **`res_csum=20` HARD FAIL** — **`4deaf6cc` FAIL reference** (and **`dabdaeb0`** still FAIL).
Class **stream24 LE alias / PACK_FAIL**: +0x53/push family (0x89→0xdc→0x2f;
independent 0xd5→0x7b→0x21 +0xA6); **DIAG force-0x14 did NOT stick**.
Host (**A-csum-probe3**): progressive residual XOR→**0x14**; lab +0x53 class ∉ residual intermediates.
**3l2 paint BLOCKED** until **non-DIAG sticky** hard `res_csum=0x14` on product RBF.
**R-csum3 FIT_DEAD_MID**; next **R-csum3b** on frozen intentional WT. **Zero thrash 4deaf6cc**.
Unblock: *P3-3l2 UNBLOCK GATE* §A–§C (policy A7 non-force product); expect green **`e8 14 53 1a`**.

Reports: `/tmp/misterplex-agent-L-3l2-rtl.txt`, `/tmp/misterplex-agent-L-3l2-gate2.txt`,
`/tmp/misterplex-agent-H-gate-rcsum2.txt`, `/tmp/misterplex-agent-R-csum-rtl3.txt`,
`/tmp/misterplex-agent-L-csum-note12.txt`,
`/tmp/misterplex-agent-A-csum-probe3.txt`, `/tmp/plex_quartus_rcsum3.log`,
`/tmp/misterplex-agent-L-csum-note9.txt`. Soft-skip still **not** hard PASS (**soft-skip ≠ hard PASS**);
3.3l-2 paint **remains BLOCKED**.

#### Where modules plug (tree as of L-3l2-rtl)

```text
slice_hdr_parser.sv  ST_PLACE
    residual_ok, residual_dc, residual_csum
    residual_coeff[0:15] signed [8:0]   (*keep*)
         │
         ▼
stream_path.sv
    passes residual_* straight out
    decode_stub today: residual_ok/tc/dc only  ← NO coeff yet
         │
         ├─ (today)  decode_stub MB0 gray = clamp(128+dc) = 104
         │
         └─ (3.3l-2 target)
              residual_coeff[0:15] + slice_qp ──► inv_quant4
                                                    │ deq[4][4]
                                                    ▼
                                                 idct4x4_add (pred=128)
                                                    │ recon_y[4][4]
                                                    ▼
                                                 decode_stub top-left 4×4 paint
                                                    RGB565 gray pack
```

| Port / reg | Source today | Consumer 3.3l-2 |
|------------|--------------|-----------------|
| `residual_coeff[0:15]` signed 9b | `slice_hdr_parser` ST_PLACE → `stream_path` out | **`inv_quant4` input** (scan order) |
| `slice_qp` [5:0] | `slice_hdr_parser` (Baseline first MB: **25**) | **`inv_quant4` qp** |
| `residual_ok` | ST_PLACE sticky | start recon FSM; keep for paint gate |
| `residual_dc` / `residual_csum` | ST_PLACE | **status only** — do not recompute in paint path |
| `recon_y[0:3][0:3]` 8b | new regs after IDCT | `decode_stub` top-left 4×4 |
| `recon_ready` | new sticky | gate paint vs incomplete IDCT |
| `fs_wr_*` | `decode_stub` → `frame_store` | unchanged bus; pixel source changes for x&lt;4,y&lt;4 |

#### Module interfaces (sketch — not in `files.qip`)

**`inv_quant4`** — match `recon::detail_r::dequant4x4` (`h264_recon.hpp`):

```text
inputs:  clk, reset, start, qp[5:0], coeff[0:15] signed [8:0]  (or [15:0])
outputs: busy, done, blk[0:3][0:3] signed [15:0]   // residual domain, row-major
tables:  kZigzag = {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15}
         kNormAdjust[6][3] = {{10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}}
math:    shift = qp/6 + 2
         mi = (i&1)+(j&1) → 0 even/even, 1 one-odd, 2 both-odd
         qmul = (kNormAdjust[qp%6][mi] * 16) << shift
         blk[i][j] = (level * qmul + 32) >> 6
         maxCoeff=16 for first luma residual (use zigzag[k], not k+1)
cycle:   1 start → clear blk; then 0..15 sequential (skip zero levels) or combo if ALM OK
gold:    residual_gold::kDeq  (qp=25, kCoeffScan)  deq00=-4224
```

**`idct4x4_add`** — match `ff_h264_idct_add` / host `idct4x4_add`:

```text
inputs:  clk, reset, start, blk[0:3][0:3] signed [15:0], pred_fill=8'd128 (or pred[16])
outputs: busy, done, recon_y[0:3][0:3] [7:0]
steps:   1) b = blk; b[0][0] += 32
         2) 4 horizontal butterflies (rows)
         3) 4 vertical butterflies (cols); clip8(pred + (z>>6))
multi-cycle FSM (~8–16 cycles); pure add/shift — **no new M10K, 0 DSP required**
gold:    residual_gold::kY  y00=73 mean=(sum+8)/16=62
```

**Optional thin `mb_recon4` wrapper** (or inline in `stream_path`):

```text
on residual_ok rise + coeffs stable:
  inv_quant4.start → wait done
  idct4x4_add.start (pred=128) → wait done → recon_ready=1
  recon_y00 = recon_y[0][0]; recon_mean = (sum+8)/16
```

#### `stream_path` / `decode_stub` wire-up plan (when unblocked)

1. **Instantiate** `inv_quant4` + `idct4x4_add` in `stream_path.sv` (next to `decode_stub`), **not** inside `slice_hdr_parser` (keep CAVLC place pure).  
2. Feed `residual_coeff` + `slice_qp` (`sl_qp`) into inv_quant; trigger on `sl_res_ok` rising edge after ST_PLACE.  
3. Extend `decode_stub` ports:

```text
// add (when wiring):
input wire        recon_ready,
input wire [7:0]  recon_y00,          // optional status mirror
input wire [7:0]  recon_y [0:15],     // row-major 4×4  OR  recon_y[0:3][0:3]
// paint:
//   if (x < 4 && y < 4 && lat_res_ok && lat_recon_ready)
//     gray = recon_y[y*4+x];          // true 4×4 — y00=73
//   else if (mb0 && lat_res_ok)
//     gray = clamp(128+lat_res_dc);   // rest of MB0 stub 104 OK
// RGB565: {R[7:3],G[7:2],B[7:3]} R=G=B
// frame_store @W=320: addrs 0..3, 320..323, 640..643, 960..963
```

4. **Do not** touch `residual_dc` / `residual_csum` packing in `Plex.sv` from paint work — leave R-csum1 (csum fix) sole owner of residual status until green.  
5. Soft telem (optional later): spare status bytes for `recon_y00`/`recon_mean`; hard residual fields stay.  
6. `files.qip`: add `rtl/inv_quant4.sv` + `rtl/idct4x4_add.sv` **only** when sole paint fit starts **after** hard csum PASS.

#### Implementation order (after hard res_csum=20)

| Step | Action | Verify |
|------|--------|--------|
| 0 | **R-csum3b** lab **`4d6ee356` HARD_FAIL**; sole menu DEPLOY_OK; FBAR PASS; hard **raw[13] additive +0x53/push** (**0x53=6739 lo**; not sticky **0x14**) → **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK**; **next: `R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute** (DIAG on raw[13] → pack proven; else multi-drive) → product sticky **0x14** (§A; soft-skip ≠ PASS; **3l2 BLOCKED**; **no thrash**) | *H-deploy-rcsum3b* + *H-gate-rcsum3b/3b2/3b3* + *P3-3l2 UNBLOCK GATE* + **L-csum-note21** |
| 1 | Add `inv_quant4.sv` + unit-sim or table ROM vs `kDeq` (optional host C++ already locks) | deq match |
| 2 | Add `idct4x4_add.sv`; pred=128 → `kY` | y00=73 mean=62 |
| 3 | Wire in `stream_path`; extend `decode_stub` paint | no residual bus thrash |
| 4 | Sole Quartus (`NUM_PARALLEL=2`); one deploy | FBAR green |
| 5 | `test_f3_idct_mb0.sh` residual hard + recon soft→hard | y00≠104 eyes-on |

#### Resource / fit notes (unchanged budget)

- Logic-only: 16×9 coeff already present; +16×16 deq regs; +16×8 recon_y; small FSM.  
- **No new M10K.** Optional 1 DSP for quant mul or pure shift-add.  
- Current fit headroom: ALM ~78% free; M10K tight at 74% — do not grow FIFO/MAXB.  
- Prefer **not** checking in orphan SV until step 1 of paint fit (avoids accidental `files.qip` thrash).

#### Hard gate reminder (L-3l2-gate / L-3l2-gate2)

```text
BLOCKED:  P3-3l2 paint / SV / files.qip  (**3l2 BLOCKED until hard res_csum=0x14**
          on non-DIAG / non-force product RBF — policy A7)
HARD:     raw[13]==0x14 (res_csum=20) AND res_dc=-24 (raw[12]==0xE8)
          both STABLE ≥2 re-pushes; FBAR green; soft-skip NOT enough;
          expected green: e8 14 53 1a
LAB NOW:  RBF 4d6ee356 HARD_FAIL; sole menu DEPLOY_OK; FBAR PASS; res_dc PASS;
          hard raw[13] additive +0x53/push (0x53=6739 lo); not sticky 0x14;
          hard SoT: e8 55…/e8 a8…/e8 fb… (85→168→251);
          3b2: e8 4e…/e8 a1…/e8 f4… (78→161→244);
          3b3: e8 47…/e8 9a…/e8 ed… (71→154→237)
CLASS:    PACK_LIKELY_FIXED + CSUM_VALUE_FAIL / CSUM_UNSTABLE_ACCUM + MAP_OK
          (class shift off stream24 PACK_FAIL on 4deaf6cc e8 53 1a 00) — L-csum-note21
SOFT:     soft-skip ≠ hard PASS
HIST:     4deaf6cc PACK_FAIL; dabdaeb0 +0x53; R-csum3b BUILD_OK → 4d6ee356
PROBES:   /tmp/misterplex-agent-H-deploy-rcsum3b.txt
          /tmp/misterplex-agent-H-gate-rcsum3b.txt
          /tmp/misterplex-agent-H-gate-rcsum3b2.txt
          /tmp/misterplex-agent-H-gate-rcsum3b3.txt
          /tmp/misterplex-agent-A-csum-map1.txt
          /tmp/misterplex-agent-L-csum-note21.txt
NEXT:     R-csum-rtl4b FIT_GO → R-csum4 DIAG sticky-0x14 pack-vs-compute
          (DIAG on raw[13] → pack proven; else multi-drive);
          3l2 BLOCKED until non-DIAG sticky 0x14; DIAG ≠ product PASS
NO FIT:   inv_quant/IDCT / 3l2 paint; soft-skip ≠ hard PASS; no thrash
```

### 3.3l-3 — First full MB (I_NxN or I16)

Golden clip is **I_NxN** (`mb0=0`):

1. Parse 16× I4 pred mode (already skipped in ST_I4MODE — **retain modes**)  
2. CBP + mb_qp_delta (done)  
3. For each coded 4×4 in scan order: CAVLC(nC), dequant, predI4, idct-add  
4. Within-MB nC from left/above TC (regs)  
5. Chroma: pred + DC/AC residual (host order)  
6. Pack MB → RGB565; stub paints **MB0 only** correctly; rest of frame diagnostic grid OK  

**Capture limit:** first full MB residual body may exceed 48 B.

| Option | Pros | Cons |
|--------|------|------|
| A. Raise MAXB 48→128/256 (regs) | Minimal rewire | ALM/reg; still not full slice |
| B. Stream bit-walker past cap | Scales to all-MB | More work; preferred long-term |

**Plan:** 3.3l-3 may use **MAXB=128** (regs, ~1 Kib — still not M10K) as bridge; 3.3l-4 switches to streaming.

**Exit:** host vs FPGA mae on MB0 Y (16×16) = 0; chroma optional same fire or +1.

### 3.3l-4 — All MBs (I-slice)

1. **Streaming residual path:** after headers, consume bits from live FIFO (or long window), not fixed 48 B only.  
2. Neighbour **top row** M10K (+ chroma).  
3. Left column + TC from previous MB (regs).  
4. Loop mbx,mby to `sps_mb_w × sps_mb_h` (20×15 @ 320×240).  
5. Per MB: write 16×16 Y (+ chroma) → convert RGB565 → `frame_store` sequential or addressed paint.  
6. `decode_stub` becomes thin: kick recon engine, wait done, swap.  
7. Status: `mb_done`, `recon_ok`, fail_mb (if any).  

**Out of scope this phase:** P-slice / motion / CABAC / deblock (optional later; host already skips LF for gold).

**Exit:** full frame maeY≈0 vs host/FFmpeg no-LF on golden 320×240 IDR; `host_owns_fs` still preferred for product until soak.

### 3.3l-5 — Hybrid gate (product)

Only when 3.3l-4 mae competitive on lab titles:

- Allow STREAM path to skip host F1 recon when FPGA `recon_ok`  
- Keep host fallback on CABAC / fail  
- Measure: ALM/M10K fit, decode latency vs present 30 fps budget  

---

## RTL notes (match host / FFmpeg)

### Inv quant (`dequant4x4`)

```text
qmul = (mf[qp%6][pos] * 16) << (qp/6 + 2)
blk[i][j] = (level * qmul + 32) >> 6
zigzag: {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15}
maxCoeff=15 → skip scan DC slot (I16 AC / chroma AC)
```

### IDCT (`idct4x4_add`)

- `block[0][0] += 32` before butterflies  
- Horizontal then vertical; `(x)>>6` add into pred; clip 0..255  
- Multi-cycle OK; no need one-cycle combo path  

### Intra I4

- MPM / rem already in bitstream (parser must **store** modes in 3.3l-3)  
- Unavailable neighbour → mode forced DC(2) where host does  
- Top-right sample rules: `lumaReady` / replicate above[3] (3.3h lessons)  

### I16 DC

- Hadamard input layout: CAVLC zigzag then **FFmpeg TRANSPOSE** before butterfly  
- Host: `invQuantHadamardDc4x4` — copy exactly or mae will fail  

### First MB availability

| Sample | First MB (0,0) |
|--------|----------------|
| Above / left / TL | unavailable → 128 for DC |
| Top-right outside pic | replicate |

---

## Status / telemetry sketch

Keep existing residual fields. Add without breaking `push_frame --status` parsers more than needed:

| Bits / field | Meaning |
|--------------|---------|
| existing res_* / res_dc | regression 3.3k |
| **`[111:104] residual_csum8`** | **3.3l-1** XOR sat8(coeff[0:15]); host `res_csum=` (gold **20**/0x14) |
| `[127:112] stream_bytes[15:0]` | after 3.3l-1 (was 24b in [127:104]) |
| `recon_y00` or mean4×4 | 3.3l-2 eyes-on — host gold **y00=73 mean=62** (`residual_gold`) |
| `mb_done[7:0]` | 3.3l-4 progress |
| `recon_ok` sticky | full I-slice done |

Host already parses `residual_csum` from raw[13] (soft until RBF). Document assign in `Plex.sv` when RTL lands.

---

## Tests

| Milestone | Unit | HW |
|-----------|------|-----|
| 3.3l-0 | `test_idct_quant` / extend `test_cavlc_dc` | — |
| 3.3l-1 | host `h264_residual_gold` coeff[16]+csum=0x14 | `test_f3_residual.sh` res_dc; soft res_csum=20 after RBF |
| 3.3l-2 | `test_idct_quant` paint/deq goldens + `FPGA_GOLD recon_*` | `tests/hw/test_f3_idct_mb0.sh` residual hard; soft→hard y00=73 mean=62 |
| 3.3l-3 | MB0 Y mae=0 | HW status + optional frame dump |
| 3.3l-4 | **retired:** full recon maeY=U=V=0; **current:** native-I420 partial ratchet (320×240 155/300 MB exact, 624×480 510/1170) | native-I420 plane scoreboard; do not cite RGB565/host-RGB green as product PASS |
| Hybrid | — | STREAM smoke; host_owns_fs policy |

Do **not** require Quartus for 3.3l-0. Fit check only when RTL lands (sole build, `NUM_PARALLEL=2`).

---

## Milestone checklist (summary)

1. **3.3l-0** ✅ Host quant/IDCT golden + first-4×4 pixel vector (`test_idct_quant`)  
2. **3.3l-1** Host gold+status ✅ (XOR sat8 full-16 **0x14**/20); **R-csum1** `dabdaeb0` FAIL ref; **R-csum2** lab **`4deaf6cc` PACK_FAIL**; **R-csum3b BUILD_OK** → lab **`4d6ee356` HARD_FAIL** (H-gate 3b/3b2/**3b3**); **FBAR PASS**; hard **raw[13] additive +0x53/push** (**0x53=6739 lo**; not sticky 0x14) → **PACK_LIKELY_FIXED + CSUM_VALUE_FAIL** / **CSUM_UNSTABLE_ACCUM** + **MAP_OK**. **Next: `R-csum-rtl4b` FIT_GO → `R-csum4` DIAG sticky-0x14 pack-vs-compute** (DIAG on raw[13] → pack proven; else multi-drive). **soft-skip ≠ hard PASS**. **No thrash**. (**L-csum-note21**)  
3. **3.3l-2** Host paint goldens + plug sketch ✅; **UNBLOCK GATE defined**; paint/SV/`files.qip` **remains BLOCKED** until hard sticky `raw[13]==0x14` on non-DIAG product RBF (contingency §D **ACTIVE** → **no 3l2**; expect green probe `e8 14 53 1a`; **soft-skip ≠ hard PASS**; DIAG bisect ≠ product PASS)  


4. **3.3l-3** First full MB (I_NxN modes+CBP+16× residual+chroma); MAXB bridge or stream start  
5. **3.3l-4** All MBs + top-row BRAM + stream CAVLC; full-frame mae  
6. **3.3l-5** Product hybrid gate when mae competitive  

---

## Non-goals (3.3l)

- Deblocking filter  
- P/B slices, motion compensation  
- CABAC  
- Resolutions above current 320×240 bring-up (scale after mae@320)  
- Replacing host F1 before 3.3l-5  
- Growing bitstream FIFO or dual YUV M10K framebuffers  

## References in tree

- Residual FSM: `fpga/Plex_MiSTer/rtl/slice_hdr_parser.sv`  
- Paint: `fpga/Plex_MiSTer/rtl/decode_stub.sv`  
- Wire-up: `fpga/Plex_MiSTer/rtl/stream_path.sv`  
- Host gold: `host/libmisterplex/h264_recon.hpp`, `h264_cavlc.hpp`, `h264_residual_gold.hpp`  
- Unit paint/IDCT: `tests/unit/test_idct_quant.cpp` (`FPGA_GOLD recon_*`)  
- HW residual: `tests/hw/test_f3_residual.sh`  
- HW paint gate: `tests/hw/test_f3_idct_mb0.sh` (host contract y00=73 mean=62; soft until paint RBF)  
- Post-3l1 handoff: section above in this doc  
- Parent log: `docs/phase3-decode.md` (Phase 3.3k → 3.3l pointer)
