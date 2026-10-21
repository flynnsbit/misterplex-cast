#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$ROOT/build/decode-throughput-unit"
mkdir -p "$WORK"

COMPARE="$WORK/compare_624.json"
RATCHET="$ROOT/tests/fixtures/p3_multinal/plex_inter_p16_624x480_decode_throughput_v1.json"
REPORT="$WORK/report_624.json"
RED_RATCHET="$WORK/red_ratchet.json"

# Synthetic compare JSON with stage_cycles data
cat >"$COMPARE" <<'JSON'
{
  "format": "misterplex.p3.frame_planes_compare.v1",
  "source": {
    "path": "tests/fixtures/p3_inter_pred/plex_inter_p16_baseline_624x480_12f.264",
    "bytes": 70348,
    "sha256": "9b79749478f331d6e523a548a88fbad38d1719beb6a2623b289e4e0190bf17a9"
  },
  "geometry": {
    "width": 624,
    "height": 480,
    "colorspace": "I420_FROM_RGB565",
    "planes": [
      {"plane": "Y", "width": 624, "height": 480},
      {"plane": "U", "width": 312, "height": 240},
      {"plane": "V", "width": 312, "height": 240}
    ]
  },
  "summary": {
    "nals": 15,
    "idr": 1,
    "p": 11,
    "cycles": 3641853,
    "first_bad_frame": 0,
    "first_bad": null,
    "strict_pass": false,
    "expectation": "red"
  },
  "frames": [{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}],
  "stage_cycles": {
    "method": "Per-frame phase tracking via stub_busy/fs_wr_reset/fs_swap transitions in Verilator sim",
    "injection_cycles": 140696,
    "nonvcl_idle_cycles": 768,
    "reset_cycles": 12,
    "frames": [
      {"frame": 0, "parse_cycles": 287, "paint_cycles": 299520},
      {"frame": 1, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 2, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 3, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 4, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 5, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 6, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 7, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 8, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 9, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 10, "parse_cycles": 4096, "paint_cycles": 299520},
      {"frame": 11, "parse_cycles": 4096, "paint_cycles": 299520}
    ],
    "parse_total": 45343,
    "paint_total": 3594240
  }
}
JSON

"$ROOT/tools/check_decode_throughput.py" \
  --compare-json "$COMPARE" --ratchet "$RATCHET" \
  --label unit-624x480 --report "$REPORT"
python3 - "$REPORT" <<'PY'
import json
import sys

r = json.load(open(sys.argv[1]))
assert r["run"]["label"] == "unit-624x480", r
assert r["source"]["sha256"] == "9b79749478f331d6e523a548a88fbad38d1719beb6a2623b289e4e0190bf17a9", r
assert r["geometry"]["mbs_per_frame"] == 1170, r
assert r["target"]["stream_path_clock_hz"] == 20_000_000, r
assert abs(r["measured"]["cycles_per_mb"] - 259.3912393162) < 1e-6, r
assert abs(r["budget"]["cycles_per_mb"] - 683.7606837607) < 1e-6, r
assert r["budget"]["margin_ratio"] > 2.6, r
assert any(s["status"] == "unknown" for s in r["stage_coverage"]), r

# Verify per-stage data is present and sensible
stages = {s["name"]: s for s in r["stage_coverage"]}
assert "parse_cavlc" in stages, f"missing parse_cavlc stage, got: {list(stages.keys())}"
assert "diagnostic_paint" in stages, f"missing diagnostic_paint stage"
assert "mc_interpolation" in stages, f"missing mc_interpolation stage"
assert "deblock" in stages, f"missing deblock stage"
assert "ddr_write" in stages, f"missing ddr_write stage"

# parse_cavlc should be measured and much smaller than the aggregate
# Note: includes P-frame 4096-cycle decode_stub timeout (inter not implemented)
pc = stages["parse_cavlc"]
assert pc["status"] == "measured", f"parse_cavlc status={pc['status']}"
assert pc["cycles_per_mb"] is not None, "parse_cavlc cycles_per_mb is None"
assert pc["cycles_per_mb"] < 5.0, f"parse_cavlc {pc['cycles_per_mb']} unexpectedly high"
assert pc["cycles_per_mb"] > 0.1, f"parse_cavlc {pc['cycles_per_mb']} unexpectedly low"

# diagnostic_paint should dominate
dp = stages["diagnostic_paint"]
assert dp["status"] == "measured", f"diagnostic_paint status={dp['status']}"
assert dp["cycles_per_mb"] is not None, "diagnostic_paint cycles_per_mb is None"
assert dp["cycles_per_mb"] > 250.0, f"diagnostic_paint {dp['cycles_per_mb']} unexpectedly low"

