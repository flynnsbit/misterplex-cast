#!/usr/bin/env python3
"""Guard: every RTL bench must list the files defining the modules it pulls in.

Motivation: `decode_stub.sv` gained instantiations of modules that live in
`h264_inter_pred.sv`. Git saw no conflict, so benches written earlier merged
cleanly with stale file lists and then failed at Verilator elaboration with
`%Error-MODMISSING`. A merge that is textually clean but semantically broken
must fail loudly here rather than in a worker's build log.

This walks the real module graph instead of hardcoding one known pair, so the
next module that moves files is caught the same way.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RTL_DIR = os.path.join(ROOT, "fpga", "Plex_MiSTer", "rtl")
UNIT_DIR = os.path.join(ROOT, "tests", "unit")

MODULE_RE = re.compile(r"^\s*module\s+([A-Za-z_]\w*)", re.M)
WORD = "[A-Za-z_]\\w*"


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def load_modules():
    """module name -> defining file basename"""
    owner = {}
    for name in sorted(os.listdir(RTL_DIR)):
        if not name.endswith(".sv") and not name.endswith(".v"):
            continue
        body = strip_comments(open(os.path.join(RTL_DIR, name), errors="replace").read())
        for mod in MODULE_RE.findall(body):
            owner[mod] = name
    return owner


def refs_of(fname, owner):
    """Module names referenced (instantiated) by an RTL file, other than its own."""
    body = strip_comments(open(os.path.join(RTL_DIR, fname), errors="replace").read())
    own = set(MODULE_RE.findall(body))
    found = set()
    for mod in owner:
        if mod in own:
            continue
        # instantiation looks like:  <module> [#(...)] <inst> (
        if re.search(r"\b%s\b\s*(?:#\s*\(|%s\s*\()" % (re.escape(mod), WORD), body):
            found.add(mod)
    return found


def listed_files(text, known):
    """Files actually handed to the simulator.

    A bench names RTL through shell variables, so a filename appearing only in
    its own assignment is NOT evidence it is compiled -- the variable may never
    be referenced. Resolve assignments, drop them, then look at what remains.
    """
    var_file, body_lines = {}, []
    assign = re.compile(r"^\s*(\w+)=[\"\']?\S*?([\w.]+\.s?v)[\"\']?\s*$")
    for line in text.splitlines():
        m = assign.match(line)
        if m and m.group(2) in known:
            var_file[m.group(1)] = m.group(2)
        else:
            body_lines.append(line)
    body = "\n".join(body_lines)

    used = {f for f in known if re.search(r"\b%s\b" % re.escape(f), body)}
    for var, f in var_file.items():
        if re.search(r"\$\{?%s\b" % re.escape(var), body):
            used.add(f)
    return used


def main():
    owner = load_modules()
    deps = {f: refs_of(f, owner) for f in set(owner.values())}

    failures = []
    for script in sorted(os.listdir(UNIT_DIR)):
        if not (script.endswith(".sh") or script.endswith(".py")):
            continue
        if script == os.path.basename(__file__):
            continue
        path = os.path.join(UNIT_DIR, script)
        text = open(path, errors="replace").read()
        # Only benches that actually elaborate RTL can hit MODMISSING. Source
        # inspection tests merely name files and must not be flagged.
        if not re.search(r"run_verilator\.sh|bin/verilator", text):
            continue
        text = strip_comments(text)
        listed = listed_files(text, set(deps))
        if not listed:
            continue

        # transitive closure of what the listed files actually need; walk in
        # sorted order so which file is blamed does not follow PYTHONHASHSEED
        need, seen = set(), sorted(listed, reverse=True)
        while seen:
            cur = seen.pop()
            for mod in sorted(deps.get(cur, ())):
                dep_file = owner[mod]
                if dep_file not in listed and dep_file not in need:
                    need.add(dep_file)
                    seen.append(dep_file)
                    failures.append((script, cur, mod, dep_file))

    if failures:
        print("FAILED: bench RTL file lists are stale (would hit %Error-MODMISSING)")
        for script, cur, mod, dep_file in sorted(failures):
            print("  %s: lists %s which instantiates %s, but does not list %s"
                  % (script, cur, mod, dep_file))
        return 1

    print("bench RTL file lists OK (%d benches checked against %d modules)"
          % (len(os.listdir(UNIT_DIR)), len(owner)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
