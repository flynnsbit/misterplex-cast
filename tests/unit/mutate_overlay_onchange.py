#!/usr/bin/env python3
"""Mutants of the overlay publisher's lab flags: MPX_OVERLAY_ONCHANGE (the gate, the
content key, the model), MPX_OVERLAY_SKIP_NO_PLANE (the BUILD_ID table, the decision),
the publisher thread and quiesce-then-hide (OverlayPublisherThread, finishFpgaPlayback),
MPX_OVERLAY_DECOUPLE's presentMu_ order and its non-blocking hot-path error text.

  python3 tests/unit/mutate_overlay_onchange.py

Each mutant is one string replacement in a copy of host/libmisterplex and
arm/misterplexd (test_overlay_onchange links the real fpga_spi.cpp). The test must
then fail; the unmutated copy must pass. The same script runs on every daemon line.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PUB = "host/libmisterplex/overlay_publish.hpp"
TERM = "host/libmisterplex/fpga_terminal.hpp"
SPI = "arm/misterplexd/fpga_spi.cpp"
TEST = "test_overlay_onchange"
MUTANTS = [
    # name, file, old, new
    ("skipped-on-change", PUB,
     "        if (key != lastKey_)\n            return OverlayPublishReason::Changed;\n", ""),
    ("keepalive-never", PUB, "        if (nowUs - lastSendUs_ >= keepaliveUs_)", "        if (false)"),
    ("no-confirm", PUB,
     "        confirmPending_ = onChange_ && fresh && reason != OverlayPublishReason::Confirm;",
     "        confirmPending_ = false;"),
    ("force-ignored", PUB,
     "        if (forceGen != forceSeen_)\n            return OverlayPublishReason::Forced;\n", ""),
    ("failure-as-success", PUB,
     "        if (!ok) {\n            failed_ = true;\n            retryFresh_ = fresh;\n            return;\n        }\n",
     "        (void)ok;\n"),
    ("retry-loses-fresh", PUB, "            retryFresh_ = fresh;\n", "            retryFresh_ = false;\n"),
    ("hidden-key-counts-text", PUB,
     "    if (!(fpga_overlay::get16(p.data() + 28) & fpga_overlay::Visible)) {", "    if (false) {"),
    ("hidden-key-counts-scale", PUB,
     "        mix(28, 31); // flags and state (scale, byte 31, is not drawn either)\n",
     "        mix(28, 32);\n"),
    ("visible-key-ignores-plane", PUB, "    mix(64, p.size());\n", ""),
    ("visible-key-ignores-colours", PUB, "    mix(28, 60);\n", "    mix(28, 36);\n"),
    ("off-not-periodic", PUB,
     "        if (!onChange_)\n            return OverlayPublishReason::Periodic;\n", ""),
    ("keepalive-env-ignored", PUB, "            s.keepaliveMs = v;", "            (void)v;"),
    ("paused-built-hidden", PUB,
     "    if (in.presented && !in.paused && !in.visible) {", "    if (in.presented && !in.visible) {"),
    ("no-plane-ignored", PUB,
     "    return !(s == OverlaySkipNoPlaneSetting::On && overlayPlaneAbsent(buildId));",
     "    return true;"),
    ("no-plane-everywhere", PUB,
     "    default:\n        return false;\n    }\n}\n\n// Whether a session",
     "    default:\n        return true;\n    }\n}\n\n// Whether a session"),
    ("no-plane-misses-5d52", PUB, "    case 0x48005D52u:\n", ""),
    ("no-plane-misses-5d45", PUB, "    case 0x48005D45u:\n", ""),
    ("no-plane-env-ignored", PUB,
     'if (std::strcmp(raw, "1") == 0)\n        return OverlaySkipNoPlaneSetting::On;',
     'if (std::strcmp(raw, "1") == 0)\n        return OverlaySkipNoPlaneSetting::Off;'),
    ("publisher-ignores-stop", PUB,
     "            h.live = [this, live = hooks.live] { return !stop_.load() && live(); };",
     "            h.live = hooks.live;"),
    ("release-before-quiesce", TERM,
     "        quiesce();\n        try { receipt.beforeReset = capture(); } catch (...) {}\n"
     "        receipt.cleanup = \"release-failed\";\n        receipt.released = release();\n",
     "        receipt.released = release();\n        quiesce();\n"
     "        try { receipt.beforeReset = capture(); } catch (...) {}\n"
     "        receipt.cleanup = \"release-failed\";\n"),
    ("fresh-ignores-force", PUB,
     " || forceGen != forceSeen_ ||\n                           retryFresh_;",
     " ||\n                           retryFresh_;"),
    ("loop-fail-limit-50", PUB, "            if (++failedTransfers >= 5) {", "            if (++failedTransfers >= 50) {"),
    ("loop-fail-limit-4", PUB, "            if (++failedTransfers >= 5) {", "            if (++failedTransfers >= 4) {"),
    ("loop-no-reset", PUB,
     "        if (sent) {\n            failedTransfers = 0;\n        } else {",
     "        if (sent) {\n        } else {"),
    ("loop-sent-always-ok", PUB,
     "        gate.sent(why, key, forceGen, sent, h.nowUs());",
     "        gate.sent(why, key, forceGen, true, h.nowUs());"),
    ("loop-catch-silent", PUB,
     "                if (h.failSession)\n                    h.failSession(\"native overlay publisher failed\");\n",
     ""),
    ("loop-stopped-counted", PUB,
     "            break; // stopped before the send: not an attempt\n        counts.note(why);\n",
     "        {\n            counts.note(why);\n            break;\n        }\n        counts.note(why);\n"),
    ("loop-done-skipped-on-throw", PUB,
     "                    h.failSession(\"native overlay publisher failed\");\n            }\n            if (done) {",
     "                    h.failSession(\"native overlay publisher failed\");\n                return;\n            }\n            if (done) {"),
    ("publish-default-unlocked", PUB, "    auto present = lockPresent();\n", ""),
    ("publish-decoupled-locks", PUB,
     "    if (decoupled) {\n        if (!live())",
     "    if (decoupled) {\n        auto held = lockPresent();\n        if (!live())"),
    ("clearErr-blocking", SPI,
     "    if (hotErrorsNonBlocking().load(std::memory_order_relaxed)) {\n"
     "        if (!updateErrorUnlessBusy(spiMutex(), err_, nullptr))",
     "    if (false) {\n        if (!updateErrorUnlessBusy(spiMutex(), err_, nullptr))"),
    ("setErrHot-blocking", SPI,
     "    if (hotErrorsNonBlocking().load(std::memory_order_relaxed)) {\n"
     "        if (!updateErrorUnlessBusy(spiMutex(), err_, msg.c_str()))",
     "    if (false) {\n        if (!updateErrorUnlessBusy(spiMutex(), err_, msg.c_str()))"),
    ("presentation-read-not-hot", SPI,
     'requireBitstreamSession(session_id, "readVideoPresentation", true)',
     'requireBitstreamSession(session_id, "readVideoPresentation", false)'),
]


def build_and_run(tree, work, name):
    out = os.path.join(work, name + ".bin")
    r = subprocess.run(["g++", "-std=c++17", "-O1", "-w", "-pthread",
                        f"-I{tree}/host", f"-I{tree}/arm/misterplexd", "-o", out,
                        os.path.join(ROOT, "tests/unit", TEST + ".cpp"),
                        os.path.join(tree, SPI)], capture_output=True, text=True)
    if r.returncode != 0:
        return "compile-error", r.stderr[-400:]
    try:
        r = subprocess.run([out], capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "fail", "FAIL hang (no result in 120 s)"
    return ("pass" if r.returncode == 0 else "fail"), (r.stdout + r.stderr)


with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as work:
    def overlay(name, path=None, old=None, new=None):
        d = os.path.join(work, name)
        shutil.copytree(os.path.join(ROOT, "host/libmisterplex"), os.path.join(d, "host/libmisterplex"))
        shutil.copytree(os.path.join(ROOT, "arm/misterplexd"), os.path.join(d, "arm/misterplexd"))
        if path:
            p = os.path.join(d, path)
            text = open(p).read()
            assert text.count(old) == 1, (name, text.count(old))
            open(p, "w").write(text.replace(old, new))
        return d

    result, tail = build_and_run(overlay("base"), work, "base")
    assert result == "pass", (result, tail[-600:])
    print("baseline: test_overlay_onchange passes")
    killed = 0
    for name, path, old, new in MUTANTS:
        result, tail = build_and_run(overlay(name, path, old, new), work, name)
        if result == "compile-error":
            sys.exit(f"mutant {name} does not compile: {tail}")
        if result == "pass":
            sys.exit(f"mutant {name} SURVIVED")
        first = next((l for l in tail.splitlines() if l.startswith("FAIL")), tail.strip().splitlines()[-1])
        print(f"  killed {name:28s} {first[first.find('test_overlay_onchange.cpp'):][:110]}")
        killed += 1
print(f"OVERLAY_ONCHANGE_MUTANTS PASS killed={killed}/{len(MUTANTS)}")
