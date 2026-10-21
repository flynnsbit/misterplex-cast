#!/usr/bin/env python3
"""Gate Quartus-vs-Verilator macro parity for product RTL.

The product Verilator/lint pass must compile the same feature macro set that
Quartus synthesizes. Targeted test-only fault macros are allowed only when they
are explicitly listed in tests/fixtures/define_parity_allowlist.json.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "fpga" / "Plex_MiSTer"
DEFAULT_ALLOWLIST = ROOT / "tests" / "fixtures" / "define_parity_allowlist.json"


@dataclass(frozen=True)
class Macro:
    name: str
    value: str
    source: str


def strip_hash_comment(line: str) -> str:
    return line.split("#", 1)[0].strip()


def normalize_value(value: str | None) -> str:
    if value is None or value == "":
        return "1"
    return value.strip().strip('"')


def parse_verilog_macro(raw: str, source: str) -> Macro | None:
    raw = raw.strip().strip('"')
    m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)(?:=(.*))?$", raw)
    if not m:
        return None
    return Macro(m.group(1), normalize_value(m.group(2)), source)


def discover_quartus_macros(qsf: Path = PROJECT / "Plex.qsf") -> dict[str, Macro]:
    macros: dict[str, Macro] = {}
    for line_no, raw in enumerate(qsf.read_text(errors="ignore").splitlines(), 1):
        line = strip_hash_comment(raw)
        m = re.search(r"\bset_global_assignment\b.*?-name\s+VERILOG_MACRO\s+(.+)$", line)
        if not m:
            continue
        macro = parse_verilog_macro(m.group(1), f"{qsf.relative_to(ROOT)}:{line_no}")
        if macro:
            macros[macro.name] = macro

    build_id = PROJECT / "sys" / "build_id.tcl"
    if build_id.exists() and "`define BUILD_DATE" in build_id.read_text(errors="ignore"):
        macros["BUILD_DATE"] = Macro(
            "BUILD_DATE",
            "<generated>",
            f"{build_id.relative_to(ROOT)}:8",
        )
    return macros


def verilator_lint_macros(qsf: Path = PROJECT / "Plex.qsf") -> dict[str, Macro]:
    return {
        name: Macro(name, "lint" if name == "BUILD_DATE" else macro.value, "scripts/rtl_lint.py:qsf-macro-injection")
        for name, macro in discover_quartus_macros(qsf).items()
    }


def verilator_define_args(qsf: Path = PROJECT / "Plex.qsf") -> list[str]:
    args: list[str] = []
    for name, macro in sorted(verilator_lint_macros(qsf).items()):
        value = macro.value
        if name == "BUILD_DATE":
            # Argv for subprocess, not a shell: Verilator must see the string "lint".
            value = '"lint"'
        args.append(f"-D{name}={value}")
    return args


def discover_test_macros(paths: list[Path] | None = None) -> dict[str, list[Macro]]:
    if paths is None:
        paths = [ROOT / "tests" / "unit", ROOT / "tests" / "rtl", ROOT / "scripts", ROOT / "Makefile"]
    files: list[Path] = []
    for path in paths:
        if path.is_dir():
            files.extend(p for p in path.rglob("*") if p.is_file())
        elif path.is_file():
            files.append(path)

    found: dict[str, list[Macro]] = {}
    patterns = [
        re.compile(r"\+define\+([A-Za-z_][A-Za-z0-9_]*)(?:=([^\s\\]+))?"),
        re.compile(r"(?<!\w)-D([A-Za-z_][A-Za-z0-9_]*)(?:=([^\s\"']+))?"),
    ]
    for path in files:
        if path.resolve() == Path(__file__).resolve():
            continue
        if path.suffix not in {".sh", ".py", ".sv", ".svh", ".v", ""} and path.name != "Makefile":
            continue
        rel = path.relative_to(ROOT).as_posix()
        text = path.read_text(errors="ignore")
        for pattern in patterns:
            for m in pattern.finditer(text):
                name = m.group(1)
                value = normalize_value(m.group(2))
                found.setdefault(name, []).append(Macro(name, value, rel))

    return {name: sorted(items, key=lambda m: m.source) for name, items in sorted(found.items())}


def load_allowlist(path: Path = DEFAULT_ALLOWLIST) -> tuple[dict[str, str], set[str], dict[str, object]]:
    data = json.loads(path.read_text())
    return (
        {str(k): str(v) for k, v in data.get("test_only_macros", {}).items()},
        {str(k) for k in data.get("volatile_value_macros", {})},
        dict(data.get("libc_feature_test_macros", {})),
    )


def format_table(quartus: dict[str, Macro], lint: dict[str, Macro], tests: dict[str, list[Macro]],
                 allow_test: dict[str, str], volatile_values: set[str], allow_libc: dict[str, object]) -> str:
    names = sorted(set(quartus) | set(lint) | set(tests))
    lines = ["| macro | Quartus | Verilator/lint | Verilator targeted tests | status |",
             "|---|---|---|---|---|"]
    for name in names:
        q = quartus.get(name)
        l = lint.get(name)
        t = tests.get(name, [])
        qv = f"{q.value} ({q.source})" if q else "—"
        lv = f"{l.value} ({l.source})" if l else "—"
        tv = "<br>".join(f"{m.value} ({m.source})" for m in t) if t else "—"
        if q and l:
            if q.value != l.value and name not in volatile_values:
                status = "VALUE-DIFF"
            else:
                status = "shared"
        elif t and not q and not l and name in allow_libc:
            status = f"allowed libc feature-test: {allow_libc[name]}"
        elif t and not q and not l and name in allow_test:
            status = f"allowed test-only: {allow_test[name]}"
        elif q and not l:
            status = "QUARTUS-ONLY"
        elif l and not q:
            status = "VERILATOR-LINT-ONLY"
        elif t and not q and not l:
            status = "UNDECLARED TEST-ONLY"
        else:
            status = "mixed"
        lines.append(f"| `{name}` | {qv} | {lv} | {tv} | {status} |")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    ap.add_argument("--qsf", type=Path, default=PROJECT / "Plex.qsf")
    ap.add_argument("--drop-verilator-macro", action="append", default=[],
                    help="self-test hook: remove a product lint macro before comparison")
    args = ap.parse_args(argv[1:])

    quartus = discover_quartus_macros(args.qsf)
    lint = verilator_lint_macros(args.qsf)
    for name in args.drop_verilator_macro:
        lint.pop(name, None)
    tests = discover_test_macros()
    allow_test, volatile_values, allow_libc = load_allowlist(args.allowlist)

    print("DEFINE_PARITY_TABLE_BEGIN")
    print(format_table(quartus, lint, tests, allow_test, volatile_values, allow_libc))
    print("DEFINE_PARITY_TABLE_END")

    errors: list[str] = []
    for name, macro in sorted(quartus.items()):
        if name not in lint:
            errors.append(f"{name}: set by Quartus at {macro.source} but absent from Verilator/lint product macros")
    for name, macro in sorted(lint.items()):
        if name not in quartus:
            errors.append(f"{name}: set by Verilator/lint at {macro.source} but absent from Quartus product macros")
    for name in sorted(set(quartus) & set(lint)):
        if quartus[name].value != lint[name].value and name not in volatile_values:
            errors.append(f"{name}: Quartus value {quartus[name].value!r} != Verilator/lint value {lint[name].value!r}")
    for name, why in sorted(allow_libc.items()):
        if not isinstance(why, str) or not why.strip() or name in allow_test:
            errors.append(f"{name}: libc_feature_test_macros needs a reason string and no other class")
    for name in sorted(set(tests) - set(quartus) - set(lint)):
        if name not in allow_test and name not in allow_libc:
            sources = ", ".join(m.source for m in tests[name])
            errors.append(f"{name}: test-only macro is not declared in {args.allowlist} ({sources})")

    if errors:
        print("DEFINE_PARITY_REJECTED(exit=1):", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("PASS define parity: Quartus product macros match Verilator/lint; test-only macros are allowlisted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
