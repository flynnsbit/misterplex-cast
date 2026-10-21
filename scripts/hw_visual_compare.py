#!/usr/bin/env python3
"""MiSTerPlex hardware visual-regression capture comparator.

This tool is intentionally geometry-aware. It reads the 480p DDR frame geometry
from the shared host/RTL layout headers, compares only the displayed picture
region, and reports quantified pixel errors plus a side-by-side diff artifact.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
HOST_LAYOUT = ROOT / "host" / "libmisterplex" / "ddr_frame_layout.hpp"
RTL_LAYOUT = ROOT / "fpga" / "Plex_MiSTer" / "rtl" / "ddr_frame_layout_params.svh"
DEFAULT_DEV = "/dev/video4"
DEFAULT_FORMAT = "mjpeg"
DEFAULT_SIZE = "1280x720"
DEFAULT_FPS = "60"
DEFAULT_WARMUP = 60
DEFAULT_ATTEMPTS = 5
COLOR_MATRICES = ("bt601", "bt709")
COLOR_RANGES = ("full", "limited")
PIXEL_FORMATS = ("yuv420p", "i420", "rgb565")


class HarnessError(RuntimeError):
    exit_code = 2


class StaleCaptureError(HarnessError):
    exit_code = 3


class CorruptCaptureError(HarnessError):
    exit_code = 4


class CaptureAbsentError(HarnessError):
    exit_code = 5


class CaptureBusyError(HarnessError):
    exit_code = 6


class DeliveryFreshnessError(HarnessError):
    exit_code = 7


class RbfIdentityError(HarnessError):
    exit_code = 8


class GoldenProvenanceError(HarnessError):
    exit_code = 9


CORRUPT_LOG_PATTERNS = (
    "corrupt",
    "v4l2 buffer contains corrupted data",
    "bad buffer",
    "input/output error",
    "cannot dequeue buffer",
    "error submitting packet to decoder",
    "invalid data found when processing input",
)


def classify_capture_log(stderr: str) -> str | None:
    """Return a capture-integrity class for ffmpeg/v4l2 stderr, if any."""
    low = stderr.lower()
    for pat in CORRUPT_LOG_PATTERNS:
        if pat in low:
            return "corrupt"
    return None


def reject_corrupt_capture_log(path: str | None) -> None:
    """Reject externally captured frames when their ffmpeg/V4L2 log is corrupt."""
    if not path:
        return
    p = Path(path)
    log = p.read_text(encoding="utf-8", errors="replace")
    if classify_capture_log(log) == "corrupt":
        raise CorruptCaptureError(
            f"capture log {p} reports corrupted V4L2/FFmpeg data; not grading image"
        )


def require_color_provenance(args: argparse.Namespace, golden_provenance: dict) -> dict:
    golden_color = golden_provenance["color"]
    golden_matrix = args.golden_color_matrix or golden_color["matrix"]
    golden_range = args.golden_color_range or golden_color["range"]
    fields = (golden_matrix, golden_range, args.capture_color_matrix, args.capture_color_range)
    if any(v is None for v in fields):
        raise HarnessError(
            "colour matrix/range provenance is required; pass "
            "--capture-color-matrix and --capture-color-range; golden colour "
            "must be present in the golden provenance sidecar"
        )
    if (golden_matrix, golden_range) != (golden_color["matrix"], golden_color["range"]):
        raise GoldenProvenanceError(
            "GOLDEN_PROVENANCE: CLI golden colour does not match sidecar: "
            f"cli={golden_matrix}/{golden_range} "
            f"sidecar={golden_color['matrix']}/{golden_color['range']}"
        )
    if (golden_matrix, golden_range) != (args.capture_color_matrix, args.capture_color_range):
        raise GoldenProvenanceError(
            "GOLDEN_PROVENANCE: refusing to compare images with different colour provenance: "
            f"golden={golden_matrix}/{golden_range} "
            f"capture={args.capture_color_matrix}/{args.capture_color_range}"
        )
    return {
        "golden": {
            "matrix": golden_matrix,
            "range": golden_range,
        },
        "capture": {
            "matrix": args.capture_color_matrix,
            "range": args.capture_color_range,
        },
    }


STATUS_FIELD_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
MD5_RE = re.compile(r"\b([0-9a-fA-F]{32})\b")
FORMAT_CODES = {
    "1": 1,
    "yuv420p": 1,
    "i420": 1,
}
FORMAT_ERROR_DEBUG_FIELDS = ("frame_debug", "frame_dbg", "ddr_debug", "ddr_dbg", "debug_state")
SURFACED_DEBUG_FIELDS = FORMAT_ERROR_DEBUG_FIELDS + ("recon_dbg",)
NON_YUV_DOORBELL_ERROR = "frame store refused non-YUV doorbell (0xE1); non-YUV DDR doorbell format error"
PLXF_ABSENT_ERROR = "frame store status unavailable (PLXF mailbox absent/unwritten)"
STATUS_SNAPSHOT_KEYS = {
    "bytes_in", "nalu", "has_frame", "has_stream", "has_idr", "frame_status",
    "frame_bank", "ddr_bank", "bank", "frame_format", "ddr_format", "format",
    "frame_seq", "ddr_seq", "doorbell_seq", "seq", "doorbell", "ddr_doorbell",
    "frame_doorbell", "doorbell_hi", "ddr_doorbell_hi", "frame_doorbell_hi",
    *SURFACED_DEBUG_FIELDS,
    "plxf_magic", "frame_magic", "frame_store_magic",
}


def normalize_md5(value: str | None) -> str | None:
    if value is None:
        return None
    m = MD5_RE.search(value)
    return m.group(1).lower() if m else None


def validate_rbf_identity(args: argparse.Namespace) -> dict | None:
    if not args.expected_rbf_md5 and not args.actual_rbf_md5 and not args.rbf_md5_log:
        return None
    expected = normalize_md5(args.expected_rbf_md5)
    actual_src = args.actual_rbf_md5
    log_path = None
    if args.rbf_md5_log:
        log_path = str(args.rbf_md5_log)
        actual_src = Path(args.rbf_md5_log).read_text(encoding="utf-8", errors="replace")
    actual = normalize_md5(actual_src)
    if expected is None:
        raise RbfIdentityError(
            "RBF_IDENTITY: expected RBF md5 was not declared; refusing to grade loaded core"
        )
    if actual is None:
        raise RbfIdentityError(
            "RBF_IDENTITY: loaded /media/fat/_Utility/Plex.rbf md5 is missing/unparseable; "
            "refusing to grade"
        )
    report = {
        "expected_md5": expected,
        "actual_md5": actual,
        "rbf_md5_log": log_path,
        "match": actual == expected,
    }
    if actual != expected:
        raise RbfIdentityError(
            f"RBF_IDENTITY: loaded core md5 {actual} != expected {expected}; not grading pixels"
        )
    return report


def parse_size_spec(spec: str | None) -> tuple[int, int] | None:
    if spec is None:
        return None
    m = re.fullmatch(r"\s*(\d+)x(\d+)\s*", spec)
    if not m:
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: invalid size {spec!r}; expected WIDTHxHEIGHT"
        )
    return int(m.group(1)), int(m.group(2))


def _require_dict(data: dict, key: str, where: Path) -> dict:
    value = data.get(key)
    if not isinstance(value, dict):
        raise GoldenProvenanceError(f"GOLDEN_PROVENANCE: {where} missing object {key!r}")
    return value


def _require_int(data: dict, key: str, where: Path) -> int:
    value = data.get(key)
    if not isinstance(value, int):
        raise GoldenProvenanceError(f"GOLDEN_PROVENANCE: {where} missing integer {key!r}")
    return value


def _require_str(data: dict, key: str, where: Path) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise GoldenProvenanceError(f"GOLDEN_PROVENANCE: {where} missing string {key!r}")
    return value


def load_golden_provenance(args: argparse.Namespace) -> dict:
    path = Path(args.golden_provenance) if args.golden_provenance else (
        Path(args.golden).with_suffix(Path(args.golden).suffix + ".provenance.json")
    )
    if not path.exists():
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: missing {path}; every hardware golden must declare "
            "source_rbf_md5, geometry, pixel_format, and colour matrix/range"
        )
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise GoldenProvenanceError(f"GOLDEN_PROVENANCE: {path} is invalid JSON: {e}") from e

    if data.get("schema") != "misterplex.hw_visual_golden.v1":
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: {path} has unsupported schema {data.get('schema')!r}"
        )
    source_type = _require_str(data, "source_type", path)
    if source_type != "hardware_capture":
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: {path} is {source_type!r}, not a hardware-captured golden"
        )
    source_md5 = normalize_md5(data.get("source_rbf_md5"))
    if source_md5 is None:
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: {path} missing valid source_rbf_md5"
        )
    pixel_format = _require_str(data, "pixel_format", path).lower()
    if pixel_format == "i420":
        pixel_format = "yuv420p"
    if pixel_format not in PIXEL_FORMATS:
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: {path} unsupported pixel_format {pixel_format!r}"
        )
    color = _require_dict(data, "color", path)
    matrix = _require_str(color, "matrix", path).lower()
    color_range = _require_str(color, "range", path).lower()
    if matrix not in COLOR_MATRICES or color_range not in COLOR_RANGES:
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: {path} unsupported colour {matrix}/{color_range}"
        )
    geometry = _require_dict(data, "geometry", path)
    compare_box = geometry.get("compare_box")
    if (
        not isinstance(compare_box, list)
        or len(compare_box) != 4
        or not all(isinstance(v, int) for v in compare_box)
    ):
        raise GoldenProvenanceError(
            f"GOLDEN_PROVENANCE: {path} geometry.compare_box must be [x,y,w,h]"
        )
    normalized = {
        "path": str(path),
        "schema": data["schema"],
        "source_type": source_type,
        "source_rbf_md5": source_md5,
        "source_rbf_label": data.get("source_rbf_label"),
        "pixel_format": pixel_format,
        "color": {"matrix": matrix, "range": color_range},
        "geometry": {
            "content_width": _require_int(geometry, "content_width", path),
            "content_height": _require_int(geometry, "content_height", path),
            "presented_width": _require_int(geometry, "presented_width", path),
            "presented_height": _require_int(geometry, "presented_height", path),
            "compare_box": compare_box,
        },
    }
    return normalized


def validate_golden_provenance(
    args: argparse.Namespace,
    g: "Geometry",
    box: tuple[int, int, int, int],
    rbf_identity: dict | None,
) -> dict:
    prov = load_golden_provenance(args)
    geom = prov["geometry"]
    problems: list[str] = []

    if (geom["presented_width"], geom["presented_height"]) != (g.presented_width, g.presented_height):
        problems.append(
            "presented geometry "
            f"{geom['presented_width']}x{geom['presented_height']} != "
            f"loaded layout {g.presented_width}x{g.presented_height}"
        )
    expected_size = parse_size_spec(args.expected_content_size)
    if expected_size is None:
        problems.append("expected content geometry is undeclared; pass --expected-content-size WIDTHxHEIGHT")
    elif (geom["content_width"], geom["content_height"]) != expected_size:
        problems.append(
            "content geometry "
            f"{geom['content_width']}x{geom['content_height']} != "
            f"expected {expected_size[0]}x{expected_size[1]}"
        )
    expected_format = (args.expected_pixel_format or "").lower()
    if expected_format == "i420":
        expected_format = "yuv420p"
    if not expected_format:
        problems.append("expected pixel format is undeclared; pass --expected-pixel-format")
    elif expected_format != prov["pixel_format"]:
        problems.append(f"pixel format {prov['pixel_format']} != expected {expected_format}")

    x0, y0, x1, y1 = box
    selected_box = [x0, y0, x1 - x0, y1 - y0]
    if geom["compare_box"] != selected_box:
        problems.append(f"compare box {geom['compare_box']} != selected {selected_box}")

    if problems:
        raise GoldenProvenanceError(
            "GOLDEN_PROVENANCE: " + "; ".join(problems) + "; not grading pixels"
        )

    if rbf_identity is None or not rbf_identity.get("actual_md5"):
        raise RbfIdentityError(
            "RBF_IDENTITY: loaded core md5 was not supplied; golden was produced by "
            f"{prov['source_rbf_md5']}; not grading pixels"
        )
    actual = rbf_identity["actual_md5"]
    if prov["source_rbf_md5"] != actual:
        raise RbfIdentityError(
            "GOLDEN_PROVENANCE: golden was produced by core "
            f"{prov['source_rbf_md5']} but loaded core is {actual}; not grading pixels"
        )
    expected = rbf_identity.get("expected_md5")
    if expected and prov["source_rbf_md5"] != expected:
        raise RbfIdentityError(
            "GOLDEN_PROVENANCE: golden was produced by core "
            f"{prov['source_rbf_md5']} but artifact under test is {expected}; not grading pixels"
        )
    return prov


def parse_int_value(value: str) -> int | None:
    """Parse decimal/hex integer status values; return None for strings."""
    v = value.strip().rstrip(",")
    if re.fullmatch(r"-?\d+", v):
        return int(v, 10)
    if re.fullmatch(r"0x[0-9a-fA-F]+", v):
        return int(v, 16)
    return None


def parse_status_fields(text: str) -> dict[str, str]:
    """Return the last push_frame-style key=value status snapshot in text."""
    fields: dict[str, str] = {}
    for line in text.splitlines():
        kv = dict(STATUS_FIELD_RE.findall(line))
        if kv and (line.strip().startswith("status") or STATUS_SNAPSHOT_KEYS.intersection(kv)):
            fields = kv
    return fields


def decode_doorbell_hi(hi: int) -> dict:
    return {
        "bank": (hi >> 31) & 0x1,
        "format": (hi >> 29) & 0x3,
        "seq": hi & 0x1FFFFFFF,
    }


def status_int(fields: dict[str, str], *names: str) -> int | None:
    for name in names:
        if name in fields:
            parsed = parse_int_value(fields[name])
            if parsed is not None:
                return parsed
    return None


def status_format(fields: dict[str, str], *names: str) -> int | None:
    for name in names:
        if name in fields:
            raw = fields[name].strip().lower().rstrip(",")
            if raw in FORMAT_CODES:
                return FORMAT_CODES[raw]
            parsed = parse_int_value(raw)
            if parsed is not None:
                return parsed
    return None


def extract_frame_token(fields: dict[str, str]) -> dict | None:
    """Extract the shared {bank, format, seq} DDR token if status exposes it."""
    doorbell_hi = status_int(fields, "doorbell_hi", "ddr_doorbell_hi", "frame_doorbell_hi")
    if doorbell_hi is None:
        doorbell = status_int(fields, "doorbell", "ddr_doorbell", "frame_doorbell")
        if doorbell is not None:
            doorbell_hi = (doorbell >> 32) if doorbell > 0xFFFFFFFF else doorbell
    if doorbell_hi is not None:
        return decode_doorbell_hi(doorbell_hi)

    bank = status_int(fields, "frame_bank", "ddr_bank", "bank")
    fmt = status_format(fields, "frame_format", "ddr_format", "format")
    seq = status_int(fields, "frame_seq", "ddr_seq", "doorbell_seq", "seq")
    if bank is None or fmt is None or seq is None:
        return None
    return {"bank": bank & 1, "format": fmt & 0x3, "seq": seq & 0x1FFFFFFF}


def load_status_snapshot(path: str) -> dict:
    p = Path(path)
    text = p.read_text(encoding="utf-8", errors="replace")
    fields = parse_status_fields(text)
    status_errors = []
    if PLXF_ABSENT_ERROR.lower() in text.lower():
        status_errors.append(PLXF_ABSENT_ERROR)
    if not fields and not status_errors:
        raise DeliveryFreshnessError(f"NO_FRESH_FRAME: no key=value status line in {p}")
    ints = {k: v for k, raw in fields.items() if (v := parse_int_value(raw)) is not None}
    frame_status = fields.get("frame_status")
    if frame_status and frame_status.strip().lower().rstrip(",") in {
        "absent", "unavailable", "missing", "none", "no_frame",
    }:
        status_errors.append(f"frame_status={frame_status} ({PLXF_ABSENT_ERROR})")
    has_frame = ints.get("has_frame")
    if has_frame == 0:
        status_errors.append(f"has_frame=0 ({PLXF_ABSENT_ERROR})")
    plxf_magic = status_int(fields, "plxf_magic", "frame_magic", "frame_store_magic")
    if plxf_magic == 0:
        status_errors.append(f"PLXF magic=0x00000000 ({PLXF_ABSENT_ERROR})")
    debug_flags = []
    for name in SURFACED_DEBUG_FIELDS:
        val = ints.get(name)
        if val is not None:
            entry = {"field": name, "value": val, "hex": f"0x{val & 0xFF:02x}"}
            if name in FORMAT_ERROR_DEBUG_FIELDS and (val & 0xFF) == 0xE1:
                entry["meaning"] = NON_YUV_DOORBELL_ERROR
            debug_flags.append(entry)
    return {
        "path": str(p),
        "fields": fields,
        "ints": ints,
        "frame_token": extract_frame_token(fields),
        "debug_flags": debug_flags,
        "status_errors": status_errors,
    }


def require_field_match(snapshot: dict, spec: str) -> str | None:
    if "=" not in spec:
        raise HarnessError(f"invalid --require-status-field {spec!r}; expected key=value")
    key, want_raw = spec.split("=", 1)
    fields = snapshot["fields"]
    if key not in fields:
        return f"{key} missing"
    have_raw = fields[key]
    have_i = parse_int_value(have_raw)
    want_i = parse_int_value(want_raw)
    if have_i is not None and want_i is not None:
        if have_i != want_i:
            return f"{key}={have_i} expected {want_i}"
    elif have_raw.strip().lower() != want_raw.strip().lower():
        return f"{key}={have_raw!r} expected {want_raw!r}"
    return None


def validate_delivery_freshness(args: argparse.Namespace) -> dict | None:
    """Reject captures that cannot be attributed to a fresh frame delivery."""
    if not args.status_log and not args.previous_status_log and args.min_bytes_in is None:
        return None
    if not args.status_log:
        raise DeliveryFreshnessError("NO_FRESH_FRAME: --status-log is required for delivery gating")

    current = load_status_snapshot(args.status_log)
    problems: list[str] = []
    if args.min_bytes_in is not None:
        bytes_in = current["ints"].get("bytes_in")
        if bytes_in is None:
            stream_nalus = current["ints"].get("stream_nalus")
            if stream_nalus is not None:
                problems.append(
                    "STATUS_TELEMETRY_LAYER: "
                    f"status exposes stream_nalus={stream_nalus}, not a byte-delivery counter; "
                    "--min-bytes-in cannot prove freshness on this ABI"
                )
            elif current["ints"].get("bytes_in_unavailable") == 1:
                problems.append(
                    "STATUS_TELEMETRY_LAYER: byte-delivery counter is not exposed by this "
                    "status ABI; use DDR frame-token freshness or raw capture provenance"
                )
            else:
                problems.append("bytes_in missing")
        elif bytes_in < args.min_bytes_in:
            nalu = current["ints"].get("nalu")
            if nalu is not None and bytes_in == nalu:
                problems.append(
                    "STATUS_TELEMETRY_LAYER: "
                    f"bytes_in={bytes_in} equals nalu={nalu}, so this status line is exposing "
                    "the post-P3 telemetry alias (NAL count), not a byte-delivery counter; "
                    f"below minimum {args.min_bytes_in}"
                )
            else:
                problems.append(f"bytes_in={bytes_in} below minimum {args.min_bytes_in}")

    for spec in args.require_status_field:
        problem = require_field_match(current, spec)
        if problem:
            problems.append(problem)

    for flag in current["debug_flags"]:
        if flag.get("meaning") == NON_YUV_DOORBELL_ERROR:
            problems.append(f"{flag['field']}={flag['hex']} {NON_YUV_DOORBELL_ERROR}")
    problems.extend(current["status_errors"])

    report = {
        "status": current,
        "min_bytes_in": args.min_bytes_in,
        "required_fields": list(args.require_status_field),
        "previous_status": None,
        "token_changed": None,
        "require_token_change": bool(args.require_token_change),
    }
    if args.previous_status_log:
        try:
            previous = load_status_snapshot(args.previous_status_log)
        except DeliveryFreshnessError as e:
            if args.require_token_change:
                problems.append(str(e))
            else:
                report["previous_status"] = {"path": args.previous_status_log, "error": str(e)}
        else:
            report["previous_status"] = previous
            cur_token = current["frame_token"]
            prev_token = previous["frame_token"]
            if cur_token is not None and prev_token is not None:
                token_changed = cur_token != prev_token
                report["token_changed"] = token_changed
                if args.require_token_change and not token_changed:
                    problems.append(f"frame token did not change ({cur_token})")
            elif args.require_token_change:
                problems.append("frame token missing; expected shared {bank,format,seq} token")

    if problems:
        raise DeliveryFreshnessError("NO_FRESH_FRAME: " + "; ".join(problems) + "; not grading pixels")
    return report


@dataclass(frozen=True)
class Geometry:
    coded_width: int
    coded_height: int
    display_width: int
    display_height: int
    presented_width: int
    presented_height: int
    crop_left: int
    crop_right: int
    crop_top: int
    crop_bottom: int
    pillarbox_left: int
    pillarbox_right: int

    @property
    def active_box(self) -> tuple[int, int, int, int]:
        x0 = self.pillarbox_left
        y0 = 0
        return (x0, y0, x0 + self.display_width, y0 + self.display_height)


def _parse_cpp_constants(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    out: dict[str, int] = {}
    for name, value in re.findall(r"constexpr\s+int\s+kPlex480p([A-Za-z0-9_]+)\s*=\s*([^;]+);", text):
        value = value.strip()
        if value.startswith("0x"):
            out[name] = int(value, 16)
        else:
            out[name] = int(value)
    return out


def _parse_svh_constants(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    out: dict[str, int] = {}
    for name, value in re.findall(r"localparam\s+int\s+DDR_FRAME_([A-Z0-9_]+)\s*=\s*([^;]+);", text):
        value = value.strip().replace("_", "")
        if "'h" in value:
            out[name] = int(value.split("'h", 1)[1], 16)
        elif value.startswith("32'h"):
            out[name] = int(value[4:], 16)
        else:
            out[name] = int(value, 0)
    return out


def load_geometry() -> Geometry:
    cpp = _parse_cpp_constants(HOST_LAYOUT)
    svh = _parse_svh_constants(RTL_LAYOUT)
    pairs = {
        "CodedWidth": "CODED_WIDTH",
        "CodedHeight": "CODED_HEIGHT",
        "DisplayWidth": "DISPLAY_WIDTH",
        "DisplayHeight": "DISPLAY_HEIGHT",
        "PresentedWidth": "PRESENTED_WIDTH",
        "PresentedHeight": "PRESENTED_HEIGHT",
        "CropLeft": "CROP_LEFT",
        "CropRight": "CROP_RIGHT",
        "CropTop": "CROP_TOP",
        "CropBottom": "CROP_BOTTOM",
        "PillarboxLeft": "PILLARBOX_LEFT",
        "PillarboxRight": "PILLARBOX_RIGHT",
    }
    for c_name, r_name in pairs.items():
        if c_name not in cpp:
            raise HarnessError(f"missing {c_name} in {HOST_LAYOUT}")
        if r_name not in svh:
            raise HarnessError(f"missing {r_name} in {RTL_LAYOUT}")
        if cpp[c_name] != svh[r_name]:
            raise HarnessError(
                f"layout divergence {c_name}/{r_name}: host={cpp[c_name]} rtl={svh[r_name]}"
            )
    g = Geometry(
        coded_width=cpp["CodedWidth"],
        coded_height=cpp["CodedHeight"],
        display_width=cpp["DisplayWidth"],
        display_height=cpp["DisplayHeight"],
        presented_width=cpp["PresentedWidth"],
        presented_height=cpp["PresentedHeight"],
        crop_left=cpp["CropLeft"],
        crop_right=cpp["CropRight"],
        crop_top=cpp["CropTop"],
        crop_bottom=cpp["CropBottom"],
        pillarbox_left=cpp["PillarboxLeft"],
        pillarbox_right=cpp["PillarboxRight"],
    )
    if g.display_width + g.crop_left + g.crop_right != g.coded_width:
        raise HarnessError(f"invalid crop geometry: {g}")
    if g.pillarbox_left + g.display_width + g.pillarbox_right != g.presented_width:
        raise HarnessError(f"invalid pillarbox geometry: {g}")
    return g


def load_rgb(path: Path, g: Geometry) -> np.ndarray:
    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:
        raise HarnessError(f"could not load image {path}: {e}") from e
    if im.size != (g.presented_width, g.presented_height):
        im = im.resize((g.presented_width, g.presented_height), Image.Resampling.BILINEAR)
    return np.array(im, dtype=np.uint8)


def save_rgb(path: Path, frame: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(frame, 0, 255).astype(np.uint8), "RGB").save(path)


def capture_v4l2_once(path: Path, dev: str, input_format: str, size: str, framerate: str,
                      warmup: int, color_matrix: str,
                      color_range: str) -> tuple[np.ndarray | None, str, int]:
    vf = (
        f"select=gte(n\\,{warmup}),"
        f"scale=in_color_matrix={color_matrix}:in_range={color_range}:out_range=full,"
        "format=rgb24"
    )
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning",
        "-f", "v4l2", "-input_format", input_format, "-video_size", size,
        "-framerate", framerate, "-i", dev,
        "-vf", vf, "-frames:v", "1", "-update", "1",
        "-y", str(path),
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30 + warmup)
    except FileNotFoundError as e:
        raise HarnessError("required dependency ffmpeg was not found; cannot capture HDMI") from e
    log = (r.stderr or r.stdout or "").strip()
    if r.returncode != 0 or not path.exists() or path.stat().st_size == 0:
        return None, log, r.returncode
    if classify_capture_log(log) == "corrupt":
        return None, log, 4
    sidecar = path.with_suffix(path.suffix + ".color.json")
    sidecar.write_text(json.dumps({
        "color_matrix": color_matrix,
        "color_range": color_range,
        "ffmpeg_filter": vf,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return np.array(Image.open(path).convert("RGB"), dtype=np.uint8), log, 0


def capture_v4l2(path: Path, dev: str, input_format: str, size: str, framerate: str,
                 warmup: int, attempts: int, color_matrix: str, color_range: str) -> np.ndarray:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not Path(dev).exists():
        raise CaptureAbsentError(f"capture device {dev} is absent")
    try:
        busy = subprocess.run(["fuser", dev], capture_output=True, text=True, timeout=5)
        holders = (busy.stdout + busy.stderr).strip()
        if busy.returncode == 0 and holders:
            raise CaptureBusyError(f"capture device {dev} is busy/exclusive-open ({holders})")
    except FileNotFoundError:
        pass
    corrupt_logs: list[str] = []
    other_logs: list[str] = []
    for attempt in range(1, attempts + 1):
        frame, log, rc = capture_v4l2_once(
            path, dev, input_format, size, framerate, warmup, color_matrix, color_range
        )
        if frame is not None:
            if attempt > 1:
                print(f"capture recovered after {attempt - 1} corrupt/failed attempt(s)",
                      file=sys.stderr)
            return frame
        if classify_capture_log(log) == "corrupt" or rc == 4:
            corrupt_logs.append(f"attempt {attempt}: {log}")
        else:
            other_logs.append(f"attempt {attempt}: rc={rc} {log}")
    if corrupt_logs:
        raise CorruptCaptureError(
            f"V4L2/ffmpeg reported corrupted capture data for {dev} "
            f"({input_format} {size}@{framerate}) on all usable attempts; "
            f"not grading. last log: {corrupt_logs[-1]}"
        )
    raise HarnessError(
        f"ffmpeg capture failed for {dev} ({input_format} {size}@{framerate}); "
        f"last log: {other_logs[-1] if other_logs else 'no attempts'}"
    )


def parse_compare_box(spec: str | None, g: Geometry) -> tuple[int, int, int, int]:
    if not spec or spec == "active":
        return g.active_box
    try:
        x, y, w, h = [int(p) for p in spec.split(",")]
    except ValueError as e:
        raise HarnessError(f"invalid compare box {spec!r}; expected x,y,w,h") from e
    x0, y0, x1, y1 = x, y, x + w, y + h
    if x0 < 0 or y0 < 0 or w <= 0 or h <= 0 or x1 > g.presented_width or y1 > g.presented_height:
        raise HarnessError(
            f"compare box {spec!r} outside presented frame {g.presented_width}x{g.presented_height}"
        )
    return x0, y0, x1, y1


def active_view(frame: np.ndarray, g: Geometry,
                box: tuple[int, int, int, int] | None = None) -> np.ndarray:
    x0, y0, x1, y1 = box if box else g.active_box
    return frame[y0:y1, x0:x1, :]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rgb_to_yuv601(rgb: np.ndarray) -> np.ndarray:
    r = rgb[..., 0].astype(np.float64)
    g = rgb[..., 1].astype(np.float64)
    b = rgb[..., 2].astype(np.float64)
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = -0.168736 * r - 0.331264 * g + 0.5 * b + 128.0
    v = 0.5 * r - 0.418688 * g - 0.081312 * b + 128.0
    return np.clip(np.rint(np.stack([y, u, v], axis=-1)), 0, 255).astype(np.int16)


def classify_error_signatures(stats: dict) -> list[dict]:
    """Attach interpretation hints after raw per-channel numbers are reported."""
    if stats["exact_match_pixels"] == stats["active_pixels"]:
        return [{"id": "exact_match", "confidence": "high"}]

    mae = [float(x) for x in stats["per_plane_mae_rgb"]]
    avg = sum(mae) / 3.0
    spread = max(mae) - min(mae)
    max_abs = int(stats["max_abs"])
    exact_ratio = float(stats["exact_match_ratio"])
    bbox = stats.get("mismatch_bbox") or {}
    display_bbox = bbox.get("display")
    sigs: list[dict] = []

    if avg >= 5.0 and spread <= max(5.0, avg * 0.08):
        sigs.append({
            "id": "balanced_rgb_error",
            "confidence": "medium",
            "note": "RGB MAE is nearly uniform; this is not the usual colour-matrix or U/V-swap shape",
        })
    if exact_ratio == 0.0 and avg >= 64.0 and max_abs >= 250 and spread <= max(8.0, avg * 0.08):
        sigs.append({
            "id": "uniform_high_rgb_error_wrong_scheme_or_core",
            "confidence": "high",
            "note": "zero exact pixels plus flat high RGB MAE suggests bytes decoded under the wrong scheme/core",
        })
    if display_bbox and display_bbox[0] == 0 and display_bbox[1] == 0 and exact_ratio < 0.05:
        sigs.append({
            "id": "first_sample_or_geometry_mismatch",
            "confidence": "medium",
            "note": "mismatches begin at the first compared sample; verify geometry, crop, and artifact provenance",
        })
    r, g_mae, b = mae
    if g_mae >= max(r, b) * 2.5 and g_mae >= 20.0:
        sigs.append({
            "id": "green_dominant_colour_transform_or_chroma_path",
            "confidence": "medium",
            "note": "green-dominant error is characteristic of matrix/range or chroma-path mistakes",
        })
    if r >= g_mae * 1.6 and b >= g_mae * 1.6 and max(r, b) >= 20.0:
        sigs.append({
            "id": "red_blue_dominant_possible_uv_swap",
            "confidence": "medium",
            "note": "red and blue dominate together; check U/V ordering before blaming luma geometry",
        })
    if not sigs:
        sigs.append({
            "id": "unclassified_visual_mismatch",
            "confidence": "low",
            "note": "use raw per-channel MAE/max/exact counts and diff artifact for diagnosis",
        })
    return sigs


def channel_dispersion(mae: list[float]) -> dict:
    arr = np.array(mae, dtype=np.float64)
    avg = float(arr.mean()) if arr.size else 0.0
    min_v = float(arr.min()) if arr.size else 0.0
    max_v = float(arr.max()) if arr.size else 0.0
    ratio = float(max_v / min_v) if min_v > 1.0e-9 else (float("inf") if max_v > 0 else 1.0)
    cv = float(arr.std() / avg) if avg > 1.0e-9 else 0.0
    return {
        "metric": "rgb_mae_channel_dispersion",
        "per_channel_mae_rgb": [float(x) for x in mae],
        "mean": avg,
        "min": min_v,
        "max": max_v,
        "max_min_ratio": ratio,
        "coefficient_of_variation": cv,
        "threshold_basis": {
            # Synthetic 624x480 active-region evidence in tests/unit/test_hw_visual_compare.py:
            # unrelated/random: ratio=1.007 cv=0.003; unrelated/solid: ratio=1.046 cv=0.018;
            # live frozen-screen report: ratio≈1.06 cv≈0.024.  These are intentionally below
            # the flat/no-frame cutoffs.  Colour-path cases are far above them: 601→709
            # ratio≈38.8 cv≈1.27, 709→601 ratio≈15.8 cv≈0.74, U/V swap ratio≈7.1 cv≈0.56.
            "no_frame_delivered": {
                "mean_mae_min": 20.0,
                "exact_match_ratio_max": 0.05,
                "max_min_ratio_max": 1.15,
                "coefficient_of_variation_max": 0.06,
            },
            "colour_path_defect": {
                "mean_mae_min": 5.0,
                "exact_match_ratio_max": 0.20,
                "max_min_ratio_min": 5.0,
                "coefficient_of_variation_min": 0.50,
            },
        },
    }


def classify_visual_verdict(stats: dict, shift_rows: list[dict] | None = None) -> dict:
    """Classify mismatch shape without hiding raw numbers.

    The verdict is diagnostic, not a pass/fail threshold.  Ambiguous high-error
    channel dispersion is deliberately refused by cmd_compare instead of being
    guessed as either a colour defect or absent frame.
    """
    dispersion = channel_dispersion([float(x) for x in stats["per_plane_mae_rgb"]])
    exact_ratio = float(stats["exact_match_ratio"])
    mean_mae = dispersion["mean"]
    ratio = dispersion["max_min_ratio"]
    cv = dispersion["coefficient_of_variation"]
    if stats["exact_match_pixels"] == stats["active_pixels"]:
        return {
            "id": "EXACT_MATCH",
            "confidence": "high",
            "dispersion": dispersion,
            "note": "captured active region exactly matches the declared golden",
        }

    current_avg = mean_mae
    if shift_rows:
        best = shift_rows[0]
        best_avg = float(sum(best["per_plane_mae_rgb"]) / 3.0)
        best_exact = float(best["exact_match_ratio"])
        if (
            (best["captured_dx"] != 0 or best["captured_dy"] != 0)
            and (best_exact >= exact_ratio + 0.20 or best_avg <= current_avg * 0.25)
        ):
            return {
                "id": "GEOMETRY_CONTENT_DEFECT",
                "confidence": "high",
                "dispersion": dispersion,
                "best_shift": best,
                "note": "a shifted capture overlap explains the mismatch better than channel dispersion; check geometry, crop, pillar, or content alignment",
            }

    no_frame = (
        mean_mae >= 20.0
        and exact_ratio <= 0.05
        and ratio <= 1.15
        and cv <= 0.06
    )
    if no_frame:
        return {
            "id": "NO_FRAME_DELIVERED",
            "confidence": "high",
            "dispersion": dispersion,
            "note": "RGB MAE is high and nearly channel-uniform; treat as absent/stale panel content (e.g. PLXF mailbox/frame delivery), not a colour conversion defect",
        }

    colour = (
        mean_mae >= 5.0
        and exact_ratio <= 0.20
        and (ratio >= 5.0 or cv >= 0.50)
    )
    if colour:
        return {
            "id": "COLOUR_PATH_DEFECT",
            "confidence": "high",
            "dispersion": dispersion,
            "note": "RGB MAE is strongly channel-skewed; check matrix/range or U/V/chroma path before delivery plumbing",
        }

    if mean_mae < 5.0 or exact_ratio >= 0.10:
        return {
            "id": "GEOMETRY_CONTENT_DEFECT",
            "confidence": "medium",
            "dispersion": dispersion,
            "note": "mismatch is sparse/structured or low-amplitude rather than flat absent-frame or strongly channel-skewed colour error",
        }

    return {
        "id": "INDETERMINATE",
        "confidence": "none",
        "dispersion": dispersion,
        "note": "RGB MAE dispersion is in the unsafe band between flat no-frame and skewed colour-path signatures; refusing to guess",
    }


def diff_stats(golden: np.ndarray, captured: np.ndarray, g: Geometry,
               box: tuple[int, int, int, int] | None = None) -> dict:
    ga = active_view(golden, g, box).astype(np.int16)
    ca = active_view(captured, g, box).astype(np.int16)
    if ga.shape != ca.shape:
        raise HarnessError(f"active region shape mismatch golden={ga.shape} captured={ca.shape}")
    diff = ca - ga
    ad = np.abs(diff)
    per_plane_mae = ad.reshape(-1, 3).mean(axis=0)
    per_plane_exact = (ad == 0).reshape(-1, 3).sum(axis=0)
    per_plane_max = ad.reshape(-1, 3).max(axis=0)
    gy = rgb_to_yuv601(ga)
    cy = rgb_to_yuv601(ca)
    yuv_ad = np.abs(cy - gy)
    yuv_mae = yuv_ad.reshape(-1, 3).mean(axis=0)
    yuv_exact = (yuv_ad == 0).reshape(-1, 3).sum(axis=0)
    yuv_max = yuv_ad.reshape(-1, 3).max(axis=0)
    exact = np.all(ad == 0, axis=2)
    worst_flat = int(np.argmax(ad))
    wy, wx, wc = np.unravel_index(worst_flat, ad.shape)
    x0, y0, _x1, _y1 = box if box else g.active_box
    mismatch_mask = np.any(ad != 0, axis=2)
    mismatch_bbox = None
    if np.any(mismatch_mask):
        ys, xs = np.where(mismatch_mask)
        mismatch_bbox = {
            "display": [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())],
            "presented": [
                int(x0 + xs.min()), int(y0 + ys.min()),
                int(x0 + xs.max()), int(y0 + ys.max()),
            ],
            "pixels": int(mismatch_mask.sum()),
        }
    stats = {
        "active_pixels": int(exact.size),
        "exact_match_pixels": int(exact.sum()),
        "exact_match_ratio": float(exact.sum() / exact.size),
        "mismatch_bbox": mismatch_bbox,
        "per_plane_exact_match_pixels_rgb": [int(x) for x in per_plane_exact],
        "per_plane_exact_match_ratio_rgb": [float(x / exact.size) for x in per_plane_exact],
        "per_plane_mae_rgb": [float(x) for x in per_plane_mae],
        "per_plane_max_abs_rgb": [int(x) for x in per_plane_max],
        "per_plane_exact_match_pixels_yuv": [int(x) for x in yuv_exact],
        "per_plane_exact_match_ratio_yuv": [float(x / exact.size) for x in yuv_exact],
        "per_plane_mae_yuv": [float(x) for x in yuv_mae],
        "per_plane_max_abs_yuv": [int(x) for x in yuv_max],
        "overall_mae": float(ad.mean()),
        "max_abs": int(ad[wy, wx, wc]),
        "worst": {
            "x_presented": int(x0 + wx),
            "y_presented": int(y0 + wy),
            "x_display": int(wx),
            "y_display": int(wy),
            "plane": "RGB"[wc],
            "golden": int(ga[wy, wx, wc]),
            "captured": int(ca[wy, wx, wc]),
            "delta": int(diff[wy, wx, wc]),
        },
    }
    stats["diagnostic_signatures"] = classify_error_signatures(stats)
    stats["visual_verdict"] = classify_visual_verdict(stats)
    return stats


def shifted_overlap_box(box: tuple[int, int, int, int], dx: int, dy: int) -> tuple[
    tuple[int, int, int, int], tuple[int, int, int, int]
]:
    x0, y0, x1, y1 = box
    ax0 = max(x0, x0 - dx)
    ax1 = min(x1, x1 - dx)
    ay0 = max(y0, y0 - dy)
    ay1 = min(y1, y1 - dy)
    return (ax0, ay0, ax1, ay1), (ax0 + dx, ay0 + dy, ax1 + dx, ay1 + dy)


def shift_sweep(golden: np.ndarray, captured: np.ndarray, g: Geometry,
                box: tuple[int, int, int, int], radius: int) -> list[dict]:
    rows = []
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            ga_box, ca_box = shifted_overlap_box(box, dx, dy)
            ga = active_view(golden, g, ga_box)
            ca = active_view(captured, g, ca_box)
            stats = diff_stats(ga, ca, g, (0, 0, ga.shape[1], ga.shape[0]))
            rows.append({
                "captured_dx": dx,
                "captured_dy": dy,
                "pixels_compared": stats["active_pixels"],
                "exact_match_pixels": stats["exact_match_pixels"],
                "exact_match_ratio": stats["exact_match_ratio"],
                "per_plane_mae_rgb": stats["per_plane_mae_rgb"],
                "per_plane_mae_yuv": stats["per_plane_mae_yuv"],
                "max_abs": stats["max_abs"],
            })
    rows.sort(key=lambda r: (sum(r["per_plane_mae_rgb"]) / 3.0, -r["exact_match_pixels"]))
    return rows


def diff_artifact(golden: np.ndarray, captured: np.ndarray, g: Geometry, out: Path,
                  box: tuple[int, int, int, int] | None = None) -> None:
    x0, y0, x1, y1 = box if box else g.active_box
    gg = golden.copy()
    cc = captured.copy()
    # Highlight compared active rectangle without altering the interior.
    for frame, color in ((gg, np.array([0, 255, 0], dtype=np.uint8)),
                         (cc, np.array([0, 255, 0], dtype=np.uint8))):
        frame[y0:y1, x0:x0 + 1] = color
        frame[y0:y1, x1 - 1:x1] = color
        frame[y0:y0 + 1, x0:x1] = color
        frame[y1 - 1:y1, x0:x1] = color
    d = np.zeros_like(golden)
    active_diff = np.abs(active_view(captured, g, box).astype(np.int16) -
                         active_view(golden, g, box).astype(np.int16))
    d[y0:y1, x0:x1, :] = np.clip(active_diff * 8, 0, 255).astype(np.uint8)
    sep = np.full((g.presented_height, 8, 3), 32, dtype=np.uint8)
    tiled = np.concatenate([gg, sep, cc, sep, d], axis=1)
    save_rgb(out, tiled)


def measured_noise(frames: list[np.ndarray], g: Geometry,
                   box: tuple[int, int, int, int] | None = None) -> dict:
    if len(frames) < 2:
        raise HarnessError("noise measurement needs at least two frames")
    base = active_view(frames[0], g, box).astype(np.int16)
    maes = []
    worst = 0
    exact = []
    hashes = []
    for fr in frames:
        hashes.append(hashlib.sha256(fr.tobytes()).hexdigest())
    for fr in frames[1:]:
        cur = active_view(fr, g, box).astype(np.int16)
        ad = np.abs(cur - base)
        maes.append(ad.reshape(-1, 3).mean(axis=0))
        worst = max(worst, int(ad.max()))
        exact.append(int(np.all(ad == 0, axis=2).sum()))
    arr = np.array(maes, dtype=np.float64)
    max_mae = arr.max(axis=0) if arr.size else np.zeros(3)
    # Threshold is measured floor plus one code value. HDMI often measures zero;
    # analogue capture gets the measured spread with margin.
    plane_thresholds = np.maximum(1.0, max_mae * 3.0 + 1.0)
    max_abs_threshold = max(2, worst * 3 + 2)
    return {
        "frames": len(frames),
        "unique_hashes": len(set(hashes)),
        "active_pixels": int(base.shape[0] * base.shape[1]),
        "max_pair_mae_rgb": [float(x) for x in max_mae],
        "max_abs_noise": int(worst),
        "exact_match_pixels_vs_first": exact,
        "threshold_mae_rgb": [float(x) for x in plane_thresholds],
        "threshold_max_abs": int(max_abs_threshold),
    }


def compare_ok(stats: dict, noise: dict | None, max_mae: float | None, max_abs: int | None) -> bool:
    maes = stats["per_plane_mae_rgb"]
    if noise:
        mae_limits = noise["threshold_mae_rgb"]
        abs_limit = int(noise["threshold_max_abs"])
    else:
        mae_limits = [max_mae if max_mae is not None else 0.0] * 3
        abs_limit = max_abs if max_abs is not None else 0
    return all(maes[i] <= mae_limits[i] for i in range(3)) and stats["max_abs"] <= abs_limit


def delivery_failure_verdict(error: DeliveryFreshnessError) -> dict:
    msg = str(error)
    if NON_YUV_DOORBELL_ERROR in msg:
        reason = "non_yuv_doorbell_refusal"
        note = "ARM status reports PLXF non-YUV doorbell refusal; delivery failed before pixels are eligible for colour classification"
    elif PLXF_ABSENT_ERROR in msg or "NO_FRESH_FRAME" in msg:
        reason = "no_fresh_frame_delivery"
        note = "ARM status reports absent/unfresh PLXF frame delivery; do not diagnose colour or geometry from panel pixels"
    else:
        reason = "delivery_freshness_failure"
        note = "delivery freshness gate failed before pixels are eligible for colour classification"
    return {
        "id": "NO_FRAME_DELIVERED",
        "confidence": "high",
        "delivery_reason": reason,
        "note": note,
        "delivery_error": msg,
    }


def cmd_geometry(_args: argparse.Namespace) -> int:
    print(json.dumps(asdict(load_geometry()), indent=2, sort_keys=True))
    return 0


def cmd_capture(args: argparse.Namespace) -> int:
    capture_v4l2(Path(args.out), args.device, args.input_format, args.video_size,
                 args.framerate, args.warmup, args.attempts,
                 args.color_matrix, args.color_range)
    print(
        f"captured {args.out} ({args.input_format} {args.video_size}@{args.framerate} "
        f"{args.color_matrix}/{args.color_range})"
    )
    return 0


def cmd_noise(args: argparse.Namespace) -> int:
    g = load_geometry()
    box = parse_compare_box(args.compare_box, g)
    frames = [load_rgb(Path(p), g) for p in args.frames]
    report = measured_noise(frames, g, box)
    report["geometry"] = asdict(g)
    report["compare_box"] = list(box)
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    g = load_geometry()
    box = parse_compare_box(args.compare_box, g)
    rbf_identity = validate_rbf_identity(args)
    golden_provenance = validate_golden_provenance(args, g, box, rbf_identity)
    color_provenance = require_color_provenance(args, golden_provenance)
    reject_corrupt_capture_log(args.capture_log)
    try:
        delivery_freshness = validate_delivery_freshness(args)
    except DeliveryFreshnessError as e:
        report = {
            "ok": False,
            "golden": str(args.golden),
            "capture": str(args.capture),
            "capture_log": str(args.capture_log) if args.capture_log else None,
            "geometry": asdict(g),
            "compare_box": list(box),
            "rbf_identity": rbf_identity,
            "golden_provenance": golden_provenance,
            "color_provenance": color_provenance,
            "freshness": None,
            "delivery_freshness": {"error": str(e)},
            "delivery_verdict": delivery_failure_verdict(e),
            "stats": None,
            "thresholds": None,
        }
        if args.report:
            out = Path(args.report)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2, sort_keys=True))
        print(f"ERROR: {e}", file=sys.stderr)
        return e.exit_code
    golden = load_rgb(Path(args.golden), g)
    captured = load_rgb(Path(args.capture), g)
    freshness = None
    if args.previous:
        prev = load_rgb(Path(args.previous), g)
        if np.array_equal(prev, captured):
            raise StaleCaptureError(
                "STALE capture: current frame is byte-identical to previous condition"
            )
        freshness = {
            "previous": str(args.previous),
            "previous_sha256": hashlib.sha256(prev.tobytes()).hexdigest(),
            "capture_sha256": hashlib.sha256(captured.tobytes()).hexdigest(),
            "byte_identical": False,
        }
    stats = diff_stats(golden, captured, g, box)
    noise = None
    if args.noise_report:
        noise = json.loads(Path(args.noise_report).read_text(encoding="utf-8"))
    ok = compare_ok(stats, noise, args.max_mae, args.max_abs)
    report = {
        "ok": ok,
        "golden": str(args.golden),
        "capture": str(args.capture),
        "capture_log": str(args.capture_log) if args.capture_log else None,
        "geometry": asdict(g),
        "compare_box": list(box),
        "rbf_identity": rbf_identity,
        "golden_provenance": golden_provenance,
        "color_provenance": color_provenance,
        "freshness": freshness,
        "delivery_freshness": delivery_freshness,
        "stats": stats,
        "thresholds": {
            "source": str(args.noise_report) if args.noise_report else "cli",
            "mae_rgb": noise["threshold_mae_rgb"] if noise else [args.max_mae] * 3,
            "max_abs": noise["threshold_max_abs"] if noise else args.max_abs,
        },
    }
    if args.diff:
        diff_artifact(golden, captured, g, Path(args.diff), box)
        report["diff"] = str(args.diff)
    if args.shift_radius:
        report["shift_sweep"] = shift_sweep(golden, captured, g, box, args.shift_radius)
        stats["visual_verdict"] = classify_visual_verdict(stats, report["shift_sweep"])
    if args.report:
        out = Path(args.report)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    if not ok and stats["visual_verdict"]["id"] == "INDETERMINATE":
        return 2
    return 0 if ok else 1


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("geometry", help="print shared host/RTL geometry")
    p.set_defaults(func=cmd_geometry)

    p = sub.add_parser("capture", help="capture one frame from v4l2")
    p.add_argument("--out", required=True)
    p.add_argument("--device", default=DEFAULT_DEV)
    p.add_argument("--input-format", default=DEFAULT_FORMAT,
                   help="v4l2 input format (default: mjpeg; use yuyv422 for luma-critical edges)")
    p.add_argument("--video-size", default=DEFAULT_SIZE)
    p.add_argument("--framerate", default=DEFAULT_FPS)
    p.add_argument("--warmup", type=int, default=DEFAULT_WARMUP)
    p.add_argument("--attempts", type=int, default=DEFAULT_ATTEMPTS,
                   help="retry corrupt-buffer grabs; only a clean attempt is returned")
    p.add_argument("--color-matrix", choices=COLOR_MATRICES, default="bt601",
                   help="explicit input YCbCr matrix for ffmpeg capture conversion")
    p.add_argument("--color-range", choices=COLOR_RANGES, default="full",
                   help="explicit input YCbCr range for ffmpeg capture conversion")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("noise", help="measure capture noise from repeated static frames")
    p.add_argument("--frames", nargs="+", required=True)
    p.add_argument("--compare-box", help="presented-frame ROI x,y,w,h; defaults to shared active region")
    p.add_argument("--out")
    p.set_defaults(func=cmd_noise)

    p = sub.add_parser("compare", help="compare a capture against the checked-in golden")
    p.add_argument("--golden", required=True)
    p.add_argument("--golden-provenance",
                   help="JSON sidecar declaring the golden's RBF/geometry/format/colour provenance; "
                   "defaults to GOLDEN.png.provenance.json")
    p.add_argument("--capture", required=True)
    p.add_argument("--capture-log",
                   help="ffmpeg/V4L2 log for this capture; corrupt logs return rc=4 before grading")
    p.add_argument("--expected-rbf-md5",
                   help="declared md5 of the RBF artifact this run intends to grade")
    p.add_argument("--actual-rbf-md5",
                   help="actual loaded /media/fat/_Utility/Plex.rbf md5, or md5sum output")
    p.add_argument("--rbf-md5-log",
                   help="file containing device md5sum /media/fat/_Utility/Plex.rbf output")
    p.add_argument("--expected-content-size",
                   help="declared content geometry for the artifact under test, WIDTHxHEIGHT")
    p.add_argument("--expected-pixel-format", choices=PIXEL_FORMATS,
                   help="declared frame-store pixel format for the artifact under test")
    p.add_argument("--status-log",
                   help="push_frame --status log for this run; implausible delivery returns rc=7")
    p.add_argument("--previous-status-log",
                   help="pre-push/previous status log for optional DDR frame-token freshness checks")
    p.add_argument("--min-bytes-in", type=int,
                   help="minimum plausible decoded input bytes required before pixel grading")
    p.add_argument("--require-status-field", action="append", default=[],
                   help="required status key=value before pixel grading; may be repeated")
    p.add_argument("--require-token-change", action="store_true",
                   help="require shared DDR {bank,format,seq} token to change vs --previous-status-log")
    p.add_argument("--previous", help="previous-condition frame for stale-capture rejection")
    p.add_argument("--noise-report")
    p.add_argument("--compare-box", help="presented-frame ROI x,y,w,h; defaults to shared active region")
    p.add_argument("--golden-color-matrix", choices=COLOR_MATRICES)
    p.add_argument("--golden-color-range", choices=COLOR_RANGES)
    p.add_argument("--capture-color-matrix", choices=COLOR_MATRICES)
    p.add_argument("--capture-color-range", choices=COLOR_RANGES)
    p.add_argument("--max-mae", type=float, default=0.0)
    p.add_argument("--max-abs", type=int, default=0)
    p.add_argument("--shift-radius", type=int, default=0,
                   help="try captured image shifts +/-N pixels and report best overlaps")
    p.add_argument("--diff", help="PNG artifact: golden | captured | amplified diff")
    p.add_argument("--report", help="write JSON report")
    p.set_defaults(func=cmd_compare)

    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except HarnessError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return e.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
