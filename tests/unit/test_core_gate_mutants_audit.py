#!/usr/bin/env python3
"""core_gate_mutants --audit teeth (T-CG-MUTANTS-E3B-E5B, review 00:43). Static: no builds, no daemons.

POS: every mutant anchor in tests/unit/core_gate_mutants.py occurs exactly once (78/78, rc 0).
NEG: E3b's anchor replaced by the bare hook line `h.repaint = [this] { paintIdle(); };`, which occurs three times in
media_player.cpp (enablePlexUi, enableBrowseV2, and the core-gate hook), must fail the audit (rc 2, one
AUDIT line naming E3b). This is the duplicate that left the whole e2e class unrunnable after browse P1 (e6cf24fa).
"""
import contextlib
import importlib.util
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load():
    spec = importlib.util.spec_from_file_location("core_gate_mutants", os.path.join(HERE, "core_gate_mutants.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def audit(mod):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = mod.audit()
    return rc, buf.getvalue()


def main():
    bad = 0
    mod = load()
    rc, out = audit(mod)
    n = sum(len(e[3]) for e in mod.M)
    ok = rc == 0 and ("%d/%d anchors unique" % (n, n)) in out and n == 78
    bad += not ok
    print("%s POS audit rc=%d %s" % ("ok  " if ok else "BAD ", rc, out.strip().splitlines()[-1]))

    mod = load()
    i = next(k for k, e in enumerate(mod.M) if e[0] == "E3b")
    mid, test, desc, edits, survives = mod.M[i]
    mod.M[i] = (mid, test, desc, [(edits[0][0], "    h.repaint = [this] { paintIdle(); };", "    h.repaint = [] {};")],
                survives)
    rc, out = audit(mod)
    lines = [l for l in out.splitlines() if l.startswith("AUDIT ")]
    ok = rc == 2 and len(lines) == 1 and lines[0].startswith("AUDIT E3b (browse)") and "occurs 3 times" in lines[0]
    bad += not ok
    print("%s NEG duplicated E3b anchor rc=%d %s" % ("ok  " if ok else "BAD ", rc, (lines[0] if lines else "no AUDIT line")[:100]))
    print("test_core_gate_mutants_audit: %s" % ("OK" if not bad else "FAIL"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
