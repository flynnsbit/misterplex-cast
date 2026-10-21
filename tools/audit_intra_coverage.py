#!/usr/bin/env python3
"""Audit intra prediction mode and QP coverage across test fixtures.

Reports raw counts and explicitly calls out untested modes/ranges.
Exit code 0 = audit complete (not necessarily full coverage).
"""
from __future__ import annotations

import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRACT = ROOT / "build" / "extract_h264_golden"


def extract_mb(bitstream: Path, mb: int) -> dict:
    out = ROOT / "build" / f"_audit_mb{mb}.json"
    r = subprocess.run(
        [str(EXTRACT), "--input", str(bitstream), "--mb", str(mb), "--output", str(out)],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        return {}
    data = json.load(out.open())
    out.unlink(missing_ok=True)
    return data


def audit_stream(name: str, bitstream: Path, width: int, height: int) -> dict:
    mb_total = (width // 16) * (height // 16)
    qps: list[int] = []
    i4_modes = Counter()
    i16_modes = Counter()
    chroma_modes = Counter()
    mb_types = Counter()
    ipcm_count = 0

    for mb in range(mb_total):
        g = extract_mb(bitstream, mb)
        if not g:
            print(f"  WARNING: MB {mb} extraction failed", file=sys.stderr)
            continue
        qp = g["macroblock"]["qp"]
        qps.append(qp)
        type_name = g["macroblock"].get("type_name", "unknown")
        mb_types[type_name] += 1
        if type_name == "I_PCM":
            ipcm_count += 1
        pred = g.get("prediction", {})
        if type_name == "I_NxN":
            # Intra 4x4: per-block modes in prediction.luma_4x4_modes
            for pm in pred.get("luma_4x4_modes", []):
                if 0 <= pm <= 8:
                    i4_modes[pm] += 1
        elif type_name in ("I_16x16_0_0_0", "I_16x16_1_0_0", "I_16x16_2_0_0",
                           "I_16x16_0_1_0", "I_16x16_1_1_0", "I_16x16_2_1_0",
                           "I_16x16_0_2_0", "I_16x16_1_2_0", "I_16x16_2_2_0",
                           "I_16x16_0_0_1", "I_16x16_1_0_1", "I_16x16_2_0_1",
                           "I_16x16_0_1_1", "I_16x16_1_1_1", "I_16x16_2_1_1",
                           "I_16x16_0_2_1", "I_16x16_1_2_1", "I_16x16_2_2_1",
                           "I_16x16_3_0_0", "I_16x16_3_1_0", "I_16x16_3_2_0",
                           "I_16x16_3_0_1", "I_16x16_3_1_1", "I_16x16_3_2_1") or \
             type_name.startswith("I_16x16"):
            pm = pred.get("luma_16x16", -1)
            if 0 <= pm <= 3:
                i16_modes[pm] += 1
        cm = pred.get("chroma", -1)
        if 0 <= cm <= 3:
            chroma_modes[cm] += 1

    return {
        "name": name,
        "geometry": f"{width}x{height}",
        "mb_total": mb_total,
        "qp_min": min(qps) if qps else -1,
        "qp_max": max(qps) if qps else -1,
        "qp_unique": sorted(set(qps)),
        "qp_histogram": dict(sorted(Counter(qps).items())),
        "i4_modes": {
            "V(0)": i4_modes.get(0, 0),
            "H(1)": i4_modes.get(1, 0),
            "DC(2)": i4_modes.get(2, 0),
            "DDL(3)": i4_modes.get(3, 0),
            "DDR(4)": i4_modes.get(4, 0),
            "VR(5)": i4_modes.get(5, 0),
            "HD(6)": i4_modes.get(6, 0),
            "VL(7)": i4_modes.get(7, 0),
            "HU(8)": i4_modes.get(8, 0),
        },
        "i16_modes": {
            "V(0)": i16_modes.get(0, 0),
            "H(1)": i16_modes.get(1, 0),
            "DC(2)": i16_modes.get(2, 0),
            "Plane(3)": i16_modes.get(3, 0),
        },
        "chroma_modes": {
            "DC(0)": chroma_modes.get(0, 0),
            "H(1)": chroma_modes.get(1, 0),
            "V(2)": chroma_modes.get(2, 0),
            "Plane(3)": chroma_modes.get(3, 0),
        },
        "ipcm_count": ipcm_count,
        "mb_types": dict(mb_types),
    }


def print_coverage(results: list[dict]) -> None:
    # Aggregate
    all_qps: set[int] = set()
    total_i4 = Counter()
    total_i16 = Counter()
    total_chroma = Counter()
    total_ipcm = 0

    for r in results:
        all_qps.update(r["qp_unique"])
        for k, v in r["i4_modes"].items():
            total_i4[k] += v
        for k, v in r["i16_modes"].items():
            total_i16[k] += v
        for k, v in r["chroma_modes"].items():
            total_chroma[k] += v
        total_ipcm += r["ipcm_count"]

    print("=" * 72)
    print("INTRA COVERAGE AUDIT — aggregate across all IDR test fixtures")
    print("=" * 72)

    print("\n--- QP RANGE ---")
    print(f"  Exercised: {sorted(all_qps)}")
    print(f"  Min: {min(all_qps)}  Max: {max(all_qps)}")
    gaps = []
    if min(all_qps) > 0:
        gaps.append(f"QP 0-{min(all_qps)-1}")
    if max(all_qps) < 51:
        gaps.append(f"QP {max(all_qps)+1}-51")
    # Check near wrap boundary
    wrap_exercised = any(q >= 48 for q in all_qps) and any(q <= 3 for q in all_qps)
    print(f"  Wrap boundary (QP near 0 and 51): {'EXERCISED' if wrap_exercised else 'NOT EXERCISED'}")
    if gaps:
        print(f"  GAPS: {', '.join(gaps)}")

    print("\n--- INTRA 4x4 MODES (9 total) ---")
    for mode, count in sorted(total_i4.items()):
        status = "✓" if count > 0 else "✗ UNTESTED"
        print(f"  {mode:8s}: {count:6d} blocks  {status}")

    print("\n--- INTRA 16x16 MODES (4 total) ---")
    for mode, count in sorted(total_i16.items()):
        status = "✓" if count > 0 else "✗ UNTESTED"
        print(f"  {mode:10s}: {count:6d} MBs  {status}")

    print("\n--- CHROMA MODES (4 total) ---")
    for mode, count in sorted(total_chroma.items()):
        status = "✓" if count > 0 else "✗ UNTESTED"
        print(f"  {mode:10s}: {count:6d} MBs  {status}")

    print(f"\n--- I_PCM MACROBLOCKS ---")
    print(f"  Count: {total_ipcm}  {'✓' if total_ipcm > 0 else '✗ UNTESTED'}")
    print(f"  RTL status: UNSUPPORTED (signals unsupported_code=UNSUP_IPCM)")

    print(f"\n--- RTL UNSUPPORTED MODES ---")
    print(f"  I16_Plane (mode 3): UNSUPPORTED in h264_intra_pred.sv")
    print(f"  I_PCM (mb_type=25): UNSUPPORTED in h264_intra_pred.sv")

    # Gaps summary
    print("\n" + "=" * 72)
    print("COVERAGE GAPS (a mode never exercised is a mode never verified)")
    print("=" * 72)
    gap_count = 0
    untested_i4 = [m for m, c in total_i4.items() if c == 0]
    untested_i16 = [m for m, c in total_i16.items() if c == 0]
    untested_chroma = [m for m, c in total_chroma.items() if c == 0]
    if untested_i4:
        print(f"  INTRA_4x4 untested: {untested_i4}")
        gap_count += len(untested_i4)
    if untested_i16:
        print(f"  INTRA_16x16 untested: {untested_i16}")
        gap_count += len(untested_i16)
    if untested_chroma:
        print(f"  CHROMA untested: {untested_chroma}")
        gap_count += len(untested_chroma)
    if not wrap_exercised:
        print("  QP WRAP BOUNDARY: NOT EXERCISED (QP range 5-27, need 0-3 and 48-51)")
        gap_count += 1
    if total_ipcm == 0:
        print("  I_PCM: NOT EXERCISED (and UNSUPPORTED in RTL)")
        gap_count += 1
    if gap_count == 0:
        print("  None found — all modes exercised")
    print("=" * 72)


def main() -> int:
    if not EXTRACT.exists():
        print("Build h264-golden-tools first: make h264-golden-tools", file=sys.stderr)
        return 2

    fixtures = [
        ("320x240 IDR (plex_real_baseline)", ROOT / "tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264", 320, 240),
        ("624x480 IDR (plex_visual)", ROOT / "tests/fixtures/hw_visual/plex_visual_624x480_1f.264", 624, 480),
    ]

    results = []
    for name, path, w, h in fixtures:
        print(f"Auditing {name} ({w}x{h}, {(w//16)*(h//16)} MBs)...", file=sys.stderr)
        r = audit_stream(name, path, w, h)
        results.append(r)

    print_coverage(results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
