# On-hardware visual decode regression harness

This harness closes the gap between Verilator decode proof and real DE10-Nano presentation. It uses the shipping
core artifact, pushes a known Baseline/CAVLC bitstream through the existing F3 path, captures the actual HDMI
output, and compares it to a checked-in golden image with quantified pixel metrics.

## Entry points

| Task | Command |
|---|---|
| Scheduled hardware gate | `VISUAL_RBF=/path/to/Plex.rbf VISUAL_GOLDEN=/path/to/current_yuv420p_golden.png VISUAL_EXPECTED_CONTENT_SIZE=624x480 VISUAL_PIXEL_FORMAT=yuv420p VISUAL_FAULT_DEMO=1 tests/hw/test_f3_visual_golden.sh` |
| Wrong-core provenance red specimen | `VISUAL_RBF=/path/to/fe7673bc.rbf VISUAL_GOLDEN=/path/to/57674f2e_golden.png tests/hw/test_f3_visual_golden.sh` (returns `rc=8`, not a visual result) |
| Legacy rollback evidence | `VISUAL_RBF=/path/to/57674f2e.rbf VISUAL_GOLDEN=tests/fixtures/hw_visual/plex_real_baseline_320x240_57674f2e_mjpeg720_golden.png VISUAL_EXPECTED_CONTENT_SIZE=320x240 VISUAL_PIXEL_FORMAT=rgb565 VISUAL_EXPECT=pass tests/hw/test_f3_visual_golden.sh` |
| Use already-loaded Plex core | `VISUAL_EXPECTED_RBF_MD5=<loaded-md5> VISUAL_GOLDEN=/path/to/matching_source_golden.png tests/hw/test_f3_visual_golden.sh` |
| Comparator unit/red-path test | `python3 tests/unit/test_hw_visual_compare.py` |
| Print geometry used by comparator | `python3 scripts/hw_visual_compare.py geometry` |

The hardware script writes artifacts under `build/hw_visual/`: `noise.json`, `compare.json`, `diff.png`,
`status.txt`, captures, and optional `diff_bad_expected_fail.png`.

The capture defaults are intentionally conservative for the current lab dongle:
`VISUAL_CAPTURE_FORMAT=mjpeg`, `VISUAL_CAPTURE_SIZE=1280x720`, `VISUAL_VIDEO_MODE=0`. Use
`VISUAL_CAPTURE_FORMAT=yuyv422` only when the rig proves clean in that mode; corrupted buffers are a harness
failure (`rc=4`), never a core result.

The capture/compare path pins colour provenance instead of relying on FFmpeg's
resolution-based defaults: `VISUAL_COLOR_MATRIX=bt601` and `VISUAL_COLOR_RANGE=full`
by default. `scripts/hw_visual_compare.py compare` refuses to grade images unless
both golden and capture matrix/range are stated, and also refuses mismatched
provenance.

Freshness is also a precondition, not an inference from the pixels. The hardware
script passes the post-push `push_frame --status` log into the comparator and
requires plausible delivery counters before grading: default
`VISUAL_MIN_BYTES_IN=512`, plus `has_frame=1`, `has_stream=1`, `has_idr=1`,
`sps_valid=1`, and `pps_valid=1`. A stale/frozen screen can exactly match a
golden; it still returns `rc=7` if the status says only a poison/trivial input
arrived. When the shared DDR frame-store token is exposed, set
`VISUAL_REQUIRE_TOKEN=1`; the comparator already understands the common
`{bank, format, seq}` / doorbell fields and will require the token to change
from `status_before.txt` to `status.txt`.

Artifact identity is another precondition. `tests/hw/test_f3_visual_golden.sh`
will not grade an unspecified loaded core or an unspecified golden. Pass
`VISUAL_RBF` (the script derives its md5) or set
`VISUAL_EXPECTED_RBF_MD5`/`VISUAL_RBF_MD5` for an already-loaded core, and always
set `VISUAL_GOLDEN`. The script records `md5sum /media/fat/_Utility/Plex.rbf`
in `rbf_md5.txt`, verifies it immediately after deploy/reload, and passes that
log into the comparator. A loaded-core mismatch returns `rc=8` before pixel
grading.