# dequant_idct should be 0 (combinational)
di = stages["dequant_idct"]
assert di["status"] == "measured", f"dequant_idct status={di['status']}"
assert di["cycles_per_mb"] == 0.0, f"dequant_idct {di['cycles_per_mb']} should be 0"

# unimplemented stages should say so
for name in ("mc_interpolation", "deblock", "ddr_write"):
    assert stages[name]["status"] == "not_implemented", f"{name} should be not_implemented"
    assert stages[name]["cycles_per_mb"] is None, f"{name} should have no cycle count"

print("OK per-stage data verified: parse is tiny, paint dominates, MC/deblock/DDR unimplemented")
PY

# Red proof: tighten parse_cavlc ratchet so it fails
python3 - "$RATCHET" "$RED_RATCHET" <<'PY'
import json
import sys

r = json.load(open(sys.argv[1]))
r["thresholds"]["max_cycles_total"] = 3_000_000
r["thresholds"]["max_cycles_per_frame"] = 250_000.0
r["thresholds"]["max_cycles_per_mb"] = 220.0
r["thresholds"]["min_budget_margin_ratio"] = 3.0
# Tighten per-stage ratchet
r["thresholds"]["stages"] = {
    "parse_cavlc": {"max_cycles_per_mb": 0.1},
    "diagnostic_paint": {"max_cycles_per_mb": 200.0}
}
json.dump(r, open(sys.argv[2], "w"), indent=2, sort_keys=True)
PY

set +e
RED_OUT="$("$ROOT/tools/check_decode_throughput.py" \
  --compare-json "$COMPARE" --ratchet "$RED_RATCHET" 2>&1)"
RED_RC=$?
set -e
if [[ "$RED_RC" -eq 0 ]]; then
  printf '%s\n' "$RED_OUT"
  echo "FAIL decode throughput red proof: regressed ratchet unexpectedly passed" >&2
  exit 1
fi
grep -q "cycles_total 3641853 > ratchet 3000000" <<<"$RED_OUT"
grep -q "budget margin 2.636 < required 3.000" <<<"$RED_OUT"
# Verify per-stage ratchet failures
grep -q "stage parse_cavlc cycles_per_mb" <<<"$RED_OUT"
grep -q "stage diagnostic_paint cycles_per_mb" <<<"$RED_OUT"
echo "RED OK decode throughput ratchet rejects cycle, margin, and per-stage regressions"

# ---- INCOMPLETE verdict gate ----
# The ratchet MUST emit INCOMPLETE (not OK) when unimplemented stages exist.
# This is a structural gate: the report cannot be green-read while MC/deblock/DDR are missing.
PASS_OUT="$("$ROOT/tools/check_decode_throughput.py" \
  --compare-json "$COMPARE" --ratchet "$RATCHET" 2>&1)"
# Must contain INCOMPLETE, not OK
if grep -q "^OK decode throughput" <<<"$PASS_OUT"; then
  echo "FAIL structural gate: ratchet emitted OK with unimplemented stages" >&2
  printf '%s\n' "$PASS_OUT" >&2
  exit 1
fi
grep -q "INCOMPLETE decode throughput" <<<"$PASS_OUT"
grep -q "UNBUDGETED_STAGES=" <<<"$PASS_OUT"
# Must name the specific missing stages
grep -q "mc_interpolation" <<<"$PASS_OUT"
grep -q "deblock" <<<"$PASS_OUT"
grep -q "ddr_write" <<<"$PASS_OUT"
echo "RED OK incomplete verdict: ratchet refuses OK when stages are not_implemented"

# Red proof the other direction: if we mark all stages as implemented, verdict should be OK.
# Create a ratchet where all stages are "measured" (no not_implemented)
ALL_IMPL_RATCHET="$WORK/all_impl_ratchet.json"
python3 - "$RATCHET" "$ALL_IMPL_RATCHET" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
for s in r["stage_coverage"]:
    if s["status"] == "not_implemented":
        s["status"] = "measured"
        s["cycles_per_mb"] = 0.0
json.dump(r, open(sys.argv[2], "w"), indent=2, sort_keys=True)
PY
ALL_IMPL_OUT="$("$ROOT/tools/check_decode_throughput.py" \
  --compare-json "$COMPARE" --ratchet "$ALL_IMPL_RATCHET" 2>&1)"
if ! grep -q "^OK decode throughput" <<<"$ALL_IMPL_OUT"; then
  echo "FAIL red proof: all-implemented ratchet should emit OK but did not" >&2
  printf '%s\n' "$ALL_IMPL_OUT" >&2
  exit 1
fi
echo "RED OK all-implemented verdict: ratchet emits OK when all stages are measured"

echo "PASS decode throughput gate"
