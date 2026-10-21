#!/usr/bin/env python3
"""T-UNIT-DANGLING-3 audit (rev 2 §4 and its rev 2.1 erratum): every build/test_* binary that the Makefile can build is
RUN by a recipe that 'make unit-unlocked' reaches, or is listed in tests/unit/unit_build_only.txt with its reason.

- Ground truth (review r2): 'make -p -n -B --trace unit-unlocked', run in 'unshare --user --map-root-user --mount'
  with an empty tmpfs over build/, so the real build/ is never read or written. The entry list is exactly
  ['unit-unlocked'] (d3, r4).
- Fail-closed (H264 n1): a dry-run command naming a binary that the grammar cannot classify is RED. So is any recipe
  line that unit-unlocked reaches and that make would execute even under -n: '+' or $(MAKE) (review 04:07 n1).
- r1, by rule name (review 04:07 n2): every rule that runs a mutant, and every rule named *mutant-check, must run its
  base before any other binary, unless the rule is named in OUT_OF_R1.
- Imports: the standard library only. The Makefile recipe runs this with a fresh PYTHONPYCACHEPREFIX (standing bar 2).
Usage: test_unit_targets_run.py [--root DIR] [--no-teeth] [--lint]
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ENTRY = ("unit-unlocked",)
BUILD_ONLY = "tests/unit/unit_build_only.txt"
BUILD_ONLY_MD5 = "fa7d21cdbaddb3b1e7fae670d7f5d29f"
# r3 and the rev 2.1 erratum (b1): scripts that the dry run executes, pinned by md5.
#   names       the script runs the listed binaries itself (they are named in it);
#   execs-argv  the script execs its argv, so a listed binary passed to it as an argument is RUN.
SCRIPT_PINS = {
    "tests/unit/test_l2_transcode_dar_demux.sh": (
        "5ff5e1aa5e8f380cb8b6baf01c53032d", "names", ("test_l2_transcode_dar_demux", "test_l2_transcode_dar_demux_mut_metadata_wins")),
    "tests/unit/ns_tmp_pid.sh": ("479e0b590fcbf2c76f08cd9241d7258c", "execs-argv", ("test_core_gate_mut_c1_abba",)),
}
CONFIG_GATED = {"L2_DEMUX_OK": ("test_l2_transcode_dar_demux", "test_l2_transcode_dar_demux_mut_metadata_wins")}
OUT_OF_R1 = {
    "test_core_gate.c1-check": "its base test_core_gate runs in the unit-unlocked recipe; the check runs the ABBA mutant "
                               "alone in a private ns (rev 2 §3.4, d2)",
}
NONEXEC = ("cp", "mv", "rm", "ln", "chmod", "test", "[", "ls", "md5sum", "cmp", "strip")
RUNNERS = ("bash", "sh", "python3")
# The 04:04 grep bar: each pattern that decides RED, and the strings it targets (linted by --lint on real inputs).
PATTERNS = {
    "LABEL": (r"^Makefile:(\d+): update target '([^']+)' due to: ", "make --trace recipe labels, one per recipe"),
    "BUILT": (r"(^|\s)-o\s*$", "the text before a binary path in a compiler '-o <binary>' argument"),
    "RECURSIVE": (r"^\t[@-]*\+|\$[({]MAKE[)}]", "recipe lines make runs under -n: database targets 'unit', 'arm-kmod'"),
    "MUTANT": (r"_(?:mut|MUT)_", "binaries built with -DMUT_*"),
}
RECURSIVE_TARGETS = {"unit", "arm-kmod"}
SEP = re.compile(r"(?:^|\s)(?:if|then|do|else|elif|while|until|!)\s|;|&&|\|\||\||\(|\{|`")
LOOP = re.compile(r"for (\w+) in ([^;]*);")


def md5(b):
    return hashlib.md5(b).hexdigest()


def capture(root, makefile=None, tmpfs=True):
    """(rc, stdout, stderr) of the dry run in a private mount ns; makefile = a mutated copy bound over Makefile."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "GNUMAKEFLAGS", "MAKEOVERRIDES")}
    env["LC_ALL"] = "C"
    sh = ("set -e; " + ('mount -t tmpfs dangling3 "$1/build"; ' if tmpfs else "") +
          '[ -z "$2" ] || mount --bind "$2" "$1/Makefile"; '
          'echo "PROBE build_entries=$(ls -A "$1/build" | wc -l) fstype=$(stat -f -c %T "$1/build")"; '
          'cd "$1"; exec make -p -n -B --trace ' + " ".join(ENTRY))
    r = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "sh", "-c", sh, "_", str(root), makefile or ""],
                       capture_output=True, text=True, env=env, timeout=600)
    return r.returncode, r.stdout, r.stderr