Each hardware golden must have a sibling `*.png.provenance.json` sidecar with:
source RBF md5, content/presented geometry and compare box, frame-store pixel
format, and colour matrix/range. Missing or mismatched geometry/format/colour
returns `rc=9`; a golden whose source RBF differs from the loaded core returns
`rc=8` and names both core md5s. This prevents testing a current I420/624×480
ARM pipeline against an old RGB565-era rollback core by accident.

## What is compared

- Default input bitstream: `tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264`.
- Default golden: **none**. `VISUAL_GOLDEN` must be explicit.
- Geometry source of truth:
  - `host/libmisterplex/ddr_frame_layout.hpp`
  - `fpga/Plex_MiSTer/rtl/ddr_frame_layout_params.svh`

The former default `plex_real_baseline_320x240_57674f2e_mjpeg720_golden.png`
is quarantined as legacy evidence for rollback `57674f2e` only. Its provenance
declares 320×240/RGB565/BT.601-full and will not grade against a current
YUV420 artifact. The 624×480 fixture/reference remains checked in as a future
target only; it is marked `generated_reference`, not a hardware-captured golden.

The comparator refuses to run if host and RTL constants diverge. It compares the active displayed picture only:
coded **624×480**, display **618×480** after right crop of 6 px, presented **640×480** with **11 px** pillars on
each side. Pillarbox columns are not compared against picture content.

For the current default 320×240 F3 vector, the hardware script narrows the compare to
`VISUAL_COMPARE_BOX=11,0,160,120`: the stable top-left decoded region containing MB0. The rest of the 480p frame
can contain reload-dependent/uninitialized pixels on rollback `57674f2e`; comparing the whole active 618×480 area
was the false-red failure mode.

`VISUAL_FULL_FRAME=1` is the opt-in investigation path for the full **618×480** active display region. It keeps
the same geometry parsing, noise calibration, stale rejection, corrupt-log `rc=4` guard, per-plane exact counts,
and per-plane MAE reporting; do not treat it as a product gate until it is proven green on `57674f2e` and red on
`fe7673bc` with one consistent vector/golden pair.

For full-frame expansion, consume the shared multi-NAL fixtures from `tests/fixtures/p3_multinal/` rather than
forking new streams. The first hardware candidate is `wcap_residual14_idr_plus_p.264` with
`wcap_residual14_idr_plus_p_sequence_v1.json` (`nal_count=5`, `vcl=2`, IDR+P, Baseline/CAVLC, 320×240). That keeps
the hardware visual gate aligned with the simulation-side multi-NAL stream-path gate and makes sim-vs-silicon
disagreements directly actionable.

2026-07-27 full-frame investigation result: **REFUSED / not a gate**. The 618×480 active region is stable within a
single static capture run on `57674f2e` (`noise max_abs=0`), but it is not stable across the menu reload/push
sequence required by the hardware script. Known-good `57674f2e` false-reded against the checked-in golden:

```text
good full-frame vs checked golden: exact 281458/296640
Y/U/V exact = [283588, 282443, 282742]
Y/U/V MAE   = [5.1175, 3.3041, 1.9793]
Y/U/V max   = [166, 112, 69]
```

Re-running good against a full-frame golden captured earlier in the same window still false-reded after another
menu reload:

```text
good rerun vs same-window full golden: exact 281249/296640
Y/U/V exact = [283483, 282293, 282531]
Y/U/V MAE   = [5.1944, 3.3532, 2.0109]
Y/U/V max   = [165, 114, 69]
```

The bad specimen still goes red full-frame, but because the known-good path is not green, full-frame must remain
refused by default:

```text
bad fe7673bc vs same full golden: exact 0/296640
Y/U/V exact = [4103, 5120, 4716]
Y/U/V MAE   = [63.9724, 24.5320, 26.1278]
Y/U/V max   = [230, 220, 171]
```

