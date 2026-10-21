#!/usr/bin/env python3
"""Static regression: last-frame hold on death and decode-stop (review-268 BLOCK).

Default-on lastFrameHoldEnabled_ skips fb_.clear()+paintIdle()+startIdle() in
MediaPlayer::stop() and skips idle restore on natural EOF / threadMain session
end, so HDMI keeps the last decoded AU. startIdle() itself does not take a hold
hook (callers paint first). startOsdPoll() still runs after stop().

Must fail on 9ecab85bbfaca62769a4aa9cef2a7cd19c58514a (default-off flag, wrong
startIdle early-return, unguarded stop/EOF paint) and pass after the r1 fix.
Comment-only / IdleMode::LastFrame grep hits are not enough.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MEDIA = Path(os.environ.get("MEDIA_PLAYER_CPP", ROOT / "arm/misterplexd/media_player.cpp"))
HPP = Path(os.environ.get("MEDIA_PLAYER_HPP", ROOT / "arm/misterplexd/media_player.hpp"))
BAD_SHA = "9ecab85bbfaca62769a4aa9cef2a7cd19c58514a"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def strip_comments(text: str) -> str:
    out: list[str] = []
    i = 0
    in_str = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if in_str:
            out.append(c)
            if c == "\\" and n:
                out.append(n)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
        elif c == '"':
            in_str = True
            out.append(c)
            i += 1
        elif c == "/" and n == "/":
            while i < len(text) and text[i] != "\n":
                i += 1
            out.append("\n")
        elif c == "/" and n == "*":
            i += 2
            while i + 1 < len(text) and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def method_body(cpp: str, sig: str) -> str:
    """Body of the first `sig {` occurrence (brace-matched)."""
    i = cpp.find(sig)
    if i < 0:
        return ""
    brace = cpp.find("{", i)
    if brace < 0:
        return ""
    depth = 1
    j = brace + 1
    while j < len(cpp) and depth:
        if cpp[j] == "{":
            depth += 1
        elif cpp[j] == "}":
            depth -= 1
        j += 1
    return cpp[brace + 1 : j - 1] if depth == 0 else ""


def first_brace_block(src: str, cond: str) -> str:
    i = src.find(cond)
    if i < 0:
        return ""
    brace = src.find("{", i)
    if brace < 0:
        return ""
    depth = 1
    j = brace + 1
    while j < len(src) and depth:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
        j += 1
    return src[brace + 1 : j - 1] if depth == 0 else ""


def after_block(src: str, cond: str) -> str:
    i = src.find(cond)
    if i < 0:
        return ""
    brace = src.find("{", i)
    if brace < 0:
        return ""
    depth = 1
    j = brace + 1
    while j < len(src) and depth:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
        j += 1
    return src[j:] if depth == 0 else ""


def snippet_after(src: str, needle: str, n: int = 400) -> str:
    i = src.find(needle)
    if i < 0:
        return ""
    return src[i : i + n]


def analyze(cpp: str, hpp: str) -> list[str]:
    errs: list[str] = []
    code = strip_comments(cpp)
    hpp_code = strip_comments(hpp)

    if "lastFrameHoldEnabled_{true}" not in hpp_code:
        errs.append("hpp must default lastFrameHoldEnabled_{true} (hold on without main.cpp)")
    if "lastFrameHoldEnabled_ = false" in hpp:
        errs.append("hpp must not default lastFrameHoldEnabled_ = false")
    if "lastFrameHoldEnabled_.store(on)" not in hpp_code:
        errs.append("setHoldLastFrame must store() into the atomic")
    if "lastFrameHoldEnabled_.load()" not in hpp_code:
        errs.append("holdLastFrame() must load() the atomic")

    start = method_body(code, "void MediaPlayer::startIdle()")
    if not start:
        errs.append("could not parse void MediaPlayer::startIdle()")
    else:
        if "lastFrameHoldEnabled_" in start:
            errs.append("startIdle() must not branch on lastFrameHoldEnabled_ "
                        "(callers paint first; skip the paint, not the thread)")
        if "setIdleMode(IdleMode::LastFrame)" in start:
            errs.append("startIdle() must not setIdleMode(LastFrame) as a hold hook")
        if "lock_guard" not in start:
            errs.append("startIdle() must take idleMu_ (base idle-thread path)")

    stop = method_body(code, "MediaPlayer::StoppedPosition MediaPlayer::stop()")
    if not stop:
        errs.append("could not parse MediaPlayer::StoppedPosition MediaPlayer::stop()")
        return errs

    # IDLE-BLACK 2026-10-18: the guards read holdLastFrameAtEnd() (the hold, except on an armed R1 idle FB core).
    if "last_frame_hold::holdAtEnd(lastFrameHoldEnabled_.load(), r1Armed_.load())" not in hpp_code:
        errs.append("holdLastFrameAtEnd() must be last_frame_hold::holdAtEnd(lastFrameHoldEnabled_, r1Armed_)")
    guard_cond = "if (!holdLastFrameAtEnd())"
    if guard_cond not in stop:
        errs.append("stop() must guard clear+idle paint with if (!holdLastFrameAtEnd())")
    guard = first_brace_block(stop, guard_cond)
    if guard_cond in stop and not guard:
        errs.append("stop() hold-off guard must be a brace block")
    if guard:
        for needle in ("fb_.clear()", "paintIdle();", "startIdle();"):
            if needle not in guard:
                errs.append(f"stop() {needle} must sit inside the hold-off guard")
        if "startOsdPoll();" in guard:
            errs.append("stop() startOsdPoll() must run even when holding (outside the guard)")
        tail = after_block(stop, guard_cond)
        for needle in ("paintIdle();", "startIdle();", "fb_.clear()"):
            if needle in tail:
                errs.append(f"stop() {needle} appears after the hold-off guard (death still overwrites)")
        if "startOsdPoll();" not in tail:
            errs.append("stop() startOsdPoll() must remain after the hold-off brace")

    fpga_anchor = "Compressed path previously skipped host idle restore on natural EOF"
    fpga_snip = strip_comments(snippet_after(cpp, fpga_anchor, 500))
    if not fpga_snip:
        errs.append("could not find fpgaThreadMain natural-EOF idle restore")
    elif "&& !holdLastFrameAtEnd()" not in fpga_snip:
        errs.append("fpgaThreadMain EOF must skip paintIdle/startIdle when lastFrameHoldEnabled_")
    else:
        eof_guard = first_brace_block(fpga_snip, "if (!shuttingDown_.load() && !holdLastFrameAtEnd()")
        if not eof_guard:
            errs.append("fpgaThreadMain EOF hold skip must be on the shuttingDown conjunct")
        else:
            if "paintIdle();" not in eof_guard or "startIdle();" not in eof_guard:
                errs.append("fpgaThreadMain EOF hold-off brace must still paintIdle+startIdle")

    raw_anchor = "The frame store latches the last frame written"
    raw_snip = strip_comments(snippet_after(cpp, raw_anchor, 400))
    if not raw_snip:
        errs.append("could not find threadMain session-end idle restore")
    elif "if (!holdLastFrameAtEnd()" not in raw_snip:
        errs.append("threadMain session end must guard paintIdle/startIdle with lastFrameHoldEnabled_")
    else:
        raw_guard = first_brace_block(raw_snip, "if (!holdLastFrameAtEnd()")
        if not raw_guard:
            errs.append("threadMain session-end hold-off guard must be a brace block")
        else:
            if "paintIdle();" not in raw_guard or "startIdle();" not in raw_guard:
                errs.append("threadMain session-end hold-off brace must still paintIdle+startIdle")

    shut = method_body(code, "void MediaPlayer::shutdown()")
    if not shut:
        errs.append("could not parse void MediaPlayer::shutdown()")
    else:
        if "fb_.clear()" in shut:
            errs.append("shutdown() must not call fb_.clear()")
        if "paintIdle()" in shut:
            errs.append("shutdown() must not call paintIdle()")

    return errs


def git_show(sha: str, path: str) -> str:
    r = subprocess.run(
        ["git", "show", f"{sha}:{path}"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        check=False,
    )
    if r.returncode != 0:
        fail(f"git show {sha}:{path} true rc={r.returncode}: {r.stderr.strip()}")
    return r.stdout


def object_exists(sha: str) -> bool:
    r = subprocess.run(
        ["git", "cat-file", "-e", sha],
        cwd=str(ROOT),
        capture_output=True,
        check=False,
    )
    return r.returncode == 0


def synthetic_bad(cpp: str, hpp: str) -> tuple[str, str]:
    """The pre-fix tree: hold defaults off, startIdle returns early, stop() paints anyway."""
    bad_hpp = hpp.replace(
        "std::atomic<bool> lastFrameHoldEnabled_{true};",
        "std::atomic<bool> lastFrameHoldEnabled_ = false;",
        1,
    )
    if bad_hpp == hpp:
        fail("could not build a default-off hold flag")
    bad_cpp = cpp.replace(
        "void MediaPlayer::startIdle() {",
        "void MediaPlayer::startIdle() {\n    if (lastFrameHoldEnabled_.load()) return;",
        1,
    )
    if bad_cpp == cpp:
        fail("could not build a startIdle hold branch")
    painted = bad_cpp.replace("if (!holdLastFrameAtEnd()) {", "if (true) {", 1)
    if painted == bad_cpp:
        fail("could not build an unguarded stop() paint")
    return painted, bad_hpp


def main() -> None:
    if not MEDIA.is_file() or not HPP.is_file():
        fail(f"missing {MEDIA} or {HPP}")

    cpp = MEDIA.read_text()
    hpp = HPP.read_text()
    here_errs = analyze(cpp, hpp)
    if here_errs:
        for e in here_errs:
            print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
    print("OK working-tree hold default-on; stop/EOF skip clear+paintIdle; startIdle unreverted")

    if object_exists(BAD_SHA):
        old_cpp = git_show(BAD_SHA, "arm/misterplexd/media_player.cpp")
        old_hpp = git_show(BAD_SHA, "arm/misterplexd/media_player.hpp")
        label = BAD_SHA[:12]
    else:
        # 9ecab85 is not in this history. The analyzer still has to reject that shape.
        old_cpp, old_hpp = synthetic_bad(cpp, hpp)
        label = "synthetic " + BAD_SHA[:12]
    old_errs = analyze(old_cpp, old_hpp)
    required = (
        "hpp must default lastFrameHoldEnabled_{true}",
        "startIdle() must not branch on lastFrameHoldEnabled_",
        "stop() must guard clear+idle paint with if (!holdLastFrameAtEnd())",
    )
    if not old_errs:
        fail(f"analyzer is vacuous: {label} unexpectedly passed")
    missing = [m for m in required if not any(m in e for e in old_errs)]
    if missing and not object_exists(BAD_SHA):
        fail("synthetic bad tree was not rejected for: " + "; ".join(missing))
    print(f"OK red {label} rejected ({len(old_errs)} defects):")
    for e in old_errs:
        print(f"  - {e}")

    print("test_last_frame_hold_static: OK")


if __name__ == "__main__":
    main()
