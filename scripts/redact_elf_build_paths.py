#!/usr/bin/env python3
"""Overwrite personal build-host paths in a shipped ELF (GH#28 T3).

FFmpeg bakes --prefix=/home/<user>/... into av_get_configuration() and
__FILE__. The daemon link already uses -ffile-prefix-map for this tree;
static libav was configured elsewhere. Same-length overwrite keeps the
ELF intact. T3 forbids /home/<user>/, /Users/, and /work/ or /work/ roots.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Same shapes test_clean_install.sh T3 greps for in bin/misterplexd.
PATH_RE = re.compile(
    br"(?:(?<![A-Za-z0-9./])/home/[a-z]+/)|(?:/Users/[A-Za-z][A-Za-z0-9._-]*/)|(?:/work(?:space)?/)"
)


def redact_bytes(data: bytes) -> tuple[bytes, int]:
    out = bytearray(data)
    n = 0
    for match in PATH_RE.finditer(data):
        start, end = match.start(), match.end()
        span = end - start
        out[start:end] = b"/" + b"." * (span - 1)
        n += 1
    return bytes(out), n


def redact_file(path: Path) -> int:
    data = path.read_bytes()
    new, n = redact_bytes(data)
    if n:
        path.write_bytes(new)
    return n


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not args.elf.is_file():
        print(f"redact_elf_build_paths: missing {args.elf}", file=sys.stderr)
        return 2
    if args.dry_run:
        _, n = redact_bytes(args.elf.read_bytes())
        print(f"would redact {n} build-host path(s) in {args.elf}")
        return 0
    n = redact_file(args.elf)
    print(f"redacted {n} build-host path(s) in {args.elf}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
