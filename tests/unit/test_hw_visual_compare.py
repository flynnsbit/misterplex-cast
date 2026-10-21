#!/usr/bin/env python3
"""Unit coverage for the hardware visual decode comparator."""
from __future__ import annotations

import json
import importlib.util
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts" / "hw_visual_compare.py"
WORK = ROOT / "build" / "hw-visual-unit"
GENERATED_REFERENCE = ROOT / "tests" / "fixtures" / "hw_visual" / "plex_visual_640x480_golden.png"
VALID_RBF_MD5 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
OTHER_RBF_MD5 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
LEGACY_RBF_MD5 = "57674f2e4c11551898275e99bd4c3067"
COLOR_ARGS = (
    "--golden-color-matrix", "bt601",
    "--golden-color-range", "full",
    "--capture-color-matrix", "bt601",
    "--capture-color-range", "full",
)
VALID_PROVENANCE_ARGS = (
    "--expected-rbf-md5", VALID_RBF_MD5,
    "--actual-rbf-md5", f"{VALID_RBF_MD5}  /media/fat/_Utility/Plex.rbf",
    "--expected-content-size", "624x480",
    "--expected-pixel-format", "yuv420p",
)
LEGACY_PROVENANCE_ARGS = (
    "--expected-rbf-md5", LEGACY_RBF_MD5,
    "--actual-rbf-md5", f"{LEGACY_RBF_MD5}  /media/fat/_Utility/Plex.rbf",
    "--expected-content-size", "320x240",
    "--expected-pixel-format", "rgb565",
)
WCAP_CORRUPT_LOG = (
    ROOT / "tests" / "fixtures" / "hw_visual" / "capture_logs" /
    "wcap_fe7673bc_yuyv422_corrupt.log"
)
WCAP_CORRUPT_640_LOG = (
    ROOT / "tests" / "fixtures" / "hw_visual" / "capture_logs" /
    "wcap_fe7673bc_yuyv422_640_corrupt.log"
)
RELOAD_STALE_CAPTURE = (
    ROOT / "tests" / "fixtures" / "hw_visual" / "reload_determinism" /
    "plex_bytes_in4_stale_screen.png"
)
RELOAD_STALE_STATUS = (
    ROOT / "tests" / "fixtures" / "hw_visual" / "reload_determinism" /
    "plex_bytes_in4_status.txt"
)

