#!/usr/bin/env python3
"""P3-3l2 integration guard for shared H.264 IQ/IDCT RTL.

The product path must instantiate fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv
from feat/p3-idct-sim rather than carrying a second transcription in
``decode_stub.sv``. When Verilator is available this test also runs the real RTL simulation suite,
including the product decode_stub reconstruction path. Without a simulator it
leaves an explicit SKIP line; that is not fit evidence.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/fixtures/p3_host_recon/mb0_luma_v1.json"
RESIDUAL_GOLD = ROOT / "host/libmisterplex/h264_residual_gold.hpp"
DECODE_STUB = ROOT / "fpga/Plex_MiSTer/rtl/decode_stub.sv"
IQ_IDCT_RTL = ROOT / "fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv"
FILES_QIP = ROOT / "fpga/Plex_MiSTer/files.qip"
NORM_ADJUST = [
    [10, 13, 16],
    [11, 14, 18],
    [13, 16, 20],
    [14, 18, 23],
    [16, 20, 25],
    [18, 23, 29],
]
ZIGZAG = [0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15]


def fail(msg: str) -> None:
    print(f"FAIL p3-idct-rtl: {msg}", file=sys.stderr)
    raise SystemExit(1)


def parse_coeff_scan() -> list[int]:
    text = RESIDUAL_GOLD.read_text()
    m = re.search(r"kCoeffScan\[16\]\s*=\s*\{([^}]+)\}", text, re.S)
    if not m:
        fail("could not parse residual_gold::kCoeffScan")
    vals = [int(x.strip()) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]
    if len(vals) != 16:
        fail(f"kCoeffScan length {len(vals)} != 16")
    return vals


def level_scale(qp: int, row: int, col: int) -> int:
    odd = (row & 1) + (col & 1)
    mi = 0 if odd == 0 else (1 if odd == 1 else 2)
    return NORM_ADJUST[qp % 6][mi]


def clip8(v: int) -> int:
    return 0 if v < 0 else (255 if v > 255 else v)


def dequant4x4(coeff: list[int], qp: int) -> list[int]:
    blk = [0] * 16
    shift = qp // 6 + 2
    for k, level in enumerate(coeff):
        if level == 0:
            continue
        zi = ZIGZAG[k]
        row, col = divmod(zi, 4)
        qmul = (level_scale(qp, row, col) * 16) << shift
        blk[row * 4 + col] = (level * qmul + 32) >> 6
    return blk


def idct4x4_recon(deq: list[int], pred: list[int]) -> tuple[list[int], list[int]]:
    b = deq[:]
    b[0] += 32
    t = [0] * 16
    for row in range(4):
        base = row * 4
        z0 = b[base + 0] + b[base + 2]
        z1 = b[base + 0] - b[base + 2]
        z2 = (b[base + 1] >> 1) - b[base + 3]
        z3 = b[base + 1] + (b[base + 3] >> 1)
        t[base + 0] = z0 + z3
        t[base + 1] = z1 + z2
        t[base + 2] = z1 - z2
        t[base + 3] = z0 - z3

    residual = [0] * 16
    for col in range(4):
        z0 = t[col] + t[8 + col]
        z1 = t[col] - t[8 + col]
        z2 = (t[4 + col] >> 1) - t[12 + col]
        z3 = t[4 + col] + (t[12 + col] >> 1)
        residual[col] = (z0 + z3) >> 6
        residual[4 + col] = (z1 + z2) >> 6
        residual[8 + col] = (z1 - z2) >> 6
        residual[12 + col] = (z0 - z3) >> 6
    recon = [clip8(p + r) for p, r in zip(pred, residual)]
    return residual, recon


def xor8(vals: list[int]) -> int:
    c = 0
    for v in vals:
        c ^= v & 0xFF
    return c


def require_integration() -> None:
    rtl = IQ_IDCT_RTL.read_text()
    for module in ("h264_dequant4x4", "h264_idct4x4", "h264_recon4x4"):
        if f"module {module}" not in rtl:
            fail(f"shared RTL missing module {module}")

    stub = DECODE_STUB.read_text()
    for inst in ("u_h264_dequant4x4", "u_h264_idct4x4", "u_h264_recon4x4"):
        if inst not in stub:
            fail(f"decode_stub.sv does not instantiate shared RTL {inst}")
    for duplicate in ("function automatic int zigzag", "function automatic int norm_adjust"):
        if duplicate in stub:
            fail("decode_stub.sv still carries a duplicate IQ/IDCT implementation; use shared RTL")
    if ".max_coeff(5'd16)" not in stub:
        fail("decode_stub.sv must feed all 16 proven residual_coeff entries into dequant")

    qip = FILES_QIP.read_text()
    if "rtl/h264_iq_idct_4x4.sv" not in qip:
        fail("files.qip does not include h264_iq_idct_4x4.sv for product Quartus builds")
    if qip.index("rtl/h264_iq_idct_4x4.sv") > qip.index("rtl/decode_stub.sv"):
        fail("files.qip should list h264_iq_idct_4x4.sv before decode_stub.sv")


def run_behavioral_sim(coeff: list[int], deq: list[int], residual: list[int], recon: list[int], qp: int) -> bool:
    del coeff, deq, residual, recon, qp
    sim_script = ROOT / "tests/unit/test_p3_idct_rtl_sim.sh"
    try:
        sim = subprocess.run(
            [str(sim_script)],
            cwd=ROOT,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError as e:
        print(f"test_p3_idct_rtl_behavior: SKIP ({e}; do not spend a fit until real RTL sim runs)")
        return False
    if sim.stdout:
        print(sim.stdout.rstrip())
    if sim.returncode != 0:
        fail(f"Verilator RTL simulation failed with rc={sim.returncode}")
    return "OK real RTL sim:" in sim.stdout and "OK decode_stub RTL sim:" in sim.stdout


def main() -> int:
    data = json.loads(FIXTURE.read_text())
    if data.get("format") != "misterplex.p3.luma_mb.v1":
        fail("unexpected fixture format")
    block0 = data["blocks"][0]
    coeff = parse_coeff_scan()
    qp = data["macroblock"]["qp"]

    require_integration()

    deq = dequant4x4(coeff, qp)
    if deq != block0["dequant"]:
        fail(f"reference dequant mismatch\n  got={deq}\n  exp={block0['dequant']}")

    residual, recon = idct4x4_recon(deq, block0["pred"])
    if residual != block0["idct"]:
        fail(f"reference idct residual mismatch\n  got={residual}\n  exp={block0['idct']}")
    if recon != block0["recon"]:
        fail(f"reference recon mismatch\n  got={recon}\n  exp={block0['recon']}")

    sig = xor8(recon)
    if sig != 0x3B:
        fail(f"recon signature 0x{sig:02x}, expected 0x3b")

    ran_sim = run_behavioral_sim(coeff, deq, residual, recon, qp)
    suffix = " + behavioral RTL sim" if ran_sim else " + integration/static guard only"
    print(
        "test_p3_idct_rtl_model: OK shared RTL integrated; mb0_luma_v1 "
        f"recon_sig=0x{sig:02x} ({sig}){suffix}"
    )
    if os.environ.get("P3_IDCT_REQUIRE_RTL_SIM") == "1" and not ran_sim:
        fail("P3_IDCT_REQUIRE_RTL_SIM=1 but the Verilator behavioral simulator did not run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
