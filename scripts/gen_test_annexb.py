#!/usr/bin/env python3
"""Synthetic H.264 annex-B bitstream for F3 nalu_scanner bring-up (not decodable)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def nal(unit_type: int, payload: bytes) -> bytes:
    # 4-byte start code + NAL header (forbidden=0, nri=3, type) + payload
    hdr = (0x60 | (unit_type & 0x1F)) & 0xFF
    return b"\x00\x00\x00\x01" + bytes([hdr]) + payload


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "plex_test_annexb.264")
    out.parent.mkdir(parents=True, exist_ok=True)
    # Minimal fake SPS/PPS/IDR-ish NALs with distinct types 7, 8, 5
    blob = b""
    blob += nal(7, b"SPS" + b"\x00" * 16)
    blob += nal(8, b"PPS" + b"\x00" * 8)
    blob += nal(5, b"IDR" + bytes(range(64)))
    # Also a 3-byte start code NAL type 1
    blob += b"\x00\x00\x01" + bytes([0x61]) + b"P" + b"\xaa" * 32
    out.write_bytes(blob)
    # Expected: 4 NAL units
    print(f"wrote {out} ({len(blob)} bytes, expect nalu_count>=4 types 7,8,5,1)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
