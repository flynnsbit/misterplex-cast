#!/usr/bin/env python3
"""T-CHMOD-RECIPE check (paper 54a8fab8; review 23:39/23:40/23:55/23:56, H264 n1-n5).

The unit recipe used to 'chmod +x' tracked scripts, which dirtied the tree. This check never writes.
It derives, from the WHOLE Makefile, every recipe command that executes a $(ROOT)/path directly (no
interpreter), and requires:
  2a (decisive) every TRACKED direct-exec target, in ANY recipe (review 23:55 superset), is 100755 in
     the git index AND executable (X_OK) in the worktree; every build/ target of the unit-unlocked
     recipe that exists is X_OK. This check is the FIRST line of that recipe (paper 54a8fab8 2, n2), so
     its prerequisites have built every build/ target; run standalone, absent ones are only counted;
  2b (softer) tracked tests/unit/*.sh|*.py and tests/hw/*.sh that are not 100755: a WARN list only.
Derivation: backslash continuations are joined BEFORE form matching (rule lines and recipe lines);
rule-line continuations are prerequisites, never commands; conditional directives inside a recipe do
not end it; each logical recipe line is split on top-level && || ; | and every command is classified
after stripping @/-/+ prefixes, VAR=x assignments and env/timeout N/nice/setsid/stdbuf wrappers;
$(if c,A,B) anywhere in a line contributes the first command of each branch. Forms are tallied.
  --evidence DIR   also write direct-exec.tsv (rule, line, form, target) + tally.txt, with md5s
  --self-test      the teeth: run the check on scratch copies (git archive + a scratch index)
"""
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

INTERP = {"bash", "sh", "python3", "python", "perl"}
WRAPPERS = {"timeout", "env", "nice", "setsid", "stdbuf"}
KEYWORDS = {"if", "then", "else", "elif", "do", "while", "until", "!", "{", "(", "fi", "done"}
ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
DIRECTIVE = re.compile(r"^(?:ifeq|ifneq|ifdef|ifndef|else|endif)\b")
RULE = re.compile(r"^([^\s:=#][^:=#]*?)\s*::?(?!=)(.*)$")
VARASSIGN = re.compile(r"^(?:(?:export|override|private)\s+)*[A-Za-z_][A-Za-z0-9_.-]*\s*(?:::?=|[?+!]?=)")
GLOBS = re.compile(r"^(?:tests/unit/[^/]+\.(?:sh|py)|tests/hw/[^/]+\.sh)$")


def logical(lines):
    """[(first_no, last_no, text, starts)] joining backslash continuations (any line kind); starts maps
    each physical line to the offset in text where it begins."""
    out, i = [], 0
    while i < len(lines):
        first, text = i, lines[i]
        starts = [(0, i + 1)]
        while text.endswith("\\") and i + 1 < len(lines):
            i += 1
            nxt = lines[i][1:] if lines[i].startswith("\t") else lines[i]
            text = text[:-1] + " "
            starts.append((len(text), i + 1))
            text += nxt
        out.append((first + 1, i + 1, text, starts))
        i += 1
    return out


def line_at(starts, pos):
    n = starts[0][1]
    for p, ln in starts:
        if p <= pos:
            n = ln
    return n


def split_top(s, seps=("&&", "||", ";", "|")):
    """Top-level shell commands of s as (offset of the stripped command, command)."""
    out, cur, beg, depth, q, i = [], "", 0, 0, None, 0
    def push():
        st = cur.strip()
        if st:
            out.append((beg + (len(cur) - len(cur.lstrip())), st))
    while i < len(s):
        ch = s[i]
        if q:
            cur += ch
            q = None if ch == q else q
            i += 1
            continue
        if ch in "'\"":
            q = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if depth == 0:
            hit = next((p for p in seps if s.startswith(p, i)), None)
            if hit:
                push()
                i += len(hit)
                cur, beg = "", i
                continue
        cur += ch
        i += 1
    push()
    return out


