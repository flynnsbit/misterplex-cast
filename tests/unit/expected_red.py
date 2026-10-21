#!/usr/bin/env python3
"""Machine-readable expected-red validation for intentional fault checks."""
from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests/expected_red_manifest.json"


class ExpectedRedError(RuntimeError):
    pass


def _entries() -> dict[str, dict[str, object]]:
    data = json.loads(MANIFEST.read_text())
    return {entry["id"]: entry for entry in data.get("entries", [])}


def require_expected_red(red_id: str, output: str, returncode: int) -> None:
    entries = _entries()
    if red_id not in entries:
        raise ExpectedRedError(f"{red_id}: missing from {MANIFEST}")
    entry = entries[red_id]
    if returncode == 0:
        raise ExpectedRedError(f"{red_id}: fault command unexpectedly returned rc=0")
    missing = [s for s in entry.get("required_substrings", []) if s not in output]
    if missing:
        raise ExpectedRedError(f"{red_id}: missing expected red substring: {missing[0]}")
    print(
        f"EXPECTED_RED {red_id}: rc={returncode} matched "
        f"{len(entry.get('required_substrings', []))} manifest substring(s) "
        f"({entry.get('owner', 'unknown owner')})"
    )


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <expected-red-id> <returncode>", file=sys.stderr)
        return 2
    red_id = argv[1]
    try:
        returncode = int(argv[2])
    except ValueError:
        print(f"expected integer returncode, got {argv[2]!r}", file=sys.stderr)
        return 2
    output = sys.stdin.read()
    try:
        require_expected_red(red_id, output, returncode)
    except ExpectedRedError as e:
        print(f"FAIL expected-red manifest: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
