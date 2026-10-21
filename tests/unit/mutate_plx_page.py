#!/usr/bin/env python3
"""Mutation run for the kPlx per-build-row PLX page (review 00:55, release 01:05; N2 review 01:51).

Each mutant edits sources in place (every old string must occur exactly once), rebuilds
build/test_plx_page and build/input_mailbox_probe_host, and must turn at least one gate
RED: build/test_plx_page, tests/unit/test_plx_page_scripts.sh, or the mailbox checks of
tests/unit/test_rtl_invariants.py. A mutant that does not compile is a broken mutant, not
a catch. Sources are restored afterwards and checked by md5; the clean tree must be GREEN.
Usage: tests/unit/mutate_plx_page.py ROOT
"""
import hashlib
import subprocess
import sys

ROOT = sys.argv[1]
PB = "host/libmisterplex/present_bank.hpp"
IDS = "host/libmisterplex/fpga_build_ids.hpp"
PG = "host/libmisterplex/plx_page.hpp"
SPEC = "host/libmisterplex/mailbox_abi_spec.hpp"
SPI = "arm/misterplexd/fpga_spi.cpp"
RBF = "scripts/misterplex_named_rbf.sh"
TOOL = "tools/input_mailbox_probe.cpp"
RING = "scripts/probe_bitstream_ring.sh"
MAIN = "arm/misterplexd/main.cpp"
FILES = (PB, IDS, PG, SPEC, SPI, RBF, TOOL, RING, MAIN)
MUTANTS = [
    # L1 probe (present_bank.hpp).
    ("probe on the old absolute page 0x3007F000",
     [(PB, "const uint32_t page = kTrue480PlxPages.page[i];", "const uint32_t page = 0x3007F000u;")]),
    ("probe drops the PLXS heartbeat",
     [(PB, "a.t480PlxdHi[i] != b.t480PlxdHi[i] || a.t480PlxsHi[i] != b.t480PlxsHi[i];",
       "a.t480PlxdHi[i] != b.t480PlxdHi[i];")]),
    ("probe reads the PLXS magic word (+0x100), not the heartbeat (+0x104)",
     [(PB, "plxWordAddr(page, mailbox_abi::kPlxsOff, true)", "plxWordAddr(page, mailbox_abi::kPlxsOff)")]),
    ("probe reads every class page, not the rows' pages",
     [(PB, "inline constexpr PlxPageSet kTrue480PlxPages = true480PlxPagesFromRows();",
       "inline constexpr PlxPageSet kTrue480PlxPages = {{0x300FF000u, 0x3007F000u, 0x301FF000u, 0u}, 3};")]),
    ("decision checks t480Changed before l4Changed",
     [(PB, "    if (l4Changed)\n        return LiveGlass::L4;\n    if (t480Changed)\n        return LiveGlass::True480;\n",
       "    if (t480Changed)\n        return LiveGlass::True480;\n    if (l4Changed)\n        return LiveGlass::L4;\n")]),
    # N2 (review 01:51): the L4 heartbeat.
    ("probe drops the L4 PLXS heartbeat",
     [(PB, "a.l4PlxdHi != b.l4PlxdHi ||\n                           a.l4PlxsHi != b.l4PlxsHi;",
       "a.l4PlxdHi != b.l4PlxdHi;")]),
    ("watcher drops the L4 PLXS heartbeat",
     [(RBF, '  echo "$_l4_d $_l4_s"', '  echo "$_l4_d"')]),
    # Row table and class table.
    ("5D45 row on the no-macro class",
     [(IDS, '{0x48005D45u, "480p", kPlx300FF,', '{0x48005D45u, "480p", PlxPageClass::NoPresentMacro,')]),
    ("unknown build gets the 0x300FF page",
     [(IDS, "return b ? plxPageForClass(b->plxPage) : 0u;",
       "return b ? plxPageForClass(b->plxPage) : kPlex480pYuv420pDoorbellPhys;")]),
    ("no-macro class page 0x300FF000",
     [(PG, "inline constexpr uint32_t kPlxPageNoPresentMacro = 0x3007F000u;",
       "inline constexpr uint32_t kPlxPageNoPresentMacro = 0x300FF000u;")]),
    # Watcher (misterplex_named_rbf.sh).
    ("watcher back on the dead 0x3007F page",
     [(RBF, 'T480_PAGES="${MISTERPLEX_T480_PAGES:-${T480_PAGES:-0x300FF000}}"',
       'T480_PAGES="${MISTERPLEX_T480_PAGES:-${T480_PAGES:-0x3007F000}}"')]),
    ("watcher samples the PLXD magic (+0x128), no heartbeat",
     [(RBF, "$((_t480_p + 0x104))", "$((_t480_p + 0x128))"),
      (RBF, "$((_t480_p + 0x12C))", "$((_t480_p + 0x128))")]),
    # Tools.
    ("input_mailbox_probe on a fixed page",
     [(TOOL, "const uint32_t page = misterplex::fpgaPlxPage(row);", "const uint32_t page = 0x3007F000u;")]),
    ("input_mailbox_probe skips the heartbeat check",
     [(TOOL, "if (!r.heartbeatLive()) {", "if (false && !r.heartbeatLive()) {")]),
    ("probe_bitstream_ring reads the old fixed page",
     [(RING, "    plxs = u64(fd, PLX_PAGE + PLXS_OFF) if PLX_PAGE else 0\n"
             "    plxd = u64(fd, PLX_PAGE + PLXD_OFF) if PLX_PAGE else 0\n    cm = ctrl",
       "    plxs = u64(fd, 0x3007F100)\n    plxd = u64(fd, 0x3007F128)\n    cm = ctrl")]),
    ("probe_bitstream_ring defaults to a page when PLX_PAGE is unset",
     [(RING, 'PLX_PAGE = int(os.environ.get("PLX_PAGE", "") or "0", 0)\nPLX_NOTE = os.environ.get("PLX_NOTE", "")\nPLXS_OFF = 0x100  # +0x104',
       'PLX_PAGE = int(os.environ.get("PLX_PAGE", "") or "0x3007F000", 0)\nPLX_NOTE = os.environ.get("PLX_NOTE", "")\nPLXS_OFF = 0x100  # +0x104')]),
    # PLX_BUILD -> row page in probe_bitstream_ring.sh (wV/release 03:10).
    ("ring row table maps 0x48005D45 to the dead 0x3007F page",
     [(RING, "0x48005D45:0x300FF000", "0x48005D45:0x3007F000")]),
    ("ring: a build without a row falls back to 0x300FF000",
     [(RING, 'no fpga_build_ids.hpp row: no PLX page, no PLX reads"\n      PLX_PAGE=""',
       'no fpga_build_ids.hpp row: no PLX page, no PLX reads"\n      PLX_PAGE="0x300FF000"')]),
    ("ring: a PLX_PAGE that disagrees with the build row silently wins",
     [(RING, 'row page $want: no PLX reads"\n      PLX_PAGE=""', 'row page $want: no PLX reads"')]),
    # review 04:05 nits.
    ("ring: leading-zero decimals accepted again (PLX_BUILD=09 aborts in $(( )))",
     [(RING, "local num='^(0[xX][0-9A-Fa-f]{1,8}|0|[1-9][0-9]{0,9})$'",
       "local num='^(0[xX][0-9A-Fa-f]{1,8}|[0-9]{1,10})$'")]),
    ("main.cpp PLXJ log literal drifts to 0x138u",
     [(MAIN, "const uint32_t plxjPhys = layout.doorbell_phys + 0x130u; // kPlxjOff",
       "const uint32_t plxjPhys = layout.doorbell_phys + 0x138u; // kPlxjOff")]),
    # Spec and daemon.
    ("absolute kPlxjAddr back in the spec",
     [(SPEC, "constexpr uint32_t kPlxjOff   = 0x130u;\n",
       "constexpr uint32_t kPlxjOff   = 0x130u;\nconstexpr uint32_t kPlxjAddr  = 0x3007F130u;\n")]),
    ("spec PLXJ offset 0x138",
     [(SPEC, "constexpr uint32_t kPlxjOff   = 0x130u;", "constexpr uint32_t kPlxjOff   = 0x138u;")]),
    ("fpga_spi PLXD back on an absolute address",
     [(SPI, "const uint32_t plxdPhys = ddrLayout_.doorbell_phys + mailbox_abi::kPlxdOff;",
       "const uint32_t plxdPhys = 0x3007F128u;")]),
    ("spec PLXS offset moved onto DIAG's slot (+0x120)",
     [(SPEC, "constexpr uint32_t kPlxsOff   = 0x100u;", "constexpr uint32_t kPlxsOff   = 0x120u;")]),
]


