#!/usr/bin/env python3
"""Generate a graded P-slice difficulty ladder for inter decode verification.

Each rung isolates one specific inter-prediction feature so that failures
are self-naming. All rungs produce Constrained Baseline Annex-B streams
at 320x240 (matching existing intra fixtures).

Rungs:
  1. p_skip_only       — P_Skip, zero MV, zero residual
  2. p16x16_integer_mv — P_16x16 with integer-pel MVs only
  3. p16x16_halfpel    — P_16x16 with half-pel MVs (6-tap filter)
  4. p16x16_quarterpel — P_16x16 with quarter-pel MVs (avg stage)
  5. sub_partitions    — 16x8, 8x16, 8x8 sub-MB partitions
  6. multi_ref_gop     — longer GOP with multiple references

Deblocked references are generated alongside each rung — MC predicts
from the deblocked picture, so the intra-era -skip_loop_filter is
actively wrong for inter.

Usage:
  python3 tools/gen_p_slice_ladder.py --output-dir build/p_slice_ladder
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRACT_TOOL = ROOT / "tools" / "extract_h264_frame_planes.py"

WIDTH = 320
HEIGHT = 240
FRAMES = 6  # 1 IDR + 5 P for each rung


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(f"FAIL: {' '.join(cmd[:5])}", file=sys.stderr)
        print(r.stdout, file=sys.stderr)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(r.returncode)
    return r


def ffprobe_frame_count(path: Path) -> int:
    r = run([
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-count_frames", "-show_entries", "stream=nb_read_frames",
        "-of", "csv=p=0", str(path),
    ])
    val = r.stdout.strip()
    if val == "N/A" or not val:
        return 0
    return int(val)


def ffprobe_frame_types(path: Path) -> list[str]:
    r = run([
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "frame=pict_type", "-of", "csv=p=0", str(path),
    ])
    return [line.strip().rstrip(",") for line in r.stdout.strip().split("\n") if line.strip()]


def gen_solid_yuv(path: Path, w: int, h: int, frames: int, y: int = 128, u: int = 128, v: int = 128) -> None:
    """Generate a solid-colour raw YUV420p file."""
    frame = bytes([y] * (w * h)) + bytes([u] * (w * h // 4)) + bytes([v] * (w * h // 4))
    with path.open("wb") as f:
        for _ in range(frames):
            f.write(frame)


def gen_gradient_yuv(path: Path, w: int, h: int, frames: int) -> None:
    """Generate gradient YUV with per-frame translational motion.

    The shift is 4 pixels/frame — large enough for x264 to use P-frames
    instead of deciding each frame is a scene cut.
    """
    with path.open("wb") as f:
        for fr in range(frames):
            y_plane = bytearray(w * h)
            for row in range(h):
                for col in range(w):
                    # Horizontal gradient with per-frame shift
                    src_col = (col + fr * 4) % w
                    y_plane[row * w + col] = (src_col * 220 // max(w - 1, 1) + 16) & 0xFF
            u_plane = bytes([128] * (w * h // 4))
            v_plane = bytes([128] * (w * h // 4))
            f.write(bytes(y_plane) + u_plane + v_plane)


def gen_blocks_yuv(path: Path, w: int, h: int, frames: int) -> None:
    """Generate checkerboard pattern with per-frame shift for motion."""
    block_size = 16
    with path.open("wb") as f:
        for fr in range(frames):
            y_plane = bytearray(w * h)
            for row in range(h):
                for col in range(w):
                    bx = ((col + fr * 8) // block_size) % 2
                    by = (row // block_size) % 2
                    y_plane[row * w + col] = 200 if (bx ^ by) else 40
            u_plane = bytes([128] * (w * h // 4))
            v_plane = bytes([128] * (w * h // 4))
            f.write(bytes(y_plane) + u_plane + v_plane)


class Rung:
    def __init__(self, name: str, description: str, rung_number: int):
        self.name = name
        self.description = description
        self.rung_number = rung_number

    def encode(self, output_dir: Path) -> Path:
        raise NotImplementedError

    def x264_common(self) -> list[str]:
        return [
            "-profile:v", "baseline",
            "-level", "3.0",
            "-pix_fmt", "yuv420p",
        ]


class PSkipOnly(Rung):
    """Rung 1: P_Skip only — zero MV, zero residual.

    Proves DPB fetch, reference selection, and plumbing in isolation.
    Encode a static scene so x264 emits only P_Skip MBs.
    """
    def __init__(self):
        super().__init__("p_skip_only", "P_Skip: zero MV, zero residual — DPB fetch + plumbing", 1)

    def encode(self, output_dir: Path) -> Path:
        raw = output_dir / f"{self.name}_input.yuv"
        out = output_dir / f"{self.name}.264"
        gen_solid_yuv(raw, WIDTH, HEIGHT, FRAMES, y=128, u=128, v=128)
        run([
            "ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "24", "-i", str(raw),
            "-c:v", "libx264", *self.x264_common(),
            "-x264-params",
            f"keyint={FRAMES}:min-keyint={FRAMES}:scenecut=0:"
            "bframes=0:ref=1:weightp=0:"
            "no-mixed-refs=1:me=dia:subme=0:partitions=none:"
            "no-8x8dct=1:aq-mode=0:qp=25",
            "-frames:v", str(FRAMES),
            str(out),
        ])
        raw.unlink(missing_ok=True)
        return out


class P16x16IntegerMV(Rung):
    """Rung 2: P_16x16 with integer-pel MVs.

    Proves MV decode + reference addressing with NO interpolation filter.
    Errors here point to MV/reference-selection bugs.
    """
    def __init__(self):
        super().__init__("p16x16_integer_mv",
                         "P_16x16 integer MV — MV decode + ref addressing, no filter", 2)

    def encode(self, output_dir: Path) -> Path:
        raw = output_dir / f"{self.name}_input.yuv"
        out = output_dir / f"{self.name}.264"
        gen_gradient_yuv(raw, WIDTH, HEIGHT, FRAMES)
        run([
            "ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "24", "-i", str(raw),
            "-c:v", "libx264", *self.x264_common(),
            "-x264-params",
            f"keyint={FRAMES}:min-keyint={FRAMES}:scenecut=0:"
            "bframes=0:ref=1:weightp=0:"
            "no-mixed-refs=1:me=esa:subme=0:partitions=none:"
            "no-8x8dct=1:aq-mode=0:qp=20",
            "-frames:v", str(FRAMES),
            str(out),
        ])
        raw.unlink(missing_ok=True)
        return out


class P16x16HalfPel(Rung):
    """Rung 3: P_16x16 with half-pel MVs.

    Engages the 6-tap interpolation filter. Errors here that don't
    appear at rung 2 isolate the filter implementation.
    """
    def __init__(self):
        super().__init__("p16x16_halfpel",
                         "P_16x16 half-pel MV — 6-tap filter engaged", 3)

    def encode(self, output_dir: Path) -> Path:
        raw = output_dir / f"{self.name}_input.yuv"
        out = output_dir / f"{self.name}.264"
        gen_gradient_yuv(raw, WIDTH, HEIGHT, FRAMES)
        run([
            "ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "24", "-i", str(raw),
            "-c:v", "libx264", *self.x264_common(),
            "-x264-params",
            f"keyint={FRAMES}:min-keyint={FRAMES}:scenecut=0:"
            "bframes=0:ref=1:weightp=0:"
            "no-mixed-refs=1:me=esa:subme=4:partitions=none:"
            "no-8x8dct=1:aq-mode=0:qp=20",
            "-frames:v", str(FRAMES),
            str(out),
        ])
        raw.unlink(missing_ok=True)
        return out


class P16x16QuarterPel(Rung):
    """Rung 4: P_16x16 with quarter-pel MVs.

    Engages the averaging/bilinear stage on top of the 6-tap filter.
    """
    def __init__(self):
        super().__init__("p16x16_quarterpel",
                         "P_16x16 quarter-pel MV — averaging stage", 4)

    def encode(self, output_dir: Path) -> Path:
        raw = output_dir / f"{self.name}_input.yuv"
        out = output_dir / f"{self.name}.264"
        gen_gradient_yuv(raw, WIDTH, HEIGHT, FRAMES)
        run([
            "ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "24", "-i", str(raw),
            "-c:v", "libx264", *self.x264_common(),
            "-x264-params",
            f"keyint={FRAMES}:min-keyint={FRAMES}:scenecut=0:"
            "bframes=0:ref=1:weightp=0:"
            "no-mixed-refs=1:me=esa:subme=7:partitions=none:"
            "no-8x8dct=1:aq-mode=0:qp=20",
            "-frames:v", str(FRAMES),
            str(out),
        ])
        raw.unlink(missing_ok=True)
        return out


class SubPartitions(Rung):
    """Rung 5: sub-MB partitions (16x8, 8x16, 8x8).

    Tests partition indexing — MV per sub-partition.
    """
    def __init__(self):
        super().__init__("sub_partitions",
                         "Sub-partitions (16x8/8x16/8x8) — partition indexing", 5)

    def encode(self, output_dir: Path) -> Path:
        raw = output_dir / f"{self.name}_input.yuv"
        out = output_dir / f"{self.name}.264"
        gen_blocks_yuv(raw, WIDTH, HEIGHT, FRAMES)
        run([
            "ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "24", "-i", str(raw),
            "-c:v", "libx264", *self.x264_common(),
            "-x264-params",
            f"keyint={FRAMES}:min-keyint={FRAMES}:scenecut=0:"
            "bframes=0:ref=1:weightp=0:"
            "no-mixed-refs=1:me=esa:subme=7:"
            "partitions=p8x8,p4x4,i8x8,i4x4:"
            "no-8x8dct=1:aq-mode=0:qp=20",
            "-frames:v", str(FRAMES),
            str(out),
        ])
        raw.unlink(missing_ok=True)
        return out


class MultiRefGOP(Rung):
    """Rung 6: longer GOP with multiple references.

    Tests DPB management and reference reordering with ref>1.
    """
    def __init__(self):
        super().__init__("multi_ref_gop",
                         "Multi-reference GOP — DPB management, ref reorder", 6)

    def encode(self, output_dir: Path) -> Path:
        raw = output_dir / f"{self.name}_input.yuv"
        out = output_dir / f"{self.name}.264"
        # More frames for multi-ref to matter
        n_frames = 12
        gen_blocks_yuv(raw, WIDTH, HEIGHT, n_frames)
        run([
            "ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "24", "-i", str(raw),
            "-c:v", "libx264", *self.x264_common(),
            "-x264-params",
            f"keyint={n_frames}:min-keyint={n_frames}:scenecut=0:"
            "bframes=0:ref=4:weightp=0:"
            "no-mixed-refs=0:me=esa:subme=7:"
            "partitions=p8x8,p4x4,i8x8,i4x4:"
            "no-8x8dct=1:aq-mode=0:qp=20",
            "-frames:v", str(n_frames),
            str(out),
        ])
        raw.unlink(missing_ok=True)
        return out


def generate_references(bitstream: Path, output_dir: Path, name: str) -> dict:
    """Generate both disabled and enabled loop-filter references."""
    # First generate sequence manifest
    seq_path = output_dir / f"{name}_sequence.json"
    golden_tool = ROOT / "build" / "extract_h264_golden"
    if not golden_tool.exists():
        print(f"  WARNING: {golden_tool} not found — run 'make h264-golden-tools'", file=sys.stderr)
        return {}

    r = run([
        str(golden_tool), "--input", str(bitstream), "--sequence",
        "--output", str(seq_path),
    ], check=False)
    if r.returncode != 0 or not seq_path.exists():
        print(f"  WARNING: sequence generation failed: {r.stdout[:200] if r.stdout else r.stderr[:200]}",
              file=sys.stderr)
        return {}

    results = {"sequence": str(seq_path)}
    for lf in ("disabled", "enabled"):
        planes_path = output_dir / f"{name}_{lf}_planes.i420"
        manifest_path = output_dir / f"{name}_{lf}_manifest.json"

        r = run([
            sys.executable, str(EXTRACT_TOOL),
            "--input", str(bitstream),
            "--sequence", str(seq_path),
            "--planes-out", str(planes_path),
            "--manifest-out", str(manifest_path),
            "--h264-loop-filter", lf,
        ], check=False)
        if r.returncode != 0:
            print(f"  WARNING: reference generation failed for {name} lf={lf}: "
                  f"{r.stderr[:200]}", file=sys.stderr)
            results[lf] = None
            continue

        results[lf] = {
            "planes": str(planes_path),
            "manifest": str(manifest_path),
        }

    return results


def analyze_mv_distribution(bitstream: Path) -> dict:
    """Use ffprobe to get basic MV statistics."""
    types = ffprobe_frame_types(bitstream)
    return {
        "frame_count": len(types),
        "frame_types": types,
        "i_frames": types.count("I"),
        "p_frames": types.count("P"),
    }


RUNGS: list[Rung] = [
    PSkipOnly(),
    P16x16IntegerMV(),
    P16x16HalfPel(),
    P16x16QuarterPel(),
    SubPartitions(),
    MultiRefGOP(),
]


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate P-slice difficulty ladder")
    ap.add_argument("--output-dir", required=True, help="Output directory")
    ap.add_argument("--rungs", help="Comma-separated rung numbers to generate (default: all)")
    args = ap.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    selected = RUNGS
    if args.rungs:
        nums = [int(x) for x in args.rungs.split(",")]
        selected = [r for r in RUNGS if r.rung_number in nums]

    manifest = {
        "format": "misterplex.p3.inter_ladder.v1",
        "geometry": {"width": WIDTH, "height": HEIGHT},
        "rungs": [],
    }

    for rung in selected:
        print(f"\n{'='*60}", file=sys.stderr)
        print(f"Rung {rung.rung_number}: {rung.name}", file=sys.stderr)
        print(f"  {rung.description}", file=sys.stderr)
        print(f"{'='*60}", file=sys.stderr)

        bitstream = rung.encode(output_dir)
        if not bitstream.exists():
            print(f"  FAIL: encoding did not produce {bitstream}", file=sys.stderr)
            continue

        mv_info = analyze_mv_distribution(bitstream)
        print(f"  Encoded: {bitstream.stat().st_size} bytes, "
              f"{mv_info['frame_count']} frames "
              f"(I={mv_info['i_frames']} P={mv_info['p_frames']})", file=sys.stderr)

        sha = sha256_file(bitstream)
        rung_entry = {
            "rung": rung.rung_number,
            "name": rung.name,
            "description": rung.description,
            "bitstream": str(bitstream),
            "sha256": sha,
            "bytes": bitstream.stat().st_size,
            "frames": mv_info,
        }

        # Generate references (both with and without deblocking)
        refs = generate_references(bitstream, output_dir, rung.name)
        rung_entry["references"] = refs
        if refs.get("enabled"):
            print(f"  Deblocked reference: {refs['enabled']['planes']}", file=sys.stderr)
        if refs.get("disabled"):
            print(f"  Raw reference: {refs['disabled']['planes']}", file=sys.stderr)

        manifest["rungs"].append(rung_entry)

    manifest_path = output_dir / "ladder_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"\nLadder manifest: {manifest_path}", file=sys.stderr)

    # Summary
    print("\n" + "=" * 60)
    print("P-SLICE DIFFICULTY LADDER SUMMARY")
    print("=" * 60)
    for entry in manifest["rungs"]:
        fr = entry["frames"]
        refs = entry.get("references", {})
        ref_status = "✓ both" if refs.get("enabled") and refs.get("disabled") else "partial"
        print(f"  Rung {entry['rung']}: {entry['name']}")
        print(f"    {entry['description']}")
        print(f"    {fr['frame_count']} frames (I={fr['i_frames']} P={fr['p_frames']}), "
              f"{entry['bytes']} bytes, refs={ref_status}")

    print("\nIMPORTANT: For inter scoring, use --reference-h264-loop-filter enabled")
    print("and the deblocked reference planes. MC predicts from the DEBLOCKED picture.")
    print("-skip_loop_filter (correct for intra) is ACTIVELY WRONG for inter.")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