def find_if(s):
    """Index of the first make $(if ...) in s, or -1. '$$(if' is a shell $(...), not make, and is skipped."""
    i = s.find("$(if ")
    while i > 0:
        j = i
        while j > 0 and s[j - 1] == "$":
            j -= 1
        if (i - j) % 2 == 0:
            break
        i = s.find("$(if ", i + 1)
    return i


def if_branches(s, start):
    """Branches of the $(if c,A[,B]) at s[start:] as [(offset in s, text)], plus the index just after its ')'.
    Like make, only the first two top-level commas split: commas after them belong to B."""
    depth, parts, cur, beg, i = 0, [], "", start + 5, start + 5
    while i < len(s):
        ch = s[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            if depth == 0:
                break
            depth -= 1
        if ch == "," and depth == 0 and len(parts) < 2:
            parts.append((beg, cur))
            cur, beg = "", i + 1
        else:
            cur += ch
        i += 1
    parts.append((beg, cur))
    return [(b + len(p) - len(p.lstrip()), p.strip()) for b, p in parts[1:]], i + 1


MAX_VARIANTS = 256


def expand_ifs(s):
    """Every expansion of the make $(if ...) calls in s, nested ones included, as [(text, origin offset per
    char, from-an-$(if)-branch flag per char)]. A missing branch expands to ''. Over MAX_VARIANTS raises."""
    todo, out = [(s, list(range(len(s))), [False] * len(s))], []
    while todo:
        t, o, f = todo.pop()
        i = find_if(t)
        if i < 0:
            out.append((t, o, f))
            continue
        branches, end = if_branches(t, i)
        branches += [(i, "")] * (2 - len(branches))
        for bo, b in branches:
            todo.append((t[:i] + b + t[end:], o[:i] + o[bo:bo + len(b)] + o[end:], f[:i] + [True] * len(b) + f[end:]))
        if len(todo) + len(out) > MAX_VARIANTS:
            raise ValueError(f"more than {MAX_VARIANTS} $(if) expansions in one line: {s[:120]}")
    return out


def classify(cmd):
    """-> (cls, forms, word, word offset in cmd, raw word length) for one shell command;
    cls DIRECT | INTERP | OTHER."""
    forms = []
    pre = re.match(r"^[@+-]*", cmd).group(0)
    if pre:
        forms.append("prefix")
    toks = [(m.start(), m.group(0)) for m in re.finditer(r"\S+", cmd)]
    if pre:
        toks[0] = (toks[0][0] + len(pre), toks[0][1][len(pre):])
        if not toks[0][1]:
            toks = toks[1:]
    while toks and toks[0][1] in KEYWORDS:
        toks = toks[1:]
    while toks and ASSIGN.match(toks[0][1]):
        forms.append("assign")
        toks = toks[1:]
    while toks and toks[0][1] in WRAPPERS:
        w, toks = toks[0][1], toks[1:]
        forms.append(w)
        while toks and toks[0][1].startswith("-"):
            toks = toks[1:]
        if w == "timeout" and toks:
            toks = toks[1:]
        while w == "env" and toks and ASSIGN.match(toks[0][1]):
            toks = toks[1:]
    if not toks:
        return "OTHER", forms, "", len(cmd), 0
    pos, raw = toks[0]
    if raw == "cd":
        return "OTHER", forms + ["cd"], "cd", pos, len(raw)
    word = raw.strip("'\"")
    if word.startswith("$(ROOT)/"):
        return "DIRECT", forms, word[len("$(ROOT)/"):], pos, len(raw)
    if word in INTERP:
        return "INTERP", forms, word, pos, len(raw)
    return "OTHER", forms, word, pos, len(raw)


def commands(text):
    """Every command of one logical recipe line, over every expansion of its $(if ...) calls (anywhere in
    the line, nested, after prefixes or wrappers): [(chained, cmd, origin offset per char, from-$(if) per char)]."""
    out = []
    for t, o, f in expand_ifs(text):
        for k, (off, c) in enumerate(split_top(t)):
            out.append((k > 0, c, o[off:off + len(c)], f[off:off + len(c)]))
    return out


def derive(mk_lines, kinds=None):
    """Recipe commands of the whole Makefile: ([(rule, line, last, forms, cls, target)], (unit start, end)).
    line = the physical line where the command starts; last = the last line of its logical line.
    kinds, if given (a dict), maps every physical line to define | recipe | recipe-cont | rule |
    prereq-cont | other, for the naive-grep reconciliation."""
    rows, rule, unit, in_define = [], None, None, False
    kinds = {} if kinds is None else kinds
    for first, last, text, starts in logical(mk_lines):
        raw = mk_lines[first - 1]

        def mark(k, k2=None):
            for n in range(first, last + 1):
                kinds[n] = k if n == first or k2 is None else k2
        if in_define:
            in_define = not raw.startswith("endef")
            mark("define")
            continue
        if raw.startswith("define "):
            in_define = True
            mark("define")
            continue
        if raw.startswith("\t"):
            if rule is None:
                mark("other")
                continue
            mark("recipe", "recipe-cont")
            seen = set()
            for chained, cmd, o, f in commands(text[1:]):
                cls, f2, word, wpos, wlen = classify(cmd)
                origin = o[min(wpos, len(o) - 1)]
                if (origin, cls, word) in seen:
                    continue
                seen.add((origin, cls, word))
                fs = ((["chain"] if chained else []) + (["if"] if any(f[wpos:wpos + wlen]) else []) + f2
                      + (["continued"] if last > first else []))
                rows.append((rule, line_at(starts, origin + 1), last, fs, cls, word))
            if rule == "unit-unlocked" and unit and unit[2]:
                unit = (unit[0], last, True)
            continue
        if not raw.strip() or raw.lstrip().startswith("#") or DIRECTIVE.match(raw):
            mark("other")
            continue
        m = RULE.match(text)
        if m and "=" not in m.group(1):
            mark("rule", "prereq-cont")
            targets = m.group(1).split()
            rule = targets[0]
            if rule == "unit-unlocked" and unit is None and mk_lines[last:last + 1] and mk_lines[last].startswith("\t"):
                unit = (first, last, True)
            elif unit and unit[2]:
                unit = (unit[0], unit[1], False)
        else:
            mark("other", "var-cont" if VARASSIGN.match(raw) else "other-cont")
            rule = None
            if unit and unit[2]:
                unit = (unit[0], unit[1], False)
    return rows, (unit[0], unit[1]) if unit else None


def git(root, *args, env=None):
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          env=env, check=True).stdout


