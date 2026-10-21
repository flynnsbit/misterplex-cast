#!/usr/bin/env python3
"""Check stream_path decode throughput against a declared realtime ratchet."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


def fail(msg: str) -> int:
    print(f"FAIL decode throughput: {msg}")
    return 1


def load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise ValueError(msg)


def derive(compare: dict, ratchet: dict) -> dict:
    require(compare.get("format") == "misterplex.p3.frame_planes_compare.v1",
            "compare JSON is not misterplex.p3.frame_planes_compare.v1")
    require(ratchet.get("format") == "misterplex.decode_throughput_ratchet.v1",
            "ratchet JSON is not misterplex.decode_throughput_ratchet.v1")
    require(compare["source"]["sha256"] == ratchet["source_sha256"],
            "compare source sha256 does not match throughput ratchet")

    geometry = compare["geometry"]
    width = int(geometry["width"])
    height = int(geometry["height"])
    mb_w = width // 16
    mb_h = height // 16
    require(width % 16 == 0 and height % 16 == 0, "geometry is not whole macroblocks")
    frames = len(compare.get("frames", []))
    if frames == 0:
        frames = int(compare["summary"]["idr"]) + int(compare["summary"]["p"])
    require(frames > 0, "compare JSON has no decoded frames")
    cycles_total = int(compare["summary"]["cycles"])
    mbs_per_frame = mb_w * mb_h
    total_mbs = mbs_per_frame * frames

    target = ratchet["target"]
    clock_hz = int(target["stream_path_clock_hz"])
    fps = float(target["fps"])
    budget_cycles_per_frame = clock_hz / fps
    budget_cycles_per_mb = budget_cycles_per_frame / mbs_per_frame
    cycles_per_frame = cycles_total / frames
    cycles_per_mb = cycles_total / total_mbs
    margin_ratio = budget_cycles_per_frame / cycles_per_frame

    # Extract per-stage cycle data if the compare JSON has it
    sc = compare.get("stage_cycles")
    stages_measured: dict | None = None
    if sc is not None:
        parse_total = int(sc["parse_total"])
        paint_total = int(sc["paint_total"])
        injection_total = int(sc["injection_cycles"])
        nonvcl_idle_total = int(sc["nonvcl_idle_cycles"])
        reset_total = int(sc["reset_cycles"])
        overhead_total = injection_total + nonvcl_idle_total + reset_total
        accounted = parse_total + paint_total
        unaccounted = cycles_total - accounted - overhead_total
        if unaccounted < 0:
            unaccounted = 0
        stages_measured = {
            "parse_cavlc": {
                "cycles": parse_total,
                "cycles_per_mb": parse_total / total_mbs if total_mbs else 0.0,
                "status": "measured",
                "method": "stub_busy rise to fs_wr_reset transition per VCL frame",
                "note": "Annex-B parse, SPS/PPS/slice header, CAVLC residual decode latency. "
                        "Overlaps with ioctl byte injection in the testbench.",
            },
            "dequant_idct": {
                "cycles": 0,
                "cycles_per_mb": 0.0,
                "status": "measured",
                "method": "combinational — h264_dequant4x4 + h264_idct4x4 + h264_recon4x4 are pure comb logic",
                "note": "Zero clock cycles. Dequant/IDCT/recon are combinational modules instantiated "
                        "in decode_stub; they settle within one clock period and consume no pipeline stages.",
            },
            "intra_pred": {
                "cycles": 0,
                "cycles_per_mb": 0.0,
                "status": "measured",
                "method": "combinational — DC Intra_4x4 prediction is pred=128 constant",
                "note": "Zero additional clock cycles. Intra prediction is currently DC-only (pred=128), "
                        "computed combinationally inside h264_recon4x4. The parse_cavlc time already "
                        "covers the residual decode; intra adds no pipeline latency.",
            },
            "diagnostic_paint": {
                "cycles": paint_total,
                "cycles_per_mb": paint_total / total_mbs if total_mbs else 0.0,
                "status": "measured",
                "method": "fs_wr_reset to fs_swap transition per VCL frame",
                "note": "NOT PRODUCTION. decode_stub paints WxH={}x{} diagnostic pixels per frame "
                        "at 1 pixel/cycle. This will not exist in the production decoder.".format(width, height),
            },
            "injection_overhead": {
                "cycles": injection_total,
                "cycles_per_mb": injection_total / total_mbs if total_mbs else 0.0,
                "status": "measured",
                "method": "ioctl feedByte loop: 2 cycles per byte (ioctl_wr=1 then ioctl_wr=0)",
                "note": "TESTBENCH ARTIFACT. Real hardware uses DDR DMA, not ioctl byte-by-byte injection. "
                        "This cost does not exist in the product pipeline.",
            },
            # mc_interpolation, deblock, ddr_write are NOT included here when
            # the sim has no data for them.  Their status comes from the ratchet
            # fixture declaration (typically "not_implemented").  When these
            # stages are built and the sim produces measurements, add them here
            # with status="measured" and real cycle counts.
        }

    # Build stage_coverage from ratchet declarations + measured data
    stage_coverage = []
    for stage_decl in ratchet["stage_coverage"]:
        entry = dict(stage_decl)
        name = entry["name"]
        if stages_measured and name in stages_measured:
            sm = stages_measured[name]
            entry["status"] = sm["status"]
            entry["cycles_per_mb"] = sm["cycles_per_mb"]
            entry["cycles"] = sm["cycles"]
            entry["method"] = sm["method"]
            entry["note"] = sm["note"]
        stage_coverage.append(entry)
    # Add any measured stages not declared in ratchet
    if stages_measured:
        declared_names = {s["name"] for s in stage_coverage}
        for name, sm in stages_measured.items():
            if name not in declared_names:
                stage_coverage.append({
                    "name": name,
                    "status": sm["status"],
                    "cycles_per_mb": sm["cycles_per_mb"],
                    "cycles": sm.get("cycles"),
                    "method": sm.get("method"),
                    "note": sm["note"],
                })

    return {
        "format": "misterplex.decode_throughput_report.v1",
        "source": compare["source"],
        "geometry": {
            "width": width,
            "height": height,
            "mb_w": mb_w,
            "mb_h": mb_h,
            "mbs_per_frame": mbs_per_frame,
            "frames": frames,
        },
        "target": target,
        "measured": {
            "cycles_total": cycles_total,
            "cycles_per_frame": cycles_per_frame,
            "cycles_per_mb": cycles_per_mb,
        },
        "budget": {
            "cycles_per_frame": budget_cycles_per_frame,
            "cycles_per_mb": budget_cycles_per_mb,
            "margin_ratio": margin_ratio,
        },
        "stage_coverage": stage_coverage,
        "thresholds": ratchet["thresholds"],
    }


def check_report(report: dict) -> list[str]:
    thresholds = report["thresholds"]
    measured = report["measured"]
    budget = report["budget"]
    failures: list[str] = []
    max_total = thresholds.get("max_cycles_total")
    if max_total is not None and measured["cycles_total"] > float(max_total):
        failures.append(f"cycles_total {measured['cycles_total']} > ratchet {max_total}")
    max_frame = thresholds.get("max_cycles_per_frame")
    if max_frame is not None and measured["cycles_per_frame"] > float(max_frame):
        failures.append(
            f"cycles_per_frame {measured['cycles_per_frame']:.3f} > ratchet {float(max_frame):.3f}"
        )
    max_mb = thresholds.get("max_cycles_per_mb")
    if max_mb is not None and measured["cycles_per_mb"] > float(max_mb):
        failures.append(
            f"cycles_per_mb {measured['cycles_per_mb']:.3f} > ratchet {float(max_mb):.3f}"
        )
    min_margin = thresholds.get("min_budget_margin_ratio")
    if min_margin is not None and budget["margin_ratio"] < float(min_margin):
        failures.append(
            f"budget margin {budget['margin_ratio']:.3f} < required {float(min_margin):.3f}"
        )
    if measured["cycles_per_frame"] > budget["cycles_per_frame"]:
        failures.append(
            f"measured frame cost {measured['cycles_per_frame']:.3f} exceeds realtime budget "
            f"{budget['cycles_per_frame']:.3f}"
        )
    # Per-stage ratchet checks
    stage_thresholds = thresholds.get("stages", {})
    for stage in report["stage_coverage"]:
        name = stage["name"]
        st = stage_thresholds.get(name)
        if st is None:
            continue
        cpm = stage.get("cycles_per_mb")
        if cpm is None:
            continue
        max_stage_mb = st.get("max_cycles_per_mb")
        if max_stage_mb is not None and float(cpm) > float(max_stage_mb):
            failures.append(
                f"stage {name} cycles_per_mb {float(cpm):.3f} > ratchet {float(max_stage_mb):.3f}"
            )
    return failures


def print_raw(report: dict) -> None:
    g = report["geometry"]
    m = report["measured"]
    b = report["budget"]
    target = report["target"]
    print(
        "DECODE_THROUGHPUT_RAW "
        f"clock_hz={target['stream_path_clock_hz']} fps={target['fps']} "
        f"frames={g['frames']} mbs_per_frame={g['mbs_per_frame']} "
        f"cycles_total={m['cycles_total']} cycles_per_frame={m['cycles_per_frame']:.3f} "
        f"cycles_per_mb={m['cycles_per_mb']:.3f} "
        f"budget_cycles_per_frame={b['cycles_per_frame']:.3f} "
        f"budget_cycles_per_mb={b['cycles_per_mb']:.3f} "
        f"margin_ratio={b['margin_ratio']:.3f}"
    )
    # Per-stage breakdown
    production_measured = 0.0
    has_unimplemented = False
    for stage in report["stage_coverage"]:
        status = stage["status"]
        cpm = stage.get("cycles_per_mb")
        rendered = "UNKNOWN" if cpm is None else f"{float(cpm):.3f}"
        method = stage.get("method", "")
        method_str = f" method={method}" if method else ""
        print(
            "DECODE_THROUGHPUT_STAGE "
            f"name={stage['name']} status={status} cycles_per_mb={rendered}{method_str} "
            f"note={stage['note']}"
        )
        if status == "not_implemented":
            has_unimplemented = True
        elif status == "measured" and cpm is not None:
            name = stage["name"]
            # Only count production-relevant stages
            if name not in ("diagnostic_paint", "injection_overhead"):
                production_measured += float(cpm)
    # Print the honest summary
    if production_measured > 0:
        print(
            f"DECODE_THROUGHPUT_PRODUCTION_COST "
            f"measured_production_cycles_per_mb={production_measured:.3f} "
            f"budget_cycles_per_mb={b['cycles_per_mb']:.3f} "
            f"production_margin={b['cycles_per_mb'] / production_measured:.1f}x"
        )
    if has_unimplemented:
        print(
            "DECODE_THROUGHPUT_WARNING "
            "mc_interpolation/deblock/ddr_write are NOT IMPLEMENTED and NOT MEASURED. "
            "The margin ratio is computed against the aggregate cycle count which is "
            "dominated by diagnostic paint overhead (not production). The real production "
            "margin CANNOT be determined until MC and deblock are in the pipeline. "
            "Do not use the aggregate margin as evidence of timing closure."
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--compare-json", required=True)
    ap.add_argument("--ratchet", required=True)
    ap.add_argument("--label", help="stable run label to copy into the report")
    ap.add_argument("--report")
    args = ap.parse_args()

    try:
        report = derive(load_json(args.compare_json), load_json(args.ratchet))
    except (KeyError, TypeError, ValueError) as e:
        return fail(str(e))
    source_path = Path(report["source"]["path"]).name
    report["run"] = {
        "label": args.label or f"{source_path}:{report['geometry']['width']}x{report['geometry']['height']}",
        "compare_json": args.compare_json,
        "ratchet": args.ratchet,
    }

    if args.report:
        Path(args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(args.report).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                     encoding="utf-8")

    print_raw(report)
    failures = check_report(report)

    # Structural coverage gate: unimplemented budgeted stages make the
    # verdict INCOMPLETE regardless of whether measured stages pass.
    unimplemented = [
        s["name"] for s in report["stage_coverage"]
        if s["status"] == "not_implemented"
        and s["name"] not in ("diagnostic_paint", "injection_overhead")
    ]

    if failures:
        for item in failures:
            print("FAIL decode throughput: " + item)
        return 1
    if unimplemented:
        print(
            "INCOMPLETE decode throughput: "
            f"cycles_per_mb={report['measured']['cycles_per_mb']:.3f} "
            f"budget={report['budget']['cycles_per_mb']:.3f} "
            f"margin={report['budget']['margin_ratio']:.3f}x "
            f"UNBUDGETED_STAGES={','.join(unimplemented)}"
        )
        # Exit 0 — the implemented stages pass, but the verdict is
        # explicitly INCOMPLETE, not OK.  A downstream gate that
        # requires OK will not match this output.
        return 0
    print(
        "OK decode throughput: "
        f"cycles_per_mb={report['measured']['cycles_per_mb']:.3f} "
        f"budget={report['budget']['cycles_per_mb']:.3f} "
        f"margin={report['budget']['margin_ratio']:.3f}x"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
