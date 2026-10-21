#!/usr/bin/env python3
"""Unit coverage for the G-VID1 edge-capture grading harness."""
from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECK_EDGES = ROOT / "scripts" / "check_edges.py"
WORK = ROOT / "build" / "capture-rig-unit"

spec = importlib.util.spec_from_file_location("check_edges", CHECK_EDGES)
check_edges = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(check_edges)


def require(ok: bool, msg: str) -> None:
    if not ok:
        raise AssertionError(msg)


def grade(kind: str) -> list[str]:
    frame = check_edges.synthetic_frame(kind)
    _lines, problems = check_edges.grade_frame(frame.astype(int))
    return problems


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECK_EDGES), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=30,
    )


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)

    problems = grade("good")
    require(not problems, f"known-good aligned frame failed: {problems}")
    print("PASS synthetic known-good aligned frame")

    problems = grade("hwrap")
    require(any(p.startswith("H WRAP") or p.startswith("H leading edge") for p in problems),
            f"horizontal wrap did not identify H edge: {problems}")
    require(not any(p.startswith("V ") for p in problems),
            f"horizontal wrap should not blame vertical edge: {problems}")
    print("PASS synthetic 1-source-pixel horizontal wrap rejected on H edge")

    problems = grade("vshift")
    require(any(p.startswith("V leading edge") for p in problems),
            f"vertical shift did not identify V leading edge: {problems}")
    print("PASS synthetic 1-source-pixel vertical shift rejected")

    baseline = WORK / "baseline.png"
    good = WORK / "good.png"
    stale = WORK / "stale.png"
    check_edges.save_image(baseline, check_edges.synthetic_frame("baseline"))
    check_edges.save_image(good, check_edges.synthetic_frame("good"))
    check_edges.save_image(stale, check_edges.synthetic_frame("good"))

    r = run_cli("--source", "file", "--input", str(baseline), "--input", str(good))
    require(r.returncode == 0 and "PASS: all four edges correct" in r.stdout,
            f"file-backed good capture failed rc={r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}")
    print("PASS file-backed capture mode grades changed good frame")

    r = run_cli("--source", "file", "--input", str(good), "--input", str(stale))
    require(r.returncode == 2 and "STALE capture" in r.stderr,
            f"stale file-backed capture was not rejected rc={r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}")
    print("PASS byte-identical consecutive grabs rejected as STALE")

    r = run_cli("--source", "synthetic", "--synthetic-case", "stale")
    require(r.returncode == 2 and "STALE capture" in r.stderr,
            f"synthetic stale case was not rejected rc={r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}")
    print("PASS synthetic stale source rejected before grading")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
