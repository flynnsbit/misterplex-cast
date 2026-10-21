#!/usr/bin/env python3
"""Post-fit guard for critical modules in Quartus hierarchy reports."""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONFIG = ROOT / "tests" / "fixtures" / "critical_fit_hierarchy.json"


@dataclass
class HierRow:
    source: str
    hierarchy_node: str
    full_hierarchy: str
    entity: str
    comb_aluts: float = 0.0
    registers: float = 0.0
    block_memory_bits: float = 0.0
    m10ks: float = 0.0
    dsps: float = 0.0
    alms_needed: float = 0.0


def parse_number(text: str) -> float:
    m = re.search(r"-?\d+(?:,\d{3})*(?:\.\d+)?", text)
    return float(m.group(0).replace(",", "")) if m else 0.0


def split_report_row(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip().strip(";").split(";")]


def normalize_header(cell: str) -> str:
    cell = re.sub(r"\s+", " ", cell.strip())
    return cell


def parse_hierarchy_report(path: Path) -> list[HierRow]:
    rows: list[HierRow] = []
    if not path.exists():
        raise FileNotFoundError(path)

    header: list[str] | None = None
    for line in path.read_text(errors="ignore").splitlines():
        if "Compilation Hierarchy Node" in line and "Full Hierarchy Name" in line:
            header = [normalize_header(c) for c in split_report_row(line)]
            continue
        if not header or not line.lstrip().startswith(";"):
            continue
        cells = split_report_row(line)
        if len(cells) != len(header) or not cells or not cells[0].startswith("|"):
            continue
        data = dict(zip(header, cells))
        entity = data.get("Entity Name", "")
        full = data.get("Full Hierarchy Name", "")
        if not entity or not full:
            continue
        rows.append(
            HierRow(
                source=str(path),
                hierarchy_node=data.get("Compilation Hierarchy Node", ""),
                full_hierarchy=full,
                entity=entity,
                comb_aluts=parse_number(data.get("Combinational ALUTs", "0")),
                registers=parse_number(data.get("Dedicated Logic Registers", "0")),
                block_memory_bits=parse_number(data.get("Block Memory Bits", "0")),
                m10ks=parse_number(data.get("M10Ks", "0")),
                dsps=parse_number(data.get("DSP Blocks", "0")),
                alms_needed=parse_number(data.get("ALMs needed [=A-B+C]", "0")),
            )
        )
    return rows


def load_config(path: Path) -> list[dict[str, object]]:
    data = json.loads(path.read_text())
    return list(data.get("modules", []))


def find_row(rows: list[HierRow], spec: dict[str, object]) -> HierRow | None:
    entity = str(spec.get("entity", ""))
    contains = str(spec.get("hierarchy_contains", ""))
    candidates = [
        row
        for row in rows
        if (not entity or row.entity == entity)
        and (not contains or contains in row.full_hierarchy or contains in row.hierarchy_node)
    ]
    if not candidates:
        return None
    return max(
        candidates,
        key=lambda row: row.comb_aluts + row.registers + row.block_memory_bits / 1000.0 + row.m10ks * 100,
    )


def scan_log(path: Path | None, module_names: list[str]) -> list[str]:
    if not path or not path.exists():
        return []
    danger = re.compile(
        r"removed|stuck|does not drive|doesn't drive|no clock|without a clock|dangling|"
        r"tied (?:to|off)|constant (?:GND|VCC|0|1)|stuck at (?:GND|VCC)",
        re.I,
    )
    module_re = re.compile("|".join(re.escape(name) for name in module_names), re.I)
    hits: list[str] = []
    for line in path.read_text(errors="ignore").splitlines():
        if module_re.search(line) and danger.search(line):
            hits.append(line.strip())
    return hits


