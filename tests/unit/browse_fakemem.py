#!/usr/bin/env python3
"""Browse e2e helpers on core_gate_fakemem.py's fake /dev/mem file.

  plxi FILE STATE CMD    publish one PLXI input word (input_mailbox.hpp layout: magic, cmd,
                         cmdSeq, seq) as the core does for a key press; STATE keeps the
                         sequence numbers. CMD 0 publishes a baseline (no command).
  bankhash FILE          SHA-1 of the frame banks below the 624x480 doorbell page
                         (0x30000000..0x300FF000): what an idle/browse paint wrote.
"""
import hashlib
import mmap
import os
import struct
import sys

PLXI_PHYS = 0x300FF108  # doorbell 0x300FF000 (DECODE 640x480, coded 624) + 0x108
PLXI_MAGIC = 0x504C5849
BANK_BASE, BANK_END = 0x30000000, 0x300FF000


def page_map(path, phys):
    fd = os.open(path, os.O_RDWR)
    try:
        base = phys & ~0xFFF
        return mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                         offset=base), phys - base
    finally:
        os.close(fd)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "plxi":
        state, command = sys.argv[3], int(sys.argv[4])
        seq = cmd_seq = 0
        if os.path.exists(state):
            seq, cmd_seq = (int(x) for x in open(state).read().split())
        seq = (seq + 1) & 0xFFFF
        if command:
            cmd_seq = (cmd_seq + 1) & 0xFF
        m, off = page_map(path, PLXI_PHYS)
        hi = (command & 0xFF) | (cmd_seq << 8) | (seq << 16)
        struct.pack_into("<I", m, off + 4, hi)
        struct.pack_into("<I", m, off, PLXI_MAGIC)
        m.flush()
        m.close()
        with open(state + ".tmp", "w") as fh:
            fh.write("%d %d\n" % (seq, cmd_seq))
        os.replace(state + ".tmp", state)
    elif cmd == "bankhash":
        fd = os.open(path, os.O_RDONLY)
        try:
            m = mmap.mmap(fd, BANK_END - BANK_BASE, mmap.MAP_SHARED, mmap.PROT_READ,
                          offset=BANK_BASE)
        finally:
            os.close(fd)
        print(hashlib.sha1(m[:]).hexdigest()[:16])
        m.close()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