def md5(p):
    with open(f"{ROOT}/{p}", "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def gate():
    b = run(["make", f"{ROOT}/build/test_plx_page", f"{ROOT}/build/input_mailbox_probe_host"])
    if b.returncode:
        return "COMPILE-FAIL", [b.stderr.strip().splitlines()[-1] if b.stderr.strip() else "make failed"]
    red = []
    if run([f"{ROOT}/build/test_plx_page"]).returncode:
        red.append("test_plx_page")
    if run(["bash", "tests/unit/test_plx_page_scripts.sh"]).returncode:
        red.append("test_plx_page_scripts")
    inv = run([sys.executable, "-c", "import sys; sys.path.insert(0, 'tests/unit'); "
               "import test_rtl_invariants as t; t.check_mailboxes(); t.check_mailbox_map_collisions()"])
    if inv.returncode:
        red.append("rtl_invariants")
    return ("RED" if red else "GREEN"), red


def main():
    before = {p: md5(p) for p in FILES}
    saved = {p: open(f"{ROOT}/{p}").read() for p in FILES}
    state, red = gate()
    print(f"BASE {state} {red}")
    bad = state != "GREEN"
    for label, edits in MUTANTS:
        try:
            for path, old, new in edits:
                text = open(f"{ROOT}/{path}").read()
                if text.count(old) != 1:
                    raise SystemExit(f"mutant '{label}': old string occurs {text.count(old)}x in {path}")
                with open(f"{ROOT}/{path}", "w") as f:
                    f.write(text.replace(old, new))
            state, red = gate()
        finally:
            for p in FILES:
                with open(f"{ROOT}/{p}", "w") as f:
                    f.write(saved[p])
        caught = state == "RED"
        bad = bad or not caught
        print(f"MUT {'CAUGHT' if caught else state} {label}: {','.join(red)}")
    after = {p: md5(p) for p in FILES}
    restored = after == before
    state, red = gate()
    print(f"RESTORED md5={'OK' if restored else 'MISMATCH'} FINAL {state} {red}")
    bad = bad or not restored or state != "GREEN"
    print(f"PLX_PAGE_MUTANTS {'PASS' if not bad else 'FAIL'} ({len(MUTANTS)} mutants)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
