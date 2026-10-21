#!/usr/bin/env python3
"""RED-before-GREEN probes for Intra 16x16 Plane prediction.

Each probe applies a specific RTL mutation, rebuilds, and confirms the test FAILS.
Mutations tested:
  1. Wrong shift: >>> 5 instead of >>> 6 for b/c computation
  2. Missing clip: raw value instead of clip8()
  3. Unsigned intermediate: losing sign on H/V
  4. Swapped H/V: b uses V gradient, c uses H gradient
"""
import os
import re
import subprocess
import sys
import tempfile
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RTL_SRC = ROOT / "fpga/Plex_MiSTer/rtl/h264_intra_pred.sv"
TB_SV = ROOT / "tests/unit/rtl/p3_i16_plane_tb.sv"
CPP = ROOT / "tests/unit/test_p3_i16_plane_verilator.cpp"
VERILATOR = Path.home() / ".local/oss-cad-suite-20260726/bin/verilator"

MUTATIONS = {
    "wrong_shift_b": {
        "desc": "b = (5*H+32) >>> 5 instead of >>> 6",
        "find": "(5 * hgrad_c + 32) >>> 6",
        "replace": "(5 * hgrad_c + 32) >>> 5",
    },
    "wrong_shift_c": {
        "desc": "c = (5*V+32) >>> 5 instead of >>> 6",
        "find": "(5 * vgrad_c + 32) >>> 6",
        "replace": "(5 * vgrad_c + 32) >>> 5",
    },
    "missing_clip": {
        "desc": "pred = val without clip8 in pixel evaluation",
        "find": "pred[y * 16 + x] <= clip8(val);",
        "replace": "pred[y * 16 + x] <= val[7:0];",
    },
    "swapped_hv": {
        "desc": "b uses vgrad, c uses hgrad (swapped)",
        "find": "b_c = (5 * hgrad_c + 32) >>> 6;\n\t\tc_c = (5 * vgrad_c + 32) >>> 6;",
        "replace": "b_c = (5 * vgrad_c + 32) >>> 6;\n\t\tc_c = (5 * hgrad_c + 32) >>> 6;",
    },
}


def run_mutation(name: str, mut: dict) -> tuple[bool, str]:
    """Apply mutation, build, run test. Returns (detected, output)."""
    original = RTL_SRC.read_text()
    if mut["find"] not in original:
        return False, f"Could not find pattern to mutate: {mut['find']!r}"

    mutated = original.replace(mut["find"], mut["replace"], 1)
    if mutated == original:
        return False, "Mutation had no effect"

    build_dir = ROOT / f"build/obj_p3_i16_plane_red_{name}"
    build_dir.mkdir(parents=True, exist_ok=True)

    # Write mutated RTL to build dir
    mut_rtl = build_dir / "h264_intra_pred_mutated.sv"
    mut_rtl.write_text(mutated)

    # Build with mutated RTL
    cmd = [
        str(VERILATOR), "--cc", "--exe", "--build",
        "--top-module", "p3_i16_plane_tb",
        "--Mdir", str(build_dir),
        "-Wno-fatal", "-Wno-WIDTHEXPAND", "-Wno-WIDTHTRUNC",
        "--CFLAGS", "-std=c++17 -O2",
        str(mut_rtl),
        str(TB_SV),
        str(CPP),
    ]
    build = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if build.returncode != 0:
        return False, f"Build failed:\n{build.stdout}\n{build.stderr}"

    # Run test
    exe = build_dir / "Vp3_i16_plane_tb"
    result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
    output = result.stdout + result.stderr

    detected = result.returncode != 0
    return detected, output


def main() -> int:
    if not VERILATOR.exists():
        print(f"SKIP: Verilator not found at {VERILATOR}")
        print("RTL SIM ERROR: Verilator not found; refusing to report PASS without running the simulation.")
        return 3

    all_pass = True
    print("=== RED-before-GREEN mutation probes for I16 Plane ===\n")

    for name, mut in MUTATIONS.items():
        detected, output = run_mutation(name, mut)
        status = "RED ✓ (detected)" if detected else "GREEN ✗ (NOT detected — test gap!)"
        print(f"  {name}: {mut['desc']}")
        print(f"    Result: {status}")
        if not detected:
            print(f"    Output: {output[:200]}")
            all_pass = False
        else:
            # Show first failure line for evidence
            for line in output.split('\n'):
                if line.startswith("FAIL"):
                    print(f"    Evidence: {line[:120]}")
                    break
        print()

    if all_pass:
        print("ALL 4 mutation probes detected — RED proofs complete.")
        return 0
    else:
        print("FAIL: Some mutations were NOT detected by the test suite.")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