Follow-up reload testing found a more dangerous phantom-green shape: five
Plex reload+push captures were byte/pixel identical (`296640/296640`, Y/U/V MAE
`[0,0,0]`) because the screen was frozen on an old frame, while status reported
`bytes_in=4`. Source audit closed the suspicious "four bytes" question: in the
current status ABI that field is not a byte counter at all. `raw[12..15]` were
reclaimed for residual/recon telemetry, and `push_frame --status` now prints
`stream_nalus` plus `bytes_in_unavailable=1` instead of a byte counter. Older
logs that still contain `bytes_in = nalu_count` are explicitly treated as a
telemetry-layer alias; the natural fixture has `nalu=4`, meaning four NALs
(SPS/PPS/SEI/IDR), not four delivered bytes. The comparator therefore names
both forms as a `STATUS_TELEMETRY_LAYER` freshness failure and refuses before
loading/grading pixels; the checked-in natural fixture
`tests/fixtures/hw_visual/reload_determinism/plex_bytes_in4_*` confirms `rc=7`
even when the capture is compared against itself.

2026-07-27 artifact-identity failure mode: rollback `57674f2e` predates the
current YUV420/624×480 DDR frame-store path. w-cap proved ARM decode→I420→DDR
was byte-exact, then screen capture against that verified DDR content returned
`exact 0/296640`, `MAE 111.73`, `max_abs 255`, and first bad at presented
`(11,0)` because the loaded core could not possibly interpret the written
format correctly. The harness now refuses to grade unless the loaded
`/media/fat/_Utility/Plex.rbf` md5 matches the declared artifact.

## Metrics and failure artifact

`scripts/hw_visual_compare.py compare` reports:

- active pixel count and exact-match pixel count
- per-plane exact-match counts and ratios
- per-plane RGB mean absolute error
- per-plane max absolute error
- overall MAE
- worst mismatch location in presented and display coordinates
- worst mismatch plane, golden value, captured value, and delta
- colour matrix/range provenance for both the golden and the capture
- `diagnostic_signatures` derived from the raw per-channel MAE shape (for
  example uniform high RGB error vs green-dominant transform/chroma error vs
  red/blue-dominant U/V-order suspicion). These are hints only; the raw numbers
  remain the evidence.

On failure it emits a PNG with **golden | captured | amplified diff**, so the failure is visually debuggable.

Exit codes are deliberately distinct so a capture-rig failure cannot be mistaken for a red core:

| Code | Meaning |
|---:|---|
| 0 | compare passed |
| 1 | genuine visual mismatch against golden |
| 2 | harness/setup error |
| 3 | stale capture (byte-identical to previous condition) |
| 4 | V4L2/FFmpeg reported corrupted capture buffers/data |
| 5 | capture device absent |
| 6 | capture device busy/exclusive-open |
| 7 | no fresh frame delivery proven (status byte counter absent/aliased, required status, or token freshness failed) |
| 8 | loaded `/media/fat/_Utility/Plex.rbf` md5 does not match the declared artifact, or golden source RBF does not match the loaded core |
| 9 | missing or mismatched golden provenance (geometry, pixel format, colour, or non-hardware reference) |

If status exposes the frame-store debug byte, `0xe1` is surfaced as
`frame store refused non-YUV doorbell (0xE1); non-YUV DDR doorbell format error`
and is treated as a freshness/setup refusal, not a visual mismatch. The visual
gate consumes `frame_debug` directly when w-osd's ARM status output exposes it;
`debug_state`/DDR debug aliases remain accepted only for compatibility.
Likewise, w-osd's absent-frame surface is authoritative: `frame_status=absent`,
`has_frame=0`, `PLXF magic=0`, or the text
`frame store status unavailable (PLXF mailbox absent/unwritten)` all return
`rc=7` before pixel grading. A channel-skewed PNG cannot be reported as
`COLOUR_PATH_DEFECT` while ARM status says PLXF absent or non-YUV-refused; the
diagnostic report uses `delivery_verdict.id=NO_FRAME_DELIVERED` and leaves
pixel `stats` unset because no pixels were graded.

## Visual verdict: absent frame vs colour path

The JSON report includes `stats.visual_verdict` so deploy-window evidence does
not require humans to infer a root class from three MAE numbers. Raw MAE is
reported first; the verdict records the same numbers plus the dispersion metric:

- `max_min_ratio = max(R,G,B MAE) / min(R,G,B MAE)`
- `coefficient_of_variation = stddev(R,G,B MAE) / mean(R,G,B MAE)`