def reconcile(mk, direct, kinds):
    """Every naive '^\\t$(ROOT)/' line and every DIRECT row, each put in exactly one bucket."""
    naive = [n for n, l in enumerate(mk, 1) if l.startswith("\t$(ROOT)/")]
    at = {}
    for r in direct:
        at.setdefault(r[1], []).append(r)
    lines, unexplained = {}, []
    for n in naive:
        k = kinds.get(n, "other")
        first_tok = mk[n - 1].split()[0][len("$(ROOT)/"):]
        lead = any(r[5] == first_tok for r in at.get(n, []))
        b = {("recipe", True): "recipe line, its first command a DIRECT row",
             ("recipe-cont", True): "recipe continuation line, its first token a DIRECT row (form continued)",
             ("recipe-cont", False): "recipe continuation line, its first token an ARGUMENT (not a command)",
             ("prereq-cont", False): "rule-line continuation: a prerequisite (not a command)",
             ("var-cont", False): "continuation of a variable assignment: a value (not a command)",
             ("define", False): "inside define/endef (not a recipe)"}.get((k, lead))
        if b is None:
            unexplained.append(n)
            b = f"UNEXPLAINED kind={k} lead={lead}"
        lines.setdefault(b, []).append(n)
    naive_set = set(naive)
    on, off = 0, {}
    for r in direct:
        if r[1] in naive_set and mk[r[1] - 1].split()[0][len("$(ROOT)/"):] == r[5]:
            on += 1
        else:
            key = "+".join(sorted(set(r[3]))) or "bare"
            off[key] = off.get(key, 0) + 1
    text = [f"naive '^\\t$(ROOT)/' lines {len(naive)} =" + " +".join(f" {len(v)} {k}" for k, v in sorted(lines.items())),
            f"DIRECT rows {len(direct)} = {on} lead the naive lines above + {len(direct) - on} not at a naive line's "
            f"start, by form {dict(sorted(off.items()))}"]
    for k, v in sorted(lines.items()):
        if "DIRECT row" not in k or "continuation" in k:
            text.append(f"  {k}: " + " ".join(f":{n}" for n in v))
    for r in direct:
        if not (r[1] in naive_set and mk[r[1] - 1].split()[0][len("$(ROOT)/"):] == r[5]):
            text.append(f"  not-at-naive-start :{r[1]} ({r[0]}) {'+'.join(r[3]) or 'bare'} {r[5]}")
    return text, unexplained