def strip_prefixes(words):
    """Drop exec-position prefixes: VAR=value, env [-u NAME | VAR=value]..., timeout [-k N] [-s SIG] N, nice [-n N],
    qemu-arm-static [-L SYSROOT], exec. Returns the remaining words, or None if a prefix is malformed."""
    i = 0
    while i < len(words):
        w = words[i]
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=\S*", w) or w == "exec":
            i += 1
        elif w == "env":
            i += 1
            while i < len(words) and (words[i] == "-u" or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=\S*", words[i])):
                i += 2 if words[i] == "-u" else 1
        elif w == "timeout":
            i += 1
            while i < len(words) and words[i] in ("-k", "-s"):
                i += 2
            if i >= len(words) or not re.fullmatch(r"\d+(\.\d+)?[smhd]?", words[i]):
                return None
            i += 1
        elif w == "nice":
            i += 1
            if i < len(words) and words[i] == "-n":
                i += 2
        elif w == "qemu-arm-static":
            # The clock and https recipes run the armhf binary under qemu. -L names the sysroot.
            i += 1
            if i < len(words) and words[i] == "-L":
                if i + 1 >= len(words):
                    return None
                i += 2
        else:
            break
    return words[i:]


def classify(before):
    """Classify a binary path by the command text before it: (kind, script) with kind BUILT, RUN, MENTION,
    SCRIPT-ARG or UNCLASSIFIED."""
    if re.search(PATTERNS["BUILT"][0], before):
        return "BUILT", None
    seps = list(SEP.finditer(before))
    seg = before[seps[-1].end():] if seps else before
    rest = strip_prefixes(seg.split())
    if rest is None:
        return "UNCLASSIFIED", None
    if not rest:
        return "RUN", None
    if rest[0] in NONEXEC:
        return "MENTION", None
    if rest[0] in RUNNERS and len(rest) >= 2 and re.search(r"\.(sh|py)$", rest[1]):
        return "SCRIPT-ARG", rest[1]
    if re.search(r"\.(sh|py)$", rest[0]):
        return "SCRIPT-ARG", rest[0]
    return "UNCLASSIFIED", None


def parse(root, out):
    """Split the capture into (probe, recipes, db blocks by target, make version, variables)."""
    lines = out.split("\n")
    probe = lines[0] if lines and lines[0].startswith("PROBE ") else ""
    body = "\n".join(lines[1:] if probe else lines)
    cut = body.find("\n# GNU Make ")
    dry, db = (body[:cut], body[cut:]) if cut >= 0 else (body, "")
    version = (re.search(r"^# (GNU Make \S+)", db, re.M) or [None, "?"])[1]
    recipes, cur = [], None
    for line in re.sub(r"\\\n", " ", dry).split("\n"):
        m = re.match(PATTERNS["LABEL"][0], line)
        if m:
            cur = [int(m[1]), m[2], []]
            recipes.append(cur)
        elif line.strip():
            if cur is None:
                recipes.append([0, "<unlabelled>", [line]])
            else:
                cur[2].append(line)
    blocks = {}
    for blk in db.split("\n\n"):
        for i, l in enumerate(blk.split("\n")):
            m = re.match(r"^([^#\s][^:]*?):(?!=)", l)
            if m and not l.startswith("\t"):
                if not (i > 0 and blk.split("\n")[i - 1].startswith("# Not a target")):
                    blocks.setdefault(m[1], blk)
                break
    variables = dict(re.findall(r"^([A-Za-z_][A-Za-z0-9_]*) :?= ?(.*)$", db, re.M))
    return probe, recipes, blocks, version, variables, dry


def analyze(root, rc, out, err, overrides=None, pins=None, build_only_md5=None):
    """Return (reds, info). overrides = {relpath: bytes} replaces file contents for the teeth."""
    root = str(root)
    overrides = overrides or {}
    pins = SCRIPT_PINS if pins is None else pins
    build_only_md5 = BUILD_ONLY_MD5 if build_only_md5 is None else build_only_md5
    reds = []

    def content(rel):
        if rel in overrides:
            return overrides[rel]
        p = pathlib.Path(root, rel)
        return p.read_bytes() if p.is_file() else None

    if rc != 0:
        reds.append(f"the dry run failed: rc {rc}: {(err.strip().splitlines() or ['?'])[0][:160]}")
    for e in err.strip().splitlines():
        reds.append(f"make stderr (target list: none): {e[:160]}")
    probe, recipes, blocks, version, variables, dry = parse(root, out)
    if probe != "PROBE build_entries=0 fstype=tmpfs":
        reds.append(f"build/ is not an empty tmpfs in the dry run's ns (r2): {probe or 'no probe line'}")
    B = re.escape(root + "/build/")
    bins = {t[len(root) + 7:] for t, blk in blocks.items()
            if re.fullmatch(B + r"test_[^\s:.%]+", t) and "#  recipe to execute" in blk}
    run, built, via, kinds = {}, set(), {}, {}
    script_ok = {}
    for rel, (want, kind, names) in pins.items():
        c = content(rel)
        script_ok[rel] = c is not None and md5(c) == want
        if not script_ok[rel]:
            reds.append(f"pinned script {rel} md5 {md5(c) if c is not None else 'ABSENT'} != {want} (r3): its binaries "
                        f"{', '.join(names)} do not count as RUN")
    first_run = {}
    for line_no, target, cmds in recipes:
        rule = target[len(root) + 7:] if target.startswith(root + "/build/") else target
        if target == "<unlabelled>":
            reds.append(f"an unlabelled dry-run line (fail-closed): {cmds[0][:140]}")
            continue
        blk = blocks.get(target, "")
        for rl in re.findall(r"^\t.*$", blk, re.M):
            if re.search(PATTERNS["RECURSIVE"][0], rl):
                reds.append(f"Makefile :{line_no} {rule}: a recipe line that make runs under -n ('+'/$(MAKE)): {rl.strip()[:120]}")
        order = []
        for cmd in cmds:
            loops = {m[1]: m[2].split() for m in LOOP.finditer(cmd)}
            spans = [(m.start(), m[1]) for m in re.finditer(B + r"(test_[A-Za-z0-9_$%{}-]+)(?![.\w])", cmd)]
            # cd $(ROOT) && ./build/test_* runs that binary with cwd at the repo root.
            # classify sees the text before ./build/, so && yields kind RUN.
            if re.search(r"\bcd\s+" + re.escape(root) + r"\b", cmd):
                spans.extend((m.start(), m[1]) for m in re.finditer(
                    r"\./build/(test_[A-Za-z0-9_$%{}-]+)(?![.\w])", cmd))
            for start, word in spans:
                v = re.search(r"\$\{?(\w+)\}?", word)
                if v and v[1] not in loops:
                    reds.append(f"Makefile :{line_no} {rule}: a binary path with an unexpanded variable: {word}")
                    continue
                names = [word[:v.start()] + x + word[v.end():] for x in loops[v[1]]] if v else [word]
                kind, script = classify(cmd[:start])
                if kind == "SCRIPT-ARG":
                    rel = script[len(root) + 1:] if script.startswith(root + "/") else script
                    pin = pins.get(rel)
                    if not pin or pin[1] != "execs-argv":
                        reds.append(f"Makefile :{line_no} {rule}: {', '.join(names)} is an argument of the unpinned "
                                    f"script {rel} (r3)")
                        continue
                    for n in names:
                        if n not in pin[2]:
                            reds.append(f"Makefile :{line_no} {rule}: {n} is passed to {rel}, which is not pinned for it")
                        elif script_ok[rel]:
                            run.setdefault(n, f"via {rel}")
                            order.append(n)
                    continue
                if kind == "UNCLASSIFIED":
                    reds.append(f"Makefile :{line_no} {rule}: UNCLASSIFIED (H264 n1): {cmd.strip()[:150]}")
                    continue
                for n in names:
                    kinds.setdefault(n, set()).add(kind)
                    if kind == "RUN":
                        run.setdefault(n, f"Makefile :{line_no}")
                        order.append(n)
                    elif kind == "BUILT":
                        built.add(n)
        first_run[rule] = order
    executed = set(re.findall(r"(" + re.escape(root) + r"/(?:tests|scripts)/[\w/.-]+\.(?:sh|py))", dry))
    for rel, (want, kind, names) in pins.items():
        if kind == "names" and root + "/" + rel in executed and script_ok[rel]:
            for n in names:
                run.setdefault(n, f"via {rel}")
    gated_off = set()
    for var, names in CONFIG_GATED.items():
        if not variables.get(var, "").strip():
            gated_off |= set(names)
    bo = content(BUILD_ONLY)
    build_only = {}
    if bo is None or md5(bo) != build_only_md5:
        reds.append(f"{BUILD_ONLY} md5 {md5(bo) if bo is not None else 'ABSENT'} != {build_only_md5} (r4)")
    else:
        for l in bo.decode().splitlines():
            name, _, reason = l.partition("\t")
            if not reason.strip():
                reds.append(f"{BUILD_ONLY}: {name} has no reason")
            build_only[name] = reason
    for name in build_only:
        if name not in bins:
            reds.append(f"{BUILD_ONLY}: {name} is not a build/test_* target")
        elif name in run:
            reds.append(f"{BUILD_ONLY}: {name} is RUN ({run[name]}); drop it from the list")
    allb = bins | built
    never = sorted(b for b in allb if b not in run and b not in build_only and b not in gated_off)
    for b in never:
        reds.append(f"NEVER RUN: {b} (built or buildable, not run by any recipe unit-unlocked reaches)")
    for rule, order in first_run.items():
        mutants = [b for b in order if re.search(PATTERNS["MUTANT"][0], b)]
        if not (rule.endswith("mutant-check") or mutants) or rule in OUT_OF_R1:
            continue
        base = rule.split(".")[0] if rule.endswith("mutant-check") else re.split(PATTERNS["MUTANT"][0], mutants[0])[0]
        if not order or order[0] != base:
            reds.append(f"r1: {rule} runs {order[0] if order else 'nothing'} first, not its base {base}")
        if base not in allb:
            reds.append(f"r1: {rule}'s base {base} is not a build/test_* binary")
    info = {"version": version, "probe": probe, "bins": len(allb), "run": sum(b in run for b in allb),
            "build_only": sorted(build_only), "config_off": sorted(gated_off & allb),
            "recipes": len(recipes), "dry_lines": dry.count("\n") + 1, "dry_md5": md5(dry.encode())[:8],
            "l2": variables.get("L2_DEMUX_OK", "").strip(), "blocks": blocks, "dry": dry, "kinds": kinds}
    return reds, info


def lint(root, out, err):
    """The 04:04 grep bar: each RED-deciding pattern's hits on the real inputs, against its target list."""
    bad = 0
    probe, recipes, blocks, version, variables, dry = parse(str(root), out)
    hits = sorted(t for t, blk in blocks.items()
                  if any(re.search(PATTERNS["RECURSIVE"][0], rl) for rl in re.findall(r"^\t.*$", blk, re.M)))
    ok = set(hits) == RECURSIVE_TARGETS
    bad += not ok
    print(f"  lint RECURSIVE {PATTERNS['RECURSIVE'][0]!r}: database hits {hits}; target list "
          f"{sorted(RECURSIVE_TARGETS)}: {'ok' if ok else 'FAIL'}")
    B = re.escape(str(root) + "/build/")
    compiled = {}
    for line_no, target, cmds in recipes:
        for cmd in cmds:
            m = re.search(r"-o\s+" + B + r"(test_[A-Za-z0-9_-]+)(?![.\w])", cmd)
            if m:
                compiled[m[1]] = cmd
    named = {b for b in compiled if re.search(PATTERNS["MUTANT"][0], b)}
    defined = {b for b, c in compiled.items() if re.search(r"\s-DMUT_\w+", c)}
    ok = named == defined
    bad += not ok
    print(f"  lint MUTANT {PATTERNS['MUTANT'][0]!r}: {len(named)} binaries by name, {len(defined)} compiled with -DMUT_*; "
          f"name-only {sorted(named - defined)}, define-only {sorted(defined - named)}: {'ok' if ok else 'FAIL'}")
    srcs = {"tests/unit/test_core_gate.cpp": (r'"(FAIL [^"]*)"', ["FAIL %s:%d: %s\\n", "FAIL C1: no progress in %d s: "
                                                                   "a lock-order deadlock (polls=%llu)\\n"]),
            "tests/unit/test_browse_paint_rg2.cpp": (r'"(RG-2 FAIL: [^"]*)"', ["RG-2 FAIL: "]),
            "tests/unit/test_ddr_page_mapper.cpp": (
                r'"([^"]*(?:blocked in futex\(\) on ddrPageMu_ until the|close\(\) returned while the)[^"]*)"',
                ["FAIL %s: close() returned while the %s was held in flight (it did not wait for the page lock)\\n",
                 "P-g close() during a HELD first map: blocked in futex() on ddrPageMu_ until the window was ",
                 "P-h close() during a HELD access on the mapped page: blocked in futex() on ddrPageMu_ until the "])}
    for rel, (rx, want) in srcs.items():
        got = re.findall(rx, pathlib.Path(root, rel).read_text())
        ok = sorted(got) == sorted(want)
        bad += not ok
        print(f"  lint recipe grep prefix in {rel}: literals {got}; target list {want}: {'ok' if ok else 'FAIL'}")
    return bad


def teeth(root, base_out):
    """Each tooth must be RED for its own named reason (or GREEN where marked)."""
    mk = pathlib.Path(root, "Makefile").read_text()
    ok_n = bad_n = 0

    def tooth(name, want, why, mutate=None, overrides=None, pins=None, bo_md5=None, tmpfs=True):
        nonlocal ok_n, bad_n
        if mutate or not tmpfs:
            text = mk
            for a, b in (mutate or []):
                if a not in text:
                    print(f"  tooth {name}: BROKEN (WRONG): the mutation anchor is not in the Makefile: {a[:60]!r}")
                    bad_n += 1
                    return
                text = text.replace(a, b, 1)
            with tempfile.NamedTemporaryFile("w", suffix=".mk", delete=False) as f:
                f.write(text)
            try:
                rc, out, err = capture(root, f.name if mutate else None, tmpfs)
            finally:
                os.unlink(f.name)
        else:
            rc, out, err = 0, base_out, ""
        reds, _ = analyze(root, rc, out, err, overrides, pins, bo_md5)
        hit = [r for r in reds if why in r]
        good = (want == "GREEN" and not reds) or (want == "RED" and bool(hit))
        shown = (hit or reds or ["(no RED)"])[0]
        print(f"  tooth {name}: {'RED' if reds else 'GREEN'} ({'ok' if good else 'WRONG'}) {shown[:140]}")
        ok_n, bad_n = (ok_n + 1, bad_n) if good else (ok_n, bad_n + 1)

    leg = "\t@$(ROOT)/build/test_r1_plx_page > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }\n"
    zz = ("v3-stable-320-policy:\n", "$(ROOT)/build/test_zz: ; @true\nv3-stable-320-policy:\n")
    tooth("A1 the test_r1_plx_page base leg removed", "RED", "NEVER RUN: test_r1_plx_page", [(leg, "")])
    tooth("A2 the c1-check's unit-unlocked line removed", "RED", "NEVER RUN: test_core_gate_mut_c1_abba",
          [("unit-unlocked unit-host-unlocked: $(ROOT)/build/test_core_gate.c1-check\n", "")])
    tooth("A3 a new build/test_zz rule that nothing runs", "RED", "NEVER RUN: test_zz", [zz])
    bo = pathlib.Path(root, BUILD_ONLY).read_bytes() + b"test_zz\tA4 tooth: an allowlisted target\n"
    tooth("A4 the same test_zz, allowlisted (pin = the list's md5)", "GREEN", "", [zz],
          overrides={BUILD_ONLY: bo}, bo_md5=md5(bo))
    tooth("A5 one byte of drift in unit_build_only.txt", "RED", f"{BUILD_ONLY} md5",
          overrides={BUILD_ONLY: pathlib.Path(root, BUILD_ONLY).read_bytes() + b" "})
    tooth("A6 a loop-exec'd pattern instance counts as RUN (real tree)", "GREEN", "")
    tooth("A6n the ddr_page_mapper mutant loop removed", "RED", "NEVER RUN: test_ddr_page_mapper_MUT_",
          [("\t\tif $(ROOT)/build/test_ddr_page_mapper_$$m > $@.$$m.log 2>&1; then echo \"MUTANT SURVIVED: $$m\"; "
            "exit 1; fi; \\\n", "\t\ttrue; \\\n")])
    tooth("A7 a new phony entry that runs an un-run binary (entry list stays ['unit-unlocked'])", "RED",
          "NEVER RUN: test_zz", [("v3-stable-320-policy:\n", "$(ROOT)/build/test_zz: ; @true\n.PHONY: zz-entry\n"
                                  "zz-entry: $(ROOT)/build/test_zz\n\t$(ROOT)/build/test_zz\nv3-stable-320-policy:\n")])
    tooth("A8a the c1_abba mutant passed to an unpinned script", "RED", "unpinned script",
          [("timeout 180 $(ROOT)/tests/unit/ns_tmp_pid.sh $(ROOT)/build/test_core_gate_mut_c1_abba",
            "timeout 180 $(ROOT)/tests/unit/zz_wrapper.sh $(ROOT)/build/test_core_gate_mut_c1_abba")])
    l2 = "tests/unit/test_l2_transcode_dar_demux.sh"
    tooth("A8b the pinned L2 demux script with another md5", "RED", f"pinned script {l2}",
          overrides={l2: pathlib.Path(root, l2).read_bytes() + b"\n"})
    tooth("A9 a line the grammar cannot classify", "RED", "UNCLASSIFIED",
          [("\t@cp $@.log $@\nunit-unlocked unit-host-unlocked: $(DANGLING3_OK)\n",
            "\t@cp $@.log $@\n\t@xargs $(ROOT)/build/test_av_sync < /dev/null\nunit-unlocked unit-host-unlocked: $(DANGLING3_OK)\n")])
    tooth("A10 no tmpfs over build/ (r2)", "RED", "is not an empty tmpfs", tmpfs=False)
    tooth("A11 the ddr_page_mapper base leg removed (r1)", "RED", "r1: test_ddr_page_mapper.mutant-check",
          [("\t@MPX_DDR_FAKEMEM=\"$(ROOT)/build/test_ddr_page_mapper.mutant-check.fakemem\" \\\n"
            "\t\t$(ROOT)/build/test_ddr_page_mapper > $@.base.log 2>&1 || { cat $@.base.log; exit 1; }\n", "")])
    ns = "tests/unit/ns_tmp_pid.sh"
    tooth("A12 ns_tmp_pid.sh with another md5 (rev 2.1 b1)", "RED", f"pinned script {ns}",
          overrides={ns: pathlib.Path(root, ns).read_bytes() + b"\n"})
    tooth("N1 a '+' recipe line that unit-unlocked reaches (review 04:07 n1)", "RED", "make runs under -n",
          [("\t@cp $@.log $@\nunit-unlocked unit-host-unlocked: $(DANGLING3_OK)\n",
            "\t@cp $@.log $@\n\t+@true\nunit-unlocked unit-host-unlocked: $(DANGLING3_OK)\n")])
    tooth("N2 a check by another suffix that runs a mutant first (r1 by name, review 04:07 n2)", "RED",
          "r1: test_r1_plxh.kill-check",
          [("v3-stable-320-policy:\n", "$(ROOT)/build/test_r1_plxh.kill-check: $(ROOT)/build/test_r1_plxh_MUT_PLXH_MAGIC_FIRST\n"
            "\t$(ROOT)/build/test_r1_plxh_MUT_PLXH_MAGIC_FIRST\nunit-unlocked: $(ROOT)/build/test_r1_plxh.kill-check\n"
            "v3-stable-320-policy:\n")])
    return ok_n, bad_n


def main():
    args = sys.argv[1:]
    root = pathlib.Path(args[args.index("--root") + 1] if "--root" in args else pathlib.Path(__file__).resolve().parents[2])
    root = pathlib.Path(os.path.abspath(root))
    rc, out, err = capture(root)
    reds, info = analyze(root, rc, out, err)
    print(f"test_unit_targets_run: {info['version']}; entry {list(ENTRY)}; {info['probe']}; L2_DEMUX_OK={info['l2']!r}")
    print(f"test_unit_targets_run: dry run {info['dry_lines']} lines md5 {info['dry_md5']}, {info['recipes']} recipes; "
          f"{info['bins']} binaries: {info['run']} run, build-only {info['build_only']}, config-off {info['config_off']}")
    for r in reds:
        print(f"  RED {r}")
    lint_bad = lint(root, out, err) if "--lint" in args else 0
    ok_n = bad_n = 0
    if "--no-teeth" not in args:
        ok_n, bad_n = teeth(root, out)
        print(f"TEETH: {ok_n} ok / {bad_n} bad")
    if reds or bad_n or lint_bad:
        print(f"test_unit_targets_run: RED ({len(reds)} RED, teeth {ok_n} ok / {bad_n} bad, lint {lint_bad} FAIL)")
        return 1
    print(f"test_unit_targets_run: OK ({info['bins']} binaries: {info['run']} run, {len(info['build_only'])} build-only, "
          f"{len(info['config_off'])} config-off; teeth {ok_n} ok / 0 bad)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
