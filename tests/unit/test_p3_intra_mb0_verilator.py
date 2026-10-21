#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERILATOR = Path.home() / ".local/oss-cad-suite-20260726/bin/verilator"
FIXTURE = ROOT / "tests/fixtures/p3_host_recon/mb0_luma_v1.json"
RTL = ROOT / "fpga/Plex_MiSTer/rtl"
TB = ROOT / "tests/unit/rtl/p3_intra_mb0_tb.sv"
sys.path.insert(0, str(ROOT / "tests/unit"))
from expected_red import ExpectedRedError, require_expected_red  # noqa: E402


def c_array(values):
    return "{" + ",".join(str(int(v)) for v in values) + "}"


def write_harness(path: Path) -> None:
    data = json.loads(FIXTURE.read_text())
    blocks = data["blocks"]
    inits = []
    for b in blocks:
        inits.append(
            "    {" + ",".join([
                str(b["block"]), str(b["x"]), str(b["y"]), str(b["pred_mode"]),
                c_array(b["idct"]), c_array(b["pred"]), c_array(b["recon"]),
            ]) + "}"
        )
    source = f'''#include "Vp3_intra_mb0_tb.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <iostream>

struct Block {{
    int block;
    int x;
    int y;
    int mode;
    int idct[16];
    int pred[16];
    int recon[16];
}};

static const Block kBlocks[] = {{
{',\n'.join(inits)}
}};

static uint32_t signed29(int v) {{
    return static_cast<uint32_t>(v) & ((1u << 29) - 1u);
}}

int main(int argc, char** argv) {{
    Verilated::commandArgs(argc, argv);
    Vp3_intra_mb0_tb dut;
    uint8_t current[16][16] = {{}};
    int failures = 0;

    for (const auto& b : kBlocks) {{
        dut.mode = static_cast<uint8_t>(b.mode);
        dut.has_above = b.y > 0;
        dut.has_left = b.x > 0;
        dut.top_left = (b.x > 0 && b.y > 0) ? current[b.y - 1][b.x - 1] : 0;

        for (int i = 0; i < 8; ++i) {{
            uint8_t v = 128;
            if (dut.has_above) {{
                int sx = b.x + i;
                v = (sx < 16) ? current[b.y - 1][sx] : current[b.y - 1][b.x + 3];
            }}
            dut.above[i] = v;
        }}
        for (int i = 0; i < 4; ++i) {{
            dut.left[i] = dut.has_left ? current[b.y + i][b.x - 1] : 128;
            dut.residual[i] = signed29(b.idct[i]);
            dut.residual[i + 4] = signed29(b.idct[i + 4]);
            dut.residual[i + 8] = signed29(b.idct[i + 8]);
            dut.residual[i + 12] = signed29(b.idct[i + 12]);
        }}

        dut.eval();
        if (dut.used_mode != b.mode) {{
            std::cerr << "block " << b.block << " used_mode got " << int(dut.used_mode)
                      << " expected " << b.mode << "\\n";
            ++failures;
        }}
        for (int i = 0; i < 16; ++i) {{
            if (dut.pred[i] != b.pred[i]) {{
                std::cerr << "block " << b.block << " pred[" << i << "] got "
                          << int(dut.pred[i]) << " expected " << b.pred[i] << "\\n";
                ++failures;
            }}
            if (dut.recon[i] != b.recon[i]) {{
                std::cerr << "block " << b.block << " recon[" << i << "] got "
                          << int(dut.recon[i]) << " expected " << b.recon[i] << "\\n";
                ++failures;
            }}
        }}
        for (int yy = 0; yy < 4; ++yy) {{
            for (int xx = 0; xx < 4; ++xx) {{
                current[b.y + yy][b.x + xx] = dut.recon[yy * 4 + xx];
            }}
        }}
    }}

    if (failures) {{
        std::cerr << "P3 intra MB0 Verilator exact check FAILED: " << failures << " mismatches\\n";
        return 1;
    }}
    std::cout << "P3 intra MB0 Verilator exact check PASS: " << (sizeof(kBlocks) / sizeof(kBlocks[0]))
              << " luma 4x4 blocks matched pred/recon exactly. "
              << "CAVEAT: MB0 only exercises I4x4 DC/H/V, not full P3-3l3 coverage.\\n";
    return 0;
}}
'''
    path.write_text(source)


def run(cmd, *, expect_success=True):
    proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(proc.stdout)
    if expect_success and proc.returncode != 0:
        raise SystemExit(proc.returncode)
    return proc.returncode


def run_capture(cmd):
    proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout


def source_fingerprint(name: str, negative: bool) -> str:
    h = hashlib.sha256()
    h.update(name.encode("utf-8"))
    h.update(b"negative" if negative else b"positive")
    for path in [RTL / "h264_iq_idct_4x4.sv", RTL / "h264_intra_pred.sv", TB, FIXTURE]:
        h.update(path.read_bytes())
    return h.hexdigest()[:12]


def build_and_run(name: str, negative: bool, expected_red_id: str | None = None) -> int:
    build_dir = ROOT / f"build/obj_{name}_{source_fingerprint(name, negative)}"
    cpp = ROOT / f"build/{name}_main.cpp"
    build_dir.mkdir(parents=True, exist_ok=True)
    cpp.parent.mkdir(parents=True, exist_ok=True)
    write_harness(cpp)
    cmd = [
        str(VERILATOR), "--cc", "--exe", "--build",
        "--top-module", "p3_intra_mb0_tb",
        "--Mdir", str(build_dir),
        "-Wno-fatal", "-Wno-WIDTHEXPAND", "-Wno-WIDTHTRUNC",
        "--CFLAGS", "-std=c++17 -O2",
    ]
    if negative:
        cmd.append("-DP3_INTRA_NEGATIVE_TEST")
    cmd += [
        str(RTL / "h264_iq_idct_4x4.sv"),
        str(RTL / "h264_intra_pred.sv"),
        str(TB),
        str(cpp),
    ]
    run(cmd)
    exe = build_dir / "Vp3_intra_mb0_tb"
    if expected_red_id:
        rc, out = run_capture([str(exe)])
        try:
            require_expected_red(expected_red_id, out, rc)
        except ExpectedRedError:
            sys.stdout.write(out)
            raise
        return rc
    return run([str(exe)], expect_success=False)