def check(root, env=None, evidence=None, quiet=False):
    root = pathlib.Path(root)
    mk = (root / "Makefile").read_text().split("\n")
    kinds = {}
    rows, unit = derive(mk, kinds)
    modes = {}
    for l in git(root, "ls-files", "-s", env=env).splitlines():
        meta, path = l.split("\t", 1)
        modes[path] = meta.split()[0]
    direct = [r for r in rows if r[4] == "DIRECT"]
    bad, warn, absent = [], [], 0
    for rule, line, last, forms, cls, t in direct:
        if t in modes:
            x = os.access(root / t, os.X_OK)
            if modes[t] != "100755" or not x:
                bad.append(f"2a Makefile:{line} ({rule}) {t}: index {modes[t]}, worktree X_OK={'yes' if x else 'no'}")
        elif t.startswith("build/") and rule == "unit-unlocked":
            p = root / t
            if p.exists() and not os.access(p, os.X_OK):
                bad.append(f"2a Makefile:{line} (unit-unlocked) {t}: built but not executable")
            absent += 0 if p.exists() else 1
        elif not t.startswith("build/"):
            warn.append(f"2c WARN Makefile:{line} ({rule}) {t}: a direct-exec target neither tracked nor under build/")
    rec, unexplained = reconcile(mk, direct, kinds)
    for n in unexplained:
        bad.append(f"2a-derive Makefile:{n}: a '\\t$(ROOT)/' line the deriver cannot account for: {mk[n - 1].strip()[:100]}")
    for path, mode in sorted(modes.items()):
        if GLOBS.match(path) and mode != "100755":
            warn.append(f"2b WARN {path}: index {mode} (needs no +x unless a recipe runs it directly)")
    tally = {}
    for rule, line, last, forms, cls, t in direct:
        key = "+".join(sorted(set(forms))) or "bare"
        tally[key] = tally.get(key, 0) + 1
    naive = sum(1 for l in mk if l.startswith("\t$(ROOT)/"))
    tracked = sum(1 for r in direct if r[5] in modes)
    summary = (f"script modes: {len(direct)} direct-exec rows in {len({r[0] for r in direct})} recipes "
               f"({tracked} tracked, {sum(1 for r in direct if r[5].startswith('build/'))} build/); "
               f"unit-unlocked :{unit[0]}..:{unit[1]}; forms {dict(sorted(tally.items()))}; naive '^\\t$(ROOT)/' "
               f"lines {naive}; {len(bad)} RED, {len(warn)} WARN, {absent} unit build/ targets not built yet")
    if evidence:
        ev = pathlib.Path(evidence)
        ev.mkdir(parents=True, exist_ok=True)
        tsv = "".join(f"{r[0]}\t{r[1]}\t{r[2]}\t{'+'.join(r[3]) or 'bare'}\t{r[5]}\t{modes.get(r[5], '-')}\n"
                      for r in direct)
        (ev / "direct-exec.tsv").write_text(tsv)
        (ev / "tally.txt").write_text("\n".join([summary] + rec) + "\n")
        print(f"evidence: direct-exec.tsv {hashlib.md5(tsv.encode()).hexdigest()[:8]} ({len(direct)} rows)")
    if not quiet:
        for w in warn:
            print(w)
        for b in bad:
            print("RED " + b)
        print(summary)
        print("\n".join(rec[:2]))
    return bad, warn, rows, unit


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    if "--self-test" in sys.argv:
        return self_test(root)
    ev = sys.argv[sys.argv.index("--evidence") + 1] if "--evidence" in sys.argv else None
    bad, _, _, _ = check(root, evidence=ev)
    if bad:
        print("test_script_modes: FAIL: a direct-exec target is not executable (fix: git update-index "
              "--chmod=+x <path>, and chmod +x it in the worktree)")
        return 1
    print("test_script_modes: OK")
    return 0