def scan_comb_loops(path: Path | None, specs: list[dict[str, object]]) -> list[str]:
    if not path or not path.exists():
        return []
    allow = {
        str(item)
        for spec in specs
        for item in spec.get("allowed_comb_loop_nodes", [])
    }
    contexts = [
        str(spec.get("hierarchy_contains", "")) for spec in specs if spec.get("hierarchy_contains")
    ] + [
        str(spec.get("log_contains", "")) for spec in specs if spec.get("log_contains")
    ] + [str(spec.get("name", "")) for spec in specs if spec.get("name")]
    hits: list[str] = []
    current: list[str] = []
    active = False
    for line in path.read_text(errors="ignore").splitlines():
        if "Warning (332125): Found combinational loop" in line:
            current = [line.strip()]
            active = True
            continue
        if active and "Warning (332126): Node" in line:
            current.append(line.strip())
            continue
        if active:
            text = "\n".join(current)
            if any(ctx and ctx in text for ctx in contexts) and not any(token and token in text for token in allow):
                hits.extend(current)
            current = []
            active = False
    if active:
        text = "\n".join(current)
        if any(ctx and ctx in text for ctx in contexts) and not any(token and token in text for token in allow):
            hits.extend(current)
    return hits


def format_table(rows: list[tuple[dict[str, object], HierRow | None]]) -> str:
    lines = [
        "| module | entity | hierarchy | comb ALUTs | registers | block bits | M10Ks | DSPs | status |",
        "|---|---|---|---:|---:|---:|---:|---:|---|",
    ]
    for spec, row in rows:
        if not row:
            lines.append(
                f"| `{spec.get('name')}` | `{spec.get('entity')}` | `{spec.get('hierarchy_contains')}` | — | — | — | — | — | MISSING |"
            )
            continue
        lines.append(
            f"| `{spec.get('name')}` | `{row.entity}` | `{row.full_hierarchy}` | "
            f"{row.comb_aluts:g} | {row.registers:g} | {row.block_memory_bits:g} | "
            f"{row.m10ks:g} | {row.dsps:g} | present |"
        )
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fit-rpt", type=Path, required=True)
    ap.add_argument("--map-rpt", type=Path)
    ap.add_argument("--log", type=Path, help="Quartus compile log to scan for removal/tie-off warnings")
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    ap.add_argument(
        "--allow-missing",
        action="append",
        default=[],
        help="Module names that may be absent (L4 720p present stubs stream_path)",
    )
    args = ap.parse_args(argv[1:])

    try:
        rows = parse_hierarchy_report(args.fit_rpt)
        if args.map_rpt:
            rows += parse_hierarchy_report(args.map_rpt)
    except FileNotFoundError as e:
        print(f"FIT_HIERARCHY_REFUSED(exit=4): missing report {e.filename}", file=sys.stderr)
        return 4

    specs = load_config(args.config)
    found = [(spec, find_row(rows, spec)) for spec in specs]
    print("FIT_HIERARCHY_TABLE_BEGIN")
    print(format_table(found))
    print("FIT_HIERARCHY_TABLE_END")

    errors: list[str] = []
    allow_missing = set(args.allow_missing)
    for spec, row in found:
        name = str(spec.get("name"))
        if not row:
            if name in allow_missing:
                continue
            errors.append(f"{name}: missing from Quartus fitted hierarchy")
            continue
        checks = [
            ("comb_aluts", row.comb_aluts),
            ("registers", row.registers),
            ("block_memory_bits", row.block_memory_bits),
            ("m10ks", row.m10ks),
            ("dsps", row.dsps),
        ]
        for field, value in checks:
            key = "min_" + field
            if key in spec and value < float(spec[key]):
                errors.append(f"{name}: {field} {value:g} < required {spec[key]}")

    warning_hits = scan_log(args.log, [str(spec.get("name")) for spec in specs])
    if warning_hits:
        errors.append("critical-module removal/tie-off warning(s):")
        errors.extend("  " + hit for hit in warning_hits[:20])
    comb_hits = scan_comb_loops(args.log, specs)
    if comb_hits:
        errors.append("critical-module combinational-loop warning(s):")
        errors.extend("  " + hit for hit in comb_hits[:20])

    if errors:
        print("FIT_HIERARCHY_REJECTED(exit=1):", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("PASS fit hierarchy: critical modules present with non-trivial resource usage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
