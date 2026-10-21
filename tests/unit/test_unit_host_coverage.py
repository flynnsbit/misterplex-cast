#!/usr/bin/env python3
"""unit-host coverage (canon port of unit-host-restore f89baa89 + f178f3c9 + e767adc1; review / review 10:49).

make unit-host is the clean-checkout gate. Its recipe is unit-unlocked's, verbatim and in the same order, minus
the commands listed in tests/unit/unit_host_exclusions.txt. This check keeps the two in step:
  C1  every unit-host-unlocked command is a unit-unlocked command, and they appear in unit-unlocked's order;
  C2  every unit-unlocked command is in unit-host-unlocked or in the exclusions file, never both;
  C3  every exclusion names a unit-unlocked command and carries a class (a, c, rtl-sim, rtl-tooling) and a reason;
  C4  both targets take the same prerequisites: the recipe rules share $(UNIT_PREREQS), and every other rule that names
      unit-unlocked is 'unit-unlocked unit-host-unlocked: ...' (no rule names only one of them);
  C5  make unit-host keeps its SLOT-FORM guard (refuses the host pid ns, a pid ns without its own /proc, and a
      visible host /tmp/misterplexd.lock, uid 0).
A new unit-unlocked test therefore fails here until it is added to unit-host or excluded with a reason.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAKEFILE = os.path.join(ROOT, "Makefile")
EXCLUSIONS = os.path.join(ROOT, "tests", "unit", "unit_host_exclusions.txt")
CLASSES = {"a", "c", "rtl-sim", "rtl-tooling"}


def norm(cmd):
    return re.sub(r"\s+", " ", cmd.strip().lstrip("@"))


def logical(lines):
    """[(first physical line no, joined text, the physical lines)] with backslash continuations joined."""
    out, i = [], 0
    while i < len(lines):
        start, text, phys = i, lines[i], [lines[i]]
        while text.endswith("\\") and i + 1 < len(lines):
            i += 1
            text = text[:-1] + " " + lines[i].strip()
            phys.append(lines[i])
        out.append((start, text, phys))
        i += 1
    return out


def targets_of(text):
    if text.startswith(("\t", "#", " ")) or ":" not in text or "=" in text.split(":", 1)[0]:
        return []
    return text.split(":", 1)[0].split()


def recipe(lines, target):
    """The one rule naming only <target> that has a recipe (tab-indented lines right after its header)."""
    found = None
    log = logical(lines)
    for k, (start, text, phys) in enumerate(log):
        if text.startswith("\t") or targets_of(text) != [target]:
            continue
        j = start + len(phys)
        if j < len(lines) and lines[j].startswith("\t"):
            if found is not None:
                raise SystemExit(f"FAIL: two '{target}:' rules with a recipe")
            out = []
            while j < len(lines) and lines[j].startswith("\t"):
                out.append(norm(lines[j]))
                j += 1
            found = (norm(text), out)
    if found is None:
        raise SystemExit(f"FAIL: no '{target}:' rule with a recipe")
    return found


def main():
    lines = open(MAKEFILE).read().split("\n")
    fails = []
    uu_rule, uu = recipe(lines, "unit-unlocked")
    uh_rule, uh = recipe(lines, "unit-host-unlocked")

    excl = {}
    for n, raw in enumerate(open(EXCLUSIONS).read().split("\n"), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        parts = [p.strip() for p in raw.split(" | ")]
        if len(parts) != 3 or parts[1] not in CLASSES or not parts[2]:
            fails.append(f"C3 exclusions:{n} is not '<command> | <class> | <reason>' with a known class: {raw[:100]}")
            continue
        if parts[0] in excl:
            fails.append(f"C3 exclusions:{n} duplicates an entry: {parts[0][:100]}")
        excl[norm(parts[0])] = parts[1]

    pos = {c: i for i, c in enumerate(uu)}
    last = -1
    for c in uh:
        if c not in pos:
            fails.append(f"C1 unit-host runs a command unit-unlocked does not: {c[:110]}")
        elif pos[c] < last:
            fails.append(f"C1 unit-host runs a command out of unit-unlocked's order: {c[:110]}")
        else:
            last = pos[c]
    in_host = set(uh)
    for c in uu:
        if c in in_host and c in excl:
            fails.append(f"C2 both run by unit-host and excluded: {c[:110]}")
        elif c not in in_host and c not in excl:
            fails.append(f"C2 unit-unlocked command neither in unit-host nor excluded: {c[:110]}")
    for c in excl:
        if c not in pos:
            fails.append(f"C3 stale exclusion (not a unit-unlocked command): {c[:110]}")

    if uu_rule != "unit-unlocked: preflight $(UNIT_PREREQS)" or uh_rule != "unit-host-unlocked: preflight $(UNIT_PREREQS)":
        fails.append(f"C4 recipe rules differ from 'preflight $(UNIT_PREREQS)': [{uu_rule}] [{uh_rule}]")
    shared = 0
    for start, text, phys in logical(lines):
        t = targets_of(text)
        if "unit-unlocked" not in t and "unit-host-unlocked" not in t:
            continue
        if norm(text) in (uu_rule, uh_rule):
            continue
        if sorted(t) != ["unit-host-unlocked", "unit-unlocked"]:
            fails.append(f"C4 Makefile:{start + 1} names {' '.join(t)} alone; use 'unit-unlocked unit-host-unlocked:' "
                         f"so both targets take the same prerequisites")
        else:
            shared += 1
    text = "\n".join(lines)
    guard = re.search(r"^unit-host:\n((?:\t.*\n)+)", text + "\n", re.M)
    body = guard.group(1) if guard else ""
    for needle in ('"pid:[4026531836]"', "readlink /proc/1/ns/pid", "-e /tmp/misterplexd.lock", '"$$(id -u)" = 0', "exit 2",
                   "$(MAKE) unit-host-unlocked"):
        if needle not in body:
            fails.append(f"C5 make unit-host lost its SLOT-FORM guard piece: {needle}")

    for f in fails:
        print("FAIL " + f)
    if fails:
        print(f"test_unit_host_coverage: {len(fails)} FAIL")
        return 1
    print(f"test_unit_host_coverage: OK (unit-unlocked {len(uu)} commands = unit-host {len(uh)} + excluded {len(excl)}; "
          f"same order; same prerequisites ($(UNIT_PREREQS) + {shared} shared rules); SLOT-FORM guard)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