The stable verdict identifiers are literal: `NO_FRAME_DELIVERED`,
`COLOUR_PATH_DEFECT`, `GEOMETRY_CONTENT_DEFECT`, and `INDETERMINATE` for
failures; exact matches report `EXACT_MATCH`.

The thresholds are derived from synthetic 624×480 active-region cases generated
from the checked-in 480p fixture, plus the live frozen-screen capture:

| Case | RGB MAE | max/min | CV | Verdict |
| --- | --- | ---:| ---:| --- |
| exact frame | `[0.000000, 0.000000, 0.000000]` | `1.000000` | `0.000000` | `EXACT_MATCH` |
| unrelated/random frame | `[124.278944, 125.010932, 124.861246]` | `1.005890` | `0.002532` | `NO_FRAME_DELIVERED` |
| live frozen screen | `[120.43, 116.88, 113.61]` | `≈1.06` | `≈0.024` | `NO_FRAME_DELIVERED` shape |
| BT.601 encoded / BT.709 decoded | `[1.170796, 25.243194, 0.649858]` | `38.844144` | `1.271725` | `COLOUR_PATH_DEFECT` |
| U/V swap | `[155.461077, 23.610076, 167.876160]` | `7.110361` | `0.564453` | `COLOUR_PATH_DEFECT` |
| +5 px horizontal shift | `[15.644637, 9.893790, 4.500182]` | `3.476445` | `0.454463` | `GEOMETRY_CONTENT_DEFECT` |
| unsafe synthetic band | `[43.226844, 26.001878, 21.984624]` | `1.966231` | `0.303046` | `INDETERMINATE` |

`NO_FRAME_DELIVERED` requires high error with near-flat channels
(`mean MAE ≥ 20`, exact ratio `≤ 0.05`, max/min `≤ 1.15`, CV `≤ 0.06`).
`COLOUR_PATH_DEFECT` requires channel-skewed error (`mean MAE ≥ 5`, exact ratio
`≤ 0.20`, and max/min `≥ 5` or CV `≥ 0.50`). Shifts/content defects are not
allowed to masquerade as colour defects; when `--shift-radius` finds a
substantially better offset, the verdict becomes `GEOMETRY_CONTENT_DEFECT`.
High-error cases in the band between flat/no-frame and skewed/colour return
`INDETERMINATE` and the compare command exits non-zero without guessing.

Colour-path classification is only trusted for delivered frames: PLXF status
must be present, no `0xe1` refusal may be surfaced, required `has_frame`/stream
fields must pass, and token freshness must pass when requested. If those status
checks fail, the comparator reports delivery failure instead of interpreting
pixels.

When grading a frame captured outside `scripts/hw_visual_compare.py capture`, pass its FFmpeg/V4L2 log with
`--capture-log`. If the log contains the real W-CAP corrupted-buffer diagnostics, compare exits `rc=4` before
looking at pixels, so a noisy PNG cannot be mislabeled as a core mismatch.
An absent capture device returns `rc=5`; a missing `ffmpeg` binary is a setup
error (`rc=2`), not a green/no-op capture path.

## Capture noise threshold

The hardware test captures the same static frame five times and derives thresholds from that measured floor:

```text
threshold_mae_rgb = max(1.0, 3 * measured_max_pair_mae_rgb + 1.0)
threshold_max_abs = max(2, 3 * measured_max_abs_noise + 2)
```

Dry-run/unit noise floor from checked-in static frames is exactly zero:

```text
max_pair_mae_rgb = [0, 0, 0], max_abs_noise = 0
threshold_mae_rgb = [1, 1, 1], threshold_max_abs = 2
```

First hardware acceptance run (HDMI MJPEG 1280×720@60, live `57674f2e`, artifacts
`build/hw_visual_accept/`) measured the same zero floor:

```text
max_pair_mae_rgb = [0, 0, 0], max_abs_noise = 0
threshold_mae_rgb = [1, 1, 1], threshold_max_abs = 2
```

The YUYV path on `/dev/video4` repeatedly produced `Dequeued v4l2 buffer contains corrupted data`; the harness
classifies that as `rc=4` and refuses to grade it. MJPEG captures at 1280×720@60 are the repaired path for the
current lab dongle and are usable for the current luma/region comparison, with retry on occasional decoder
invalid-data warnings.

