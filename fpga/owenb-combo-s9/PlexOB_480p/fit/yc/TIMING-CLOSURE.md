# PlexOB Y/C timing closure

BUILD_ID is `0x0B59C11A` (word 88 is MPXB + this id). `ring_in` is restored. The sweep below stamped `0x0B59C119` on a bitstream that did not keep `ring_in`. That seed 5 closure is void. The re-fit winner is SEED 10.

Built on lab-pc from `9226e259` with `fit/yc/launch_yc.sh`: an isolated copy, the eac1064 pin, stock `sys/` (ascal `1b17376e`), notifications silenced. Nothing was deployed. The recipe pinned the seed that closed timing. The qsf change in that pin was the `SEED` line only (`45e3bdc54e581eed26658a4bf79332bc`, BUILD_ID `0x0B59C119`). The current qsf stamps `32'h0B59C11A` and SEED 10. That earlier sweep's closure is void.

## Re-fit, ring_in restored (RTL 08076620)

`ring_in` is restored. BUILD_ID is `0x0B59C11A`. The seed 5 closure is void.

The re-fit sweep winner is **SEED 10**. pll_hdmi setup is **-0.166 ns** (the bar is -0.3 ns). Every other clock is **≥ 0** at all four corners. ALM is 41,033 / 41,910 (**97.9%**). `ring_in_inst` is 601 ALUTs / 1026 regs. The bundled RBF md5 prefix is `c8b34604`. Word 88 expects `4D505842/0B59C11A`. Gate-open must not show `OB input region DEAD`. Seed 6 (prefix `ae7e35a6`) is the documented fallback only if seed 10 is unavailable. It is not bundled.

The qsf change from the void seed-5 pin is the `SEED` line only (`55648a1335bf640fae56a11ba10730d2`).

## Seed 9, the first fit

The committed qsf at `9226e259` was SEED 9 (`ae3d84801945d9018feae6a1f88cc006`). Every clock met timing except pll_hdmi setup: **-0.386 ns** (TNS -9.731, worst at Slow -40C). The d17b8df2 baseline was -0.109 ns. The HDMI-EXP bar is -0.3 ns, so -0.386 is a regression. Slow 100C was -0.287 ns. Both fast corners were clean. Hold on pll_hdmi was positive (0.099 ns). fmax_check and hdmi_pll_check still PASSed, and the RBF was not marked `_MARGINAL`. ALM was 38,797 / 41,910 (93%). RBF md5 `55d1ee3c856d4ef7567dd8fdedf0159a`.

## pll_hdmi path dump

Read-only STA, `report_timing -less_than_slack 0`, on that seed-9 fit. All **391** failing paths (29 endpoints) are stock `ascal:ascal|`, entirely inside pll_hdmi. Most are `o_adrs_pre[*]_OTERM` into `ascal|Mult12~8`. None of them touch `yc_out` or `emu`. Nothing crosses clock domains. There are 0 hold failures. The miss is placement of the stock scaler, not the Y/C logic.

## Seed sweep

Each seed was an rsync of the pre-flighted seed-9 tree (`$YC_FIT_ROOT`, excluding `db`, `incremental_db`, `output_files`, `releases`, and `.notify_url`) into `$YC_FIT_ROOT-s<N>`. The only edit was the `SEED` line. Each copy then ran `NOTIFY_SILENT=1 ./run_quartus_docker.sh --compile --name plexob-yc-0b59c119-s<N>`. Inputs stayed the eac1064 pin, stock sys (empty `diff -rq` against eac `sys/` on every seed), ascal `1b17376e`, emu `c5026baf`, ring_stat `1a2d93aa`.

Each qsf was diffed against `9226e259:…/vendor/H264.qsf`. The diff was the SEED line only, both before and after the compile, and the qsf md5 did not change across the compile. Seeds 1–6 started together. The path-dump job was the other slot, so the total stayed inside the cap of 7. Seeds 7, 8, and 10–12 were not started: seeds 2, 5, and 6 had already passed, and the sweep stopped there.

Numbers are the worst across all four corners, from Multicorner Timing Analysis Summary in `H264.sta.rpt`.

