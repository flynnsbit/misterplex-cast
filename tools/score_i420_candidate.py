#!/usr/bin/env python3
"""Score an explicit I420 candidate against provenanced frame-plane goldens."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


NATIVE_I420 = "I420_NATIVE"


class ProvenanceRefusal(Exception):
    pass


def refuse(msg: str) -> None:
    raise ProvenanceRefusal(msg)


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def require_loop_filter(manifest: dict[str, Any], expected: str) -> None:
    decoder = manifest.get("decoder", {})
    provenance = manifest.get("provenance", {})
    got = provenance.get("h264_loop_filter")
    if got != expected:
        refuse(
            "reference manifest provenance.h264_loop_filter mismatch: "
            f"got={got!r} expected={expected!r}"
        )
    dec_loop = decoder.get("loop_filter")
    if expected == "disabled" and dec_loop != "skip_loop_filter=all":
        refuse(
            "reference manifest decoder.loop_filter must be skip_loop_filter=all "
            "when provenance.h264_loop_filter=disabled"
        )
    if expected == "enabled" and dec_loop == "skip_loop_filter=all":
        refuse(
            "reference manifest decoder.loop_filter still disables deblock while "
            "provenance.h264_loop_filter=enabled"
        )


def plane_views(frame: int, width: int, height: int) -> list[tuple[str, int, int, int, int]]:
    y_count = width * height
    cw = width // 2
    ch = height // 2
    c_count = cw * ch
    frame_bytes = y_count + c_count * 2
    base = frame * frame_bytes
    return [
        ("Y", base, width, height, y_count),
        ("U", base + y_count, cw, ch, c_count),
        ("V", base + y_count + c_count, cw, ch, c_count),
    ]


def normalize_mv(value: Any) -> Any:
    if isinstance(value, dict):
        return value
    if isinstance(value, list) and len(value) == 2:
        return {"x": value[0], "y": value[1]}
    return value


def load_mb_metadata(path: str | None, width: int, height: int) -> dict[tuple[int, int], dict[str, Any]]:
    if not path:
        return {}
    data = read_json(Path(path))
    if data.get("format") != "misterplex.p3.inter_mb_metadata.v1":
        raise SystemExit("inter MB metadata is not misterplex.p3.inter_mb_metadata.v1")
    geom = data.get("geometry", {})
    if geom and (int(geom.get("width", width)) != width or int(geom.get("height", height)) != height):
        raise SystemExit("inter MB metadata geometry does not match candidate geometry")
    out: dict[tuple[int, int], dict[str, Any]] = {}
    entries = []
    for frame in data.get("frames", []):
        frame_index = int(frame["frame_index"])
        for mb in frame.get("macroblocks", []):
            item = dict(mb)
            item.setdefault("frame_index", frame_index)
            entries.append(item)
    entries.extend(data.get("macroblocks", []))
    for mb in entries:
        frame_index = int(mb["frame_index"])
        mb_index = int(mb.get("mb_index", int(mb["mb_y"]) * (width // 16) + int(mb["mb_x"])))
        item = dict(mb)
        if "mv_l0" in item:
            item["mv_l0"] = normalize_mv(item["mv_l0"])
        out[(frame_index, mb_index)] = item
    return out


def score_plane(
    candidate: bytes, golden: bytes, off: int, width: int, height: int, count: int
) -> dict[str, Any]:
    exact = 0
    sum_abs = 0
    max_abs = 0
    first_bad: dict[str, Any] | None = None
    for i in range(count):
        got = candidate[off + i]
        ref = golden[off + i]
        diff = abs(got - ref)
        if diff == 0:
            exact += 1
        else:
            if first_bad is None:
                x = i % width
                y = i // width
                first_bad = {"x": x, "y": y, "got": got, "ref": ref, "abs": diff}
            sum_abs += diff
            max_abs = max(max_abs, diff)
    return {
        "exact_pixels": exact,
        "total_pixels": count,
        "mae": sum_abs / count if count else 0.0,
        "max_abs": max_abs,
        "first_bad": first_bad,
    }


def plane_sample_block(blob: bytes, frame: int, width: int, height: int, plane: str, mb_x: int, mb_y: int) -> list[int]:
    frame_bytes = width * height * 3 // 2
    base = frame * frame_bytes
    if plane == "Y":
        off = base
        pw = width
        block_w = block_h = 16
        sx = mb_x * 16
        sy = mb_y * 16
    elif plane == "U":
        off = base + width * height
        pw = width // 2
        block_w = block_h = 8
        sx = mb_x * 8
        sy = mb_y * 8
    elif plane == "V":
        off = base + width * height + (width // 2) * (height // 2)
        pw = width // 2
        block_w = block_h = 8
        sx = mb_x * 8
        sy = mb_y * 8
    else:
        raise SystemExit(f"unknown plane {plane}")
    return [
        blob[off + (sy + y) * pw + (sx + x)]
        for y in range(block_h)
        for x in range(block_w)
    ]


def mb_exact(candidate: bytes, golden: bytes, frame: int, width: int, height: int, mb_x: int, mb_y: int) -> bool:
    y_base = frame * (width * height * 3 // 2)
    u_base = y_base + width * height
    v_base = u_base + (width // 2) * (height // 2)
    for yy in range(16):
        y = mb_y * 16 + yy
        for xx in range(16):
            x = mb_x * 16 + xx
            idx = y_base + y * width + x
            if candidate[idx] != golden[idx]:
                return False
    cw = width // 2
    for yy in range(8):
        y = mb_y * 8 + yy
        for xx in range(8):
            x = mb_x * 8 + xx
            cidx = y * cw + x
            if candidate[u_base + cidx] != golden[u_base + cidx]:
                return False
            if candidate[v_base + cidx] != golden[v_base + cidx]:
                return False
    return True


def analyze_mv_precision(mv: dict[str, Any] | None) -> dict[str, Any]:
    """Classify MV as integer-pel, half-pel, or quarter-pel.

    H.264 MVs are in quarter-pel units. Integer MVs mean the interpolator
    filter is NOT exercised — errors at integer MVs point to MV/reference
    selection bugs, not filter bugs.
    """
    if mv is None:
        return {"classification": "unknown", "mv_present": False}
    mx = int(mv.get("x", 0))
    my = int(mv.get("y", 0))
    frac_x = mx % 4 if mx >= 0 else (-mx) % 4
    frac_y = my % 4 if my >= 0 else (-my) % 4
    if frac_x == 0 and frac_y == 0:
        classification = "integer"
    elif frac_x % 2 == 0 and frac_y % 2 == 0:
        classification = "half_pel"
    else:
        classification = "quarter_pel"
    return {
        "classification": classification,
        "mv_present": True,
        "mv_x_qpel": mx,
        "mv_y_qpel": my,
        "frac_x_qpel": frac_x,
        "frac_y_qpel": frac_y,
        "integer_pel": classification == "integer",
    }


def analyze_error_source(
    candidate_block: list[int],
    reference_block: list[int],
    predicted_block: list[int] | None,
) -> dict[str, Any]:
    """Decompose candidate-vs-reference error into prediction and residual components.

    If the predicted_block (motion-compensated prediction from the DUT) is
    available, compute:
      candidate_residual = candidate - predicted  (should equal decoded residual)
      reference_residual = reference - predicted  (what residual SHOULD be, if pred matched)
      prediction_error   = candidate_pred vs expected_pred (if we had ref pred)

    Since we only have the DUT prediction, we infer:
      If predicted == reference => residual must be zero, but candidate != reference,
        so residual decoding is wrong.
      If predicted != reference => prediction is wrong (MC/interpolation bug).
    """
    n = len(candidate_block)
    if n != len(reference_block):
        return {"analysis": "size_mismatch"}

    total_error = sum(abs(candidate_block[i] - reference_block[i]) for i in range(n))
    max_error = max(abs(candidate_block[i] - reference_block[i]) for i in range(n))
    error_pixels = sum(1 for i in range(n) if candidate_block[i] != reference_block[i])

    result: dict[str, Any] = {
        "total_abs_error": total_error,
        "max_abs_error": max_error,
        "error_pixels": error_pixels,
        "total_pixels": n,
    }

    if predicted_block is None or len(predicted_block) != n:
        result["decomposition"] = "unavailable"
        result["reason"] = "no predicted_block metadata"
        return result

    # Compute candidate residual: what the DUT thinks residual is
    cand_residual = [candidate_block[i] - predicted_block[i] for i in range(n)]
    # Compute "expected residual if prediction were correct": ref - pred
    expected_residual = [reference_block[i] - predicted_block[i] for i in range(n)]

    # Prediction error: does candidate's prediction match the reference?
    # If predicted_block matched reference, expected_residual would be all-zero
    # and the whole error would be in the residual.
    pred_error_pixels = sum(1 for i in range(n) if predicted_block[i] != reference_block[i])
    pred_total_abs = sum(abs(predicted_block[i] - reference_block[i]) for i in range(n))
    pred_max_abs = max(abs(predicted_block[i] - reference_block[i]) for i in range(n))

    # Residual error: does candidate residual match expected residual?
    # (This checks if CAVLC/IDCT/dequant is correct given the prediction)
    res_error_pixels = sum(1 for i in range(n) if cand_residual[i] != expected_residual[i])
    res_total_abs = sum(abs(cand_residual[i] - expected_residual[i]) for i in range(n))

    result["decomposition"] = "available"
    result["prediction_error"] = {
        "error_pixels": pred_error_pixels,
        "total_abs": pred_total_abs,
        "max_abs": pred_max_abs,
    }
    result["residual_error"] = {
        "error_pixels": res_error_pixels,
        "total_abs": res_total_abs,
    }

    if pred_error_pixels > 0 and res_error_pixels == 0:
        result["error_source"] = "prediction"
        result["diagnosis"] = (
            "Prediction (motion compensation) differs from reference. "
            "Residual decoding appears correct given the prediction."
        )
    elif pred_error_pixels == 0 and res_error_pixels > 0:
        result["error_source"] = "residual"
        result["diagnosis"] = (
            "Prediction matches reference, but residual reconstruction diverges. "
            "Check CAVLC/IDCT/dequant for this MB."
        )
    elif pred_error_pixels > 0 and res_error_pixels > 0:
        result["error_source"] = "both"
        result["diagnosis"] = (
            "Both prediction and residual diverge from reference. "
            "Fix prediction first — residual error may be a consequence."
        )
    else:
        result["error_source"] = "none"
        result["diagnosis"] = "Block-level comparison shows no error (pixel-level rounding?)."

    return result


def annotate_mb_context(
    fb: dict[str, Any],
    candidate: bytes,
    golden: bytes,
    width: int,
    height: int,
    mb_meta: dict[tuple[int, int], dict[str, Any]],
) -> dict[str, Any]:
    out = dict(fb)
    meta = mb_meta.get((int(fb["frame_index"]), int(fb["mb_index"])), {})
    if meta:
        out["mb_type"] = meta.get("mb_type", "P_UNKNOWN")
        if "ref_idx_l0" in meta:
            out["ref_idx_l0"] = meta["ref_idx_l0"]
        if "mv_l0" in meta:
            out["mv_l0"] = meta["mv_l0"]
        for key in ("partition", "part_mode", "skip_run"):
            if key in meta:
                out[key] = meta[key]
    plane = str(fb["plane"])
    frame = int(fb["frame_index"])
    mb_x = int(fb["mb_x"])
    mb_y = int(fb["mb_y"])
    out["candidate_block"] = plane_sample_block(candidate, frame, width, height, plane, mb_x, mb_y)
    out["reference_block"] = plane_sample_block(golden, frame, width, height, plane, mb_x, mb_y)
    pred_key = {"Y": "pred_y", "U": "pred_u", "V": "pred_v"}[plane]
    if pred_key in meta:
        out["predicted_block"] = meta[pred_key]

    # Inter RCA: MV precision and error decomposition
    mv = out.get("mv_l0")
    out["mv_precision"] = analyze_mv_precision(mv)
    out["error_analysis"] = analyze_error_source(
        out["candidate_block"],
        out["reference_block"],
        out.get("predicted_block"),
    )
    return out


def format_inter_rca_diagnostic(fb: dict[str, Any]) -> str:
    """Format a human-readable diagnostic for the first divergent inter MB."""
    lines = []
    lines.append("=" * 72)
    lines.append("INTER RCA: first divergent macroblock in raster order")
    lines.append("=" * 72)
    lines.append(
        f"  frame={fb.get('frame_index')} mb_index={fb.get('mb_index')} "
        f"mb=({fb.get('mb_x')},{fb.get('mb_y')}) plane={fb.get('plane')}"
    )
    lines.append(
        f"  first_bad_pixel=({fb.get('x')},{fb.get('y')}) "
        f"got={fb.get('got')} ref={fb.get('ref')} abs={fb.get('abs')}"
    )
    mb_type = fb.get("mb_type", "?")
    lines.append(f"  mb_type={mb_type}")

    ref_idx = fb.get("ref_idx_l0")
    mv = fb.get("mv_l0")
    if ref_idx is not None:
        lines.append(f"  ref_idx_l0={ref_idx}")
    if mv is not None:
        lines.append(f"  mv_l0=({mv.get('x', '?')},{mv.get('y', '?')}) [quarter-pel units]")

    mvp = fb.get("mv_precision", {})
    if mvp.get("mv_present"):
        cls = mvp.get("classification", "?")
        lines.append(f"  mv_precision={cls} frac=({mvp.get('frac_x_qpel', '?')},{mvp.get('frac_y_qpel', '?')})")
        if cls == "integer":
            lines.append(
                "  >> INTEGER MV: interpolation filter NOT involved. "
                "Error points to MV value, reference selection, or DPB."
            )
        else:
            lines.append(
                f"  >> SUB-PEL MV ({cls}): interpolation filter IS exercised. "
                "Error could be filter coefficients, rounding, or tap fetch."
            )

    ea = fb.get("error_analysis", {})
    if ea.get("decomposition") == "available":
        src = ea.get("error_source", "?")
        lines.append(f"  error_source={src}")
        pe = ea.get("prediction_error", {})
        re_ = ea.get("residual_error", {})
        lines.append(
            f"  prediction_error: {pe.get('error_pixels', '?')}/{ea.get('total_pixels', '?')} pixels, "
            f"total_abs={pe.get('total_abs', '?')} max_abs={pe.get('max_abs', '?')}"
        )
        lines.append(
            f"  residual_error: {re_.get('error_pixels', '?')}/{ea.get('total_pixels', '?')} pixels, "
            f"total_abs={re_.get('total_abs', '?')}"
        )
        diag = ea.get("diagnosis", "")
        if diag:
            lines.append(f"  >> {diag}")
    elif ea.get("decomposition") == "unavailable":
        lines.append(f"  error_decomposition=unavailable ({ea.get('reason', '?')})")
        lines.append("  >> Supply --mb-metadata with pred_y/pred_u/pred_v to enable prediction vs residual RCA.")
    lines.append("=" * 72)
    return "\n".join(lines)


def first_bad_mb(
    candidate: bytes,
    golden: bytes,
    frame: int,
    width: int,
    height: int,
    mb_meta: dict[tuple[int, int], dict[str, Any]],
) -> dict[str, Any] | None:
    mb_w = width // 16
    mb_h = height // 16
    for mb_y in range(mb_h):
        for mb_x in range(mb_w):
            if mb_exact(candidate, golden, frame, width, height, mb_x, mb_y):
                continue
            y_base = frame * (width * height * 3 // 2)
            u_base = y_base + width * height
            v_base = u_base + (width // 2) * (height // 2)
            for plane, base, pw, px_per_mb, py_per_mb in (
                ("Y", y_base, width, 16, 16),
                ("U", u_base, width // 2, 8, 8),
                ("V", v_base, width // 2, 8, 8),
            ):
                for yy in range(py_per_mb):
                    for xx in range(px_per_mb):
                        x = mb_x * px_per_mb + xx
                        y = mb_y * py_per_mb + yy
                        idx = base + y * pw + x
                        got = candidate[idx]
                        ref = golden[idx]
                        if got != ref:
                            return {
                                "frame_index": frame,
                                "plane": plane,
                                "mb_x": mb_x,
                                "mb_y": mb_y,
                                "mb_index": mb_y * mb_w + mb_x,
                                "x": x,
                                "y": y,
                                "pixel_in_mb_x": xx,
                                "pixel_in_mb_y": yy,
                                "got": got,
                                "ref": ref,
                                "abs": abs(got - ref),
                            }
    return None


def inter_mb_type(
    frame_index: int,
    mb_index: int,
    mb_meta: dict[tuple[int, int], dict[str, Any]],
) -> str:
    meta = mb_meta.get((frame_index, mb_index))
    if not meta:
        return "P_UNKNOWN"
    return str(meta.get("mb_type", "P_UNKNOWN"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sequence", required=True, help="misterplex.p3.nal_sequence.v1 manifest")
    ap.add_argument("--golden-manifest", required=True, help="frame-plane golden manifest")
    ap.add_argument("--golden-planes", required=True, help="reference I420 planes")
    ap.add_argument("--candidate-planes", required=True, help="candidate I420 planes to score")
    ap.add_argument("--candidate-colorspace", required=True, help="must be I420_NATIVE")
    ap.add_argument("--reference-h264-loop-filter", required=True, choices=("disabled", "enabled"))
    ap.add_argument("--candidate-h264-loop-filter", required=True, choices=("disabled", "enabled"))
    ap.add_argument("--mb-metadata", help="optional misterplex.p3.inter_mb_metadata.v1 with P MB type/MV context")
    ap.add_argument("--output", help="write JSON score")
    ap.add_argument("--expect-red", action="store_true", help="candidate must diverge")
    args = ap.parse_args()

    if args.candidate_colorspace != NATIVE_I420:
        refuse(f"candidate colorspace is {args.candidate_colorspace}, expected {NATIVE_I420}")
    if args.candidate_h264_loop_filter != args.reference_h264_loop_filter:
        refuse(
            "candidate/reference H.264 loop-filter mismatch: "
            f"candidate={args.candidate_h264_loop_filter} reference={args.reference_h264_loop_filter}"
        )

    seq = read_json(Path(args.sequence))
    manifest = read_json(Path(args.golden_manifest))
    if seq.get("format") != "misterplex.p3.nal_sequence.v1":
        raise SystemExit("sequence manifest is not misterplex.p3.nal_sequence.v1")
    if manifest.get("format") != "misterplex.p3.frame_planes_golden.v1":
        raise SystemExit("golden manifest is not misterplex.p3.frame_planes_golden.v1")
    require_loop_filter(manifest, args.reference_h264_loop_filter)
    if manifest.get("geometry", {}).get("colorspace") != NATIVE_I420:
        refuse("reference manifest geometry.colorspace is not I420_NATIVE")

    width = int(manifest["geometry"]["coded_width"])
    height = int(manifest["geometry"]["coded_height"])
    if width % 16 or height % 16:
        raise SystemExit("score_i420_candidate currently requires coded dimensions divisible by 16")
    frames_meta = manifest["frames"]
    golden = Path(args.golden_planes).read_bytes()
    candidate = Path(args.candidate_planes).read_bytes()
    if len(candidate) != len(golden):
        raise SystemExit(f"candidate size {len(candidate)} != golden size {len(golden)}")
    frame_bytes = width * height * 3 // 2
    if len(golden) != len(frames_meta) * frame_bytes:
        raise SystemExit("golden size does not match manifest frame count/geometry")
    mb_meta = load_mb_metadata(args.mb_metadata, width, height)

    sequence_frames = [n for n in seq.get("nals", []) if "vcl_index" in n]
    if len(sequence_frames) != len(frames_meta):
        raise SystemExit("sequence VCL count does not match frame-plane manifest")

    mb_total_per_frame = (width // 16) * (height // 16)
    populations: dict[str, dict[str, Any]] = {
        "I": {"frames": 0, "mb_exact": 0, "mb_total": 0},
        "P": {"frames": 0, "mb_exact": 0, "mb_total": 0},
    }
    inter_by_type: dict[str, dict[str, int]] = {}
    first_bad_overall: dict[str, Any] | None = None
    first_bad_inter: dict[str, Any] | None = None
    frame_reports: list[dict[str, Any]] = []
    strict_pass = True

    for frame in frames_meta:
        fidx = int(frame["frame_index"])
        kind = str(frame.get("slice_kind", sequence_frames[fidx].get("slice_kind", "?")))
        mb_ok = 0
        for mb_y in range(height // 16):
            for mb_x in range(width // 16):
                mb_index = mb_y * (width // 16) + mb_x
                exact = mb_exact(candidate, golden, fidx, width, height, mb_x, mb_y)
                mb_ok += exact
                if kind == "P":
                    name = inter_mb_type(fidx, mb_index, mb_meta)
                    bucket = inter_by_type.setdefault(name, {"mb_exact": 0, "mb_total": 0})
                    bucket["mb_exact"] += int(exact)
                    bucket["mb_total"] += 1
        fb = first_bad_mb(candidate, golden, fidx, width, height, mb_meta)
        if fb is not None:
            fb = annotate_mb_context(fb, candidate, golden, width, height, mb_meta)
            strict_pass = False
            if first_bad_overall is None:
                first_bad_overall = dict(fb)
                first_bad_overall["slice_kind"] = kind
            if kind == "P" and first_bad_inter is None:
                first_bad_inter = dict(fb)
                first_bad_inter["slice_kind"] = kind
                print(format_inter_rca_diagnostic(first_bad_inter), file=sys.stderr)
        plane_reports = []
        for name, off, pw, ph, count in plane_views(fidx, width, height):
            st = score_plane(candidate, golden, off, pw, ph, count)
            st["plane"] = name
            plane_reports.append(st)
        pop = populations.setdefault(kind, {"frames": 0, "mb_exact": 0, "mb_total": 0})
        pop["frames"] += 1
        pop["mb_exact"] += mb_ok
        pop["mb_total"] += mb_total_per_frame
        frame_reports.append(
            {
                "frame_index": fidx,
                "frame_num": frame.get("frame_num"),
                "slice_kind": kind,
                "mb_exact": mb_ok,
                "mb_total": mb_total_per_frame,
                "first_bad": fb,
                "planes": plane_reports,
            }
        )
        for pr in plane_reports:
            print(
                f"I420_CANDIDATE_SCORE raw frame={fidx} slice={kind} plane={pr['plane']} "
                f"exact={pr['exact_pixels']} pixels={pr['total_pixels']} "
                f"mae={pr['mae']:.6f} max_abs={pr['max_abs']}"
            )

    report = {
        "format": "misterplex.p3.i420_candidate_score.v1",
        "colorspace": NATIVE_I420,
        "reference": {
            "manifest": args.golden_manifest,
            "h264_loop_filter": args.reference_h264_loop_filter,
        },
        "candidate": {
            "planes": args.candidate_planes,
            "colorspace": args.candidate_colorspace,
            "h264_loop_filter": args.candidate_h264_loop_filter,
        },
        "mb_metadata": args.mb_metadata,
        "geometry": {"width": width, "height": height, "mb_total_per_frame": mb_total_per_frame},
        "summary": {
            "frames": len(frames_meta),
            "strict_pass": strict_pass,
            "intra": populations.get("I", {"frames": 0, "mb_exact": 0, "mb_total": 0}),
            "inter": populations.get("P", {"frames": 0, "mb_exact": 0, "mb_total": 0}),
            "inter_by_mb_type": inter_by_type,
            "first_bad": first_bad_overall,
            "first_bad_inter": first_bad_inter,
        },
        "frames": frame_reports,
    }
    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    intra = report["summary"]["intra"]
    inter = report["summary"]["inter"]
    print(
        "I420_CANDIDATE_SCORE summary "
        f"intra={intra['mb_exact']}/{intra['mb_total']} "
        f"inter={inter['mb_exact']}/{inter['mb_total']} "
        f"strict_pass={1 if strict_pass else 0}"
    )
    if args.expect_red:
        if strict_pass:
            raise SystemExit("candidate unexpectedly matched golden in expect-red mode")
        print("score_i420_candidate: OK expected-red candidate diverged from golden")
        return 0
    if not strict_pass:
        raise SystemExit("candidate I420 planes diverged from golden")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ProvenanceRefusal as e:
        print(str(e), file=sys.stderr)
        raise SystemExit(9)