def write_guard_harness(path: Path) -> None:
    source = r'''#include "Vh264_intra_mode_guard.h"
#include "verilated.h"
#include <iostream>

static int failures = 0;

static void tick(Vh264_intra_mode_guard& dut) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
}

static void expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << msg << "\n";
        ++failures;
    }
}

static void reset(Vh264_intra_mode_guard& dut) {
    dut.reset = 1;
    dut.mb_valid = 0;
    tick(dut);
    dut.reset = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vh264_intra_mode_guard dut;

    reset(dut);
    expect(dut.unsupported_seen == 0, "reset should clear sticky unsupported_seen");

    // I16 Plane is now supported — should NOT trigger unsupported
    dut.mb_valid = 1;
    dut.mb_type = 1;
    dut.i16_pred_mode = 3;
    dut.mb_index = 42;
    dut.block_index = 7;
    tick(dut);
    expect(dut.unsupported_valid == 0, "I16 Plane should NOT pulse unsupported_valid (now implemented)");
    expect(dut.unsupported_seen == 0, "I16 Plane should NOT set sticky unsupported_seen (now implemented)");

    // I_PCM remains unsupported — should still trigger unsupported
    reset(dut);
    dut.mb_valid = 1;
    dut.mb_type = 25;
    dut.i16_pred_mode = 0;
    tick(dut);
    expect(dut.unsupported_valid == 1, "IPCM should pulse unsupported_valid (still unsupported)");
    expect(dut.unsupported_code == 2, "IPCM should report code 2");
    expect(dut.unsupported_seen == 1, "IPCM should set sticky unsupported_seen");

    // Bad MB type should still trigger unsupported
    reset(dut);
    dut.mb_valid = 1;
    dut.mb_type = 99;
    dut.i16_pred_mode = 0;
    tick(dut);
    expect(dut.unsupported_valid == 1, "unknown MB type should pulse unsupported_valid");
    expect(dut.unsupported_code == 3, "unknown MB type should report code 3");

    // Normal I16 modes (0, 1, 2) should still NOT trigger unsupported
    for (int m = 0; m < 3; ++m) {
        reset(dut);
        dut.mb_valid = 1;
        dut.mb_type = 1;
        dut.i16_pred_mode = m;
        tick(dut);
        if (dut.unsupported_valid) {
            std::cerr << "I16 mode " << m << " falsely triggered unsupported\n";
            ++failures;
        }
    }

    // Normal I4 (mb_type=0) should still NOT trigger unsupported
    reset(dut);
    dut.mb_valid = 1;
    dut.mb_type = 0;
    dut.i16_pred_mode = 0;
    tick(dut);
    expect(dut.unsupported_valid == 0, "I4 should NOT pulse unsupported_valid");

    if (failures) {
        std::cerr << "P3 intra mode guard Verilator check FAILED: " << failures << " mismatches\n";
        return 1;
    }
    std::cout << "P3 intra mode guard Verilator check PASS: I16 Plane accepted, IPCM+bad type still caught.\n";
    return 0;
}
'''
    path.write_text(source)


def build_and_run_guard() -> int:
    build_dir = ROOT / f"build/obj_p3_intra_guard_{source_fingerprint('guard', False)}"
    cpp = ROOT / "build/p3_intra_guard_main.cpp"
    build_dir.mkdir(parents=True, exist_ok=True)
    cpp.parent.mkdir(parents=True, exist_ok=True)
    write_guard_harness(cpp)
    cmd = [
        str(VERILATOR), "--cc", "--exe", "--build",
        "--top-module", "h264_intra_mode_guard",
        "--Mdir", str(build_dir),
        "--CFLAGS", "-std=c++17 -O2",
        str(RTL / "h264_intra_pred.sv"),
        str(cpp),
    ]
    run(cmd)
    return run([str(build_dir / "Vh264_intra_mode_guard")], expect_success=False)


def main() -> int:
    if not VERILATOR.exists():
        print(f"SKIP P3_INTRA_MB0_VERILATOR: Verilator not found at {VERILATOR}; RTL behavioural test NOT run")
        if os.environ.get("ALLOW_MISSING_VERILATOR", "0") != "1":
            print("RTL SIM ERROR: Verilator not found; refusing to report PASS without running the simulation.")
            print("A skipped RTL gate is NOT a pass. Set ALLOW_MISSING_VERILATOR=1 only if you accept that RTL was never verified.")
            return 3
        return 0
    neg_rc = build_and_run("p3_intra_mb0_neg", True, "p3_intra_mb0_negative")
    if neg_rc == 0:
        print("P3 intra MB0 negative-direction check FAILED: perturbing RTL behaviour still passed")
        return 1
    print("P3 intra MB0 negative-direction check PASS: RTL perturbation was detected (red path).")
    pos_rc = build_and_run("p3_intra_mb0", False)
    if pos_rc != 0:
        return pos_rc
    guard_rc = build_and_run_guard()
    if guard_rc != 0:
        return guard_rc
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
