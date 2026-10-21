#!/usr/bin/env python3
"""Every kRecognisedFpgaBuilds row is named in each list that must name it (review 09:58).

9bdb535b added 0x48005D52 and three twin lists kept the older IDs (fixed in 5f64605c); the R1
rows 0x4800A1FB and 0x4800A152 met the same twins on canon. One check, so that a new row cannot
miss one again:
  T1 R6_BUILDS in scripts/misterplexd-mast-load.src.sh == the table, as a set.
  T2 the shipped scripts/misterplexd-mast-load.sh has the same R6_BUILDS line, and its last line
     (the wrap-script digest) is the md5 of everything before it: regenerated, not hand-edited.
  T3 PLX_ROW_PAGES in scripts/probe_bitstream_ring.sh == the table's ID:page rows, in order.
  T4 the verified map in test_plx_page.cpp recognisedRows() == the table, as a set.
  T5 test_overlay_onchange.cpp puts every row in exactly one of its two lists (no plane, plane),
     and the cases of overlayPlaneAbsent (overlay_publish.hpp) are exactly its no-plane list.
  T6 test_av_sync.cpp testRows(): its 45/50 MHz list and its skip list == the rows with an A/V
     trim (avTrimMs != 0 in fpga_build_ids.hpp).
The table comes from the compiled header: build/test_plx_page --print-row-pages. T0: the IDs
parsed from fpga_build_ids.hpp (for T6 and the self-test) are that same table.
Then each mutant breaks one twin in a scratch copy, and the check must FAIL on it. The
self-test adds a row (with a trim) to, and drops 0x48005D52 from, a scratch
fpga_build_ids.hpp: every twin T1-T6 must FAIL on each.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BUILD", ROOT / "build"))
FILES = {
    "src": "scripts/misterplexd-mast-load.src.sh",
    "ship": "scripts/misterplexd-mast-load.sh",
    "probe": "scripts/probe_bitstream_ring.sh",
    "plx": "tests/unit/test_plx_page.cpp",
    "ovl_hpp": "host/libmisterplex/overlay_publish.hpp",
    "ovl_test": "tests/unit/test_overlay_onchange.cpp",
    "av": "tests/unit/test_av_sync.cpp",
    "hpp": "host/libmisterplex/fpga_build_ids.hpp",
}
DIGEST = re.compile(r"^# mister-loadcore-guard v2 digest ([0-9a-f]{32})$")


def one(pattern, text, what, fails):
    hits = re.findall(pattern, text, re.M)
    if len(hits) != 1:
        fails.append(f"{what}: want exactly one match, got {len(hits)}")
        return None
    return hits[0]


def block(text, start, what, fails):
    i = text.find(start)
    j = text.find("\n}\n", i)
    if i < 0 or j < 0:
        fails.append(f"{what}: no '{start}' block")
        return ""
    return text[i:j]


def ids(text):
    return ["0x" + i for i in re.findall(r"0x([0-9A-F]{8})u", text)]


def hpp_rows(text):
    """(id, has an A/V trim) per kRecognisedFpgaBuilds row, parsed from the header source."""
    i = text.find("kRecognisedFpgaBuilds[] = {")
    j = text.find("\n};", i)
    out = []
    for r in re.split(r"\n    \{0x", text[i:j])[1:]:
        r = re.sub(r'"(?:[^"\\]|\\.)*"', '""', re.sub(r"//[^\n]*", "", r))
        trim = re.search(r'""(?:\s*"")*\s*,\s*(-?\d+)\s*,', r)
        out.append(("0x" + r[:8].upper(), bool(trim and int(trim.group(1)) != 0)))
    return out


def check(paths, rows):
    """Returns (fail lines, pass lines) for the twin files in paths."""
    fails, passes = [], []
    table = [r.split(":")[0] for r in rows]
    tset = set(table)
    t = {k: Path(p).read_text() for k, p in paths.items()}

    src_line = one(r'^R6_BUILDS="(.*)"$', t["src"], "T1 loader source R6_BUILDS", fails)
    if src_line is not None:
        have = set(src_line.split())
        if have != tset:
            fails.append(f"T1 R6_BUILDS != the table: missing {sorted(tset - have)}, extra {sorted(have - tset)}")
        else:
            passes.append(f"T1 R6_BUILDS == the table ({len(tset)} IDs)")

    ship = t["ship"]
    ship_line = one(r'^R6_BUILDS="(.*)"$', ship, "T2 shipped loader R6_BUILDS", fails)
    lines = ship.rstrip("\n").split("\n")
    m = DIGEST.match(lines[-1]) if lines else None
    body = ship[: ship.rstrip("\n").rfind("\n") + 1]
    if ship_line is not None and src_line is not None and ship_line != src_line:
        fails.append("T2 the shipped loader's R6_BUILDS line is not the source's: regenerate it "
                     "(mister-loadcore-guard wrap-script < misterplexd-mast-load.src.sh)")
    elif ship_line is not None and set(ship_line.split()) != tset:
        fails.append(f"T2 the shipped loader's R6_BUILDS != the table: missing "
                     f"{sorted(tset - set(ship_line.split()))}, extra {sorted(set(ship_line.split()) - tset)}")
    elif not m:
        fails.append("T2 the shipped loader does not end in a wrap-script digest line")
    elif hashlib.md5(body.encode()).hexdigest() != m.group(1):
        fails.append("T2 the shipped loader was edited after wrap-script (digest mismatch)")
    elif ship_line is not None:
        passes.append("T2 shipped loader: same R6_BUILDS line, digest " + m.group(1)[:8] + " matches")

    probe = one(r'^PLX_ROW_PAGES="(.*)"$', t["probe"], "T3 PLX_ROW_PAGES", fails)
    if probe is not None:
        if probe.split() != rows:
            fails.append(f"T3 PLX_ROW_PAGES != the table rows: missing {sorted(set(rows) - set(probe.split()))}, "
                         f"extra {sorted(set(probe.split()) - set(rows))}, or out of order")
        else:
            passes.append(f"T3 PLX_ROW_PAGES == the table rows, in order ({len(rows)})")

    verified = re.findall(r"\{(0x[0-9A-F]{8})u, k\w+Page\}",
                          block(t["plx"], "void recognisedRows() {", "T4 test_plx_page.cpp", fails))
    if set(verified) != tset:
        fails.append(f"T4 test_plx_page verified map != the table: missing {sorted(tset - set(verified))}, "
                     f"extra {sorted(set(verified) - tset)}")
    else:
        passes.append(f"T4 test_plx_page verified map == the table ({len(verified)})")

    ot = t["ovl_test"]
    i = ot.find("cores without the plane")
    lists = re.findall(r"for \(uint32_t id : \{([^}]*)\}\)", ot[i:], re.S)[:2] if i >= 0 else []
    if len(lists) != 2:
        fails.append("T5 test_overlay_onchange.cpp: no 'cores without the plane' block with two id lists")
    else:
        absent, present = set(ids(lists[0])), set(ids(lists[1]))
        both = sorted(tset & absent & present)
        none = sorted(tset - absent - present)
        cases = set(re.findall(r"case (0x[0-9A-F]{8})u:", block(
            t["ovl_hpp"], "inline bool overlayPlaneAbsent(uint32_t buildId) {", "T5 overlay_publish.hpp", fails)))
        if both or none:
            fails.append(f"T5 overlay plane: rows in both lists {both}, rows in neither {none}")
        elif cases != absent:
            fails.append(f"T5 overlayPlaneAbsent cases != the no-plane list: cases only {sorted(cases - absent)}, "
                         f"list only {sorted(absent - cases)}")
        elif cases - tset:
            fails.append(f"T5 overlayPlaneAbsent names non-table builds {sorted(cases - tset)}")
        else:
            passes.append(f"T5 overlay plane: every row in one list ({len(tset & absent)} without, "
                          f"{len(tset & present)} with); overlayPlaneAbsent == the no-plane list")

    parsed = hpp_rows(t["hpp"])
    if sorted(r for r, _ in parsed) != sorted(tset):
        fails.append(f"T0 fpga_build_ids.hpp parse {sorted(r for r, _ in parsed)} != the table")
    trims = sorted(r for r, tr in parsed if tr)
    av = block(t["av"], "void testRows() {", "T6 test_av_sync.cpp", fails)
    lst = re.search(r"for \(uint32_t id : \{([^}]*)\}\)", av)
    skip = re.search(r"if \((b\.id == [^)]*)\)\s*continue;", av)
    for what, m in (("45/50 MHz list", lst), ("skip list", skip)):
        have = sorted(ids(m.group(1))) if m else []
        if have != trims:
            fails.append(f"T6 test_av_sync {what} {have} != the trim rows {trims}")
        else:
            passes.append(f"T6 test_av_sync {what} == the trim rows ({len(trims)})")
    return fails, passes


def drop_id(line_re, text, victim):
    def sub(m):
        kept = [w for w in m.group(1).split() if not w.startswith(victim)]
        return m.group(0).replace(m.group(1), " ".join(kept))
    out = re.sub(line_re, sub, text, count=1, flags=re.M)
    assert out != text, "mutant did not change the text"
    return out


def drop_token(start, token):
    """Drops the first `token` at or after `start` with one adjacent ", ", wherever it sits in a list."""
    def edit(s):
        i = s.find(start)
        j = s.find(token, i) if i >= 0 else -1
        if j < 0:
            return s
        k = j + len(token)
        if s[k:k + 2] == ", ":
            return s[:j] + s[k + 2:]
        if s[j - 2:j] == ", ":
            return s[:j - 2] + s[k:]
        return s
    return edit


def redigest(text):
    body = text[: text.rstrip("\n").rfind("\n") + 1]
    return body + "# mister-loadcore-guard v2 digest " + hashlib.md5(body.encode()).hexdigest() + "\n"


def in_block(start, old, new):
    """An edit of the first `old` at or after `start` only."""
    def edit(s):
        i = s.find(start)
        return s if i < 0 else s[:i] + s[i:].replace(old, new, 1)
    return edit


def main():
    exe = BUILD / "test_plx_page"
    rows = subprocess.run([str(exe), "--print-row-pages"], check=True, capture_output=True,
                          text=True).stdout.split()
    if not rows:
        print(f"FAIL {exe} --print-row-pages printed no rows")
        return 1
    real = {k: str(ROOT / v) for k, v in FILES.items()}
    fails, passes = check(real, rows)
    for p in passes:
        print("PASS " + p)
    for f in fails:
        print("FAIL " + f)
    if fails:
        print(f"test_build_table_twins: {len(fails)} failure(s) on the tree")
        return 1

    last = rows[-1].split(":")[0][2:]
    r6 = r'^R6_BUILDS="(.*)"$'
    absent_fn = "inline bool overlayPlaneAbsent(uint32_t buildId) {"
    absent_cases = re.findall(r"case (0x[0-9A-F]{8})u:", block(
        Path(real["ovl_hpp"]).read_text(), absent_fn, "overlay_publish.hpp", []))
    if not absent_cases:
        print("FAIL overlayPlaneAbsent has no cases to mutate")
        return 1
    mutants = {
        # name: (file key, edit of that file's text); each must make check() FAIL.
        "r6-source-misses-a-row": ("src", lambda s: drop_id(r6, s, "0x" + last)),
        "r6-shipped-stale": ("ship", lambda s: redigest(drop_id(r6, s, "0x" + last))),
        "r6-shipped-hand-edit": ("ship", in_block("R6_BUILDS=", "\n#", "\n# ")),
        "probe-misses-a-row": ("probe", lambda s: drop_id(r'^PLX_ROW_PAGES="(.*)"$', s, "0x" + last)),
        "plx-map-misses-a-row": ("plx", in_block("void recognisedRows() {", f"{{0x{last}u, k480pPage}}", "")),
        "overlay-row-unclassified": ("ovl_test", in_block("cores without the plane", f"0x{last}u", "0x0BADF00Du")),
        "av-list-misses-a-trim-row": ("av", drop_token("void testRows() {", "0x48005D52u")),
        "overlay-case-not-listed": ("ovl_hpp", in_block(
            absent_fn, f"    case {absent_cases[-1]}u:\n",
            f"    case {absent_cases[-1]}u:\n    case 0x0BADF00Du:\n")),
    }
    bad = 0
    add_row = ('    {0x4800ABCDu, "480p", kPlx300FF, false, false, 9, "self-test", -45, true,\n'
               "     -14.6},\n")
    self_tests = (
        ("add-row 0x4800ABCD (trim -45)", rows + ["0x4800ABCD:0x300FF000"],
         lambda s: s.replace("kRecognisedFpgaBuilds[] = {\n",
                             "kRecognisedFpgaBuilds[] = {\n" + add_row, 1)),
        ("drop-row 0x48005D52", [r for r in rows if not r.startswith("0x48005D52:")],
         lambda s: re.sub(r"\n    \{0x48005D52u,.*?-14\.6\},", "", s, count=1, flags=re.S)),
    )
    for name, mut_rows, edit in self_tests:
        text = Path(real["hpp"]).read_text()
        mutated = edit(text)
        if mutated == text or len(mut_rows) == len(rows):
            print(f"FAIL self-test {name}: the edit did not apply")
            bad += 1
            continue
        with tempfile.TemporaryDirectory(prefix="twins-self.") as tmp:
            paths = dict(real)
            paths["hpp"] = os.path.join(tmp, "fpga_build_ids.hpp")
            Path(paths["hpp"]).write_text(mutated)
            mf, _ = check(paths, mut_rows)
            red = sorted({f.split()[0] for f in mf})
            want = ["T1", "T2", "T3", "T4", "T5", "T6"]
            if [x for x in want if x not in red] or "T0" in red:
                print(f"FAIL self-test {name}: RED {red}, want all of {want} and not T0")
                bad += 1
            else:
                print(f"  self-test {name} RED on {red}")
    with tempfile.TemporaryDirectory(prefix="twins-mut.") as tmp:
        for name, (key, edit) in mutants.items():
            paths = dict(real)
            text = Path(real[key]).read_text()
            mutated = edit(text)
            if mutated == text:
                print(f"FAIL mutant {name}: the edit did not apply")
                bad += 1
                continue
            paths[key] = os.path.join(tmp, name + "." + Path(real[key]).name)
            Path(paths[key]).write_text(mutated)
            mf, _ = check(paths, rows)
            if mf:
                print(f"  mutant {name} RED: {mf[0][:150]}")
            else:
                print(f"FAIL mutant {name} stayed GREEN")
                bad += 1
    if bad:
        print(f"test_build_table_twins: {bad} mutant(s) without teeth")
        return 1
    print(f"test_build_table_twins: PASS ({len(rows)} rows, {len(mutants)} mutants and the "
          "add/drop-row self-test RED)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
