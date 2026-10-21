#!/usr/bin/env python3
"""Generate a 320x240 RGB565 little-endian raw frame for MiSTerPlex F1 load.

Usage:
  python3 scripts/gen_test_frame.py [out.raw]
Copy to MiSTer SD and load via OSD F1, then set Video source = Frame store.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "plex_test_320x240.rgb565")
    w, h = 320, 240
    buf = bytearray()
    for y in range(h):
        for x in range(w):
            # SMPTE-ish bars + moving diagonal stripe
            bar = (x * 8) // w
            colors = [
                (180, 180, 180),
                (180, 180, 16),
                (16, 180, 180),
                (16, 180, 16),
                (180, 16, 180),
                (180, 16, 16),
                (16, 16, 180),
                (16, 16, 16),
            ]
            r, g, b = colors[bar]
            if abs((x - y) % 40) < 3:
                r, g, b = 255, 64, 0
            # corner markers
            if x < 8 or y < 8 or x >= w - 8 or y >= h - 8:
                r, g, b = 255, 255, 0
            pix = rgb565(r, g, b)
            buf += struct.pack("<H", pix)
    out.write_bytes(buf)
    print(f"wrote {out} ({len(buf)} bytes = {w}x{h} RGB565 LE)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
