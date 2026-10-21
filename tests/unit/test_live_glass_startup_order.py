#!/usr/bin/env python3
"""probeLiveGlass startup-only exception (review 08:18 (1)-(3)).

present_bank.hpp probeLiveGlass maps the doorbell page(s) transiently, outside the
FpgaSpi doorbell-page mapper. That is allowed only while it provably runs first:

  (2a) main.cpp calls probeLiveGlass exactly once (two arms of one ?:), with no
       std::thread, no d52:: call and no session start before it, and the D52 start
       line (d52::instance) after it.
  (2b) The d52 poller thread starts only in d52::armSession, which only
       FpgaSpi::beginBitstreamSession calls (a play, after startup).
  (2c) The persistent doorbell-page map (ensureDdrMailboxMap, the one mapper, f9ca3646) is
       called only by ddrMailboxWord, plxPageRead32, plxWord (R1 PLX page), mailboxWordDirect
       (MPX_BROWSE_V2 direct readers), readOwenbRingCtrlWord (PlexOB Y/C word 87,
       same mapper, OSD poll only) and plxlBeatStore. plxlBeatStore is the F1 PLXL beat
       (b50aa2a1, review 01:39 k1-k3); its thread starts only in the R1 idle-FB arm
       (MediaPlayer, f1Beat_.start() on an idleBeatPlxl row), after start-up. Every FpgaSpi function that reaches
       it (the transitive closure, pinned in PERSISTENT_REACHERS: a new one is a FAIL to
       review) is absent from main() before the probe and from the pre-probe startup calls
       (FpgaSpi::open, runCoreGateProbe, ..., MediaPlayer::coreGateTick).
  (3)  Every successful transient mmap is munmap'd before the read lambda returns,
       and the fd is closed on every path after open.
  (1)  Transient map/unmap pairs per start: 2 samples x (3 L4 words + 3 words per
       true480 page) -- printed.

  python3 tests/unit/test_live_glass_startup_order.py [--mutants]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = "arm/misterplexd/main.cpp"
SPI = "arm/misterplexd/fpga_spi.cpp"
MP = "arm/misterplexd/media_player.cpp"
D52 = "host/libmisterplex/d52_log.hpp"
PB = "host/libmisterplex/present_bank.hpp"
# Every FpgaSpi function that reaches ensureDdrMailboxMap (computed by reachers(); pinned).
PERSISTENT_REACHERS = {
    "beginBitstreamSession", "beginDdrBankIngest", "beginFpgaVideoSession", "blitDisplayedBank",
    "commitDdrBankIngest", "ddrBankVirt", "ddrMailboxWord", "ensureDdrMap", "kickDdrDoorbell",
    "mailboxWordDirect", "plxLoad64", "plxPageRead32", "plxStore32", "plxStore64", "plxWord",
    "plxlBeatStore", "readBankRelease", "readInputMailboxDirect", "readOsdMailboxDirect", "readOwenbRingCtrlWord", "readDdrDoorbellStatus", "readFrameStoreStatus",
    "readInputMailbox", "readOsdMailbox", "readSourceAspectAck", "sendBitstreamChunkDdr",
    "sendDdrFrame", "sendSourceAspect", "sendYuv420pFrameDdr", "waitPlxdHeardKick",
    "waitPlxfHeardKick"}
SESSION_ACCESSORS = PERSISTENT_REACHERS
PRE_PROBE_FPGA = ["open", "runCoreGateProbe", "onCoreReturned", "dropStaleSession", "fabricState"]


def read(root, rel):
    with open(os.path.join(root, rel)) as f:
        return f.read()


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def body(src, signature_re):
    m = re.search(signature_re, src, flags=re.M)
    if not m:
        return None
    i = src.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def enclosing_spi_fns(src, needle):
    names = set()
    for m in re.finditer(r"^[A-Za-z][^\n;]*?FpgaSpi::(\w+)\([^;{]*\)\s*(const\s*)?\{", src, flags=re.M):
        b = body(src[m.start():], r"\{")
        if b and needle in strip_comments(b):
            names.add(m.group(1))
    return names


def reachers(spi):
    reach = {"ensureDdrMailboxMap"}
    while True:
        new = set()
        for n in reach:
            new |= enclosing_spi_fns(spi, n + "(")
        if new <= reach:
            return reach - {"ensureDdrMailboxMap"}
        reach |= new


def check(root):
    fails = []
    main = strip_comments(read(root, MAIN))
    spi = strip_comments(read(root, SPI))
    mp = strip_comments(read(root, MP))
    d52 = strip_comments(read(root, D52))
    pb = read(root, PB)

    # (2a) main.cpp order
    probes = [m.start() for m in re.finditer(r"probeLiveGlass\(", main)]
    if len(probes) != 2:
        fails.append(f"2a: main.cpp has {len(probes)} probeLiveGlass( calls, want 2 (one ?:)")
    else:
        stmt = main[probes[0]:probes[1]]
        if ":" not in stmt or ";" in stmt:
            fails.append("2a: the two probeLiveGlass calls are not one ?: statement")
    mstart = re.search(r"^int main\(", main, flags=re.M)
    d52i = main.find("d52::instance(")
    if not mstart or not probes:
        fails.append("2a: main() or probe not found")
    else:
        pre = main[mstart.start():probes[0]]
        for bad in ("std::thread", "d52::", "beginBitstreamSession", ".play(", "armSession",
                    *SESSION_ACCESSORS):
            if bad in pre:
                fails.append(f"2a: '{bad}' appears in main() before probeLiveGlass")
        if d52i < 0 or d52i < probes[-1]:
            fails.append("2a: d52::instance() is missing or precedes probeLiveGlass")

    # (2b) poller start
    starts = [m.start() for m in re.finditer(r"\bensureStarted\(\)", d52)]
    calls = [s for s in starts if not re.match(r"ensureStarted\(\)\s*\{", d52[s:])]
    arm = body(d52, r"^inline void armSession\(")
    if len(calls) != 1 or not arm or "poller.ensureStarted()" not in arm:
        fails.append(f"2b: poller.ensureStarted() call sites {len(calls)}, want 1 inside armSession")
    if re.findall(r"std::thread\s*\(|std::thread\s*\{|std::async|pthread_create", d52) != ["std::thread("]:
        fails.append("2b: d52_log.hpp starts a thread other than the poller's")
    arm_callers = enclosing_spi_fns(spi, "d52::armSession(")
    other = [r for r in (MAIN, MP) if "armSession(" in strip_comments(read(root, r))]
    if arm_callers != {"beginBitstreamSession"} or other:
        fails.append(f"2b: d52::armSession callers {sorted(arm_callers)} + {other}, want beginBitstreamSession only")

    # (2c) persistent map reachability
    ensure_callers = enclosing_spi_fns(spi, "ensureDdrMailboxMap(")
    if not ensure_callers <= {"ddrMailboxWord", "plxPageRead32", "plxWord", "mailboxWordDirect",
                              "plxlBeatStore", "ensureDdrMailboxMap"}:
        fails.append(f"2c: ensureDdrMailboxMap callers {sorted(ensure_callers)}, want ddrMailboxWord, plxPageRead32, plxWord, mailboxWordDirect, plxlBeatStore")
    reach = reachers(spi)
    if reach != PERSISTENT_REACHERS:
        fails.append(f"2c: functions reaching the persistent map changed: +{sorted(reach - PERSISTENT_REACHERS)} "
                     f"-{sorted(PERSISTENT_REACHERS - reach)}")
    for fn in PRE_PROBE_FPGA:
        b = body(spi, rf"^[A-Za-z][^\n;]*?FpgaSpi::{fn}\([^;{{]*\)\s*(const\s*)?\{{")
        if b is None:
            continue
        for acc in reach | PERSISTENT_REACHERS | {"ensureDdrMailboxMap"}:
            if acc + "(" in strip_comments(b):
                fails.append(f"2c: FpgaSpi::{fn} (pre-probe) calls {acc}")
    tick = body(mp, r"^void MediaPlayer::coreGateTick\(\)")
    if tick is None:
        fails.append("2c: MediaPlayer::coreGateTick not found")
    else:
        for acc in reach | PERSISTENT_REACHERS | {"armSession"}:
            if acc + "(" in strip_comments(tick):
                fails.append(f"2c: coreGateTick calls {acc}")

    # (3) transient maps closed
    probe = body(pb, r"^inline LiveGlass probeLiveGlass\(")
    if probe is None:
        fails.append("3: probeLiveGlass not found")
    else:
        p = strip_comments(probe)
        if p.count("::mmap(") != 1 or p.count("::munmap(map, kPage)") != 1:
            fails.append("3: probeLiveGlass must have exactly one mmap and one munmap(map, kPage)")
        lam = p[p.find("readWord = [&]"):p.find("};", p.find("readWord = [&]"))]
        if not re.search(r"memcpy\([^;]*\);\s*::munmap\(map, kPage\);\s*return word;", lam):
            fails.append("3: the read lambda does not munmap right after the copy, before return")
        if not re.search(r"MAP_FAILED\)\s*return 0;", lam):
            fails.append("3: MAP_FAILED path changed")
        after_open = p[p.find("::open(memPath"):]
        returns = [m.start() for m in re.finditer(r"\breturn\b", after_open)]
        close_at = after_open.find("::close(fd);")
        lam_end = after_open.find("};", after_open.find("readWord = [&]"))
        bad = [r for r in returns if r > lam_end and (close_at < 0 or r < close_at)]
        if close_at < 0 or bad or "if (fd < 0)" not in after_open:
            fails.append("3: the fd is not closed on every path after open")
    return fails


def pairs_per_start(root):
    src = os.path.join(root, "build", "live_glass_pairs.cpp")
    exe = os.path.join(root, "build", "live_glass_pairs")
    os.makedirs(os.path.dirname(src), exist_ok=True)
    with open(src, "w") as f:
        f.write('#include "libmisterplex/present_bank.hpp"\n#include <cstdio>\n'
                "int main(){int n=0;auto rw=[&](uint32_t){++n;return 0u;};"
                "misterplex::probeLiveGlassWith(rw,[]{});"
                "std::printf(\"%d %zu\\n\",n,misterplex::kTrue480PlxPages.count);}\n")
    subprocess.check_call(["g++", "-std=c++20", "-I", os.path.join(root, "host"), "-I", root,
                           "-o", exe, src])
    n, pages = subprocess.check_output([exe], text=True).split()
    return int(n), int(pages)


MUTANTS = [
    ("probe-after-d52", MAIN, "    // MIX-GLASS-MAX: probe live doorbell page once.",
     "    (void)misterplex::d52::instance();\n    // MIX-GLASS-MAX: probe live doorbell page once."),
    ("thread-before-probe", MAIN, "    // MIX-GLASS-MAX: probe live doorbell page once.",
     "    std::thread([] {}).detach();\n    // MIX-GLASS-MAX: probe live doorbell page once."),
    ("gate-probe-reads-bank", SPI, "CoreGateEvent FpgaSpi::runCoreGateProbe(int timeout_ms, int64_t nowUs) {\n",
     "CoreGateEvent FpgaSpi::runCoreGateProbe(int timeout_ms, int64_t nowUs) {\n"
     "    { BankReleaseStatus b; (void)readBankRelease(b); }\n"),
    ("new-mailbox-caller", SPI, "bool FpgaSpi::kickDdrDoorbell(int bank) {\n",
     "static volatile uint32_t* mutLeak(FpgaSpi*) { return nullptr; }\n"
     "bool FpgaSpi::kickDdrDoorbell(int bank) {\n"),
    # review 17:59 (1): a scratch DIRECT caller of the one mapper (not plxlBeatStore) still fails 2c.
    ("new-direct-mapper-caller", SPI, "bool FpgaSpi::kickDdrDoorbell(int bank) {\n",
     "bool FpgaSpi::mutScratchMap() { volatile uint32_t* m = nullptr; return ensureDdrMailboxMap(0, m); }\n"
     "bool FpgaSpi::kickDdrDoorbell(int bank) {\n"),
    ("poller-start-elsewhere", D52, "        i->logger.start(static_cast<int>(::getpid()));\n",
     "        i->logger.start(static_cast<int>(::getpid()));\n        i->poller.ensureStarted();\n"),
    ("probe-leaks-map", PB, "        ::munmap(map, kPage);\n        return word;\n", "        return word;\n"),
    ("probe-leaks-fd", PB, "    ::close(fd);\n    return glass;\n", "    return glass;\n"),
]


def run_mutants():
    killed = 0
    for name, rel, old, new in MUTANTS:
        tmp = tempfile.mkdtemp(prefix="lgso-", dir=os.environ.get("TMPDIR"))
        try:
            for d in ("arm/misterplexd", "host/libmisterplex"):
                shutil.copytree(os.path.join(ROOT, d), os.path.join(tmp, d))
            path = os.path.join(tmp, rel)
            s = read(tmp, rel)
            if s.count(old) != 1:
                print(f"  BROKEN {name}: anchor count {s.count(old)}")
                continue
            with open(path, "w") as f:
                f.write(s.replace(old, new))
            fails = check(tmp)
            if name == "new-mailbox-caller":
                # a new caller is only a kill once it calls ddrMailboxWord
                with open(path) as f:
                    s2 = f.read()
                with open(path, "w") as f:
                    f.write(s2.replace("static volatile uint32_t* mutLeak(FpgaSpi*) { return nullptr; }",
                                       "bool FpgaSpi::mutLeak() { return ddrMailboxWord(0) != nullptr; }"))
                fails = check(tmp)
            if fails:
                killed += 1
                print(f"  killed {name:24s} {fails[0]}")
            else:
                print(f"  SURVIVED {name}")
        finally:
            shutil.rmtree(tmp)
    ok = killed == len(MUTANTS)
    print(f"LIVE_GLASS_ORDER_MUTANTS {'PASS' if ok else 'FAIL'} killed={killed}/{len(MUTANTS)}")
    return ok


def main():
    fails = check(ROOT)
    for f in fails:
        print("FAIL", f)
    n, pages = pairs_per_start(ROOT)
    want = 2 * (3 + 3 * pages)
    if n != want:
        fails.append(f"1: {n} reads per probe, want {want}")
        print("FAIL", fails[-1])
    if fails:
        return 1
    print(f"test_live_glass_startup_order: OK (probe before d52 and any session; persistent map only "
          f"via {len(PERSISTENT_REACHERS)} session-path FpgaSpi functions; {n} transient map/unmap pairs per start, "
          f"{pages} true480 page(s), all unmapped, fd closed)")
    if "--mutants" in sys.argv:
        return 0 if run_mutants() else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
