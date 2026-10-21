#!/usr/bin/env python3
"""T-DAEMON-LOCK-SHARED T5 needle (review 23:26, 23:59, 00:00; H264 needle bars (1)-(4)).

The two prod lock paths are host-global: /tmp/misterplexd.lock (ProcessSingleton) and
/tmp/misterplex_spi.lock (FlockGuard). flock is on the inode, so unlinking either path while a holder
lives lets a later opener lock a NEW inode beside it: two owners. Scope: every tracked text file under
tests/ and scripts/, plus the Makefile.

  L-1  No unlink of either path. Matched anywhere in a continuation-joined line, so after any
       command-prefix word (act, sudo, env, command, timeout N) and inside quoted ssh strings:
       rm, unlink, find ... -delete, os.remove, os.unlink, Path(...).unlink. The path may be a literal
       (with or without a $VAR/ prefix, e.g. boot.sh's "$ROOT/tmp/..." where ROOT is empty on the
       device) or a variable assigned it in the same file (X=, local/export/readonly/declare X=,
       ${Y:-...}, ${Y-...}, chains B=$A), used as $X, ${X} or "$X".
  L-2  A file that removes $X, where X is a lock variable name assigned somewhere in scope but not
       in this file (nor in a file it sources and the needle can follow), is UNRESOLVED. RED.
  L-3  Every tests/unit script that names a host misterplexd binary ($OUT/misterplexd[-head|-base],
       build/misterplexd, or a variable assigned .../build/misterplexd) has exactly one
       'export MPX_LOCK_DIR' line, and it precedes every other line naming that binary (its exec
       lines; compiles through a function parameter included), so no test daemon touches a prod path.
  L-4  The exceptions file (tests/unit/lock_paths_exceptions.txt) has the md5 pinned below. Each
       entry is checked by its file:line text md5, and a GUARDED-ALLOW entry also by the md5 of its
       guard block (both as 'sed -n Lp | md5sum' / 'sed -n a,bp | md5sum' print them). Any entry that fails is RED, never skipped. KNOWN-RED entries are reported (they
       stay visible) but do not fail the needle; a stale one (the line changed or went away) does.
  L-5  (H264 c5) No test passes BOOT_LOCK as a prod lock path, or MPX_LOCK_DIR / MPX_TEST_LOCK_DIR
       equal to /tmp (a private dir under /tmp is fine).
  L-6  (H264 k3, after the 1c320fe7 session_e2e 'SPILOCK: unbound variable') In each converted script
       (CONVERTED below), every $X / ${X} use of a name ending in LOCK has an assignment of X in the same
       file or in a file it sources (followed as for L-2). ShellCheck SC2154 cannot see these: it skips
       UPPERCASE names as possible environment variables.

Teeth (edited copies; each must come out as stated): see TEETH at the bottom.
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXCEPTIONS = "tests/unit/lock_paths_exceptions.txt"
EXCEPTIONS_MD5 = "dec67b7eda1c0af44c468301f8284f6c"  # md5 of EXCEPTIONS; a change needs this pin changed too
LOCK = r"(?:\$\{?[A-Za-z_]\w*\}?)?/tmp/misterplex(?:d|_spi)\.lock\b"
LOCK_RE = re.compile(LOCK)
ASSIGN_SH = re.compile(
    r"(?:^|[\s;&|(])(?:(?:local|export|readonly|declare(?:\s+-\w+)*)\s+)?([A-Za-z_]\w*)=(\"[^\"]*\"|'[^']*'|\S+)")
ASSIGN_PY = re.compile(r"^\s*([A-Za-z_]\w*)\s*(?::\s*\w+\s*)?=\s*(.+)$")
SOURCE_SH = re.compile(r"(?:^|[\s;&|])(?:\.|source)\s+(\"[^\"]+\"|'[^']+'|[^\s;&|]+)")
DAEMON_TOK = re.compile(r"(?:\$\{?OUT\}?/misterplexd(?:-head|-base)?|build/misterplexd)(?![.\w/-])")
DAEMON_VAR = re.compile(r"^\s*(?:export\s+)?([A-Za-z_]\w*)=\"?([^\s\"]*/build/misterplexd)\"?\s*$")
EXPORT_LOCKDIR = re.compile(r"^\s*export\s+MPX_LOCK_DIR(?:=|\s*$)")
L5_BOOT = re.compile(r"\bBOOT_LOCK=['\"]?(?:\$\{?[A-Za-z_]\w*\}?)?/tmp/misterplex(?:d|_spi)\.lock\b")
L5_DIR = re.compile(r"(?:(?<![\w])|(?<=-D))MPX_(?:TEST_)?LOCK_DIR=(?:\\?['\"])*/tmp(?:/\.?)?(?:\\?['\"])*(?=[\s;)]|$)")
CONVERTED = ["tests/unit/" + n + ".sh" for n in (
    "test_core_gate_daemon_e2e", "test_core_gate_session_e2e", "test_audio_recovery_e2e", "test_browse_e2e",
    "test_n2_eof_e2e", "test_companion_hold", "test_companion_http", "test_play_file_delivery", "test_plex_browse",
    "test_lock_paths")]
L6_USE = re.compile(r"\$\{?([A-Za-z_]\w*LOCK)\}?(?!\w)")
L6_LOOP = re.compile(r"(?:^|[\s;&|(])(?:for\s+([A-Za-z_]\w*)\s+in\b|read\s+(?:-\w+\s+)*([A-Za-z_]\w*))")
SESSION_1C320FE7_MD5 = "90f60c2166dd0e2b86653099a59b4222"  # git show 1c320fe7:tests/unit/test_core_gate_session_e2e.sh


def sha(s):
    return hashlib.md5(s.encode()).hexdigest()


def tracked():
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "tests", "scripts", "Makefile"],
                         capture_output=True, text=True, check=True).stdout.split()
    return [p for p in out if (ROOT / p).is_file()]


def read(path, over):
    if path in over:
        return over[path]
    try:
        return (ROOT / path).read_text()
    except (UnicodeDecodeError, OSError):
        return None


def joined(text):
    """[(first_line_no, joined_text)] with backslash continuations joined; comment lines dropped."""
    out, buf, first = [], "", None
    for n, raw in enumerate(text.split("\n"), 1):
        if first is None:
            if raw.lstrip().startswith("#"):
                continue
            first = n
        if raw.endswith("\\"):
            buf += raw[:-1] + " "
            continue
        out.append((first, buf + raw))
        buf, first = "", None
    if first is not None:
        out.append((first, buf))
    return out


def lock_vars(path, text):
    """Names assigned a prod-capable lock path in this file, closed over chains B=$A."""
    names, pairs = set(), []
    py = path.endswith(".py")
    for _, line in joined(text):
        if py:
            m = ASSIGN_PY.match(line)
            found = [(m.group(1), m.group(2))] if m else []
        else:
            found = [(m.group(1), m.group(2)) for m in ASSIGN_SH.finditer(line)]
        for name, val in found:
            pairs.append((name, val))
            if LOCK_RE.search(val):
                names.add(name)
    grew = True
    while grew:
        grew = False
        for name, val in pairs:
            if name not in names and any(re.search(r"\$\{?" + v + r"\b", val) for v in names):
                names.add(name)
                grew = True
            elif name not in names and py and any(re.search(r"\b" + v + r"\b", val) for v in names):
                names.add(name)
                grew = True
    return names


def sources(path, text):
    """Files this shell script sources, resolved against the repo and its own dir; None = unresolved."""
    res = []
    for _, line in joined(text):
        for m in SOURCE_SH.finditer(line):
            arg = m.group(1).strip("\"'")
            arg = re.sub(r"\$\{?ROOT\}?", str(ROOT), arg)
            arg = re.sub(r"\$\(dirname\s+\"?\$0\"?\)|\$\{?(?:HERE|DIR|SCRIPT_DIR)\}?",
                         str((ROOT / path).parent), arg)
            if "$" in arg:
                res.append(None)
                continue
            p = pathlib.Path(arg)
            p = p if p.is_absolute() else (ROOT / path).parent / p
            try:
                res.append(str(p.resolve().relative_to(ROOT)))
            except ValueError:
                res.append(None)
    return res


def unlink_hits(path, text, names):
    """[(line_no, joined_text, how)] for every unlink of a lock literal or a lock variable."""
    var = (r"\$\{?(?:" + "|".join(sorted(names)) + r")\}?\b") if names else None
    pyvar = (r"\b(?:" + "|".join(sorted(names)) + r")\b") if names else None
    target = LOCK + (("|" + var) if var else "")
    hits = []
    for n, line in joined(text):
        if path.endswith(".py"):
            t = LOCK + (("|" + pyvar) if pyvar else "")
            pats = [r"\bos\.(?:remove|unlink)\(\s*[^)]*(?:" + t + ")",
                    r"(?:" + t + r")[^\n]*\)\s*\.unlink\(", r"\bPath\([^)]*(?:" + t + r")[^)]*\)\.unlink\("]
        else:
            pats = [r"\brm\b(?:\s+-\w+)*[^;&|\n]*(?:" + target + ")",
                    r"\bunlink\b[^;&|\n]*(?:" + target + ")",
                    r"\bfind\b[^;&|\n]*(?:" + target + r")[^;&|\n]*-delete\b"]
        for p in pats:
            if re.search(p, line):
                hits.append((n, line.strip(), p.split("\\b")[1] if "\\b" in p else "unlink"))
                break
    return hits


def removed_vars(text):
    return set(re.findall(r"\b(?:rm|unlink)\b[^;&|\n]*?\$\{?([A-Za-z_]\w*)\}?", text))


def load_exceptions(over):
    text = read(EXCEPTIONS, over)
    if text is None:
        return None, [], ["L-4 exceptions file missing: " + EXCEPTIONS]
    probs = []
    if sha(text) != EXCEPTIONS_MD5:
        probs.append(f"L-4 {EXCEPTIONS} md5 {sha(text)[:8]} != pinned {EXCEPTIONS_MD5[:8]}")
    ents = []
    for raw in text.split("\n"):
        if not raw.strip() or raw.startswith("#"):
            continue
        f = raw.split()
        kind, loc, line_md5 = f[0], f[1], f[2]
        guard, guard_md5, ticket = f[3], f[4], f[5]
        file, rest = loc.split(":", 1)
        line_no = int(rest.split("@")[0])
        ents.append(dict(kind=kind, file=file, line=line_no, md5=line_md5, guard=guard,
                         guard_md5=guard_md5, ticket=ticket, raw=raw))
    return text, ents, probs


SELF = os.path.relpath(os.path.realpath(__file__), ROOT)  # l3: exactly this one file is excluded


def check(over, quiet=False):
    # The needle's own file (by realpath, exactly one path) is excluded: its TEETH fixtures spell
    # every forbidden form on purpose; a copy of any fixture line in any OTHER file is RED (tooth z).
    files = sorted((set(tracked()) | set(over)) - {SELF})
    texts = {p: read(p, over) for p in files}
    texts = {p: t for p, t in texts.items() if t is not None}
    names_by = {p: lock_vars(p, t) for p, t in texts.items()}
    global_names = set().union(*names_by.values()) if names_by else set()
    _, ents, probs = load_exceptions(over)
    known, allowed, red = [], [], []
    for e in ents:
        t = texts.get(e["file"])
        lines = t.split("\n") if t is not None else []
        if not (0 < e["line"] <= len(lines)) or sha(lines[e["line"] - 1] + "\n") != e["md5"]:
            probs.append(f"L-4 stale {e['kind']} {e['file']}:{e['line']} (line md5 mismatch or gone)")
            continue
        if e["kind"] == "GUARDED-ALLOW":
            a, b = (int(x) for x in e["guard"].split("-"))
            if sha("\n".join(lines[a - 1:b]) + "\n") != e["guard_md5"]:
                probs.append(f"L-4 GUARDED-ALLOW {e['file']}:{e['line']} guard block :{a}-{b} changed: lapsed")
                continue
    exc = {(e["file"], e["line"]): e for e in ents}
    for p, t in texts.items():
        names = set(names_by[p])
        for s in sources(p, t) if not p.endswith(".py") else []:
            if s is not None and s in names_by:
                names |= names_by[s]
        for n, line, how in unlink_hits(p, t, names):
            e = exc.get((p, n))
            if e and e["kind"] in ("KNOWN-RED", "GUARDED-ALLOW") and not any(f"{e['file']}:{e['line']}" in q for q in probs):
                (known if e["kind"] == "KNOWN-RED" else allowed).append(f"{p}:{n} {e['kind']} {e['ticket']}")
            else:
                red.append(f"L-1 {p}:{n} unlinks a prod lock path ({how}): {line[:110]}")
        unresolved = (removed_vars(t) & global_names) - names
        srcs = sources(p, t) if not p.endswith(".py") else []
        for v in sorted(unresolved):
            red.append(f"L-2 {p}: removes ${v} (a lock variable elsewhere) not resolved here"
                       + (" (unfollowed source)" if None in srcs else ""))
        if p.startswith("tests/unit/") and not p.endswith(".py"):
            red += l3(p, t)
        if p.startswith("tests/"):
            red += l5(p, t, exc, probs, allowed)
        if p in CONVERTED:
            red += l6(p, t, texts)
    red += probs
    if not quiet:
        for k in known:
            print("  KNOWN-RED  " + k)
        for a in allowed:
            print("  ALLOWED    " + a)
        for r in red:
            print("  RED        " + r)
    return red, known, allowed


def l3(p, t):
    """L-3: a tests/unit script naming a host misterplexd exports MPX_LOCK_DIR before using it."""
    lines = t.split("\n")
    vars_ = set()
    for line in lines:
        m = DAEMON_VAR.match(line)
        if m and not line.lstrip().startswith("#"):
            vars_.add(m.group(1))
    names = [i for i, l in enumerate(lines) if not l.lstrip().startswith("#") and DAEMON_TOK.search(l)]
    if not names and not vars_:
        return []
    exports = [i for i, l in enumerate(lines) if EXPORT_LOCKDIR.match(l)]
    if len(exports) != 1:
        return [f"L-3 {p}: names a host misterplexd but has {len(exports)} 'export MPX_LOCK_DIR' lines"]
    use = re.compile("|".join([DAEMON_TOK.pattern] + [r"\$\{?" + v + r"\}?(?![\w/-])" for v in vars_]))
    for i, line in enumerate(lines):
        s = line.lstrip()
        if s.startswith("#") or not use.search(line) or DAEMON_VAR.match(line):
            continue
        if re.match(r"(?:if\s+|\[\[?|test\s|!\s)", s) and "-x" in s:
            continue  # an existence check, not an exec
        if i < exports[0]:
            return [f"L-3 {p}:{i + 1} names the daemon before 'export MPX_LOCK_DIR' (:{exports[0] + 1})"]
    return []


def l5(p, t, exc, probs, allowed):
    """L-5 (H264 c5): no test passes BOOT_LOCK as a prod lock path or a lock dir equal to /tmp.
    A TOOTH-ALLOW entry (the md5-pinned exceptions file) admits a refusal tooth that must pass /tmp."""
    red = []
    for n, line in joined(t):
        if L5_BOOT.search(line) or L5_DIR.search(line):
            e = exc.get((p, n))
            if e and e["kind"] == "TOOTH-ALLOW" and not any(f"{e['file']}:{e['line']}" in q for q in probs):
                allowed.append(f"{p}:{n} TOOTH-ALLOW {e['ticket']}")
            else:
                red.append(f"L-5 {p}:{n} points a test lock at the prod /tmp: {line.strip()[:100]}")
    return red


def assigned_names(t):
    out = set()
    for _, line in joined(t):
        if line.lstrip().startswith("#"):
            continue
        out |= {m.group(1) for m in ASSIGN_SH.finditer(line)}
        out |= {g for m in L6_LOOP.finditer(line) for g in m.groups() if g}
    return out


def l6(p, t, texts):
    """L-6 (H264 k3): every ${...LOCK} use in a converted script is assigned here or in a file it sources."""
    have = assigned_names(t)
    for s in sources(p, t):
        if s is not None and s in texts:
            have |= assigned_names(texts[s])
    red = []
    for n, line in joined(t):
        if line.lstrip().startswith("#"):
            continue
        for v in sorted({m.group(1) for m in L6_USE.finditer(line)} - have):
            red.append(f"L-6 {p}:{n} uses ${v} but no assignment of {v} in this file or a file it sources")
    return red


def main():
    red, known, allowed = check({})
    print(f"real tree: {len(red)} RED, {len(known)} KNOWN-RED, {len(allowed)} ALLOWED")
    bad = 1 if red else 0
    for name, over, want in TEETH():
        r, _, _ = check(over, quiet=True)
        ok = (len(r) > 0) == want
        print(f"  tooth {name}: {'RED' if r else 'GREEN'} ({'ok' if ok else 'WRONG'})" + (f": {r[0][:100]}" if r else ""))
        bad |= 0 if ok else 1
    print("LOCK-PATHS-NEEDLE " + ("PASS" if not bad else "FAIL"))
    return bad


def TEETH():
    base = "tests/unit/test_core_gate_session_e2e.sh"
    boot = "scripts/misterplexd-boot.sh"
    t_base = (ROOT / base).read_text()
    t_boot = (ROOT / boot).read_text()
    new = "tests/unit/zz_needle_tooth.sh"
    newpy = "tests/unit/zz_needle_tooth.py"
    exc = (ROOT / EXCEPTIONS).read_text()
    yield "a literal rm", {new: "rm -f /tmp/misterplexd.lock\n"}, True
    yield "b X= then rm \"$X\"", {new: "X=/tmp/misterplexd.lock\nrm -f \"$X\"\n"}, True
    yield "c ${X} unquoted", {new: "X=/tmp/misterplex_spi.lock\nrm -f ${X}\n"}, True
    yield "d act + ${Y:-} default", {new: "X=\"${Y:-/tmp/misterplexd.lock}\"\nact rm -f \"$X\"\n"}, True
    yield "e $ROOT/tmp prefix", {new: "L=\"${B:-$ROOT/tmp/misterplexd.lock}\"\nrm -f \"$L\"\n"}, True
    yield "f quoted ssh string", {new: "ssh_m 'killall x; rm -f /tmp/misterplex_spi.lock'\n"}, True
    yield "g chain B=$A", {new: "A=/tmp/misterplex_spi.lock\nB=$A\nrm -f \"$B\"\n"}, True
    yield "h unlink command", {new: "unlink /tmp/misterplexd.lock\n"}, True
    yield "i find -delete", {new: "find /tmp/misterplexd.lock -delete\n"}, True
    yield "j os.remove", {newpy: "import os\nos.remove(\"/tmp/misterplexd.lock\")\n"}, True
    yield "k os.unlink var", {newpy: "import os\nLOCK = \"/tmp/misterplex_spi.lock\"\nos.unlink(LOCK)\n"}, True
    yield "l Path.unlink", {newpy: "from pathlib import Path\nPath(\"/tmp/misterplexd.lock\").unlink()\n"}, True
    yield "m unresolved var", {new: ". \"$SOMEWHERE/lib.sh\"\nrm -f \"$LOCK\"\n"}, True
    yield "n boot guard edited", {boot: t_boot.replace("elif [ -z \"$LIVE\" ]; then", "elif true; then")}, True
    yield "o exceptions edited", {EXCEPTIONS: exc + "# x\n"}, True
    yield "p export dropped", {base: re.sub(r"(?m)^export MPX_LOCK_DIR=.*\n", "", t_base)}, True
    yield "q export moved late", {base: re.sub(r"(?m)^(export MPX_LOCK_DIR=.*\n)", "", t_base) + "export MPX_LOCK_DIR=/x\n"}, True
    yield "r rm of an unrelated file", {new: "X=/tmp/other.lock\nrm -f \"$X\" /tmp/misterplexd.log\n"}, False
    yield "s comment only", {new: "# rm -f /tmp/misterplexd.lock\n"}, False
    yield "t L-5 BOOT_LOCK prod", {new: "export BOOT_LOCK=/tmp/misterplexd.lock\n"}, True
    yield "u L-5 MPX_LOCK_DIR=/tmp", {new: "export MPX_LOCK_DIR=/tmp\n"}, True
    yield "v L-5 baked /tmp", {new: "\"$CXX\" -DMPX_TEST_LOCK_DIR=\"\\\"/tmp/\\\"\" x.cpp\n"}, True
    yield "w L-5 private dir", {new: "export MPX_LOCK_DIR=\"$OUT/locks\"\n"}, False
    yield "x L-5 mktemp under /tmp", {new: "MPX_LOCK_DIR=$(mktemp -d \"${TMPDIR:-/tmp}/mpx_locks.XXXXXX\")\n"
                                             "export MPX_LOCK_DIR\n"}, False
    yield "y L-3 build() param, no export", {new: "build() { \"$CXX\" -o \"$2\" main.cpp; }\n"
                                                    "build \"$ROOT\" \"$OUT/misterplexd-head\"\n"
                                                    "\"$OUT/misterplexd-head\" --port 1 &\n"}, True
    own = (ROOT / SELF).read_text().split("\n")
    fixture = [l for l in own if l.lstrip().startswith('yield "') and
               ("Path(\\\"/tmp/misterplexd.lock\\\").unlink()" in l or "export BOOT_LOCK=/tmp/misterplexd.lock" in l)]
    assert len(fixture) == 2, fixture
    yield "z l3: this file's fixture lines copied into another file", {"tests/unit/zz_fixture_copy.py": "\n".join(fixture) + "\n"}, True
    tl = "tests/unit/test_lock_paths.sh"
    t_tl = (ROOT / tl).read_text()
    first = next(e for e in load_exceptions({})[1] if e["kind"] == "TOOTH-ALLOW")
    drift = t_tl.split("\n")
    drift[first["line"] - 1] += " "
    yield "aa l4: a TOOTH-ALLOW line drifts (one trailing space)", {tl: "\n".join(drift)}, True
    yield "ab l4: a line inserted above a TOOTH-ALLOW line", {tl: "#\n" + t_tl}, True
    spl = [l for l in t_base.split("\n") if l.startswith("SPILOCK=")]
    assert len(spl) == 1, spl
    old_session = "\n".join(l for l in t_base.split("\n") if l != spl[0])
    assert sha(old_session) == SESSION_1C320FE7_MD5, "the L-6 NEG fixture is not byte-equal to 1c320fe7's session_e2e"
    yield "ac L-6: the 1c320fe7 session_e2e (SPILOCK used at the flock, never assigned)", {base: old_session}, True
    pb = "tests/unit/test_plex_browse.sh"
    t_pb = (ROOT / pb).read_text()
    t_pb += "" if t_pb.endswith("\n") else "\n"   # the file has no trailing newline; do not glue onto its last line
    yield "ad L-6: another converted script uses an unassigned ${DAEMONLOCK}", {pb: t_pb + "flock \"${DAEMONLOCK}\" true\n"}, True
    yield "ae L-6: the same use, assigned in that file first", \
        {pb: t_pb + "DAEMONLOCK=$MPX_LOCK_DIR/misterplexd.lock\nflock \"${DAEMONLOCK}\" true\n"}, False


if __name__ == "__main__":
    sys.exit(main())