def self_test(root):
    """Teeth on scratch copies (never the worktree): git archive of HEAD + a scratch index."""
    ok = bad = 0
    head_mk = (root / "Makefile").read_text()

    def run(name, want_red, mk_edit=None, chmod_index=None, chmod_tree=None, add_files=None, want=None, rows_ok=None):
        nonlocal ok, bad
        with tempfile.TemporaryDirectory() as t:
            tp = pathlib.Path(t)
            subprocess.run(f"git -C '{root}' archive HEAD | tar -x -C '{t}'", shell=True, check=True)
            idx = tp / ".scratch-index"
            env = dict(os.environ, GIT_INDEX_FILE=str(idx), GIT_DIR=str(root / ".git") if (root / ".git").is_dir()
                       else git(root, "rev-parse", "--git-dir").strip(), GIT_WORK_TREE=t)
            subprocess.run(["git", "read-tree", "HEAD"], env=env, check=True, capture_output=True)
            for path, data in (add_files or {}).items():
                (tp / path).parent.mkdir(parents=True, exist_ok=True)
                (tp / path).write_text(data)
                (tp / path).chmod(0o644)
                subprocess.run(["git", "update-index", "--add", path], env=env, check=True, capture_output=True, cwd=t)
            for path in chmod_index or []:
                subprocess.run(["git", "update-index", "--chmod=-x", path], env=env, check=True, capture_output=True, cwd=t)
            for path in chmod_tree or []:
                (tp / path).chmod(0o644)
            mk = mk_edit(head_mk) if mk_edit else head_mk  # always the worktree Makefile, never HEAD's
            (tp / "Makefile").write_text(mk)
            red, warn, rows, _ = check(tp, env=env, quiet=True)
            w = want(mk) if callable(want) else want
            hit = any(w in r for r in red) if w else bool(red)
            good = hit if want_red else not red
            extra = ""
            if rows_ok:
                rgood, extra = rows_ok(rows)
                good = good and rgood
                extra = f" [{extra}]"
            print(f"  tooth {name}: {'RED' if red else 'GREEN'} ({'ok' if good else 'WRONG'})"
                  + (f": {red[0][:110]}" if red else "") + (f" [{len(warn)} WARN]" if warn else "") + extra)
            ok, bad = (ok + 1, bad) if good else (ok, bad + 1)

    first_direct = next(l.split()[0][len("$(ROOT)/"):] for l in head_mk.split("\n")
                        if l.startswith("\t$(ROOT)/tests/"))
    unit_hdr = next(l for l in head_mk.split("\n") if l.startswith("unit-unlocked:"))
    s = "tests/unit/zz_tooth.sh"

    def add_row(row):
        return lambda m: m.replace(unit_hdr + "\n", unit_hdr + "\n" + row + "\n", 1)

    def add_rule(rule):
        return lambda m: m + "\n" + rule + "\n"

    def at(line_text, rule):
        """The RED text for target s attributed to the physical line whose full text is line_text."""
        return lambda m: f"2a Makefile:{m.split(chr(10)).index(line_text) + 1} ({rule}) {s}:"

    def rows_for(rule, want_n, no_if=False, all_if=False):
        def f(rows):
            mine = [r for r in rows if r[0] == rule and r[4] == "DIRECT"]
            good = len(mine) == want_n and (not no_if or all("if" not in r[3] for r in mine)) and (
                not all_if or all("if" in r[3] for r in mine))
            return good, f"{rule}: {len(mine)} DIRECT rows " + " ".join(f":{r[1]}:{'+'.join(r[3]) or 'bare'}:{r[5]}" for r in mine)
        return f

    run("b real tree", False)
    run("a 2b WARN only (glob script 100644)", False, chmod_index=[s], add_files={s: "true\n"})
    run("e1 bare row, target 100644", True, add_row("\t$(ROOT)/" + s), add_files={s: "true\n"}, want=s)
    run("e2 $(if) row", True, add_row("\t$(if $(X),$(ROOT)/" + s + ",true)"), add_files={s: "true\n"}, want=s)
    run("e3 @ prefix", True, add_row("\t@$(ROOT)/" + s), add_files={s: "true\n"}, want=s)
    run("e4 - prefix", True, add_row("\t-$(ROOT)/" + s), add_files={s: "true\n"}, want=s)
    run("e5 VAR=x prefix", True, add_row("\tFOO=1 $(ROOT)/" + s), add_files={s: "true\n"}, want=s)
    run("e6 cd && chain", True, add_row("\tcd $(ROOT) && $(ROOT)/" + s), add_files={s: "true\n"}, want=s)
    run("e7 timeout N", True, add_row("\ttimeout 60 $(ROOT)/" + s), add_files={s: "true\n"}, want=s)
    run("e8 continued line, chained (cited at the continuation line)", True, add_row("\ttrue && \\\n\t$(ROOT)/" + s),
        add_files={s: "true\n"}, want=at("\t$(ROOT)/" + s, "unit-unlocked"))
    run("e9 $(if) on a continued line in a NON-unit recipe (cited at that line)", True,
        add_rule("zz-other:\n\ttrue && \\\n\t\t$(if $(Y),$(ROOT)/" + s + ")"), add_files={s: "true\n"},
        want=at("\t\t$(if $(Y),$(ROOT)/" + s + ")", "zz-other"))
    run("e10 an existing direct-exec target set 100644 in the index", True, chmod_index=[first_direct], want=first_direct)
    run("e11 $(if) after a wrapper: timeout 60 $(if ...)", True, add_row("\ttimeout 60 $(if $(X),$(ROOT)/" + s + ")"),
        add_files={s: "true\n"}, want=s)
    run("e12 nested $(if) (inner else-branch)", True,
        add_row("\t$(if $(A),$(if $(B),true,$(ROOT)/" + s + "),true)"), add_files={s: "true\n"}, want=s)
    run("e13 $(if) after VAR=x", True, add_row("\tFOO=1 $(if $(X),$(ROOT)/" + s + ")"), add_files={s: "true\n"}, want=s)
    run("f worktree X_OK cleared, index still 100755", True, chmod_tree=[first_direct], want=first_direct)
    run("n1 a rule-line continuation is a prerequisite, not a command", False,
        add_rule("zz-pre: a \\\n\t$(ROOT)/" + s + "\n\ttrue"), add_files={s: "true\n"})
    run("n2 $(if)s in the ARGUMENTS of a direct exec: 1 row, not an if row", False,
        add_rule("zz-args:\n\t$(ROOT)/" + first_direct + " $(if $(A),--a,) $(if $(B),--b,--c)"),
        rows_ok=rows_for("zz-args", 1, no_if=True))
    run("n3 nested $(if): every branch that execs is a row, each an if row", False,
        add_rule("zz-nest:\n\t$(if $(A),$(if $(B),$(ROOT)/" + first_direct + ",true),$(ROOT)/" + first_direct + " x)"),
        rows_ok=rows_for("zz-nest", 2, all_if=True))
    run("n4 a variable-assignment continuation is a value, not a command", False,
        add_rule("ZZ_DEPS := \\\n\t$(ROOT)/" + s), add_files={s: "true\n"})
    run("n5 an unexplained '\\t$(ROOT)/' line fails closed (2a-derive)", True,
        add_rule("$(eval ZZ := 1) \\\n\t$(ROOT)/" + first_direct), want="2a-derive")
    print(f"SCRIPT-MODES-TEETH: {ok} ok / {bad} bad")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
