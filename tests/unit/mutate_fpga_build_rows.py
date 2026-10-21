#!/usr/bin/env python3
"""Mutation run for the explicit build-row layout (V3-FIT-PRECHECK-1003).

Each mutant edits one source in place, rebuilds test_fpga_build_ids and
test_status_osd_v3, and must make at least one of them fail. Sources are
restored afterwards (checked by md5).
"""
import hashlib
import os
import subprocess
import sys

ROOT = sys.argv[1]
H = "host/libmisterplex/fpga_build_ids.hpp"
C = "arm/misterplexd/fpga_spi.cpp"
FILES = (H, C)
TESTS = ("test_fpga_build_ids", "test_status_osd_v3")
MUTANTS = [
    ("2400F40B byte12Echoed=0 (spec)", H, '{0x2400F40Bu, "240p", true, true, 10,',
     '{0x2400F40Bu, "240p", true, false, 10,'),
    ("2400F40B byte15Echoed=0", H, '{0x2400F40Bu, "240p", true, true, 10,',
     '{0x2400F40Bu, "240p", false, true, 10,'),
    ("2400F40B confVersion 9", H, '{0x2400F40Bu, "240p", true, true, 10,',
     '{0x2400F40Bu, "240p", true, true, 9,'),
    ("4801F40A byte12Echoed=0", H, '{0x4801F40Au, "480i", true, true, 10,',
     '{0x4801F40Au, "480i", true, false, 10,'),
    ("2400F36A byte15Echoed=1", H, '{0x2400F36Au, "240p", false, false, 9,',
     '{0x2400F36Au, "240p", true, false, 9,'),
    ("4800F36A confVersion 10", H, '{0x4800F36Au, "480p", false, false, 9,',
     '{0x4800F36Au, "480p", false, false, 10,'),
    ("4801F40B row added", H, '    {0x4801F40Au, "480i", true, true, 10,',
     '    {0x4801F40Bu, "480i", true, true, 10, "x"},\n    {0x4801F40Au, "480i", true, true, 10,'),
    ("48005D45 row dropped", H,
     '    {0x48005D45u, "480p", false, false, 9,\n'
     '     "N5 2b625b7c + clk_dec 45 MHz: 4c210d99 (Plex_480p.qsf:178, RBF 534cb8e7)", -45, true,\n'
     '     -14.6},\n', ''),
    ("48005D50 row dropped", H,
     '    {0x48005D50u, "480p", false, false, 9,\n'
     '     "N5 2b625b7c + clk_dec 50 MHz: 2b770497 (Plex_480p.qsf:178, RBF 7fdb10b9)", -45, true,\n'
     '     -14.6},\n', ''),
    ("48005D52 row dropped", H,
     '    {0x48005D52u, "480p", false, false, 9,\n'
     '     "dec50 cut 4: DPB_SPAN_CUT 9c86d9b2 on cut 3 62c6df94 (dev/dec50-cut4); "\n'
     '     "RBF s2 d53bdd17 (glass), s9 b04cbb84 (backup)", -45, true,\n'
     '     -14.6},\n', ''),
    ("48005D52 trim -40", H, '"RBF s2 d53bdd17 (glass), s9 b04cbb84 (backup)", -45, true,',
     '"RBF s2 d53bdd17 (glass), s9 b04cbb84 (backup)", -40, true,'),
    ("48005D45 confVersion 10", H, '{0x48005D45u, "480p", false, false, 9,',
     '{0x48005D45u, "480p", false, false, 10,'),
    ("48005D50 byte12Echoed=1", H, '{0x48005D50u, "480p", false, false, 9,',
     '{0x48005D50u, "480p", false, true, 9,'),
    ("48005D50 raster 480i", H, '{0x48005D50u, "480p", false, false, 9,',
     '{0x48005D50u, "480i", false, false, 9,'),
    ("byte role ignores byte12Echoed", H,
     "(byte == status_telemetry::kOsdPositionByte && b.byte12Echoed)",
     "(byte == status_telemetry::kOsdPositionByte && b.byte15Echoed)"),
    ("CONF_STR parse always v,10", H, "            if (i == end)\n                return v;",
     "            if (i == end)\n                return 10;"),
    ("parseCoreStatus ignores the row (byte 12)", C,
     "s.osd_byte12 = build && build->byte12Echoed;", "s.osd_byte12 = false;"),
    ("parseCoreStatus ignores the row (byte 15)", C,
     "s.osd_byte15 = build && build->byte15Echoed;", "s.osd_byte15 = false;"),
    ("parseCoreStatus keeps residual_dc on v3", C,
     "    if (!s.osd_byte12)\n        s.residual_dc", "    if (true)\n        s.residual_dc"),
    ("parseCoreStatus swaps byte 12 / 15 decode", C,
     "        if (s.osd_byte12) {\n            s.osd.h_position",
     "        if (s.osd_byte15) {\n            s.osd.h_position"),
]


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def build_and_run():
    failed = []
    for t in TESTS:
        target = os.path.join(ROOT, "build", t)
        if os.path.exists(target):
            os.remove(target)
        b = subprocess.run(["make", "-s", target], cwd=ROOT, capture_output=True, text=True)
        if b.returncode:
            failed.append(f"{t}:BUILD")
            continue
        r = subprocess.run([target], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            line = next((l for l in (r.stderr + r.stdout).splitlines() if "FAIL" in l), "")
            failed.append(f"{t}:{line[:90]}")
    return failed


orig = {f: open(os.path.join(ROOT, f)).read() for f in FILES}
before = {f: md5(os.path.join(ROOT, f)) for f in FILES}
killed = 0
try:
    for name, path, old, new in MUTANTS:
        for f, s in orig.items():
            open(os.path.join(ROOT, f), "w").write(s)
        s = orig[path]
        if s.count(old) != 1:
            print(f"NOMATCH {name}")
            continue
        open(os.path.join(ROOT, path), "w").write(s.replace(old, new))
        failed = build_and_run()
        killed += bool(failed)
        print(f"{'KILLED' if failed else 'SURVIVED'} {name}: {'; '.join(failed)}", flush=True)
finally:
    for f, s in orig.items():
        open(os.path.join(ROOT, f), "w").write(s)
after = {f: md5(os.path.join(ROOT, f)) for f in FILES}
assert before == after, "sources not restored"
clean = build_and_run()
print(f"mutants killed {killed}/{len(MUTANTS)}; sources restored; clean run "
      f"{'PASS' if not clean else 'FAIL ' + str(clean)}")
sys.exit(0 if killed == len(MUTANTS) and not clean else 1)
