#!/usr/bin/env python3
"""Mutation run for the Status block heartbeat writer and its build row (OSD-MENU-SPEC-1002 section 9).

Usage: mutate_status_heartbeat.py ROOT

Each mutant edits one source in place, rebuilds test_status_heartbeat and test_fpga_build_ids, and must make
at least one of them fail. A mutant whose text does not match exactly once is a failure (NOMATCH), never a
skip. Sources are restored afterwards and checked by md5. Ported from the 1cb004e5 lineage's statusBlock
mutants (tests/unit/mutate_fpga_build_rows.py there), adapted to the canon line's CoreGate-based gate.
Tonight (Track B): re-pointed to the 480058BC row (statusBlock) and the 4800C88F row (without), plus
the positional-slip mutant (statusBlock in the idleBeatPlxl slot).
"""
import hashlib
import os
import subprocess
import sys

ROOT = sys.argv[1]
H = "host/libmisterplex/fpga_build_ids.hpp"
C = "arm/misterplexd/fpga_spi.cpp"
CH = "arm/misterplexd/fpga_spi.hpp"
HB = "host/libmisterplex/status_heartbeat.hpp"
FILES = (H, C, CH, HB)
TESTS = ("test_status_heartbeat", "test_fpga_build_ids")
ROW_58BC_TAIL = ('"cb1e5f8b (Plex_480p_r1.qsf); no RBF yet",\n'
                 '     -45, true, -14.6, true, true, false, true},')
ROW_C88F_TAIL = '"7cb897b7 (Plex_480p_r1.qsf); RBF s9 5bdd22da", -45, true, -14.6, true, true},'
MUTANTS = [
    # Rows.
    ("480058BC statusBlock=0", H, ROW_58BC_TAIL, ROW_58BC_TAIL.replace("true, true, false, true}", "true, true, false, false}")),
    # Positional slip (review 17:24): statusBlock spelled in the 13th (idleBeatPlxl) slot.
    ("480058BC statusBlock in the idleBeatPlxl slot", H, ROW_58BC_TAIL,
     ROW_58BC_TAIL.replace("true, true, false, true}", "true, true, true}")),
    ("4800C88F statusBlock=1", H, ROW_C88F_TAIL, ROW_C88F_TAIL.replace("true, true},", "true, true, false, true},")),
    ("statusBlock default true", H, "bool statusBlock = false;", "bool statusBlock = true;"),
    # Row rule and gate.
    ("row rule ignores the flag", CH, "return row && (row->statusBlock || force);", "return row != nullptr;"),
    ("row rule ignores the force", CH, "return row && (row->statusBlock || force);", "return row && row->statusBlock;"),
    ("no row passes", CH, "return row && (row->statusBlock || force);", "return !row || row->statusBlock || force;"),
    ("active ignores the core gate", C, "    else if (gate.idleErrorPaintAllowed())\n", "    else if (true)\n"),
    ("active ignores pairing-error idle", C, "    else if (gate.idleErrorPaintAllowed())\n",
     "    else if (false)\n"),
    ("write skips the active check", C, "    if (!statusBlockActive()) {\n", "    if (false) {\n"),
    ("force on by default", CH, "std::atomic<bool> statusBlockForce_{false};", "std::atomic<bool> statusBlockForce_{true};"),
    # Word compose.
    ("H[7] written from h", C, "running | (h & hb::kDaemonBits)", "h"),
    ("H[7] forced 0", C, "running | (h & hb::kDaemonBits)", "(h & hb::kDaemonBits)"),
    # Layout.
    ("H moved to byte 12", HB, "constexpr int kByte = 6;", "constexpr int kByte = 12;"),
    ("Casting on H[7]", HB, "constexpr uint8_t kCasting = 0x08;", "constexpr uint8_t kCasting = 0x80;"),
    ("29.97 as class 5", HB, "{30000, 1001, 4}", "{30000, 1001, 5}"),
    ("clean stop clears the toggle", HB, "return static_cast<uint8_t>(last & kToggle);", "return 0;"),
    ("force env accepts any value", HB, "return v && v[0] == '1' && v[1] == '\\0';", "return v && v[0];"),
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
killed = nomatch = 0
try:
    for name, path, old, new in MUTANTS:
        for f, s in orig.items():
            open(os.path.join(ROOT, f), "w").write(s)
        s = orig[path]
        if s.count(old) != 1:
            nomatch += 1
            print(f"NOMATCH {name} (count={s.count(old)})", flush=True)
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
ok = killed == len(MUTANTS) and not nomatch and not clean
print(f"STATUS_HB_MUTANTS killed {killed}/{len(MUTANTS)} nomatch {nomatch}; sources restored; clean run "
      f"{'PASS' if not clean else 'FAIL ' + str(clean)}; {'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
