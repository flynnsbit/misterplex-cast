#!/usr/bin/env python3
"""Refuse to let unresolved git conflict markers reach a commit.

A merge committed with markers still in place is silent corruption: the file
still "exists", tooling may still parse most of it, and the damage surfaces
later as an unrelated-looking failure. Makefile recipes, shell gates and RTL
file lists are all shapes where a stray marker can disable a check rather than
break it loudly.

Run RED-first: sabotage a tracked file with a marker and confirm this exits 1.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Markers are matched at start-of-line only, which is where git writes them.
MARKERS = ("<<<<<<< ", ">>>>>>> ", "|||||||  ")
BARE_MID = "======="

# This file necessarily contains marker-shaped text.
SELF = Path(__file__).resolve()


def tracked_files():
    out = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z"],
        check=True, capture_output=True, text=True,
    ).stdout
    return [ROOT / p for p in out.split("\0") if p]


def main():
    bad = []
    for path in tracked_files():
        if path.resolve() == SELF or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError):
            continue  # binary or unreadable: markers are not a concern
        for lineno, line in enumerate(text.splitlines(), 1):
            if line.startswith(MARKERS) or line.rstrip() == BARE_MID:
                # A bare ======= is legitimate as a text rule/underline, so it
                # only counts when a real <<<<<<< opener exists in the file.
                if line.rstrip() == BARE_MID and not any(
                    l.startswith("<<<<<<< ") for l in text.splitlines()
                ):
                    continue
                bad.append(f"{path.relative_to(ROOT)}:{lineno}: {line[:80]}")

    if bad:
        print("FAIL: unresolved conflict markers in tracked files:", file=sys.stderr)
        for b in bad:
            print(f"  {b}", file=sys.stderr)
        return 1

    print("OK: no unresolved conflict markers in tracked files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
