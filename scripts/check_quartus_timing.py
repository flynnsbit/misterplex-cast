#!/usr/bin/env python3
"""Fail Quartus STA reports with negative timing slack."""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class SlackRow:
    section: str
    clock: str
    slack: float
    tns: float


def cells(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip(";").split(";")]


def number(text: str) -> float:
    m = re.search(r"-?\d+(?:\.\d+)?", text)
    return float(m.group(0)) if m else 0.0


def parse_slack_rows(path: Path) -> list[SlackRow]:
    rows: list[SlackRow] = []
    section = ""
    saw_header = False
    section_rows = 0
    for line in path.read_text(errors="ignore").splitlines():
        m = re.match(r"; (Setup|Hold|Recovery|Removal|Minimum Pulse Width) Summary\s+;", line)
        if m:
            section = m.group(1)
            saw_header = False
            section_rows = 0
            continue
        if section and "Clock" in line and "Slack" in line:
            saw_header = True
            continue
        if section and saw_header and line.startswith("+---") and section_rows > 0:
            section = ""
            saw_header = False
            section_rows = 0
            continue
        if section and saw_header and line.startswith(";"):
            parts = cells(line)
            if len(parts) >= 3 and parts[0] and not parts[0].startswith("+") and parts[0] != "Clock":
                rows.append(SlackRow(section, parts[0], number(parts[1]), number(parts[2])))
                section_rows += 1
            continue
    return rows


def parse_fmax_rows(path: Path) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    in_fmax = False
    saw_header = False
    for line in path.read_text(errors="ignore").splitlines():
        if line.startswith("; Fmax Summary"):
            in_fmax = True
            continue
        if in_fmax and "Fmax" in line and "Clock Name" in line:
            saw_header = True
            continue
        if in_fmax and saw_header and line.startswith(";"):
            parts = cells(line)
            if len(parts) >= 3 and parts[0] and not parts[0].startswith("+"):
                rows.append((parts[2], parts[0], parts[1]))
            continue
        if in_fmax and saw_header and not line.strip():
            break
    return rows


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sta-rpt", type=Path, required=True)
    args = ap.parse_args(argv[1:])
    if not args.sta_rpt.exists():
        print(f"QUARTUS_TIMING_REFUSED(exit=4): missing STA report {args.sta_rpt}", file=sys.stderr)
        return 4

    fmax = parse_fmax_rows(args.sta_rpt)
    slack_rows = parse_slack_rows(args.sta_rpt)
    print("TIMING_FMAX_TABLE_BEGIN")
    print("| clock | Fmax | Restricted Fmax |")
    print("|---|---:|---:|")
    for clock, fmax_value, restricted in fmax:
        print(f"| `{clock}` | {fmax_value} | {restricted} |")
    print("TIMING_FMAX_TABLE_END")
    print("TIMING_SLACK_TABLE_BEGIN")
    print("| section | clock | slack | endpoint TNS |")
    print("|---|---|---:|---:|")
    for row in slack_rows:
        print(f"| {row.section} | `{row.clock}` | {row.slack:g} | {row.tns:g} |")
    print("TIMING_SLACK_TABLE_END")

    failures = [row for row in slack_rows if row.slack < 0 or row.tns < 0]
    if failures:
        print("QUARTUS_TIMING_REJECTED(exit=1): negative slack/TNS", file=sys.stderr)
        for row in failures:
            print(
                f"  {row.section}: {row.clock}: slack={row.slack:g} endpoint_tns={row.tns:g}",
                file=sys.stderr,
            )
        return 1
    print("PASS timing: no negative setup/hold/recovery/removal/min-pulse slack")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
