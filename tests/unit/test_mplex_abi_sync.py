#!/usr/bin/env python3
"""K1: kmod/mplex_ddr/mplex_ddr.c and host/libmisterplex/mplex_ddr_abi.hpp (+ ddr_frame_layout.hpp) must agree on
every mplex_ddr ABI value: window base and size, cached pgoff, idle-FB phys/size/pgoff, the CAPS bit and both
ioctl numbers ('M' 1 and 2). Exit 1 on any difference. Usage: test_mplex_abi_sync.py [repo root]"""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[2])
C = (ROOT / "kmod/mplex_ddr/mplex_ddr.c").read_text()
H = (ROOT / "host/libmisterplex/mplex_ddr_abi.hpp").read_text()
L = (ROOT / "host/libmisterplex/ddr_frame_layout.hpp").read_text()
fails = 0
checks = 0


def cdef(name):
    m = re.search(r"^#define\s+%s\s+(.+?)\s*(?:/\*.*)?$" % re.escape(name), C, re.M)
    if not m:
        raise SystemExit("FAIL: kmod has no #define %s" % name)
    return m.group(1).strip()


def cnum(name):
    v = cdef(name)
    m = re.fullmatch(r"\(?\s*(0x[0-9A-Fa-f]+|\d+)\s*U?L?\s*\)?", v)
    if not m:
        raise SystemExit("FAIL: kmod %s is not a plain number: %s" % (name, v))
    return int(m.group(1), 0)


def hnum(text, name):
    m = re.search(r"constexpr\s+(?:[\w:]+\s+)+%s\s*=\s*([^;]+);" % re.escape(name), text)
    if not m:
        raise SystemExit("FAIL: host has no constexpr %s" % name)
    expr = re.sub(r"(0x[0-9A-Fa-f]+|\d+)(?:[uU][lL]{0,2}|[lL]{1,2}[uU]?)\b", r"\1", m.group(1))
    if not re.fullmatch(r"[0-9A-Fa-fx\s*+()]+", expr):
        raise SystemExit("FAIL: host %s is not a plain integer expression: %s" % (name, m.group(1)))
    return eval(expr, {"__builtins__": {}})


def same(what, a, b):
    global fails, checks
    checks += 1
    if a != b:
        fails += 1
        print("FAIL %s: kmod %s != host %s" % (what, hex(a) if isinstance(a, int) else a, hex(b) if isinstance(b, int) else b))


same("window base", cnum("MPLEX_DDR_PHYS"), hnum(L, "kMplexDdrPhys"))
same("window size", cnum("MPLEX_DDR_SIZE"), hnum(L, "kMplexDdrSize"))
same("cached pgoff", cnum("MPLEX_CACHED_PGOFF"), hnum(H, "kMplexCachedPgoff"))
same("cached slot bytes", 4096 << int(cdef("MPLEX_CACHED_ORDER").split()[0]), hnum(H, "kMplexCachedSlotBytes"))
same("idle-FB phys", cnum("MPLEX_IDLEFB_PHYS"), hnum(H, "kMplexIdleFbPhys"))
same("idle-FB size", cnum("MPLEX_IDLEFB_SIZE"), hnum(H, "kMplexIdleFbBytes"))
same("idle-FB pgoff (4 KiB pages)", (cnum("MPLEX_IDLEFB_PHYS") - cnum("MPLEX_DDR_PHYS")) >> 12, hnum(H, "kMplexIdleFbPgoff"))
same("CAPS bit0", cnum("MPLEX_CAP_IDLEFB_WC"), hnum(H, "kMplexCapIdleFbWc"))
for name, nr, ctype, htype in (("MPLEX_IOC_CACHED_PHYS", 1, "struct mplex_cached_phys", "misterplex::MplexCachedPhys"),
                               ("MPLEX_IOC_CAPS", 2, "u32", "uint32_t")):
    want_c = "_IOR('M', %d, %s)" % (nr, ctype)
    m = re.search(r"#define\s+%s\s+(_IOR\([^)]*\))" % name, H)
    same(name + " (kmod)", cdef(name), want_c)
    same(name + " (host)", m.group(1) if m else "missing", "_IOR('M', %d, %s)" % (nr, htype))
# The structs behind the ioctl sizes: the same three 32-bit fields in the same order.
cs = re.search(r"struct mplex_cached_phys\s*{([^}]*)}", C).group(1)
hs = re.search(r"struct MplexCachedPhys\s*{([^}]*)}", H).group(1)
same("cached_phys fields", re.sub(r"\s+", " ", cs.replace("u32", "U32")).strip(),
     re.sub(r"\s+", " ", hs.replace("uint32_t", "U32")).strip())
print("test_mplex_abi_sync: %s (%d checks)" % ("OK" if fails == 0 else "%d FAIL" % fails, checks))
sys.exit(1 if fails else 0)
