#!/usr/bin/env python3
"""RED-before-GREEN probes for Chroma 8x8 Plane prediction.

Each probe applies a specific RTL mutation, rebuilds, and confirms the test FAILS.
Mutations tested:
  1. Wrong constant: 5*H instead of 17*H (confusing luma/chroma constants)
  2. Wrong offset: (x-7) instead of (x-3) (confusing luma/chroma block dimensions)
  3. Missing clip: raw value instead of clip8()
  4. Swapped H/V: b uses V gradient, c uses H gradient
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RTL_SRC = ROOT / "fpga/Plex_MiSTer/rtl/h264_intra_pred.sv"
TB_SV = ROOT / "tests/unit/rtl/p3_chroma_plane_tb.sv"
CPP = ROOT / "tests/unit/test_p3_chroma_plane_verilator.cpp"
VERILATOR = Path.home() / ".local/oss-cad-suite-20260726/bin/verilator"

MUTATIONS = {
    "wrong_constant_17_to_5": {
        "desc": "b = (5*H+16) >>> 5 instead of (17*H+16) >>> 5 (luma constant)",
        "find": "(17 * hgrad_c + 16) >>> 5",
        "replace": "(5 * hgrad_c + 16) >>> 5",
    },
    "wrong_offset_3_to_7": {
        "desc": "bx[i] = b*(i-7) instead of b*(i-3) (luma offset)",
        "find": "bx_r[i] <= b_c * (i - 3);",
        "replace": "bx_r[i] <= b_c * (i - 7);",
    },
    "missing_clip": {
        "desc": "pred = val without clip8 in pixel evaluation",
        "find": "pred[y * 8 + x] <= clip8(val);",
        "replace": "pred[y * 8 + x] <= val[7:0];",
    },
    "swapped_hv": {
        "desc": "b uses vgrad, c uses hgrad (swapped)",
        "find": "b_c = (17 * hgrad_c + 16) >>> 5;\n\t\tc_c = (17 * vgrad_c + 16) >>> 5;",
        "replace": "b_c = (17 * vgrad_c + 16) >>> 5;\n\t\tc_c = (17 * hgrad_c + 16) >>> 5;",
    },
}


def run_mutation(name: str, mut: dict) -> tuple[bool, str]:
    """Apply mutation, build, run test. Returns (detected, output)."""
    original = RTL_SRC.read_text()

    # For chroma module, we need to find the pattern in the chroma section.
    # The chroma module has its own hgrad_c/vgrad_c/b_c/c_c variables.
    # Since the luma module also has these, we need to target the chroma copy.
    # Strategy: find the pattern in the chroma module context.
    chroma_start = original.find("module h264_chroma8x8_pred")
    if chroma_start < 0:
        return False, "Could not find chroma module"

    chroma_section = original[chroma_start:]
    chroma_end = chroma_section.find("\nmodule ")
    if chroma_end < 0:
        chroma_end = len(chroma_section)
    chroma_text = chroma_section[:chroma_end]

    if mut["find"] not in chroma_text:
        return False, f"Could not find pattern in chroma module: {mut['find']!r}"

    # Replace only in chroma section
    mutated_chroma = chroma_text.replace(mut["find"], mut["replace"], 1)
    mutated = original[:chroma_start] + mutated_chroma + original[chroma_start + chroma_end:]

    build_dir = ROOT / f"build/obj_p3_chroma_plane_red_{name}"
    build_dir.mkdir(parents=True, exist_ok=True)

    mut_rtl = build_dir / "h264_intra_pred_mutated.sv"
    mut_rtl.write_text(mutated)

    cmd = [
        str(VERILATOR), "--cc", "--exe", "--build",
        "--top-module", "p3_chroma_plane_tb",
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

    exe = build_dir / "Vp3_chroma_plane_tb"
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
    print("=== RED-before-GREEN mutation probes for Chroma Plane ===\n")

    for name, mut in MUTATIONS.items():
        detected, output = run_mutation(name, mut)
        status = "RED ✓ (detected)" if detected else "GREEN ✗ (NOT detected — test gap!)"
        print(f"  {name}: {mut['desc']}")
        print(f"    Result: {status}")
        if not detected:
            print(f"    Output: {output[:200]}")
            all_pass = False
        else:
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
