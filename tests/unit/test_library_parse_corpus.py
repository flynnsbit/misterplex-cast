#!/usr/bin/env python3
"""Library XML corpus: library_browser.hpp parseLibraryXml must return, for every Directory /
Video / Track row, the same title / key / ratingKey / type that a real XML parser (ElementTree)
returns.

Fixtures (tests/fixtures/mpx_menu_xml/):
  l1/  mpx-menu P0b L1 pages (24 files, 232 rows): no regression on the shipping labels.
  p0/  mpx-menu P0 pages (11 files, 132 rows) with 'SERVERS >', 'NEXT >' and '&lt; PREV'.
       dbcc42fa dropped 3 of those rows and drew '&lt;' (4 problems).
Usage: test_library_parse_corpus.py   (CXX from the environment, default g++)
"""
import glob
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIX = os.path.join(ROOT, "tests", "fixtures", "mpx_menu_xml")
BIN = os.path.join(ROOT, "build", "library_parse_dump")
EXPECT = {"l1": (24, 232), "p0": (11, 132)}


def main():
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    cxx = os.environ.get("CXX", "g++")
    subprocess.check_call([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-I" + os.path.join(ROOT, "host"),
                           "-o", BIN, os.path.join(ROOT, "tests", "unit", "library_parse_dump.cpp")])
    problems = 0
    for name, (want_files, want_rows) in sorted(EXPECT.items()):
        files = sorted(glob.glob(os.path.join(FIX, name, "*.xml")))
        out = subprocess.check_output([BIN] + files).decode("utf-8")
        got, cur = {}, None
        for line in out.splitlines():
            f = line.split("\t")
            if f[0] == "FILE":
                cur = f[1]
                got[cur] = []
            else:
                got[cur].append(tuple(f + [""] * (5 - len(f))))
        rows = 0
        for fn in files:
            root = ET.parse(fn).getroot()
            want = []
            for tag, kind in (("Directory", "D"), ("Video", "V"), ("Track", "V")):
                for e in root.iter(tag):
                    r = (kind, e.get("title", ""), e.get("key", ""), e.get("ratingKey", ""), e.get("type", ""))
                    if r[1] or r[2] or r[3]:
                        want.append(r)
            rows += len(want)
            if sorted(want) != sorted(got[fn]):
                problems += 1
                print("MISMATCH %s/%s" % (name, os.path.basename(fn)))
                print("  xml   :", sorted(set(want) - set(got[fn]))[:4])
                print("  daemon:", sorted(set(got[fn]) - set(want))[:4])
        print("%s: %d files, %d rows" % (name, len(files), rows))
        if (len(files), rows) != (want_files, want_rows):
            problems += 1
            print("COUNT %s: got %d files / %d rows, want %d / %d" % (name, len(files), rows, want_files, want_rows))
    print("test_library_parse_corpus: %s (%d problems)" % ("OK" if problems == 0 else "FAIL", problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
