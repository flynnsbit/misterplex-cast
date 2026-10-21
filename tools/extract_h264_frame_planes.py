#!/usr/bin/env python3
"""Generate and verify per-frame I420 golden planes for H.264 Annex-B fixtures."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


FORMAT = "misterplex.p3.frame_planes_golden.v1"
SEQUENCE_FORMAT = "misterplex.p3.nal_sequence.v1"
NATIVE_I420 = "I420_NATIVE"
LOOP_FILTER_DISABLED = "disabled"
LOOP_FILTER_ENABLED = "enabled"
LOOP_FILTER_STATES = (LOOP_FILTER_DISABLED, LOOP_FILTER_ENABLED)


class ProvenanceRefusal(Exception):
    pass


def refuse(message: str) -> None:
    raise ProvenanceRefusal(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        obj = json.load(f)
    if not isinstance(obj, dict):
        raise SystemExit(f"{path}: top-level JSON is not an object")
    return obj


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(Path.cwd().resolve()).as_posix()
    except ValueError:
        return path.name


def tool_path(name: str) -> str:
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"{name} not found")
    return found


def version_line(tool: str) -> str:
    out = subprocess.check_output([tool, "-version"], text=True, stderr=subprocess.STDOUT)
    return out.splitlines()[0]


def ffprobe_geometry(ffprobe: str, bitstream: Path) -> tuple[int, int, int]:
    out = subprocess.check_output(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,nb_read_frames",
            "-count_frames",
            "-of",
            "csv=p=0",
            str(bitstream),
        ],
        text=True,
    ).strip()
    parts = out.split(",")
    if len(parts) != 3 or parts[2] == "N/A":
        raise SystemExit(f"ffprobe did not report width,height,frame_count for {bitstream}: {out!r}")
    return int(parts[0]), int(parts[1]), int(parts[2])


def frame_meta(sequence: dict[str, Any]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for n in sequence.get("nals", []):
        if isinstance(n, dict) and "vcl_index" in n:
            out.append(
                {
                    "frame_index": int(n["vcl_index"]),
                    "frame_num": int(n.get("frame_num", -1)),
                    "slice_kind": str(n.get("slice_kind", "unknown")),
                    "nal_index": int(n.get("index", -1)),
                    "nal_type": int(n.get("nal_type", -1)),
                }
            )
    out.sort(key=lambda x: x["frame_index"])
    return out


def require_sequence_matches(
    sequence: dict[str, Any], sequence_path: Path, bitstream: Path, source_sha: str, source_bytes: int
) -> tuple[str, int, int, int]:
    if sequence.get("format") != SEQUENCE_FORMAT:
        raise SystemExit(f"{sequence_path}: format is not {SEQUENCE_FORMAT}")
    src = sequence.get("source", {})
    if src.get("sha256") != source_sha or int(src.get("bytes", -1)) != source_bytes:
        raise SystemExit(f"{sequence_path}: source hash/size does not match {bitstream}")
    seq = sequence.get("sequence", {})
    vcl = int(seq.get("vcl", -1))
    if vcl < 2 or int(seq.get("idr", 0)) < 1 or int(seq.get("p_slices", 0)) < 1:
        raise SystemExit(f"{sequence_path}: need multi-frame IDR+P sequence, got {seq}")
    frame = sequence.get("frame", {})
    coded_w = int(frame.get("coded_width", 0))
    coded_h = int(frame.get("coded_height", 0))
    return sha256_file(sequence_path), coded_w, coded_h, vcl


def decode_i420(ffmpeg: str, bitstream: Path, planes_out: Path, h264_loop_filter: str) -> list[str]:
    planes_out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        ffmpeg,
        "-v",
        "error",
        "-y",
    ]
    if h264_loop_filter == LOOP_FILTER_DISABLED:
        cmd += ["-skip_loop_filter", "all"]
    elif h264_loop_filter != LOOP_FILTER_ENABLED:
        refuse(f"unknown H.264 loop-filter state {h264_loop_filter!r}")
    cmd += [
        "-i",
        str(bitstream),
        "-an",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p",
        str(planes_out),
    ]
    subprocess.check_call(cmd)
    out = ["ffmpeg", "-v", "error", "-y"]
    if h264_loop_filter == LOOP_FILTER_DISABLED:
        out += ["-skip_loop_filter", "all"]
    out += ["-i", rel(bitstream), "-an", "-f", "rawvideo", "-pix_fmt", "yuv420p", rel(planes_out)]
    return out


def build_manifest(
    bitstream: Path,
    sequence_path: Path,
    planes_path: Path,
    source_sha: str,
    source_bytes: int,
    sequence_sha: str,
    sequence: dict[str, Any],
    ffmpeg: str,
    ffprobe: str,
    ffmpeg_cmd: list[str],
    width: int,
    height: int,
    frames: int,
    h264_loop_filter: str,
) -> dict[str, Any]:
    if width % 2 or height % 2:
        raise SystemExit(f"I420 requires even geometry, got {width}x{height}")
    y_bytes = width * height
    uv_w = width // 2
    uv_h = height // 2
    uv_bytes = uv_w * uv_h
    frame_bytes = y_bytes + uv_bytes * 2
    blob = planes_path.read_bytes()
    expected = frame_bytes * frames
    if len(blob) != expected:
        raise SystemExit(f"{planes_path}: byte count {len(blob)} != expected {expected}")

    seq_frame = sequence.get("frame", {})
    meta = {
        "format": FORMAT,
        "source": {"path": rel(bitstream), "bytes": source_bytes, "sha256": source_sha},
        "sequence_manifest": {
            "path": rel(sequence_path),
            "bytes": sequence_path.stat().st_size,
            "sha256": sequence_sha,
            "format": SEQUENCE_FORMAT,
        },
        "decoder": {
            "ffmpeg_version": version_line(ffmpeg),
            "ffprobe_version": version_line(ffprobe),
            "command": " ".join(ffmpeg_cmd),
            "command_argv": ffmpeg_cmd,
            "input_format": "Annex-B H.264",
            "output_format": "rawvideo",
            "pix_fmt": "yuv420p",
            "h264_loop_filter": h264_loop_filter,
            "h264_loop_filter_ffmpeg": "-skip_loop_filter all"
            if h264_loop_filter == LOOP_FILTER_DISABLED
            else "ffmpeg default",
            "loop_filter": "skip_loop_filter=all"
            if h264_loop_filter == LOOP_FILTER_DISABLED
            else "ffmpeg default",
        },
        "provenance": {
            "source_domain": "decoded H.264 planes",
            "pixel_format": "I420/YUV420p planar 8-bit 4:2:0",
            "colorspace": NATIVE_I420,
            "h264_loop_filter": h264_loop_filter,
            "rgb_roundtrip": False,
            "rgb565_roundtrip": False,
            "presentation_border_or_pillar_mask": False,
        },
        "geometry": {
            "coded_width": width,
            "coded_height": height,
            "display_width": int(seq_frame.get("display_width", width)),
            "display_height": int(seq_frame.get("display_height", height)),
            "colorspace": NATIVE_I420,
            "planes": [
                {"plane": "Y", "width": width, "height": height, "stride": width},
                {"plane": "U", "width": uv_w, "height": uv_h, "stride": uv_w},
                {"plane": "V", "width": uv_w, "height": uv_h, "stride": uv_w},
            ],
        },
        "plane_blob": {
            "path": rel(planes_path),
            "bytes": len(blob),
            "sha256": sha256_bytes(blob),
            "layout": "I420 planar per frame: Y then U then V",
            "frame_bytes": frame_bytes,
        },
        "frames": [],
    }

    frames_meta = frame_meta(sequence)
    if len(frames_meta) != frames:
        raise SystemExit(f"{sequence_path}: VCL metadata count {len(frames_meta)} != decoded frames {frames}")
    for f in range(frames):
        base = f * frame_bytes
        planes = [
            ("Y", width, height, width, base, y_bytes),
            ("U", uv_w, uv_h, uv_w, base + y_bytes, uv_bytes),
            ("V", uv_w, uv_h, uv_w, base + y_bytes + uv_bytes, uv_bytes),
        ]
        frame_obj = {
            "frame_index": f,
            "frame_num": frames_meta[f]["frame_num"],
            "slice_kind": frames_meta[f]["slice_kind"],
            "nal_index": frames_meta[f]["nal_index"],
            "nal_type": frames_meta[f]["nal_type"],
            "planes": [],
        }
        for plane, pw, ph, stride, off, count in planes:
            frame_obj["planes"].append(
                {
                    "plane": plane,
                    "width": pw,
                    "height": ph,
                    "stride": stride,
                    "offset": off,
                    "bytes": count,
                    "sha256": sha256_bytes(blob[off : off + count]),
                }
            )
        meta["frames"].append(frame_obj)
    return meta


def validate_manifest(
    manifest: dict[str, Any],
    bitstream: Path,
    sequence_path: Path,
    planes_path: Path,
    expected_h264_loop_filter: str | None,
) -> None:
    if manifest.get("format") != FORMAT:
        raise SystemExit(f"manifest format is not {FORMAT}")
    src = manifest.get("source", {})
    blob = planes_path.read_bytes()
    if src.get("sha256") != sha256_file(bitstream) or int(src.get("bytes", -1)) != bitstream.stat().st_size:
        raise SystemExit("frame-plane golden source provenance does not match bitstream")
    seq = manifest.get("sequence_manifest", {})
    if seq.get("sha256") != sha256_file(sequence_path) or int(seq.get("bytes", -1)) != sequence_path.stat().st_size:
        raise SystemExit("frame-plane golden sequence provenance does not match manifest")
    plane_blob = manifest.get("plane_blob", {})
    if plane_blob.get("sha256") != sha256_bytes(blob) or int(plane_blob.get("bytes", -1)) != len(blob):
        raise SystemExit("frame-plane golden blob hash/size mismatch")
    decoder = manifest.get("decoder", {})
    if decoder.get("pix_fmt") != "yuv420p":
        refuse("frame-plane golden decoder pix_fmt is not yuv420p")
    if not expected_h264_loop_filter:
        refuse("expected H.264 loop-filter state is undeclared; pass --expected-h264-loop-filter")
    if expected_h264_loop_filter not in LOOP_FILTER_STATES:
        refuse(f"unknown expected H.264 loop-filter state {expected_h264_loop_filter!r}")
    decoder_loop_filter = decoder.get("h264_loop_filter")
    if decoder_loop_filter not in LOOP_FILTER_STATES:
        refuse("frame-plane golden decoder does not declare H.264 loop-filter state")
    if decoder_loop_filter != expected_h264_loop_filter:
        refuse(
            "frame-plane golden H.264 loop-filter mismatch: "
            f"golden={decoder_loop_filter} expected={expected_h264_loop_filter}"
        )
    decoder_loop_filter_alias = decoder.get("loop_filter")
    if decoder_loop_filter == LOOP_FILTER_DISABLED and decoder_loop_filter_alias != "skip_loop_filter=all":
        refuse("frame-plane golden decoder loop_filter is not skip_loop_filter=all")
    if decoder_loop_filter == LOOP_FILTER_ENABLED and decoder_loop_filter_alias == "skip_loop_filter=all":
        refuse("frame-plane golden decoder loop_filter disables H.264 deblock")
    command_argv = decoder.get("command_argv")
    if not isinstance(command_argv, list) or not all(isinstance(v, str) for v in command_argv):
        refuse("frame-plane golden decoder command_argv is missing or invalid")
    has_skip_all = any(
        command_argv[i] == "-skip_loop_filter" and i + 1 < len(command_argv) and command_argv[i + 1] == "all"
        for i in range(len(command_argv))
    )
    if decoder_loop_filter == LOOP_FILTER_DISABLED and not has_skip_all:
        refuse(
            "frame-plane golden declares H.264 loop filter disabled but decoder command "
            "does not include '-skip_loop_filter all'"
        )
    if decoder_loop_filter == LOOP_FILTER_ENABLED and has_skip_all:
        refuse(
            "frame-plane golden declares H.264 loop filter enabled but decoder command "
            "includes '-skip_loop_filter all'"
        )
    provenance = manifest.get("provenance", {})
    if provenance.get("colorspace") != NATIVE_I420 or provenance.get("pixel_format") != "I420/YUV420p planar 8-bit 4:2:0":
        refuse("frame-plane golden provenance does not declare native I420/YUV420p")
    if provenance.get("h264_loop_filter") != decoder_loop_filter:
        refuse("frame-plane golden provenance does not match decoder H.264 loop-filter state")
    if provenance.get("rgb_roundtrip") is not False or provenance.get("rgb565_roundtrip") is not False:
        refuse("frame-plane golden provenance allows an RGB/RGB565 round-trip")
    if provenance.get("presentation_border_or_pillar_mask") is not False:
        refuse("frame-plane golden provenance allows presentation masking")
    geom = manifest.get("geometry", {})
    if geom.get("colorspace") != NATIVE_I420:
        refuse("frame-plane golden colorspace is unknown or not I420_NATIVE")
    if plane_blob.get("layout") != "I420 planar per frame: Y then U then V":
        refuse("frame-plane golden plane layout is unknown or not I420")

    frame_bytes = int(plane_blob.get("frame_bytes", 0))
    frames = manifest.get("frames", [])
    if frame_bytes <= 0 or not isinstance(frames, list) or len(frames) < 2:
        raise SystemExit("frame-plane golden must contain >=2 frames")
    if len(blob) != frame_bytes * len(frames):
        raise SystemExit("frame-plane golden blob size does not match frame count")
    for f, frame in enumerate(frames):
        if int(frame.get("frame_index", -1)) != f:
            raise SystemExit("frame indices are not contiguous")
        for p in frame.get("planes", []):
            off = int(p.get("offset", -1))
            count = int(p.get("bytes", -1))
            if off < 0 or count <= 0 or off + count > len(blob):
                raise SystemExit("frame-plane golden has invalid plane range")
            if p.get("sha256") != sha256_bytes(blob[off : off + count]):
                raise SystemExit(
                    f"frame-plane golden plane hash mismatch frame={f} plane={p.get('plane', '?')}"
                )


def compare_candidate(
    manifest: dict[str, Any], golden_path: Path, candidate_path: Path, candidate_colorspace: str | None
) -> bool:
    golden_colorspace = manifest.get("geometry", {}).get("colorspace")
    if not candidate_colorspace:
        refuse("candidate colorspace is unknown; refusing plane comparison")
    if candidate_colorspace != golden_colorspace:
        refuse(
            f"candidate colorspace mismatch: candidate={candidate_colorspace} golden={golden_colorspace}"
        )
    golden = golden_path.read_bytes()
    candidate = candidate_path.read_bytes()
    if len(candidate) != len(golden):
        raise SystemExit(f"candidate plane blob size {len(candidate)} != golden size {len(golden)}")
    exact_all = True
    for frame in manifest["frames"]:
        frame_index = int(frame["frame_index"])
        for plane in frame["planes"]:
            name = str(plane["plane"])
            off = int(plane["offset"])
            count = int(plane["bytes"])
            exact = 0
            sum_abs = 0
            max_abs = 0
            for i in range(count):
                d = abs(candidate[off + i] - golden[off + i])
                exact += d == 0
                sum_abs += d
                max_abs = max(max_abs, d)
            mae = sum_abs / count
            print(
                f"FRAME_PLANE_COMPARE raw frame={frame_index} plane={name} "
                f"exact={exact} pixels={count} mae={mae:.6f} max_abs={max_abs}"
            )
            if exact != count:
                exact_all = False
    print(
        f"FRAME_PLANE_COMPARE summary frames={len(manifest['frames'])} "
        f"bytes={len(golden)} strict_pass={1 if exact_all else 0}"
    )
    return exact_all


def generate(args: argparse.Namespace) -> int:
    bitstream = Path(args.input)
    sequence_path = Path(args.sequence)
    planes_out = Path(args.planes_out)
    manifest_out = Path(args.manifest_out)
    source_sha = sha256_file(bitstream)
    source_bytes = bitstream.stat().st_size
    sequence = read_json(sequence_path)
    sequence_sha, coded_w, coded_h, vcl = require_sequence_matches(
        sequence, sequence_path, bitstream, source_sha, source_bytes
    )
    ffmpeg = tool_path("ffmpeg")
    ffprobe = tool_path("ffprobe")
    width, height, frames = ffprobe_geometry(ffprobe, bitstream)
    if (width, height) != (coded_w, coded_h):
        raise SystemExit(f"ffprobe geometry {width}x{height} != sequence coded geometry {coded_w}x{coded_h}")
    if frames != vcl:
        raise SystemExit(f"ffprobe frames {frames} != sequence VCL count {vcl}")
    if args.expect_width and width != args.expect_width:
        raise SystemExit(f"decoded width {width} != expected {args.expect_width}")
    if args.expect_height and height != args.expect_height:
        raise SystemExit(f"decoded height {height} != expected {args.expect_height}")
    if not args.h264_loop_filter:
        refuse("H.264 loop-filter state is required for generation; pass --h264-loop-filter")
    cmd = decode_i420(ffmpeg, bitstream, planes_out, args.h264_loop_filter)
    manifest = build_manifest(
        bitstream,
        sequence_path,
        planes_out,
        source_sha,
        source_bytes,
        sequence_sha,
        sequence,
        ffmpeg,
        ffprobe,
        cmd,
        width,
        height,
        frames,
        args.h264_loop_filter,
    )
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(manifest, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print(
        f"extract_h264_frame_planes: OK input={bitstream} frames={frames} "
        f"geometry={width}x{height} planes_sha256={manifest['plane_blob']['sha256']}"
    )
    return 0


def verify(args: argparse.Namespace) -> int:
    manifest = read_json(Path(args.manifest))
    validate_manifest(
        manifest,
        Path(args.input),
        Path(args.sequence),
        Path(args.planes),
        args.expected_h264_loop_filter,
    )
    if args.candidate_planes:
        exact = compare_candidate(
            manifest, Path(args.planes), Path(args.candidate_planes), args.candidate_colorspace
        )
        if args.expect_red:
            if exact:
                raise SystemExit("candidate unexpectedly matched golden in expect-red mode")
            print("extract_h264_frame_planes: OK expected-red candidate diverged from golden")
            return 0
        if not exact:
            raise SystemExit("candidate plane comparison diverged from golden")
    geom = manifest["geometry"]
    print(
        f"extract_h264_frame_planes: OK verify frames={len(manifest['frames'])} "
        f"geometry={geom['coded_width']}x{geom['coded_height']} "
        f"planes_sha256={manifest['plane_blob']['sha256']}"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, help="Annex-B H.264 bitstream")
    ap.add_argument("--sequence", required=True, help="misterplex.p3.nal_sequence.v1 manifest")
    ap.add_argument("--planes-out", help="output I420 plane blob")
    ap.add_argument("--manifest-out", help="output frame-plane golden JSON")
    ap.add_argument("--h264-loop-filter", choices=LOOP_FILTER_STATES,
                    help="reference decoder in-loop deblock state for generated planes")
    ap.add_argument("--expected-h264-loop-filter", "--expect-h264-loop-filter",
                    dest="expected_h264_loop_filter", choices=LOOP_FILTER_STATES,
                    help="required expected loop-filter state when verifying/comparing")
    ap.add_argument("--expect-width", type=int, default=0)
    ap.add_argument("--expect-height", type=int, default=0)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--planes", help="existing I420 plane blob to verify")
    ap.add_argument("--manifest", help="existing frame-plane golden JSON to verify")
    ap.add_argument("--candidate-planes", help="optional I420 candidate blob to compare against the golden")
    ap.add_argument("--candidate-colorspace", help="required with --candidate-planes; must match manifest colorspace")
    ap.add_argument("--expect-red", action="store_true", help="candidate comparison must fail strict equality")
    args = ap.parse_args()
    try:
        if args.verify:
            if not args.planes or not args.manifest:
                raise SystemExit("--verify requires --planes and --manifest")
            return verify(args)
        if not args.planes_out or not args.manifest_out:
            raise SystemExit("generation requires --planes-out and --manifest-out")
        return generate(args)
    except ProvenanceRefusal as e:
        print(str(e), file=sys.stderr)
        return 9


if __name__ == "__main__":
    raise SystemExit(main())
