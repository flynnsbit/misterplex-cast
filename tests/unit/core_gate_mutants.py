#!/usr/bin/env python3
"""Core gate v2 mutation check.

Each mutant is one or more exact edits (each FIND must occur exactly once) to copies of
gate sources. `unit` mutants build tests/unit/test_core_gate.cpp against the copies;
`e2e` mutants run tests/unit/test_core_gate_daemon_e2e.sh, `sess` mutants
tests/unit/test_core_gate_session_e2e.sh and `audio` mutants (v2.3) the scenarios of
tests/unit/test_audio_recovery_e2e.sh named with them, all with CORE_GATE_MUT_DIR. `browse` mutants run the v2
scenario of tests/unit/test_browse_e2e.sh the same way. A
mutant is killed when the test prints a FAIL line; a mutant that does not build is
reported as invalid. Expected survivors are listed with the reason they survive.

    python3 tests/unit/core_gate_mutants.py [unit|e2e|sess|audio|browse|all] [ID ...]
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "build", "core_gate_mutants")
SPI = "arm/misterplexd/fpga_spi.cpp"
MP = "arm/misterplexd/media_player.cpp"
MAIN = "arm/misterplexd/main.cpp"
GATE = "host/libmisterplex/core_gate.hpp"
TOOL = "arm/misterplexd/core_gate_tool.hpp"
SET_STATUS = "tools/set_status.cpp"

M = []  # (id, test, description, [(file, find, replace)], expect_survive_reason)
SCENARIOS = {}  # audio mutants: the test_audio_recovery_e2e.sh scenarios that must kill them


def mut(mid, test, desc, edits, survives=None, scenarios=None):
    M.append((mid, test, desc, edits, survives))
    if scenarios:
        SCENARIOS[mid] = scenarios


# ---- FpgaSpi choke points (unit)
mut("U1", "unit", "SPI frame ignores a closed gate", [(SPI,
    "    const bool open = gate.open();\n"
    "    const bool hbIdle = statusHeartbeatScope() && gate.idleErrorPaintAllowed();\n"
    "    if (!open && !hbIdle)\n"
    "        return false;\n"
    "    const FabricState fabric = readFabricState(map);",
    "    const FabricState fabric = readFabricState(map);")])
mut("U2", "unit", "SPI frame skips the fabric read", [(SPI,
    "    const FabricState fabric = readFabricState(map);\n    if (fabric.live())",
    "    const FabricState fabric = readFabricState(map);\n    if (true)")])
mut("U3", "unit", "MainSafeWindow stops Main while closed", [(SPI,
    "lock(outer && gate), win(map, attempts, outer && gate) {",
    "lock(outer), win(map, attempts, outer) {")])
mut("U4", "unit", "SPI fabric not re-read after Main is stopped", [(SPI,
    "spiWindowSafe() = gate && win.safe && spiGateAllows(map);",
    "spiWindowSafe() = gate && win.safe;")])
mut("U5", "unit", "choke points ignore a closed gate", [(SPI,
    "    if (!gate.open() && !(ringProbe && gate.ringProbeAllowed())) {",
    "    if (false) {")])
mut("U6", "unit", "choke points skip the fabric read", [(SPI,
    "    // while CORENAME still says Plex.\n    const FabricState fabric = readFabricState(map_);\n    if (!fabric.live()) {",
    "    // while CORENAME still says Plex.\n    const FabricState fabric = readFabricState(map_);\n    if (false) {")])
mut("U7", "unit", "ensureDdrMap ungated", [(SPI,
    '    if (!coreGateAllows("ensureDdrMap"))\n        return false;\n', "")])
mut("U8", "unit", "ensureBitstreamDdrMap ungated", [(SPI,
    '    if (!coreGateAllows("ensureBitstreamDdrMap", /*ringProbe=*/true))\n        return false;\n',
    "")])
mut("U9", "unit", "mailbox map and commit ungated", [
    (SPI, '    if (!coreGateAllows("ensureDdrMailboxMap"))\n        return false;\n', ""),
    (SPI, '    if (!coreGateAllows("commitDdrBankIngest"))\n        return false;\n', "")])
mut("U10", "unit", "reopen probe keeps the dead session", [(SPI,
    "        if (bitstreamSessionActive_ || bitstreamResetPending_ || bitstreamAbortPending_)\n"
    "            releaseBitstreamDdrMap();\n", "")])
mut("U11", "unit", "allowlist bypass: any CAPS reply opens", [(SPI,
    "        if (!fpgaBuildRefusal(caps.build_id).empty())\n"
    "            refusal = \"not on the allowlist\";\n", "")])
mut("U12", "unit", "an unmapped register file closes the gate", [(SPI,
    "        if (fabric.known) // an unmapped register file is no reload signal\n"
    "            gate.closeNow(fabric.describe());\n        setErr(",
    "        gate.closeNow(fabric.describe());\n        setErr(")])
# ---- gate state machine (unit)
mut("U13", "unit", "gate starts open", [(GATE,
    "    static constexpr bool kStartOpen = false;", "    static constexpr bool kStartOpen = true;")])
mut("U14", "unit", "a new Main is no reload signal", [(GATE,
    "        } else if (!first && o.main.pid != 0 && o.main != lastMain_) {",
    "        } else if (false) {")])
mut("U15", "unit", "a stale CORENAME is trusted", [(GATE,
    "        else if (needFreshName_)\n            hold = freshWhy_;",
    "        else if (false)\n            hold = freshWhy_;")])
mut("U16", "unit", "fabric edges are no reload signal", [(GATE,
    "        if (!o.fabric.live() && (first || last_.fabric.live())) {", "        if (false) {")])
mut("U17", "unit", "probe ring exception for every thread", [(GATE,
    "        static thread_local bool probing = false;", "        static bool probing = false;")])
mut("U18", "unit", "audio/session checks skip the fabric read", [(GATE,
    "        if (!open())\n            return false;\n        const FabricState fabric = "
    "readFabricState(regs_.load(std::memory_order_acquire));",
    "        return open();\n        const FabricState fabric = "
    "readFabricState(regs_.load(std::memory_order_acquire));")])
mut("U19", "unit", "no settle before the probe", [(GATE,
    "    static constexpr int64_t kSettleUs = 600000;", "    static constexpr int64_t kSettleUs = 0;")])
mut("U20", "unit", "closeNow does not ask for a fresh CORENAME", [(GATE,
    "        if (!needFreshName_) {\n            needFreshName_ = true;\n            freshWhy_ = "
    "\"waiting for the new Main to rewrite CORENAME\";\n            preReloadName_ = "
    "last_.coreName;\n        }\n        if (state_ == CoreGateState::Open)\n            closeLocked",
    "        if (state_ == CoreGateState::Open)\n            closeLocked")],
    survives="redundant layer: every load also moves Main's PID, which asks for a fresh name")
# ---- daemon paths, checked in media_player.cpp (unit source check)
mut("U21", "unit", "audio pump unchecked", [(MP,
    "            if (!coreGateAllowsAudioWrite()) {\n                audioFailure(coreGateClosedError"
    "(\"MrAudio write\"));\n                break;\n            }\n", "")])
mut("U22", "unit", "AU loop unchecked", [(MP,
    "                if (!coreGateAllowsSession()) {\n                    failFpgaSession("
    "coreGateClosedError(\"FPGA session\"));\n                    break;\n                }\n", "")])
mut("U23", "unit", "video_mode write ungated", [(MP,
    "    // Core gate: no video_mode switch while the loaded core is not a verified Plex build.\n"
    "    if (!CoreGate::process().open()) {", "    if (false) {")])
mut("U24", "unit", "fb0 clear ungated", [(MP,
    "            if (CoreGate::process().open())\n                fb_.clear();",
    "            fb_.clear();")])
mut("U25", "unit", "tick hook skips onCoreReturned", [(MP,
    "        std::lock_guard<std::mutex> lk(presentMu_);\n        fpga_.onCoreReturned();\n",
    "        std::lock_guard<std::mutex> lk(presentMu_);\n")])
mut("U26", "unit", "stale epoch kept at session end", [(MP,
    "        if (begun && !released && coreGateEpochIsStale(session, fpgaSessionCoreChanges_)) {",
    "        if (false) {")])
# ---- pairing, Main identity, the tick sequence (unit)
mut("U27", "unit", "a core load of another raster stays rejected", [(GATE,
    "        if (rasterChange) {\n"
    "            pending_.push_back(\"core gate re-paired: \" + pairedRaster_ + \" -> \" + raster +\n"
    "                               \" on core load build \" + id);\n"
    "            pairedRaster_ = raster;\n"
    "            pairedWhy_ = std::string(\"core load build \") + id;\n"
    "        }\n",
    "")])
mut("U28", "unit", "Main identity is the newest MiSTer process", [(SPI,
    "        if (oldest.pid == 0 || start < oldest.startTicks ||",
    "        if (oldest.pid == 0 || start > oldest.startTicks ||")])
mut("U29", "unit", "a moment with no Main readable is a reload", [(GATE,
    "        } else if (!first && o.main.pid != 0 && o.main != lastMain_) {",
    "        } else if (!first && o.main != last_.main) {")])
mut("U30", "unit", "step never probes", [(GATE,
    "    if (event == CoreGateEvent::ProbeWanted && !h.playing())", "    if (false)")])
mut("U31", "unit", "step probes while a cast plays", [(GATE,
    "    if (event == CoreGateEvent::ProbeWanted && !h.playing())",
    "    if (event == CoreGateEvent::ProbeWanted)")])
mut("U32", "unit", "step skips the per-core reset", [(GATE,
    "        h.coreReturned();\n        returnPending = true;", "        returnPending = true;")])
mut("U33", "unit", "step finishes the return while a cast plays", [(GATE,
    "    if (!returnPending || !gate.open() || h.playing())",
    "    if (!returnPending || !gate.open())")])
mut("U34", "unit", "step repaints a closed gate", [(GATE,
    "    if (!returnPending || !gate.open() || h.playing())",
    "    if (!returnPending || h.playing())")])
mut("U35", "unit", "a configured pairing is ignored", [(GATE,
    "        pairedRaster_ = raster;\n        pairedWhy_ = why;\n        pending_.push_back(pairingLine());",
    "        pairedWhy_ = why;\n        pending_.push_back(pairingLine());")])
# ---- v2.1: names older than Main at the first look, RBFNAME a Plex core (unit)
mut("U36", "unit", "(a) the last core's names trusted at the first look", [(GATE,
    "        if (first && o.fabric.live() && o.main.pid != 0 && namesPredateMain(o) &&\n"
    "            !needFreshName_) {",
    "        if (false) {")])
mut("U37", "unit", "(b) RBFNAME not checked for a Plex core", [(GATE,
    "        else if (!rbfNameIsPlex(o.rbfName.text))\n"
    "            hold = \"RBFNAME '\" + o.rbfName.text + \"' is not a Plex core\";\n", "")])
mut("U38", "unit", "Main's start time without its start ticks", [(GATE,
    "    return boot + static_cast<int64_t>(m.startTicks) * 1000000000LL / hz;",
    "    return boot;")])
mut("U39", "unit", "(a) names from before a clock step held as stale", [(GATE,
    "            if (o.bootWallNs >= 0 && n.mtimeNs < o.bootWallNs)\n"
    "                return false; // written before a clock step: not comparable\n", "")])
mut("U40", "unit", "btime 0 (RTC-less, before ntpd's step) read as unknown", [(GATE,
    "    if (end == digits || btime < 0)\n        return -1;",
    "    if (end == digits || btime <= 0)\n        return -1;")])
mut("U41", "unit", "Main's start unknown when btime is 0", [(GATE,
    "    if (hz <= 0 || boot < 0)\n        return 0;", "    if (hz <= 0 || boot <= 0)\n        return 0;")])
# review 11:36 R3: the MPX_BROWSE_V2 direct readers' gate (coreGateAllowsQuiet) ignores a closed gate.
# Their mapper still refuses, but loudly (setErr), which the closed-phase quiet row sees.
mut("U42", "unit", "quiet mailbox readers ignore a closed gate", [(SPI,
    "bool FpgaSpi::coreGateAllowsQuiet() {\n    CoreGate& gate = CoreGate::process();\n"
    "    if (!gate.managed())\n        return true;\n    if (!gate.open())\n        return false;\n",
    "bool FpgaSpi::coreGateAllowsQuiet() {\n    CoreGate& gate = CoreGate::process();\n"
    "    if (!gate.managed())\n        return true;\n")])
# ---- whole daemon (e2e)
mut("E1", "e2e", "no main-loop tick", [(MAIN,
    "        player.coreGateTick();\n"
    "        if (player.owenbIdleWanted()) {   // OWENB-P1B T7: the PMS screen on the OB idle card\n",
    "        if (player.owenbIdleWanted()) {   // OWENB-P1B T7: the PMS screen on the OB idle card\n")])
mut("E2", "e2e", "tick hook never probes", [(MP,
    "        return fpga_.runCoreGateProbe(500);", "        return CoreGateEvent::None;")])
mut("E3", "e2e", "no repaint when the gate opens", [(MP,
    "        probeMastAtGateOpen();\n    };\n    h.repaint = [this] { paintIdle(); };",
    "        probeMastAtGateOpen();\n    };\n    h.repaint = [] {};")])
mut("E4", "e2e", "paintIdle and ensureDdrMap ungated", [
    (MP, "    // Core gate: never paint or ring the doorbell while another core is loaded.\n"
         "    if (!CoreGate::process().open() && !pairingPaint)\n        return;\n", ""),
    (SPI, '    if (!coreGateAllows("ensureDdrMap"))\n        return false;\n', "")])
mut("E5", "e2e", "OSD poll and SPI frame ungated", [
    (MP, "            bool viaMailbox = false;\n            if (CoreGate::process().open()) {\n"
         "                std::lock_guard<std::mutex> lk(presentMu_);",
         "            bool viaMailbox = false;\n            if (true) {\n"
         "                std::lock_guard<std::mutex> lk(presentMu_);"),
    (SPI, "    const bool open = gate.open();\n"
          "    const bool hbIdle = statusHeartbeatScope() && gate.idleErrorPaintAllowed();\n"
          "    if (!open && !hbIdle)\n"
          "        return false;\n"
          "    const FabricState fabric = readFabricState(map);",
          "    const FabricState fabric = readFabricState(map);")])
# T-CG-MUTANTS-E3B-E5B (review 00:43): browse P1's V2 copies of E3's and E5's sites, unpinned since 591bca98 anchored
# E3/E5 on the legacy sites (media_player.cpp :1240 enableBrowseV2's hook, :944 startOsdPollV2 @8d52ace6).
# E3b: the browse controller's repaint hook paints nothing; killed by the browse e2e's v2 scenario (two browse steps must
# each move the frame bank, each polled <= 5 s; the IDLE_SCREEN=logo safety net is 30 s,
# host/libmisterplex/r1_idle_paint.hpp:247, so two moves within 10 s cannot both be safety-net ticks).
mut("E3b", "browse", "no repaint from the browse controller (V2 hook)", [(MP,
    "    BrowseController::Hooks h;\n    h.repaint = [this] { paintIdle(); };",
    "    BrowseController::Hooks h;\n    h.repaint = [] {};")])
# E5b mirrors E5's two edits. The V2 poll's gate alone is an equivalent mutant: with the gate closed,
# readOsdMailboxDirect refuses at its own gate (fpga_spi.cpp :1631) and the getCoreStatus fallback's SPI frame is
# refused at the frame gate (E5's second edit), so nothing observable happens. With both removed, a closed-gate V2
# poll strobes SPI; killed by the e2e's busyv2 case (quiet() while MENU is loaded).
# COUPLING (H264 e4): that equivalence holds only while readOsdMailboxDirect keeps its own coreGateAllowsQuiet gate
# (fpga_spi.cpp :1630-1631 @8d52ace6). If that gate moves or changes, re-derive E5b (a one-edit E5b may then be live).
mut("E5b", "e2e", "V2 OSD poll and SPI frame ungated", [
    (MP, "            if (CoreGate::process().open()) {\n"
         "                // The mailbox read takes no presentMu_ (readOsdMailboxDirect: only the doorbell-",
         "            if (true) {\n"
         "                // The mailbox read takes no presentMu_ (readOsdMailboxDirect: only the doorbell-"),
    (SPI, "    const bool open = gate.open();\n"
          "    const bool hbIdle = statusHeartbeatScope() && gate.idleErrorPaintAllowed();\n"
          "    if (!open && !hbIdle)\n"
          "        return false;\n"
          "    const FabricState fabric = readFabricState(map);",
          "    const FabricState fabric = readFabricState(map);")])
mut("E6", "e2e", "gate starts open (unmanaged)", [(GATE,
    "    static constexpr bool kStartOpen = false;", "    static constexpr bool kStartOpen = true;")])
mut("E7", "e2e", "a stale CORENAME is trusted", [(GATE,
    "        else if (needFreshName_)\n            hold = freshWhy_;",
    "        else if (false)\n            hold = freshWhy_;")])
mut("E8", "e2e", "allowlist bypass", [(SPI,
    "        if (!fpgaBuildRefusal(caps.build_id).empty())\n"
    "            refusal = \"not on the allowlist\";\n", "")])
mut("E9", "e2e", "a core load of another raster stays rejected", [(GATE,
    "        if (rasterChange) {\n"
    "            pending_.push_back(\"core gate re-paired: \" + pairedRaster_ + \" -> \" + raster +\n"
    "                               \" on core load build \" + id);\n"
    "            pairedRaster_ = raster;\n"
    "            pairedWhy_ = std::string(\"core load build \") + id;\n"
    "        }\n",
    "")])
mut("E10", "e2e", "Main identity is the newest MiSTer process", [(SPI,
    "        if (oldest.pid == 0 || start < oldest.startTicks ||",
    "        if (oldest.pid == 0 || start > oldest.startTicks ||")])
mut("E11", "e2e", "the daemon is not paired with its FPGA mode at start", [(MAIN,
    "        misterplex::CoreGate::process().setPairedRaster(\n", "        (void)(\n")])
mut("E12", "e2e", "(a) the last core's names trusted at the first look", [(GATE,
    "        if (first && o.fabric.live() && o.main.pid != 0 && namesPredateMain(o) &&\n"
    "            !needFreshName_) {",
    "        if (false) {")])
mut("E13", "e2e", "set_status skips the core gate", [(SET_STATUS,
    "    if (!misterplex::coreGateVerifyForTool(spi, \"set_status\"))\n        return 3;\n", "")])
mut("E14", "e2e", "(b) RBFNAME not checked for a Plex core", [(GATE,
    "        else if (!rbfNameIsPlex(o.rbfName.text))\n"
    "            hold = \"RBFNAME '\" + o.rbfName.text + \"' is not a Plex core\";\n", "")])
mut("E15", "e2e", "the tools ignore a running misterplexd", [(TOOL,
    "    if (misterplexdRunning()) {", "    if (false) {")])
mut("E16", "e2e", "(a) names from before a clock step held as stale", [(GATE,
    "            if (o.bootWallNs >= 0 && n.mtimeNs < o.bootWallNs)\n"
    "                return false; // written before a clock step: not comparable\n", "")])
mut("E17", "e2e", "the daemon's observe hook does not read boot", [(MP,
    "        o.bootWallNs = bootWallNs();\n", "")])
mut("E18", "e2e", "the tools' observe hook does not read boot", [(TOOL,
        "        o.bootWallNs = bootWallNs();\n", "")])
mut("E19", "e2e", "btime 0 (RTC-less, before ntpd's step) read as unknown", [(GATE,
    "    if (end == digits || btime < 0)\n        return -1;",
    "    if (end == digits || btime <= 0)\n        return -1;")])
mut("E20", "e2e", "Main's start unknown when btime is 0", [(GATE,
    "    if (hz <= 0 || boot < 0)\n        return 0;", "    if (hz <= 0 || boot <= 0)\n        return 0;")])
# ---- a running session in the real daemon (session e2e, libav host build)
mut("S1", "sess", "AU ring map ungated", [(SPI,
    '    if (!coreGateAllows("ensureBitstreamDdrMap", /*ringProbe=*/true))\n        return false;\n',
    "")])
mut("S2", "sess", "stale epoch kept at session end", [(MP,
    "        if (begun && !released && coreGateEpochIsStale(session, fpgaSessionCoreChanges_)) {",
    "        if (false) {")])
mut("S3", "sess", "AU loop unchecked", [(MP,
    "                if (!coreGateAllowsSession()) {\n                    failFpgaSession("
    "coreGateClosedError(\"FPGA session\"));\n                    break;\n                }\n", "")],
    survives="second layer: the gated AU push below it stops the session too (U22 catches the "
             "removal in the source check)")

# ---- v2.3: the MrAudio session lane in the real daemon (audio recovery e2e, libav build)
mut("A1", "audio", "a Begin the consumer did not adopt read as owned", [(SPI,
    "    return ours ? AudioLane::Owned : AudioLane::NotOwned;",
    "    return AudioLane::Owned;")], scenarios="reboot")
mut("A2", "audio", "the command is not withdrawn before the lane is read", [(SPI,
    "    words[5] = 0;\n    __sync_synchronize();\n"
    "    if (!readFreshAudioConsumerStatus(kAudioLaneFreshPublications, kAudioLaneFreshMs, status)) {",
    "    if (!readFreshAudioConsumerStatus(kAudioLaneFreshPublications, kAudioLaneFreshMs, status)) {")],
    survives="second layer: a Begin forwarded after the verdict is an orphan, and the re-begin's "
             "recoverOrphanedAudioConsumer reclaims it (A4)", scenarios="reboot reboot_reject")
mut("A3", "audio", "no re-begin after a Begin the consumer did not adopt", [(MP,
    "                        if (sessionAttempt == 1 && !audioBegun && current() &&",
    "                        if (false && sessionAttempt == 1 && !audioBegun && current() &&")],
    scenarios="reboot")
mut("A4", "audio", "a killed owner's consumer is not reclaimed", [(SPI,
    "    if (!recoverOrphanedAudioConsumer(capabilities, timeout_ms))\n        return false;\n", "")],
    scenarios="killed")
mut("A5", "audio", "command tokens restart at 0 in a new daemon", [(SPI,
    "    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);",
    "    return 0;")], scenarios="killed")
mut("A6", "audio", "a Reset with nothing of ours to reset is not done", [(SPI,
    "        if (command == AudioControl::Reset) {\n            clearErr();\n            return true;\n"
    "        }\n", "")], scenarios="midreset")
mut("A7", "audio", "the orphan check trusts a stale MAST", [(SPI,
    "    if (!readFreshAudioConsumerStatus(kAudioLaneFreshPublications,\n"
    "                                      std::min(timeout_ms, kAudioLaneFreshMs), orphan)) {",
    "    if (!readAudioConsumerStatus(orphan)) {")], scenarios="wedged")


def apply(mdir, edits):
    for rel, find, repl in edits:
        dst = os.path.join(mdir, rel)
        src = dst if os.path.exists(dst) else os.path.join(ROOT, rel)
        text = open(src).read()
        n = text.count(find)
        if n != 1:
            raise RuntimeError(f"{rel}: find text occurs {n} times: {find[:60]!r}")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "w").write(text.replace(find, repl))


def run_unit(mid, mdir):
    mp = os.path.join(mdir, MP)
    if not os.path.exists(mp):  # the source check reads media_player.cpp from MPX_SOURCE_ROOT
        os.makedirs(os.path.dirname(mp), exist_ok=True)
        shutil.copy(os.path.join(ROOT, MP), mp)
    spi = os.path.join(mdir, SPI)
    spi = spi if os.path.exists(spi) else os.path.join(ROOT, SPI)
    exe = os.path.join(mdir, "test_core_gate")
    build = subprocess.run(
        ["g++", "-std=c++17", "-O2", "-I" + os.path.join(mdir, "host"), "-I" + os.path.join(ROOT, "host"),
         "-I" + os.path.join(ROOT, "arm/misterplexd"), "-pthread",
         f'-DMPX_DEVMEM_PATH="{os.path.join(mdir, "fake.mem")}"', f'-DMPX_SOURCE_ROOT="{mdir}"',
         "-o", exe, os.path.join(ROOT, "tests/unit/test_core_gate.cpp"), spi],
        capture_output=True, text=True)
    if build.returncode != 0:
        open(os.path.join(mdir, "build.log"), "w").write(build.stderr)
        return "invalid (no build)"
    run = subprocess.run([exe], capture_output=True, text=True, timeout=600)
    open(os.path.join(mdir, "run.log"), "w").write(run.stdout + run.stderr)
    if run.returncode == 77:   # test_core_gate's own VOID: not in a private PID ns with its own /proc
        return "invalid (VOID 77: run inside heavy-sim-slot + unshare --pid --fork --mount-proc)"
    fails = [l for l in (run.stdout + run.stderr).splitlines() if l.startswith("FAIL ")]
    return f"killed ({len(fails)} FAIL)" if fails else ("killed (rc)" if run.returncode else "SURVIVED")


def run_e2e(mid, mdir):
    env = dict(os.environ, CORE_GATE_MUT_DIR=mdir, CORE_GATE_E2E_OUT=os.path.join(mdir, "e2e"))
    run = subprocess.run(["bash", os.path.join(ROOT, "tests/unit/test_core_gate_daemon_e2e.sh")],
                         capture_output=True, text=True, env=env, timeout=900)
    out = run.stdout + run.stderr
    open(os.path.join(mdir, "run.log"), "w").write(out)
    fails = [l for l in out.splitlines() if l.startswith("FAIL [")]
    if fails:
        return "killed: " + fails[0][:110]
    return "invalid (no build)" if run.returncode else "SURVIVED"


def run_browse(mid, mdir):
    """T-CG-MUTANTS-E3B-E5B: the browse e2e's v2 scenario on the mutated copies (test_browse_e2e.sh builds its head
    daemon from CORE_GATE_MUT_DIR, :49-60). Its fail() prints 'FAIL [<case>]: ...'."""
    env = dict(os.environ, CORE_GATE_MUT_DIR=mdir, BROWSE_E2E_OUT=os.path.join(mdir, "browse"),
               BROWSE_E2E_SCENARIOS="v2")
    run = subprocess.run(["bash", os.path.join(ROOT, "tests/unit/test_browse_e2e.sh")],
                         capture_output=True, text=True, env=env, timeout=1200)
    out = run.stdout + run.stderr
    open(os.path.join(mdir, "run.log"), "w").write(out)
    fails = [l for l in out.splitlines() if l.startswith("FAIL [")]
    if fails:
        return "killed: " + fails[0][:110]
    return "invalid (no build)" if run.returncode else "SURVIVED"


def run_sess(mid, mdir):
    env = dict(os.environ, CORE_GATE_MUT_DIR=mdir,
               CORE_GATE_SESSION_OUT=os.path.join(mdir, "session"))
    run = subprocess.run(["bash", os.path.join(ROOT, "tests/unit/test_core_gate_session_e2e.sh")],
                         capture_output=True, text=True, env=env, timeout=900)
    out = run.stdout + run.stderr
    open(os.path.join(mdir, "run.log"), "w").write(out)
    if "SKIP" in out:
        return "invalid (session e2e skipped: no host libav/ffmpeg)"
    fails = [l for l in out.splitlines() if l.startswith("FAIL [")]
    if fails:
        return "killed: " + fails[0][:110]
    return "invalid (no build)" if run.returncode else "SURVIVED"


def run_audio(mid, mdir):
    env = dict(os.environ, CORE_GATE_MUT_DIR=mdir, AUDIO_RECOVERY_OUT=os.path.join(mdir, "audio"))
    if mid in SCENARIOS:
        env["AUDIO_RECOVERY_SCENARIOS"] = SCENARIOS[mid]
    run = subprocess.run(["bash", os.path.join(ROOT, "tests/unit/test_audio_recovery_e2e.sh")],
                         capture_output=True, text=True, env=env, timeout=1800)
    out = run.stdout + run.stderr
    open(os.path.join(mdir, "run.log"), "w").write(out)
    if "SKIP" in out:
        return "invalid (audio e2e skipped: no host libav/ffmpeg)"
    fails = [l for l in out.splitlines() if l.startswith("FAIL [")]
    if fails:
        return "killed: " + fails[0][:110]
    return "invalid (no build)" if run.returncode else "SURVIVED"


def audit():
    """Preflight (review 00:37): every mutant anchor occurs exactly once in its file, else exit 2 naming
    the anchors, before any build. Browse P1 (e6cf24fa) had duplicated E3's and E5's, which left the
    whole e2e class unrunnable."""
    bad, n = [], 0
    for mid, test, desc, edits, survives in M:
        for rel, find, repl in edits:
            n += 1
            k = open(os.path.join(ROOT, rel)).read().count(find)
            if k != 1:
                bad.append(f"AUDIT {mid} ({test}) {rel}: find text occurs {k} times: {find.strip()[:60]!r}")
    for b in bad:
        print(b, flush=True)
    print(f"core_gate_mutants --audit: {n - len(bad)}/{n} anchors unique" + (f", {len(bad)} NOT" if bad else ""),
          flush=True)
    return 2 if bad else 0


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    only = set(sys.argv[2:])
    rc = audit()
    if which == "--audit" or rc:
        return rc
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    for mid, test, desc, edits, survives in M:
        if (which != "all" and test != which) or (only and mid not in only):
            continue
        mdir = os.path.join(OUT, mid)
        shutil.rmtree(mdir, ignore_errors=True)
        os.makedirs(mdir)
        apply(mdir, edits)
        runner = {"unit": run_unit, "e2e": run_e2e, "sess": run_sess, "audio": run_audio, "browse": run_browse}[test]
        verdict = runner(mid, mdir)
        ok = verdict.startswith("killed") or (survives and verdict == "SURVIVED")
        note = f" (expected: {survives})" if survives and verdict == "SURVIVED" else ""
        if not ok:
            bad += 1
        print(f"{mid:4} {test:4} {desc:48} {verdict}{note}", flush=True)
    print("core_gate_mutants: " + ("OK" if bad == 0 else f"{bad} unexpected"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
