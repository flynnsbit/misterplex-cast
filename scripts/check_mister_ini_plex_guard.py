#!/usr/bin/env python3
"""Verify MiSTer.ini keeps required [Plex] pins. [PlexOB] Y/C keys are checked only if present."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REFERENCE = ROOT / "assets/MiSTer.ini.Plex.required"

PLEXOB_VGA = frozenset({"rgb", "svideo", "cvbs"})
PLEXOB_NTSC = frozenset({"0", "1", "2"})


def parse_ini_section(path: Path, section: str, case_insensitive: bool = False) -> tuple[bool, dict[str, str]]:
    found = False
    active = False
    values: dict[str, str] = {}
    want = section.lower() if case_insensitive else section
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1].strip()
            active = name.lower() == want if case_insensitive else name == section
            found = found or active
            continue
        if not active or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.split(";", 1)[0].split("#", 1)[0].strip()
    return found, values


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ini", type=Path)
    ap.add_argument(
        "--reference",
        type=Path,
        default=DEFAULT_REFERENCE,
        help="required [Plex] key/value reference",
    )
    args = ap.parse_args(argv)

    problems: list[str] = []
    try:
        ref_found, required = parse_ini_section(args.reference, "Plex")
        got_found, actual = parse_ini_section(args.ini, "Plex")
        if not ref_found:
            raise ValueError("[Plex] section is missing from the reference")
        if not got_found:
            raise ValueError("[Plex] section is missing")
    except Exception as e:
        print(
            f"FAIL: MiSTer.ini [Plex] guard could not parse input: {e}. "
            "Why it matters: missing or commented video_mode pins let MiSTer retune the Plex "
            "core and can scramble the user's display. Restore the checked-in "
            "assets/MiSTer.ini.Plex.required keys in the live [Plex] section.",
            file=sys.stderr,
        )
        return 1

    for key, want in required.items():
        got = actual.get(key)
        if got != want:
            problems.append(f"[Plex] {key}: expected {want!r}, got {got!r}")

    try:
        ob_found, plexob = parse_ini_section(args.ini, "PlexOB", case_insensitive=True)
    except Exception as e:
        problems.append(f"[PlexOB] {e}")
        ob_found = False
        plexob = {}
    if ob_found:
        if "vga_mode" in plexob and plexob["vga_mode"] not in PLEXOB_VGA:
            problems.append(f"[PlexOB] vga_mode: invalid value {plexob['vga_mode']!r}")
        if "ntsc_mode" in plexob and plexob["ntsc_mode"] not in PLEXOB_NTSC:
            problems.append(f"[PlexOB] ntsc_mode: invalid value {plexob['ntsc_mode']!r}")
        scaler = plexob.get("vga_scaler")
        if scaler is not None and scaler not in {"0", "1"}:
            problems.append(f"[PlexOB] vga_scaler: invalid value {scaler!r}")

    if problems:
        print("FAIL: MiSTer.ini [Plex] video-mode guard mismatch.", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(
            "Why it matters: video_mode/video_mode_ntsc/video_mode_pal are deliberate "
            "anti-retune pins for the Plex core. If they are commented out or changed, "
            "MiSTer may choose an unsafe mode and scramble the display. Restore the "
            "reference keys or intentionally update assets/MiSTer.ini.Plex.required "
            "with lab evidence. [PlexOB] vga_mode/ntsc_mode are checked only when present; "
            "their absence means rgb / ntsc 0.",
            file=sys.stderr,
        )
        return 1

    print(f"PASS MiSTer.ini [Plex] guard: {args.ini}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