| Seed | pll_hdmi setup | pll_hdmi TNS | other clocks (setup / hold / recovery / removal) | Result |
|---|---|---|---|---|
| 9 | -0.386 | -9.731 | 0.466 / 0.085 / 1.783 / 0.254 | below the pll_hdmi bar |
| 1 | -0.479 | -19.060 | 0.586 / 0.060 / 2.196 / 0.273 | fail |
| 2 | -0.077 | -1.536 | 0.540 / 0.073 / 1.910 / 0.260 | pass |
| 3 | -1.098 | -31.739 | sys_pll hold **-0.150** | fail |
| 4 | -0.433 | -12.466 | 0.535 / 0.069 / 2.153 / 0.261 | fail |
| **5** | **+0.008** | **0.000** | **0.874 / 0.112 / 1.882 / 0.257** | **pass (best)** |
| 6 | -0.156 | -1.848 | 0.337 / 0.088 / 1.857 / 0.249 | pass |

pll_hdmi hold / recovery / removal stayed positive on every seed. Seed 5's pll_hdmi hold is 0.088, recovery 4.232, removal 0.444.

| Seed | hdmi_pll_check | fmax (clk_dec 100C/-40C; clk_vid 100C/-40C MHz) | _MARGINAL | ALM | RBF md5 |
|---|---|---|---|---|---|
| 9 | PASS | PASS (105.8/104.74; 97.72/93.65) | no | 38,797 / 41,910 | 55d1ee3c856d4ef7567dd8fdedf0159a |
| 1 | PASS | PASS (109.41/106.22; 94.27/95.29) | no | 38,801 / 41,910 | ac85dcb1c24c2277066f60ffe6dca295 |
| 2 | PASS | PASS (110.0/107.57; 93.03/89.11) | no | 38,915 / 41,910 | 2d03aa8dbea7fd39f588d814433f59b7 |
| 3 | PASS | PASS (101.68/104.29; 98.36/96.55) | no | 38,986 / 41,910 | 1a8c30faae757feb1410c9d114116647 |
| 4 | PASS | PASS (110.28/111.32; 94.56/93.22) | no | 38,975 / 41,910 | 910f7d866c4a48eb811a2ab16cff4632 |
| **5** | **PASS** | **PASS (110.11/107.62; 92.06/87.76)** | **no** | **38,966 / 41,910** | **void, no ring_in (md5 purged)** |
| 6 | PASS | PASS (108.59/107.56; 94.36/91.45) | no | 39,079 / 41,910 | d3403646eed3922e3bfe99025e424620 |

lint_undriven and netlist_canary PASSed on every seed. Fitter status was Successful and the compile rc was 0 on all six. Seed 5's sof md5 is `34b4613520acee1e7cc8326847fc8e4b`. Word 88 is MPXB + `0x0B59C119`. yc_out uses 2 DSPs.

## Why seed 5

Seed 5 is the only seed whose pll_hdmi setup is positive (+0.008 ns, TNS 0.000) and it has the best non-HDMI setup and hold in the sweep (0.874 / 0.112). Seeds 2 and 6 also meet the bar, but they still have negative pll_hdmi slack. Seed 5 has full timing on every clock in all four corners.

## Acceptance bar

From the HDMI-EXP note, applied by `timing_gate.py` after a `launch_yc.sh fit`:

- pll_hdmi setup slack **≥ -0.3 ns**
- every other clock **≥ 0** on setup, hold, recovery, and removal
- `hdmi_pll_check` PASS and `fmax_check` PASS
- the RBF name has no `_MARGINAL`

A fit that misses the slack bar, fails hdmi_pll_check, or packs a `_MARGINAL` RBF is recorded in `RESULT.env` (`PLL_HDMI_*`, `OTHER_*`, `TIMING_PASS=0`) and the script exits without printing `FIT done`.

## clk_vid margin

The thinnest fmax margin among the passing seeds is clk_vid at -40C on seed 5: **87.76 MHz** against an **86.0 MHz** target. The 100C corner is 92.06 MHz. fmax_check still PASSes. clk_dec on the same seed is 110.11 / 107.62 MHz.

## RBF

Seed 5 RBF md5 is omitted. That bitstream is void (no ring_in).

It was produced in the sweep copy on lab-pc (`plexob-yc-0b59c119-s5_20261019_0754.rbf`). It has not been copied to a device.