spec = importlib.util.spec_from_file_location("hw_visual_compare", TOOL)
hw_visual_compare = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = hw_visual_compare
spec.loader.exec_module(hw_visual_compare)


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=60,
    )


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def write_png(path: Path, pixels: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(pixels.astype(np.uint8), "RGB").save(path)


def write_provenance(path: Path, *, rbf: str = VALID_RBF_MD5,
                     content_size: tuple[int, int] = (624, 480),
                     pixel_format: str = "yuv420p",
                     matrix: str = "bt601",
                     color_range: str = "full",
                     compare_box: list[int] | None = None) -> None:
    if compare_box is None:
        compare_box = [11, 0, 618, 480]
    path.with_suffix(path.suffix + ".provenance.json").write_text(json.dumps({
        "schema": "misterplex.hw_visual_golden.v1",
        "source_type": "hardware_capture",
        "source_rbf_md5": rbf,
        "source_rbf_label": "synthetic unit-test capture provenance",
        "pixel_format": pixel_format,
        "geometry": {
            "content_width": content_size[0],
            "content_height": content_size[1],
            "presented_width": 640,
            "presented_height": 480,
            "compare_box": compare_box,
        },
        "color": {"matrix": matrix, "range": color_range},
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def rgb_to_yuv601(rgb: np.ndarray) -> np.ndarray:
    r = rgb[..., 0].astype(np.float64)
    g = rgb[..., 1].astype(np.float64)
    b = rgb[..., 2].astype(np.float64)
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = -0.168736 * r - 0.331264 * g + 0.5 * b + 128.0
    v = 0.5 * r - 0.418688 * g - 0.081312 * b + 128.0
    return np.stack([y, u, v], axis=-1)


def rgb_to_yuv709(rgb: np.ndarray) -> np.ndarray:
    r = rgb[..., 0].astype(np.float64)
    g = rgb[..., 1].astype(np.float64)
    b = rgb[..., 2].astype(np.float64)
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    u = -0.114572 * r - 0.385428 * g + 0.5 * b + 128.0
    v = 0.5 * r - 0.454153 * g - 0.045847 * b + 128.0
    return np.stack([y, u, v], axis=-1)


def yuv_to_rgb601(yuv: np.ndarray) -> np.ndarray:
    y = yuv[..., 0]
    u = yuv[..., 1] - 128.0
    v = yuv[..., 2] - 128.0
    r = y + 1.402 * v
    g = y - 0.344136 * u - 0.714136 * v
    b = y + 1.772 * u
    return np.clip(np.rint(np.stack([r, g, b], axis=-1)), 0, 255).astype(np.uint8)


def yuv_to_rgb709(yuv: np.ndarray) -> np.ndarray:
    y = yuv[..., 0]
    u = yuv[..., 1] - 128.0
    v = yuv[..., 2] - 128.0
    r = y + 1.5748 * v
    g = y - 0.187324 * u - 0.468124 * v
    b = y + 1.8556 * u
    return np.clip(np.rint(np.stack([r, g, b], axis=-1)), 0, 255).astype(np.uint8)


def print_verdict_case(name: str, report: dict) -> None:
    verdict = report["stats"]["visual_verdict"]
    disp = verdict["dispersion"]
    mae = [round(x, 6) for x in disp["per_channel_mae_rgb"]]
    print(
        f"VISUAL_VERDICT_CASE {name} id={verdict['id']} "
        f"mae_rgb={mae} ratio={disp['max_min_ratio']:.6f} "
        f"cv={disp['coefficient_of_variation']:.6f} "
        f"exact={report['stats']['exact_match_ratio']:.6f}"
    )


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)

    g = run("geometry")
    require(g.returncode == 0, f"geometry failed\nstdout={g.stdout}\nstderr={g.stderr}")
    geom = json.loads(g.stdout)
    require(geom["coded_width"] == 624, f"coded width wrong: {geom}")
    require(geom["display_width"] == 618, f"display width wrong: {geom}")
    require(geom["presented_width"] == 640, f"presented width wrong: {geom}")
    require(geom["pillarbox_left"] == 11, f"pillarbox wrong: {geom}")
    box = hw_visual_compare.parse_compare_box("11,0,160,120", hw_visual_compare.load_geometry())
    require(box == (11, 0, 171, 120), f"compare-box parsing wrong: {box}")
    print("PASS shared host/RTL geometry parsed")

    generated_reference_refusal = run(
        "compare",
        "--golden", str(GENERATED_REFERENCE),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(GENERATED_REFERENCE),
    )
    require(generated_reference_refusal.returncode == 9 and "not a hardware-captured golden" in generated_reference_refusal.stderr,
            "generated reference PNG must be quarantined from hardware grading, "
            f"not accepted as a captured golden\nstdout={generated_reference_refusal.stdout}\n"
            f"stderr={generated_reference_refusal.stderr}")
    print("PASS generated reference golden is quarantined by machine-readable provenance")

    golden = np.array(Image.open(GENERATED_REFERENCE).convert("RGB"), dtype=np.uint8)
    valid_golden = WORK / "valid_yuv420_624x480_golden.png"
    write_png(valid_golden, golden)
    write_provenance(valid_golden)
    cap1 = WORK / "cap1.png"
    cap2 = WORK / "cap2.png"
    write_png(cap1, golden)
    write_png(cap2, golden)

    noise = WORK / "noise.json"
    n = run("noise", "--frames", str(cap1), str(cap2), "--out", str(noise))
    require(n.returncode == 0, f"noise failed\nstdout={n.stdout}\nstderr={n.stderr}")
    nr = json.loads(noise.read_text())
    require(nr["max_abs_noise"] == 0, f"expected zero synthetic HDMI noise: {nr}")
    print("PASS zero-noise floor measured from identical static frames")

    good_report = WORK / "good.json"
    good_diff = WORK / "good_diff.png"
    c = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
        "--noise-report", str(noise),
        "--report", str(good_report),
        "--diff", str(good_diff),
    )
    require(c.returncode == 0, f"known-good compare failed\nstdout={c.stdout}\nstderr={c.stderr}")
    gr = json.loads(good_report.read_text())
    require(gr["stats"]["exact_match_pixels"] == gr["stats"]["active_pixels"],
            f"known-good exact count wrong: {gr}")
    require(gr["stats"]["per_plane_exact_match_pixels_rgb"] ==
            [gr["stats"]["active_pixels"]] * 3,
            f"known-good per-plane exact counts wrong: {gr}")
    require(gr["stats"]["per_plane_exact_match_pixels_yuv"] ==
            [gr["stats"]["active_pixels"]] * 3,
            f"known-good YUV per-plane exact counts wrong: {gr}")
    require(gr["stats"]["visual_verdict"]["id"] == "EXACT_MATCH",
            f"known-good visual verdict wrong: {gr['stats']['visual_verdict']}")
    require(good_diff.exists() and good_diff.stat().st_size > 0, "good diff artifact missing")
    require(gr["color_provenance"]["golden"] == {"matrix": "bt601", "range": "full"},
            f"good compare did not record golden colour provenance: {gr}")
    require(gr["color_provenance"]["capture"] == {"matrix": "bt601", "range": "full"},
            f"good compare did not record capture colour provenance: {gr}")
    print("PASS known-good frame exact-matches active display region")

    missing_colour = run(
        "compare",
        "--golden", str(valid_golden),
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
        "--noise-report", str(noise),
    )
    require(missing_colour.returncode == 2 and "colour matrix/range provenance is required" in missing_colour.stderr,
            "compare without colour provenance must be refused, "
            f"not graded\nstdout={missing_colour.stdout}\nstderr={missing_colour.stderr}")
    mismatched_colour = run(
        "compare",
        "--golden", str(valid_golden),
        "--golden-color-matrix", "bt601",
        "--golden-color-range", "full",
        "--expected-rbf-md5", VALID_RBF_MD5,
        "--actual-rbf-md5", f"{VALID_RBF_MD5}  /media/fat/_Utility/Plex.rbf",
        "--expected-content-size", "624x480",
        "--expected-pixel-format", "yuv420p",
        "--capture", str(cap2),
        "--capture-color-matrix", "bt709",
        "--capture-color-range", "full",
        "--noise-report", str(noise),
    )
    require(mismatched_colour.returncode == 9 and "different colour provenance" in mismatched_colour.stderr,
            "compare with mismatched colour provenance must be refused, "
            f"not graded\nstdout={mismatched_colour.stdout}\nstderr={mismatched_colour.stderr}")
    print("PASS unknown/mismatched colour provenance refused before grading")

    no_sidecar = WORK / "no_sidecar_golden.png"
    write_png(no_sidecar, golden)
    missing_provenance = run(
        "compare",
        "--golden", str(no_sidecar),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
    )
    require(missing_provenance.returncode == 9 and "missing" in missing_provenance.stderr,
            "golden without machine-readable sidecar must be refused, "
            f"not graded\nstdout={missing_provenance.stdout}\nstderr={missing_provenance.stderr}")

    wrong_geometry_prov = WORK / "wrong_geometry.provenance.json"
    write_provenance(valid_golden, content_size=(320, 240))
    valid_golden.with_suffix(valid_golden.suffix + ".provenance.json").replace(wrong_geometry_prov)
    write_provenance(valid_golden)
    wrong_geometry = run(
        "compare",
        "--golden", str(valid_golden),
        "--golden-provenance", str(wrong_geometry_prov),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
    )
    require(wrong_geometry.returncode == 9 and "content geometry 320x240 != expected 624x480" in wrong_geometry.stderr,
            "golden content geometry mismatch must be refused, "
            f"not graded\nstdout={wrong_geometry.stdout}\nstderr={wrong_geometry.stderr}")

    wrong_format = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        "--expected-rbf-md5", VALID_RBF_MD5,
        "--actual-rbf-md5", f"{VALID_RBF_MD5}  /media/fat/_Utility/Plex.rbf",
        "--expected-content-size", "624x480",
        "--expected-pixel-format", "rgb565",
        "--capture", str(cap2),
    )
    require(wrong_format.returncode == 9 and "pixel format yuv420p != expected rgb565" in wrong_format.stderr,
            "golden pixel-format mismatch must be refused, "
            f"not graded\nstdout={wrong_format.stdout}\nstderr={wrong_format.stderr}")

    wrong_color_prov = WORK / "wrong_color.provenance.json"
    write_provenance(valid_golden, matrix="bt709")
    valid_golden.with_suffix(valid_golden.suffix + ".provenance.json").replace(wrong_color_prov)
    write_provenance(valid_golden)
    wrong_color = run(
        "compare",
        "--golden", str(valid_golden),
        "--golden-provenance", str(wrong_color_prov),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
    )
    require(wrong_color.returncode == 9 and "CLI golden colour does not match sidecar" in wrong_color.stderr,
            "golden colour provenance mismatch must be refused, "
            f"not graded\nstdout={wrong_color.stdout}\nstderr={wrong_color.stderr}")

    golden_source_mismatch = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        "--expected-rbf-md5", OTHER_RBF_MD5,
        "--actual-rbf-md5", f"{OTHER_RBF_MD5}  /media/fat/_Utility/Plex.rbf",
        "--expected-content-size", "624x480",
        "--expected-pixel-format", "yuv420p",
        "--capture", str(cap2),
    )
    require(golden_source_mismatch.returncode == 8 and
            f"golden was produced by core {VALID_RBF_MD5} but loaded core is {OTHER_RBF_MD5}" in golden_source_mismatch.stderr,
            "golden source RBF mismatch must say which core produced it and which is loaded, "
            f"not graded\nstdout={golden_source_mismatch.stdout}\nstderr={golden_source_mismatch.stderr}")
    print("PASS golden provenance refuses missing sidecar, geometry, format, colour, and source-RBF mismatches")

    bad = golden.copy()
    # Corrupt one active pixel, not a pillarbox pixel; this is the red-path proof
    # that the comparator reports a precise location and emits a useful diff.
    bad[20, 20, 1] = (int(bad[20, 20, 1]) + 64) & 0xFF
    bad_path = WORK / "bad.png"
    write_png(bad_path, bad)
    bad_report = WORK / "bad.json"
    bad_diff = WORK / "bad_diff.png"
    b = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(bad_path),
        "--noise-report", str(noise),
        "--report", str(bad_report),
        "--shift-radius", "1",
        "--diff", str(bad_diff),
    )
    require(b.returncode == 1, f"corrupted frame did not fail\nstdout={b.stdout}\nstderr={b.stderr}")
    br = json.loads(bad_report.read_text())
    require(br["stats"]["worst"]["x_presented"] == 20, f"wrong worst x: {br}")
    require(br["stats"]["worst"]["y_presented"] == 20, f"wrong worst y: {br}")
    require(br["stats"]["mismatch_bbox"]["presented"] == [20, 20, 20, 20],
            f"wrong mismatch bbox: {br}")
    require(br["stats"]["max_abs"] >= 64, f"bad max_abs too small: {br}")
    require(br["stats"]["per_plane_exact_match_pixels_rgb"][1] ==
            br["stats"]["active_pixels"] - 1,
            f"bad per-plane exact count should isolate one green-plane pixel: {br}")
    require(br["stats"]["per_plane_mae_yuv"][0] > 0,
            f"bad YUV per-plane MAE did not report the injected pixel: {br}")
    require(br["stats"]["diagnostic_signatures"][0]["id"] == "unclassified_visual_mismatch",
            f"single-pixel red path should not be over-classified: {br['stats']['diagnostic_signatures']}")
    require(br["shift_sweep"][0]["captured_dx"] == 0 and br["shift_sweep"][0]["captured_dy"] == 0,
            f"shift sweep should prefer no shift for single-pixel corruption: {br['shift_sweep'][:3]}")
    require(bad_diff.exists() and bad_diff.stat().st_size > 0, "bad diff artifact missing")
    print("PASS corrupted active pixel rejected with precise worst mismatch + diff artifact")

    def compare_variant(name: str, pixels: np.ndarray, expected_id: str,
                        expected_rc: int = 1, shift_radius: int = 0) -> dict:
        path = WORK / f"{name}.png"
        report = WORK / f"{name}.json"
        write_png(path, pixels)
        args = [
            "compare",
            "--golden", str(valid_golden),
            *COLOR_ARGS,
            *VALID_PROVENANCE_ARGS,
            "--capture", str(path),
            "--report", str(report),
        ]
        if shift_radius:
            args.extend(["--shift-radius", str(shift_radius)])
        proc = run(*args)
        require(proc.returncode == expected_rc,
                f"{name} rc={proc.returncode}, want {expected_rc}\nstdout={proc.stdout}\nstderr={proc.stderr}")
        data = json.loads(report.read_text())
        print_verdict_case(name, data)
        require(data["stats"]["visual_verdict"]["id"] == expected_id,
                f"{name} verdict wrong: {data['stats']['visual_verdict']}")
        return data

    exact_case = compare_variant("verdict_exact", golden, "EXACT_MATCH", expected_rc=0)
    require(exact_case["stats"]["visual_verdict"]["dispersion"]["max_min_ratio"] == 1.0,
            f"exact dispersion should be ratio 1: {exact_case['stats']['visual_verdict']}")

    rng = np.random.default_rng(12345)
    unrelated = rng.integers(0, 256, golden.shape, dtype=np.uint8)
    no_frame = compare_variant("verdict_unrelated_random", unrelated, "NO_FRAME_DELIVERED")
    require(no_frame["stats"]["visual_verdict"]["dispersion"]["max_min_ratio"] < 1.02,
            f"unrelated/random should be flat channel error: {no_frame['stats']['visual_verdict']}")

    wrong_709 = yuv_to_rgb709(rgb_to_yuv601(golden))
    matrix = compare_variant("verdict_601_encoded_709_decoded", wrong_709, "COLOUR_PATH_DEFECT")
    require(matrix["stats"]["visual_verdict"]["dispersion"]["max_min_ratio"] > 30.0,
            f"601/709 matrix mismatch should be strongly channel-skewed: {matrix['stats']['visual_verdict']}")

    yuv = rgb_to_yuv601(golden)
    uv_swapped_yuv = yuv.copy()
    uv_swapped_yuv[..., 1] = yuv[..., 2]
    uv_swapped_yuv[..., 2] = yuv[..., 1]
    uv_swap = compare_variant("verdict_uv_swap", yuv_to_rgb601(uv_swapped_yuv), "COLOUR_PATH_DEFECT")
    require(uv_swap["stats"]["visual_verdict"]["dispersion"]["max_min_ratio"] > 5.0,
            f"U/V swap should be channel-skewed: {uv_swap['stats']['visual_verdict']}")

    shifted = golden.copy()
    shifted[:, 5:, :] = golden[:, :-5, :]
    shifted[:, :5, :] = 0
    shifted_case = compare_variant("verdict_shift_x5", shifted, "GEOMETRY_CONTENT_DEFECT",
                                   shift_radius=6)
    require(shifted_case["stats"]["visual_verdict"]["id"] != "COLOUR_PATH_DEFECT",
            f"pixel shift must not be labelled colour: {shifted_case['stats']['visual_verdict']}")
    require(shifted_case["stats"]["visual_verdict"]["best_shift"]["captured_dx"] == 5,
            f"shift verdict did not identify +5px capture shift: {shifted_case['stats']['visual_verdict']}")

    ambiguous = golden.copy().astype(np.int16)
    ambiguous[:, :, 0] = np.clip(ambiguous[:, :, 0] + 80, 0, 255)
    ambiguous[:, :, 1] = np.clip(ambiguous[:, :, 1] + 50, 0, 255)
    ambiguous[:, :, 2] = np.clip(ambiguous[:, :, 2] + 40, 0, 255)
    ambiguous_case = compare_variant("verdict_ambiguous_dispersion", ambiguous.astype(np.uint8),
                                     "INDETERMINATE", expected_rc=2)
    disp = ambiguous_case["stats"]["visual_verdict"]["dispersion"]
    require(1.15 < disp["max_min_ratio"] < 5.0 and disp["coefficient_of_variation"] < 0.50,
            f"ambiguous case must land in the refused unsafe band: {disp}")
    print("PASS visual verdict classifies exact/no-frame/colour/geometry and refuses ambiguous dispersion")

    uniform_sig = hw_visual_compare.classify_error_signatures({
        "active_pixels": 296640,
        "exact_match_pixels": 0,
        "exact_match_ratio": 0.0,
        "per_plane_mae_rgb": [112.09, 111.50, 111.60],
        "max_abs": 255,
        "mismatch_bbox": {"display": [0, 0, 617, 479], "presented": [11, 0, 628, 479], "pixels": 296640},
    })
    require(any(s["id"] == "uniform_high_rgb_error_wrong_scheme_or_core" for s in uniform_sig),
            f"flat high RGB error shape not flagged: {uniform_sig}")
    green_sig = hw_visual_compare.classify_error_signatures({
        "active_pixels": 19200,
        "exact_match_pixels": 10,
        "exact_match_ratio": 10 / 19200,
        "per_plane_mae_rgb": [17.77, 157.31, 20.42],
        "max_abs": 255,
        "mismatch_bbox": {"display": [0, 0, 159, 119], "presented": [11, 0, 170, 119], "pixels": 19190},
    })
    require(any(s["id"] == "green_dominant_colour_transform_or_chroma_path" for s in green_sig),
            f"green-dominant colour/chroma shape not flagged: {green_sig}")
    uv_sig = hw_visual_compare.classify_error_signatures({
        "active_pixels": 19200,
        "exact_match_pixels": 100,
        "exact_match_ratio": 100 / 19200,
        "per_plane_mae_rgb": [96.0, 24.0, 102.0],
        "max_abs": 240,
        "mismatch_bbox": {"display": [0, 0, 159, 119], "presented": [11, 0, 170, 119], "pixels": 19100},
    })
    require(any(s["id"] == "red_blue_dominant_possible_uv_swap" for s in uv_sig),
            f"red/blue-dominant U/V-swap shape not flagged: {uv_sig}")
    print("PASS per-channel error-shape diagnostics flag wrong-scheme, green-dominant, and U/V-swap patterns")

    stale = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--previous", str(cap1),
        "--capture", str(cap1),
        "--noise-report", str(noise),
    )
    require(stale.returncode == 3 and "STALE capture" in stale.stderr,
            f"stale capture was not rejected\nstdout={stale.stdout}\nstderr={stale.stderr}")
    print("PASS stale previous-condition capture rejected")

    require(RELOAD_STALE_CAPTURE.exists(), "natural bytes_in=4 stale capture fixture missing")
    require(RELOAD_STALE_STATUS.exists(), "natural bytes_in=4 status fixture missing")
    stale_delivery = run(
        "compare",
        "--golden", str(RELOAD_STALE_CAPTURE),
        *COLOR_ARGS,
        *LEGACY_PROVENANCE_ARGS,
        "--capture", str(RELOAD_STALE_CAPTURE),
        "--noise-report", str(noise),
        "--status-log", str(RELOAD_STALE_STATUS),
        "--min-bytes-in", "512",
        "--require-status-field", "has_frame=1",
        "--require-status-field", "has_stream=1",
        "--require-status-field", "has_idr=1",
    )
    require(stale_delivery.returncode == 7 and
            "STATUS_TELEMETRY_LAYER: bytes_in=4 equals nalu=4" in stale_delivery.stderr,
            "bytes_in=4/nalu=4 stale-screen status must be refused as status-telemetry aliasing "
            "before an exact pixel match can pass, not graded\n"
            f"stdout={stale_delivery.stdout}\nstderr={stale_delivery.stderr}")
    print("PASS natural bytes_in=4/nalu=4 stale-screen fixture is rejected as telemetry-layer aliasing")

    renamed_status = WORK / "status_stream_nalus_no_bytes.txt"
    renamed_status.write_text(
        "status has_frame=1 has_stream=1 has_idr=1 sps_valid=1 pps_valid=1 "
        "stream_nalus=4 bytes_in_unavailable=1\n",
        encoding="utf-8",
    )
    renamed_delivery = run(
        "compare",
        "--golden", str(RELOAD_STALE_CAPTURE),
        *COLOR_ARGS,
        *LEGACY_PROVENANCE_ARGS,
        "--capture", str(RELOAD_STALE_CAPTURE),
        "--noise-report", str(noise),
        "--status-log", str(renamed_status),
        "--min-bytes-in", "512",
    )
    require(renamed_delivery.returncode == 7 and
            "status exposes stream_nalus=4, not a byte-delivery counter" in renamed_delivery.stderr,
            "renamed stream_nalus status must not satisfy --min-bytes-in byte freshness\n"
            f"stdout={renamed_delivery.stdout}\nstderr={renamed_delivery.stderr}")
    print("PASS stream_nalus status cannot satisfy byte-delivery freshness gate")

    fresh_status_before = WORK / "status_before_token.txt"
    fresh_status_after = WORK / "status_after_token.txt"
    fresh_status_before.write_text(
        "status has_frame=1 has_stream=1 has_idr=1 sps_valid=1 pps_valid=1 "
        "frame_bank=0 frame_format=yuv420p frame_seq=41 bytes_in=4096\n",
        encoding="utf-8",
    )
    fresh_status_after.write_text(
        "status has_frame=1 has_stream=1 has_idr=1 sps_valid=1 pps_valid=1 "
        "frame_bank=1 frame_format=yuv420p frame_seq=42 bytes_in=6227\n",
        encoding="utf-8",
    )
    fresh_report = WORK / "fresh_delivery.json"
    fresh_delivery = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
        "--noise-report", str(noise),
        "--status-log", str(fresh_status_after),
        "--previous-status-log", str(fresh_status_before),
        "--min-bytes-in", "512",
        "--require-status-field", "has_frame=1",
        "--require-status-field", "has_stream=1",
        "--require-status-field", "has_idr=1",
        "--require-token-change",
        "--report", str(fresh_report),
    )
    require(fresh_delivery.returncode == 0,
            f"fresh delivery status with changed token did not allow exact compare\n"
            f"stdout={fresh_delivery.stdout}\nstderr={fresh_delivery.stderr}")
    fr = json.loads(fresh_report.read_text())
    require(fr["delivery_freshness"]["token_changed"] is True,
            f"fresh token change not reported: {fr['delivery_freshness']}")
    require(fr["rbf_identity"]["match"] is True,
            f"matching RBF identity not reported: {fr['rbf_identity']}")
    print("PASS fresh delivery counters, matching RBF md5, and shared {bank,format,seq} token allow grading")

    unchanged_token = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
        "--noise-report", str(noise),
        "--status-log", str(fresh_status_before),
        "--previous-status-log", str(fresh_status_before),
        "--min-bytes-in", "512",
        "--require-status-field", "has_frame=1",
        "--require-status-field", "has_stream=1",
        "--require-status-field", "has_idr=1",
        "--require-token-change",
    )
    require(unchanged_token.returncode == 7 and "frame token did not change" in unchanged_token.stderr,
            "unchanged DDR frame token must be refused when token freshness is required, "
            f"not graded\nstdout={unchanged_token.stdout}\nstderr={unchanged_token.stderr}")
    print("PASS unchanged shared frame token is rejected before pixel grading")

    wrong_core = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        "--expected-rbf-md5", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "--actual-rbf-md5", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  /media/fat/_Utility/Plex.rbf",
        "--capture", str(cap2),
        "--noise-report", str(noise),
        "--status-log", str(fresh_status_after),
        "--min-bytes-in", "512",
    )
    require(wrong_core.returncode == 8 and "loaded core md5 bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" in wrong_core.stderr,
            "wrong loaded RBF md5 must be refused before an exact pixel match can pass, "
            f"not graded\nstdout={wrong_core.stdout}\nstderr={wrong_core.stderr}")
    undeclared_core = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        "--actual-rbf-md5", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  /media/fat/_Utility/Plex.rbf",
        "--capture", str(cap2),
        "--noise-report", str(noise),
    )
    require(undeclared_core.returncode == 8 and "expected RBF md5 was not declared" in undeclared_core.stderr,
            "loaded RBF md5 without declared expected artifact must be refused, "
            f"not graded\nstdout={undeclared_core.stdout}\nstderr={undeclared_core.stderr}")
    print("PASS wrong or undeclared loaded RBF identity is rejected before pixel grading")

    status_skewed_colour = WORK / "status_skewed_colour.png"
    write_png(status_skewed_colour, wrong_709)
    absent_status = WORK / "status_frame_absent.txt"
    absent_status.write_text(
        "status frame_status=absent has_frame=0 has_stream=1 has_idr=1 bytes_in=6227\n",
        encoding="utf-8",
    )
    absent_status_report = WORK / "status_absent_must_not_colour.json"
    absent_guard = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(status_skewed_colour),
        "--noise-report", str(noise),
        "--status-log", str(absent_status),
        "--min-bytes-in", "512",
        "--report", str(absent_status_report),
    )
    require(absent_guard.returncode == 7 and
            "frame_status=absent" in absent_guard.stderr and
            "PLXF mailbox absent/unwritten" in absent_guard.stderr and
            "COLOUR_PATH_DEFECT" not in (absent_guard.stdout + absent_guard.stderr) and
            absent_status_report.exists(),
            "ARM frame_status=absent/has_frame=0 must refuse before a channel-skewed image "
            "can be labelled as a colour defect\n"
            f"stdout={absent_guard.stdout}\nstderr={absent_guard.stderr}")
    absent_report = json.loads(absent_status_report.read_text())
    require(absent_report["stats"] is None and
            absent_report["delivery_verdict"]["id"] == "NO_FRAME_DELIVERED" and
            absent_report["delivery_verdict"]["delivery_reason"] == "no_fresh_frame_delivery",
            f"PLXF absent status must map to delivery_verdict without pixel stats: {absent_report}")

    absent_text_status = WORK / "status_plxf_absent_text.txt"
    absent_text_status.write_text(
        "frame store status unavailable (PLXF mailbox absent/unwritten)\n",
        encoding="utf-8",
    )
    absent_text_guard = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(status_skewed_colour),
        "--noise-report", str(noise),
        "--status-log", str(absent_text_status),
        "--min-bytes-in", "512",
    )
    require(absent_text_guard.returncode == 7 and
            "frame store status unavailable (PLXF mailbox absent/unwritten)" in absent_text_guard.stderr and
            '"delivery_verdict":' in absent_text_guard.stdout and
            '"id": "NO_FRAME_DELIVERED"' in absent_text_guard.stdout and
            '"stats": null' in absent_text_guard.stdout,
            "plain PLXF absent/unwritten status text must be a named delivery refusal, "
            f"not graded\nstdout={absent_text_guard.stdout}\nstderr={absent_text_guard.stderr}")

    non_yuv_status = WORK / "status_non_yuv_debug.txt"
    non_yuv_status.write_text(
        "status has_frame=1 has_stream=1 has_idr=1 sps_valid=1 pps_valid=1 "
        "frame_bank=1 frame_format=yuv420p frame_seq=43 frame_debug=0xe1 bytes_in=6227\n",
        encoding="utf-8",
    )
    non_yuv = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
        "--noise-report", str(noise),
        "--status-log", str(non_yuv_status),
        "--min-bytes-in", "512",
    )
    require(non_yuv.returncode == 7 and
            "frame store refused non-YUV doorbell (0xE1)" in non_yuv.stderr and
            "non-YUV DDR doorbell format error" in non_yuv.stderr and
            '"delivery_verdict":' in non_yuv.stdout and
            '"id": "NO_FRAME_DELIVERED"' in non_yuv.stdout,
            "frame_debug=0xe1 must be surfaced as a named non-YUV doorbell freshness failure, "
            f"not graded\nstdout={non_yuv.stdout}\nstderr={non_yuv.stderr}")

    debug_state_non_yuv_status = WORK / "status_non_yuv_debug_state.txt"
    debug_state_non_yuv_status.write_text(
        "status has_frame=1 has_stream=1 has_idr=1 sps_valid=1 pps_valid=1 "
        "frame_bank=1 frame_format=yuv420p frame_seq=44 debug_state=0x1234e1 bytes_in=6227\n",
        encoding="utf-8",
    )
    debug_state_non_yuv = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(status_skewed_colour),
        "--noise-report", str(noise),
        "--status-log", str(debug_state_non_yuv_status),
        "--min-bytes-in", "512",
    )
    require(debug_state_non_yuv.returncode == 7 and
            "debug_state=0xe1" in debug_state_non_yuv.stderr and
            "frame store refused non-YUV doorbell (0xE1)" in debug_state_non_yuv.stderr and
            '"delivery_reason": "non_yuv_doorbell_refusal"' in debug_state_non_yuv.stdout and
            "COLOUR_PATH_DEFECT" not in (debug_state_non_yuv.stdout + debug_state_non_yuv.stderr),
            "debug_state low byte 0xe1 must refuse before a channel-skewed image can be "
            "labelled as a colour defect\n"
            f"stdout={debug_state_non_yuv.stdout}\nstderr={debug_state_non_yuv.stderr}")
    print("PASS ARM absent/0xe1 status overrides colour-shape classification before grading pixels")

    v4l2_log = "[video4linux2,v4l2 @ 0x123] Dequeued v4l2 buffer contains corrupted data (0 bytes)."
    require(hw_visual_compare.classify_capture_log(v4l2_log) == "corrupt",
            "actual V4L2 corrupt-buffer wording was not classified as corrupt")
    mjpeg_log = "Error submitting packet to decoder: Invalid data found when processing input"
    require(hw_visual_compare.classify_capture_log(mjpeg_log) == "corrupt",
            "MJPEG decoder invalid-data wording was not classified as corrupt")
    require(WCAP_CORRUPT_LOG.exists(), "W-CAP corrupt capture log fixture missing")
    wcap_log = WCAP_CORRUPT_LOG.read_text()
    wcap_640_log = WCAP_CORRUPT_640_LOG.read_text()
    require("1843200 bytes" in wcap_log and "yuyv422, 1280x720" in wcap_log,
            "W-CAP 1280x720 fixture lost exact corrupt-buffer details")
    require("614400 bytes" in wcap_640_log,
            "W-CAP 640x480 fixture lost exact corrupt-buffer details")
    require(hw_visual_compare.classify_capture_log(wcap_log) == "corrupt",
            "W-CAP fe7673bc corrupt capture log fixture was not classified as corrupt")
    require(hw_visual_compare.classify_capture_log(wcap_640_log) == "corrupt",
            "W-CAP fe7673bc 640x480 corrupt capture log fixture was not classified as corrupt")
    corrupt_logged = run(
        "compare",
        "--golden", str(valid_golden),
        *COLOR_ARGS,
        *VALID_PROVENANCE_ARGS,
        "--capture", str(cap2),
        "--capture-log", str(WCAP_CORRUPT_LOG),
        "--noise-report", str(noise),
    )
    require(corrupt_logged.returncode == 4,
            "compare with W-CAP corrupt capture log must return capture-integrity rc=4, "
            f"not grade pixels\nstdout={corrupt_logged.stdout}\nstderr={corrupt_logged.stderr}")
    print("PASS V4L2 corrupt-buffer diagnostics classified distinctly")

    absent_capture = run(
        "capture",
        "--out", str(WORK / "absent_capture.png"),
        "--device", str(WORK / "no_such_video_device"),
    )
    require(absent_capture.returncode == 5 and "is absent" in absent_capture.stderr,
            "absent capture device must not silently pass, "
            f"stdout={absent_capture.stdout}\nstderr={absent_capture.stderr}")
    original_run = hw_visual_compare.subprocess.run
    try:
        def missing_ffmpeg(*_args, **_kwargs):
            raise FileNotFoundError("ffmpeg")

        hw_visual_compare.subprocess.run = missing_ffmpeg
        try:
            hw_visual_compare.capture_v4l2_once(
                WORK / "ffmpeg_missing.png", str(WORK / "fake_video_device"),
                "mjpeg", "1280x720", "60", 0, "bt601", "full"
            )
        except hw_visual_compare.HarnessError as e:
            require("ffmpeg" in str(e), f"missing ffmpeg refusal was unclear: {e}")
        else:
            raise AssertionError("missing ffmpeg dependency did not raise a harness error")
    finally:
        hw_visual_compare.subprocess.run = original_run
    print("PASS absent capture device/dependency paths are explicit refusals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