Capture-repair differential test, 2026-07-27:

| Source | Pipeline | Result |
|---|---|---|
| MiSTer menu | YUYV 1280×720@10, 60-frame warm-up | `Dequeued v4l2 buffer contains corrupted data (1843200 bytes)`, PNG 63,940 bytes |
| MiSTer menu | YUYV 640×480@10, 60-frame warm-up | `Dequeued v4l2 buffer contains corrupted data (614400 bytes)`, PNG 15,863 bytes |
| MiSTer menu | YUYV 640×480@5, 120-frame warm-up | still `614400 bytes` corrupt; longer warm-up did not fix raw YUYV |
| MiSTer menu | MJPEG 1280×720@60, 60-frame warm-up | clean log, PNG 36,065 bytes |

That makes the original W-CAP symptom a capture-pipeline/format problem, not evidence that Plex alone emitted
bad HDMI timing: the exact YUYV pipeline also reports full-frame corrupt buffers while the MiSTer menu is live.
`v4l2-ctl --list-formats-ext` shows the dongle supports MJPEG up to 1080p30 and 720p60, while YUYV at 720p is
only advertised at 10 fps. The dongle sits on a shared 480M USB bus, so high-resolution raw YUYV should not be
used as the visual gate.

W-CAP's original failed visual attempt is checked in as log fixtures under
`tests/fixtures/hw_visual/capture_logs/`:

```text
RBF md5: fe7673bc959f37fd7da44e8a865f7db3, live path /media/fat/_Utility/Plex.rbf
Device: /dev/video4, V4L2 raw yuyv422, -framerate 10, ~60-frame warm-up
hw-fe7673bc-capture.log:     yuyv422 1280x720, corrupted data (1843200 bytes = 1280*720*2)
hw-fe7673bc-capture-640.log: yuyv422 640x480,  corrupted data (614400 bytes = 640*480*2)
```

The corresponding noisy PNGs (`hw-fe7673bc-padded.png`, `hw-fe7673bc-640x480.png`, and zoom derivatives) remain
W-CAP build artifacts, not goldens; the checked-in unit test proves those logs force capture-integrity `rc=4`.

## Red-path proof

`VISUAL_FAULT_DEMO=1` deliberately corrupts one active captured pixel after the green compare and requires the
comparator to fail with a precise location and a diff PNG. This is the visual red-path self-test. A different
loaded RBF paired with a golden captured from another RBF is now a provenance refusal (`rc=8`), not a visual red.

The current characterized hardware red specimen is **`fe7673bc`**: residual telemetry stays green
(`raw[13]=0x14`) but reconstruction signature is `raw[14]=0x00` instead of the simulated `0x3b`, matching a
pred-only/flat-block failure.

Hardware proof on the actual rig with the repaired MJPEG 1280×720@60 path:

| Specimen | Result | Evidence |
|---|---|---|
| `57674f2e` legacy rollback | **EVIDENCE-BACKED legacy GREEN only** when explicit rollback/RGB565 contract is declared; **UNSUBSTANTIATED** for current YUV420 product decode | compare box `11,0,160,120`, exact `19200/19200`, MAE `[0,0,0]`, max_abs `0`, `rc=0` |
| `fe7673bc` against `57674f2e` golden | PROVENANCE RED | now refused before pixel grading as wrong golden source (`rc=8`) |

That acceptance used a good hardware capture as the golden and the existing 320×240 baseline F3 vector, because
the checked-in 624×480 fixture did not yet exercise the current rollback RBF reliably. Keep the 624×480 fixture as
the target final gate, but do not claim it as a hardware PASS until its status/visual output is proven.

## Decoder debug output

The harness captures the existing status telemetry in `status_before.txt` before the push and in `status.txt`
immediately after the F3 push:

```text
sps_valid pps_valid has_idr slice_type mb0 qp res_ok res_tc res_t1 res_dc res_csum recon_sig bytes_in
```

Those fields already expose decoded macroblock/QP/slice/residual/signature state without adding synthesized RTL
scaffolding, and `bytes_in` is now a hard freshness gate. For on-screen debug, the safe path is to render these
status fields through the existing `PlaybackOverlay` text compositor from the ARM side after reading status
telemetry; do not add test-only RTL.
