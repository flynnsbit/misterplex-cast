#!/usr/bin/env python3
"""Fake /dev/mem for the core-gate daemon end-to-end test.

A sparse 4 GiB file. `create` fills the windows misterplexd can touch (DDR
0x30000000 + 8 MiB, the FPGA-manager page 0xFF700000 + 64 KiB) with 0xA5 and sets
the fabric live: FPGA-manager stat mode 4 (user mode), GPO core-reset bit 30 clear.
`fill` refills in place and keeps the stat and GPO words (the fabric the script set).
`fabric MODE RESET` sets the stat mode and GPO bit 30, as Main does while it loads a
core. `hash` prints a SHA-1 of the windows without the stat, GPO and GPI words (the
script's and the fake core's; SPI is counted by core_gate_responder) and without MAST
(core-owned, republished every poll). `hash-noring` also
leaves out the AU-ring window, so a CAPS probe does not move it and a paint does.
"""
import hashlib
import mmap
import os
import struct
import sys

WINDOWS = ((0x30000000, 0x00800000), (0xFF700000, 0x00010000))
RING_BASE, RING_LEN = 0x30100000, 0x41000  # ring data + CTRL/status/CAPS page
# MAST, the MrAudio consumer status the core's audio mailbox republishes on every poll
# (audio_session_mailbox.sv): core-owned, never written by misterplexd, so no hash
# that stands for daemon writes covers it.
MAST_BASE, MAST_LEN = 0x30140140, 0x40
STAT = 0x6000  # 0xFF706000 in the FPGA-manager window
GPO = 0x6010
GPI = 0x6014
GPO_SENTINEL = 0x0000A5A5  # no strobe, bit 30 clear


def windows(path):
    fd = os.open(path, os.O_RDWR)
    try:
        return [mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                          offset=base) for base, length in WINDOWS]
    finally:
        os.close(fd)


def get32(m, off):
    return struct.unpack_from("<I", m, off)[0]


def put32(m, off, value):
    struct.pack_into("<I", m, off, value)


def fill(path, stat=None, gpo=None):
    ddr, regs = windows(path)
    if stat is None:
        stat, gpo = get32(regs, STAT), get32(regs, GPO)
    ddr[:] = b"\xa5" * len(ddr)
    keep_gpi = get32(regs, GPI)
    regs[:] = b"\xa5" * len(regs)
    put32(regs, STAT, stat)
    put32(regs, GPO, gpo)
    put32(regs, GPI, keep_gpi)
    for m in (ddr, regs):
        m.flush()
        m.close()


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "create":
        with open(path, "wb") as f:
            f.truncate(1 << 32)
        fill(path, stat=4, gpo=GPO_SENTINEL)
    elif cmd == "fill":  # in place: the daemon keeps its maps
        fill(path)
    elif cmd == "fabric":
        mode, reset = int(sys.argv[3]), int(sys.argv[4])
        _, regs = windows(path)
        put32(regs, STAT, mode)
        gpo = get32(regs, GPO)
        gpo = (gpo | (1 << 30)) if reset else (gpo & ~(1 << 30))
        put32(regs, GPO, gpo)
        regs.flush()
        regs.close()
    elif cmd in ("hash", "hash-noring"):
        # hash-noring leaves out the AU-ring window (ring data, CTRL, status, CAPS), so it
        # moves only on frame-bank, doorbell or mailbox writes (a paint), not on a probe.
        h = hashlib.sha1()
        for (base, _), m in zip(WINDOWS, windows(path)):
            data = bytearray(m[:])
            m.close()
            if base == 0xFF700000:
                data[STAT:STAT + 4] = b"\0" * 4
                data[GPO:GPI + 4] = b"\0" * 8
            else:
                start = MAST_BASE - base
                data[start:start + MAST_LEN] = b"\0" * MAST_LEN
                if cmd == "hash-noring":
                    start = RING_BASE - base
                    data[start:start + RING_LEN] = b"\0" * RING_LEN
            h.update(data)
        print(h.hexdigest())
    else:
        sys.exit(f"usage: {sys.argv[0]} create|fill|hash|hash-noring FILE | fabric FILE MODE RESET")


if __name__ == "__main__":
    main()
