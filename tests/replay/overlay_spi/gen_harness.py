#!/usr/bin/env python3
"""Generate one overlay SPI replay program from a ref's arm/misterplexd/media_player.cpp.

  gen_harness.py <media_player.cpp> <harness_template.cpp> <out.cpp> <ref label>

Extracts, verbatim, the four regions that hold the native-overlay publisher:
  BLOCK     fpgaThreadMain, after `std::deque<PendingPicture> pending;` up to
            `int64_t firstPtsUs = ...` (the publisher thread / lambda and, on later refs,
            the lab flags and the shared publisher)
  CALLSITE  after `aspectSent = true;` up to the brace that closes `if (!aspectSent)`
  QUIESCE   finishFpgaPlayback's quiesce lambda, after `demux.requestStop();` up to
            `if (audioThr_.joinable()) audioThr_.join();`
  RELEASE   its release lambda, from `std::lock_guard<std::mutex> control(pauseControlMu_);`
            up to `bool released = !begun;`
applies the two clock substitutions (counted below), and writes <out.cpp>. Writes
<media_player.cpp>.residual: the file with the four regions cut out, for the diff of
everything else. Prints what it extracted.
"""
import sys

src, template, out, label = sys.argv[1:5]
text = open(src).read()

SLEEP = "std::this_thread::sleep_for(std::chrono::milliseconds(200))"
NOW = "std::chrono::steady_clock::now()"


def find_once(hay, needle, start=0):
    i = hay.find(needle, start)
    if i < 0:
        sys.exit(f"gen_harness: marker not found: {needle!r}")
    return i


def region(start_marker, end_marker, after=0, include_start=False):
    s = find_once(text, start_marker, after)
    body_start = s if include_start else s + len(start_marker)
    e = find_once(text, end_marker, body_start)
    return body_start, e


if text.count("                    aspectSent = true;\n") != 1:
    sys.exit("gen_harness: expected one first-AU `aspectSent = true;`")

regions = {}
regions["BLOCK"] = region("    std::deque<PendingPicture> pending;\n",
                          "    int64_t firstPtsUs = ddr_bitstream_ring::kNoTimestamp;\n")
a = find_once(text, "                    aspectSent = true;\n") + len("                    aspectSent = true;\n")
b = find_once(text, "\n                }\n", a)
regions["CALLSITE"] = (a, b + 1)
q0 = find_once(text, "        captureTerminal, [&] {\n")
regions["QUIESCE"] = region("        demux.requestStop();\n",
                            "        if (audioThr_.joinable()) audioThr_.join();\n", q0)
regions["RELEASE"] = region("        std::lock_guard<std::mutex> control(pauseControlMu_);\n",
                            "        bool released = !begun;\n", regions["QUIESCE"][1], include_start=True)

order = sorted(regions.items(), key=lambda kv: kv[1][0])
for (n1, (s1, e1)), (n2, (s2, e2)) in zip(order, order[1:]):
    if e1 > s2:
        sys.exit(f"gen_harness: regions {n1} and {n2} overlap")

residual, pos = [], 0
for name, (s, e) in order:
    residual.append(text[pos:s])
    residual.append(f"@@{name}@@\n")
    pos = e
residual.append(text[pos:])
open(src + ".residual", "w").write("".join(residual))
open(src + ".block", "w").write(text[regions["BLOCK"][0]:regions["BLOCK"][1]])

snips, counts = {}, {}
for name, (s, e) in regions.items():
    body = text[s:e]
    counts[name] = (body.count(SLEEP), body.count(NOW))
    body = body.replace(SLEEP, "harness::sleepMs(200)").replace(NOW, "harness::steadyNow()")
    for bad in ("sleep_for", "steady_clock::now", "system_clock::now"):
        if bad in body:
            sys.exit(f"gen_harness: {name} still uses {bad} after the substitutions")
    snips[name] = body

has_old = "    std::atomic<bool> stopOverlay{false};\n" in snips["BLOCK"]
has_lab = "    OverlayPublisherThread overlayPublisher;\n" in snips["BLOCK"]
has_pub = '#include "libmisterplex/overlay_publish.hpp"' in text
if not (has_old or has_lab):
    sys.exit("gen_harness: no overlay publisher in BLOCK")

t = open(template).read()
for name, body in snips.items():
    key = f"@{name}@"
    if t.count(key) != 1:
        sys.exit(f"gen_harness: template needs one {key}")
    t = t.replace(key, body)
head = (f"#define HARNESS_REF \"{label}\"\n"
        f"#define HARNESS_HAS_OLD {int(has_old)}\n"
        f"#define HARNESS_HAS_LAB {int(has_lab)}\n"
        f"#define HARNESS_HAS_PUBLISH_HPP {int(has_pub)}\n")
open(out, "w").write(head + t)

line_of = lambda i: text.count("\n", 0, i) + 1
print(f"ref {label}: base loop {'yes' if has_old else 'no'}, shared publisher {'yes' if has_lab else 'no'}")
for name, (s, e) in order:
    print(f"  {name:8s} media_player.cpp:{line_of(s)}-{line_of(e) - 1}  "
          f"sleep_for(200ms)->sleepMs: {counts[name][0]}  steady_clock::now->steadyNow: {counts[name][1]}")
