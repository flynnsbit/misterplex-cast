#!/usr/bin/env python3
"""Validate Plex.sv CONF_STR structure before it can garble MiSTer's OSD."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PLEX_SV = ROOT / "fpga/Plex_MiSTer/Plex.sv"


def fail(msg: str) -> None:
    print(f"FAIL: CONF_STR guard: {msg}", file=sys.stderr)
    raise SystemExit(1)


def extract_conf_strings(path: Path) -> list[str]:
    text = path.read_text()
    m = re.search(r"localparam\s+CONF_STR\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        fail(f"{path} has no `localparam CONF_STR = {{...}};` block")
    body = m.group(1)
    strings = [bytes(s, "utf-8").decode("unicode_escape")
               for s in re.findall(r'"((?:\\.|[^"\\])*)"', body)]
    if not strings:
        fail(f"{path} CONF_STR contains no string literals")
    return strings


def conf_entries(strings: list[str]) -> list[str]:
    joined = "".join(strings)
    entries: list[str] = []
    for raw in joined.split(";"):
        entry = raw.strip()
        if entry and entry != "V,v":
            entries.append(entry)
    return entries


def bit_range(spec: str) -> tuple[int, int]:
    m = re.fullmatch(r"\[(\d+)(?::(\d+))?\]", spec)
    if not m:
        fail(f"bad bit range `{spec}`")
    a = int(m.group(1))
    b = int(m.group(2) if m.group(2) is not None else m.group(1))
    return min(a, b), max(a, b)


def validate_file(entry: str) -> None:
    parts = entry.split(",")
    if len(parts) != 3:
        fail(
            f"`{entry}` has {len(parts)} comma fields; F entries must be exactly "
            "F#,ext,label. Extra commas usually mean a file-extension field was "
            "miscounted and will corrupt later OSD text. MiSTer scans extensions "
            "in fixed 3-character chunks."
        )
    slot, exts, label = parts
    if not re.fullmatch(r"F\d+", slot):
        fail(f"`{entry}` has invalid file slot `{slot}`")
    if not label.strip():
        fail(f"`{entry}` has an empty file label")
    if "," in exts or len(exts) == 0 or len(exts) % 3 != 0:
        fail(
            f"`{entry}` has malformed extension field `{exts}`. MiSTer scans file "
            "extensions in fixed 3-character chunks; a miscount here is what turns "
            "OSD text into fragments like `*.raw,RBS,565, fr,ame`."
        )
    chunks = [exts[i:i + 3] for i in range(0, len(exts), 3)]
    for chunk in chunks:
        if not re.fullmatch(r"[A-Za-z0-9]{3}", chunk):
            fail(f"`{entry}` has non-3char extension chunk `{chunk}`")


def validate_option(entry: str, used: dict[int, str]) -> None:
    parts = entry.split(",")
    head = parts[0]
    m = re.fullmatch(r"O(\[\d+(?::\d+)?\])", head)
    if not m:
        fail(f"`{entry}` has invalid O entry head `{head}`")
    lo, hi = bit_range(m.group(1))
    width = hi - lo + 1
    options = parts[2:]
    expected = 1 << width
    if len(parts) < 4:
        fail(f"`{entry}` must have a label and at least two choices")
    if len(options) != expected:
        fail(
            f"`{entry}` has {len(options)} choices for {width} status bit(s); "
            f"expected exactly {expected}. Fix the CONF_STR field count or change "
            "the bit range intentionally."
        )
    for bit in range(lo, hi + 1):
        if bit in used:
            fail(f"`{entry}` overlaps status bit {bit} already owned by `{used[bit]}`")
        used[bit] = entry


def validate_trigger_or_reset(entry: str, used: dict[int, str]) -> None:
    parts = entry.split(",")
    if len(parts) != 2:
        fail(f"`{entry}` must have exactly two fields")
    head = parts[0]
    m = re.fullmatch(r"[TR](\[\d+\])", head)
    if not m:
        fail(f"`{entry}` has invalid trigger/reset head `{head}`")
    lo, hi = bit_range(m.group(1))
    if lo != hi:
        fail(f"`{entry}` must name a single bit")
    owner = used.get(lo)
    if owner and lo != 0:
        fail(f"`{entry}` overlaps status bit {lo} already owned by `{owner}`")
    used[lo] = owner or entry


def validate(entries: list[str]) -> None:
    if not entries or entries[0] != "Plex":
        fail("first CONF_STR entry must be the core title `Plex`")
    used: dict[int, str] = {}
    seen: set[str] = set()
    for entry in entries[1:]:
        if entry == "-":
            continue
        if entry.startswith("F"):
            validate_file(entry)
            seen.add(entry.split(",", 1)[0])
        elif entry.startswith("O"):
            validate_option(entry, used)
            seen.add(entry.split(",", 2)[0])
        elif entry.startswith(("T", "R")):
            validate_trigger_or_reset(entry, used)
            seen.add(entry.split(",", 1)[0])
        elif entry.startswith("J"):
            parts = entry.split(",")
            if len(parts) < 2 or not re.fullmatch(r"J\d+", parts[0]):
                fail(f"`{entry}` has invalid joystick mapper syntax")
            seen.add(parts[0])
        elif entry.startswith("v,"):
            parts = entry.split(",")
            if len(parts) != 2 or not parts[1].isdigit():
                fail(f"`{entry}` has invalid config-version syntax")
            seen.add("v")
        else:
            fail(f"unrecognised entry `{entry}`")

    # v8+: O[5:4] content resolution (240p/480p/720p). Accept legacy O[4] too.
    required = ["F1", "F2", "F3", "O[9:6]", "O[15:14]", "J1", "v"]
    for req in required:
        if req not in seen:
            fail(f"required CONF_STR entry `{req}` is missing")
    if "O[5:4]" not in seen and "O[4]" not in seen:
        fail("required CONF_STR entry `O[5:4]` (or legacy `O[4]`) is missing")
    print("PASS CONF_STR guard: structure, field counts, file slots and status bits are sane")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("plex_sv", nargs="?", type=Path, default=DEFAULT_PLEX_SV)
    args = ap.parse_args(argv)
    validate(conf_entries(extract_conf_strings(args.plex_sv)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
