#!/usr/bin/env python3
"""OWENB-P1B T6: cross-check the daemon's overlay pages against owenb's independent overlay model.

test_owenb_osd --dump DIR writes, per golden, the page (NAME.page.bin: from the descriptor's base to the end of its
band), the descriptor (NAME.desc: base w h stride format band_first band_last raster_w raster_h interlaced) and the
test's own C++ model of the core's premultiplied overlay on the raster (NAME.ovl.rgba). This script feeds the same
page and descriptor to owenb's tools/ovl_ref.py run() (written from docs/architecture.md, not from the RTL: its
own unpack, premultiply with round(x / 255), anti-twitter (1,2,1) at 480i / 576i, the descriptor checks) and
requires the result to match the C++ model byte for byte, and the descriptor to be accepted.

    owenb_osd_ovl_ref.py DUMP_DIR [--tools PATH]     (default $OVL_REF_TOOLS; skips when unset)
Exits 0 with SKIP when owenb's tools are not on this machine.
"""
import glob
import os
import sys


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    dump = a[0]
    tools = os.environ.get("OVL_REF_TOOLS") or ""
    if "--tools" in a:
        tools = a[a.index("--tools") + 1]
    if not os.path.isfile(os.path.join(tools, "ovl_ref.py")):
        print("SKIP owenb_osd_ovl_ref: no tools/ovl_ref.py at %s" % tools)
        return 0
    sys.path.insert(0, tools)
    import ovl_ref as R  # noqa: E402

    class BufMem:
        def __init__(self, base, data):
            self.base, self.data = base, data

        def get(self, addr, n):
            o = addr - self.base
            if o < 0 or o + n > len(self.data):
                return 0
            return int.from_bytes(self.data[o:o + n], "little")

    fails = n = 0
    for desc in sorted(glob.glob(os.path.join(dump, "*.desc"))):
        stem = desc[:-5]
        name = os.path.basename(stem)
        base, w, h, stride, fmt, bf, bl, rw, rh, il = map(int, open(desc).read().split())
        page = open(stem + ".page.bin", "rb").read()
        want = open(stem + ".ovl.rgba", "rb").read()
        assert fmt == R.FMT["8888"], name
        d = R.desc_words(1, fmt="8888", base=base, w=w, h=h, stride=stride, band=(bf, bl))
        err, _ = R.check(d)
        geo = None if rw == 720 and rh in (480, 576) else (rw, rh, 0, 0, 0)
        sc = dict(il=il, pal=1 if rh == 576 else 0, refreshes=[(d, 1, il), (d, 1, il)],
                  mem=BufMem(base, page), flags=0, geo=geo)
        exp, stat = R.run(sc)
        img = exp[1]
        got = bytearray()
        for row in img:
            for px in row:
                got += bytes(px)
        n += 1
        ok = err == 0 and stat[1][1] & 3 == 1 and bytes(got) == want
        if not ok:
            fails += 1
            diff = sum(1 for i in range(min(len(got), len(want))) if got[i] != want[i])
            print("FAIL %s: err %d state %d, %d bytes differ of %d" % (name, err, stat[1][1] & 3, diff, len(want)))
        else:
            print("ok   %s: accepted, %dx%d band %d..%d%s, the model's composite matches" % (
                name, w, h, bf, bl, " AT" if il else ""))
    print("owenb_osd_ovl_ref: %d pages, %d failures" % (n, fails))
    return 1 if fails or n == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
