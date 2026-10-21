#!/usr/bin/env python3
"""c782 f2/f3/f4 source contract + us-into-pts red twin for PTS jump clamp.

GREEN: helper unit + media_player.cpp call-site pins.
RED: r1 CHECKs against a throwaway recreation of the 291c0d2352 us-into-pts body.
No Makefile target (c656/c782: do not add a make unit line).
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLAYER = ROOT / "arm" / "misterplexd" / "media_player.cpp"
HELPER = ROOT / "host" / "libmisterplex" / "pts_jump_clamp.hpp"
UNIT = ROOT / "tests" / "unit" / "test_pts_jump_clamp.cpp"
C656_PROG = ROOT / "docs" / "progress" / "C656-FIX-PTS-JUMP-CLAMP-R1.md"
LEFTOVER = "579846058"


def strip_mpx_av_trace(src: str) -> str:
    key = "MPX_AV_TRACE("
    out = []
    i = 0
    while True:
        j = src.find(key, i)
        if j < 0:
            out.append(src[i:])
            break
        out.append(src[i:j])
        k = j + len(key)
        depth = 1
        while k < len(src) and depth:
            if src[k] == "(":
                depth += 1
            elif src[k] == ")":
                depth -= 1
            k += 1
        i = k
    return "".join(out)


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def main() -> int:
    fails = []
    player = PLAYER.read_text()
    helper = HELPER.read_text()
    c656_prog = C656_PROG.read_text()

    if "packet.pts = last_ok_pts_ + frame_duration_us_" in player:
        fails.append("us-into-pts write still in media_player.cpp")
    if "delta < -50000" in player:
        fails.append("-50000 hysteresis still in media_player.cpp")
    if "clampPtsJump" not in player:
        fails.append("clampPtsJump missing from media_player.cpp")
    if "ptsJumpIsAcceptedNFrames" not in player:
        fails.append("ptsJumpIsAcceptedNFrames missing from media_player.cpp")
    if "packet.pts = clampPtsJump(" not in player:
        fails.append("live packet.pts = clampPtsJump( call missing")
    if player.find("ptsJumpIsAcceptedNFrames") > player.find("packet.pts = clampPtsJump("):
        fails.append("ptsJumpIsAcceptedNFrames must precede clampPtsJump")
    stripped = strip_mpx_av_trace(player)
    if "clampPtsJump" not in stripped:
        fails.append("clampPtsJump still inside MPX_AV_TRACE or missing after strip")
    if "ptsJumpIsAcceptedNFrames" not in stripped:
        fails.append("ptsJumpIsAcceptedNFrames still inside MPX_AV_TRACE")
    if "span.value(2, packet.pts)" not in player:
        fails.append("post-clamp TRACE span.value(2, packet.pts) missing")
    clamp_at = player.find("packet.pts = clampPtsJump(")
    post_at = player.find("span.value(2, packet.pts)")
    if clamp_at < 0 or post_at < 0 or clamp_at > post_at:
        fails.append("post-clamp TRACE must follow clampPtsJump")
    if "inline int64_t clampPtsJump" not in helper:
        fails.append("clampPtsJump helper missing")
    if "delta < 0 || delta > 2 * frame_duration_ticks" not in helper:
        fails.append("tick-space trip missing from helper")
    if "ptsJumpIsAcceptedNFrames" not in helper:
        fails.append("ptsJumpIsAcceptedNFrames missing from helper")
    if LEFTOVER not in c656_prog:
        fails.append(f"c656 progress missing leftover SHA {LEFTOVER}")
    if "index.lock" not in c656_prog or "EACCES" not in c656_prog:
        fails.append("c656 progress missing index.lock EACCES")

    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    green_bin = build / "test_pts_jump_clamp"
    green_rc, green_out = run(
        ["g++", "-std=c++17", "-Ihost", "-o", str(green_bin), str(UNIT)], cwd=str(ROOT)
    )
    print(f"green_compile_rc={green_rc}")
    if green_rc != 0:
        fails.append(f"green compile rc={green_rc}\n{green_out}")
    else:
        run_rc, run_out = run([str(green_bin)])
        print(f"green_run_rc={run_rc}")
        if run_rc != 0:
            fails.append(f"green run rc={run_rc}\n{run_out}")

    empty_rc, empty_out = run(
        ["g++", "-std=c++17", "-I/tmp/empty-pts-jump-include", "-o", "/tmp/test_pts_jump_clamp_empty", str(UNIT)],
        cwd=str(ROOT),
    )
    print(f"empty_I_compile_rc={empty_rc}")
    if empty_rc == 0:
        fails.append("empty -I compile should fail (missing header)")
    if "pts_jump_clamp.hpp" not in empty_out and "No such file" not in empty_out:
        # still a compile fail; keep the documented fatal
        pass

    us_body = r'''
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cmath>
static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)
// Throwaway recreation of 291c0d2352 us-into-pts clamp (c782 f2 red twin).
static int64_t oldUsIntoPts(int64_t pts, int64_t& last_ok_pts, int64_t& frame_duration_us,
                            uint32_t tbNum, uint32_t tbDen) {
    int64_t curUs = static_cast<int64_t>(
        static_cast<long double>(pts) * tbNum * 1000000 / tbDen);
    if (last_ok_pts != INT64_MIN && frame_duration_us > 0) {
        int64_t delta = curUs - last_ok_pts;
        if (delta < -50000 || delta > 2 * frame_duration_us) {
            pts = last_ok_pts + frame_duration_us;
            curUs = static_cast<int64_t>(
                static_cast<long double>(pts) * tbNum * 1000000 / tbDen);
        }
    }
    if (last_ok_pts == INT64_MIN) {
        last_ok_pts = curUs;
    } else {
        int64_t prevUs = last_ok_pts;
        last_ok_pts = curUs;
        if (frame_duration_us == 0) {
            int64_t diff = curUs - prevUs;
            if (diff > 0 && diff < 500000)
                frame_duration_us = diff;
        }
    }
    return pts;
}
int main() {
    const int64_t fd_unused = 3000;
    (void)fd_unused;
    int64_t last = INT64_MIN;
    int64_t learned = 0;
    CHECK(oldUsIntoPts(0, last, learned, 1, 90000) == 0);
    CHECK(last == 0);
    CHECK(oldUsIntoPts(3000, last, learned, 1, 90000) == 3000);
    CHECK(last == 3000);
    CHECK(oldUsIntoPts(90000, last, learned, 1, 90000) == 6000);
    CHECK(last == 6000);
    // last in microseconds after 6000 ticks @ 90 kHz; one-frame rewind is
    // -33333 us, which delta < -50000 misses. r1 wants clamp to last+1 frame.
    last = 66666;
    learned = 33333;
    CHECK(oldUsIntoPts(3000, last, learned, 1, 90000) == 9000);
    if (fails) { std::fprintf(stderr, "%d FAIL\n", fails); return 1; }
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "us_into_pts_red.cpp"
        src.write_text(us_body)
        red_bin = Path(td) / "us_into_pts_red"
        red_c_rc, red_c_out = run(["g++", "-std=c++17", "-o", str(red_bin), str(src)])
        print(f"us_into_pts_red_compile_rc={red_c_rc}")
        if red_c_rc != 0:
            fails.append(f"us-into-pts red twin failed to compile\n{red_c_out}")
        else:
            red_r_rc, red_r_out = run([str(red_bin)])
            print(f"us_into_pts_red_run_rc={red_r_rc}")
            if red_r_rc == 0:
                fails.append("us-into-pts red twin ran green; CHECKs must FAIL on 66666/us last")
            if "FAIL" not in red_r_out:
                fails.append("us-into-pts red twin produced no FAIL lines")
            print(red_r_out.strip())

    if fails:
        for f in fails:
            print("FAIL:", f)
        print(f"{len(fails)} FAIL")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
