#!/usr/bin/env python3
"""Regenerate ascal-hpoly-pipe.patch from vendor/sys/ascal.vhd (T1b base)."""
from pathlib import Path
import hashlib
import subprocess
import sys

HERE = Path(__file__).resolve().parent
BASE = HERE.parent / "vendor/sys/ascal.vhd"
PATCH_OUT = HERE / "ascal-hpoly-pipe.patch"
PAT = Path("/tmp/owenb-hpoly-patched.vhd")

text = BASE.read_text()
repls = [
    (
        "\tSIGNAL o_sh,o_sh1,o_sh2,o_sh3,o_sh4 : std_logic;",
        "\tSIGNAL o_sh,o_sh1,o_sh2,o_sh3,o_sh4,o_sh5 : std_logic;",
    ),
    (
        "\tSIGNAL o_hpixs,o_hpix0,o_hpix1,o_hpix2,o_hpix3 : type_pix;",
        "\tSIGNAL o_hpixs,o_hpixs_d,o_hpix0,o_hpix1,o_hpix2,o_hpix3 : type_pix;",
    ),
    (
        "\tSIGNAL o_v_poly_phase, o_v_poly_phase2, o_h_poly_phase, o_poly_phase, o_poly_phase1 : poly_phase_interp_t;",
        "\tSIGNAL o_v_poly_phase, o_v_poly_phase2, o_h_poly_phase, o_h_poly_phase_d, o_poly_phase, o_poly_phase1 : poly_phase_interp_t;",
    ),
    (
        "\tSIGNAL o_h_poly_t,o_h_poly_t2,o_v_poly_t   : type_poly_t;\n\n\tSIGNAL o_v_poly_adaptive",
        "\tSIGNAL o_h_poly_t,o_h_poly_t2,o_v_poly_t   : type_poly_t;\n"
        "\tSIGNAL o_hpoly_wr_lat : unsigned(3 DOWNTO 0);\n"
        "\tSIGNAL o_hpoly_wadl_lat : natural RANGE 0 TO OHRESH-1;\n"
        "\tSIGNAL o_hpoly_ldw_lat  : type_pix;\n\n"
        "\tSIGNAL o_v_poly_adaptive",
    ),
    (
        "\t\t\to_sh1<=o_sh; o_sh2<=o_sh1; o_sh3<=o_sh2; o_sh4<=o_sh3;",
        "\t\t\to_sh1<=o_sh; o_sh2<=o_sh1; o_sh3<=o_sh2; o_sh4<=o_sh3; o_sh5<=o_sh4;",
    ),
    (
        "-- C9 : Apply Polyphase\n"
        "\t\t\to_h_poly_t<=poly_calc(o_h_poly_phase,o_hpixq(8));\n\n"
        "\t\t\t-- C10 : Sum and bound\n"
        "\t\t\to_h_poly_pix<=poly_final(o_h_poly_t);",
        "-- C9 : Apply Polyphase (hpoly-pipe: MAC then sum, pixels + phase delayed via o_sh5 / o_h_poly_phase_d)\n"
        "\t\t\to_h_poly_phase_d<=o_h_poly_phase;\n"
        "\t\t\to_h_poly_t<=poly_calc(o_h_poly_phase_d,o_hpixq(8));\n"
        "\t\t\to_h_poly_t2<=o_h_poly_t;\n"
        "\t\t\to_h_poly_pix<=poly_final(o_h_poly_t2);",
    ),
]
for old, new in repls:
    if old not in text:
        sys.exit(f"missing block: {old[:80]!r}")
    text = text.replace(old, new, 1)

start = text.index("\t\t\tIF o_sh4='1' THEN")
end = text.index("\t\t\t------------------------------------------------------", start)
sh4_old = text[start:end].rstrip()
if "hpix_v:=o_hpixs;" not in sh4_old:
    sys.exit("unexpected o_sh4 block")
sh4_new = sh4_old.replace(
    "\t\t\tIF o_sh4='1' THEN\n\t\t\t\thpix_v:=o_hpixs;",
    "\t\t\tIF o_sh4='1' THEN\n\t\t\t\t\to_hpixs_d<=o_hpixs;\n\t\t\tEND IF;\n\n"
    "\t\t\tIF o_sh5='1' THEN\n\t\t\t\thpix_v:=o_hpixs_d;",
)
sh4_new = (
    sh4_new.replace("o_hpixs.b", "o_hpixs_d.b")
    .replace("o_hpixs.g", "o_hpixs_d.g")
    .replace("o_hpixs.r", "o_hpixs_d.r")
)
text = text[:start] + sh4_new + text[end:]

olbuf_old = (
    "\t\t\tEND CASE;\n"
    "\t\t\t------------------------------------------------------\n"
    "\t\tEND IF;\n"
    "\tEND PROCESS HSCAL;\n\n"
    "\t-----------------------------------------------------------------------------\n"
    "\t-- Line buffers 4 x OHRES x (R+G+B)\n"
    "\tOLBUF:PROCESS(o_clk) IS\n"
    "\tBEGIN\n"
    "\t\tIF rising_edge(o_clk) THEN\n"
    "\t\t\t-----------------------------------------------\n"
    "\t\t\t-- WRITES\n"
    "\t\t\tIF o_wr(0)='1' AND o_wadl < OHRESL  THEN o_line0(o_wadl MOD OHRESL)<=o_ldw; END IF;\n"
    "\t\t\tIF o_wr(1)='1' AND o_wadl < OHRESL  THEN o_line1(o_wadl MOD OHRESL)<=o_ldw; END IF;\n"
    "\t\t\tIF o_wr(2)='1' AND o_wadl < OHRESL  THEN o_line2(o_wadl MOD OHRESL)<=o_ldw; END IF;\n"
    "\t\t\tIF o_wr(3)='1' AND o_wadl < OHRESL  THEN o_line3(o_wadl MOD OHRESL)<=o_ldw; END IF;\n"
    "\t\t\tIF OHRES = 2304 OR OHRES = 2560 THEN\n"
    "\t\t\t\tIF o_wr(0)='1' AND o_wadl >= OHRESL THEN o_linf0(o_wadl MOD OHRESM)<=o_ldw; END IF;\n"
    "\t\t\t\tIF o_wr(1)='1' AND o_wadl >= OHRESL THEN o_linf1(o_wadl MOD OHRESM)<=o_ldw; END IF;\n"
    "\t\t\t\tIF o_wr(2)='1' AND o_wadl >= OHRESL THEN o_linf2(o_wadl MOD OHRESM)<=o_ldw; END IF;\n"
    "\t\t\t\tIF o_wr(3)='1' AND o_wadl >= OHRESL THEN o_linf3(o_wadl MOD OHRESM)<=o_ldw; END IF;\n"
    "\t\t\tEND IF;"
)
olbuf_new = (
    "\t\t\tEND CASE;\n"
    "\t\t\t\to_hpoly_wadl_lat<=o_wadl;\n"
    "\t\t\t\to_hpoly_wr_lat<=o_wr;\n"
    "\t\t\t\to_hpoly_ldw_lat<=o_ldw;\n"
    "\t\t\t------------------------------------------------------\n"
    "\t\tEND IF;\n"
    "\tEND PROCESS HSCAL;\n\n"
    "\t-----------------------------------------------------------------------------\n"
    "\t-- Line buffers 4 x OHRES x (R+G+B)\n"
    "\tOLBUF:PROCESS(o_clk) IS\n"
    "\t\tVARIABLE wr_v : unsigned(3 DOWNTO 0);\n"
    "\t\tVARIABLE wadl_v : natural RANGE 0 TO OHRESH-1;\n"
    "\t\tVARIABLE ldw_v : type_pix;\n"
    "\tBEGIN\n"
    "\t\tIF rising_edge(o_clk) THEN\n"
    "\t\t\tIF o_hmode(2 DOWNTO 0) > \"011\" THEN\n"
    "\t\t\t\twr_v := o_hpoly_wr_lat;\n"
    "\t\t\t\twadl_v := o_hpoly_wadl_lat;\n"
    "\t\t\t\tldw_v := o_hpoly_ldw_lat;\n"
    "\t\t\tELSE\n"
    "\t\t\t\twr_v := o_wr;\n"
    "\t\t\t\twadl_v := o_wadl;\n"
    "\t\t\t\tldw_v := o_ldw;\n"
    "\t\t\tEND IF;\n"
    "\t\t\t-----------------------------------------------\n"
    "\t\t\t-- WRITES\n"
    "\t\t\tIF wr_v(0)='1' AND wadl_v < OHRESL  THEN o_line0(wadl_v MOD OHRESL)<=ldw_v; END IF;\n"
    "\t\t\tIF wr_v(1)='1' AND wadl_v < OHRESL  THEN o_line1(wadl_v MOD OHRESL)<=ldw_v; END IF;\n"
    "\t\t\tIF wr_v(2)='1' AND wadl_v < OHRESL  THEN o_line2(wadl_v MOD OHRESL)<=ldw_v; END IF;\n"
    "\t\t\tIF wr_v(3)='1' AND wadl_v < OHRESL  THEN o_line3(wadl_v MOD OHRESL)<=ldw_v; END IF;\n"
    "\t\t\tIF OHRES = 2304 OR OHRES = 2560 THEN\n"
    "\t\t\t\tIF wr_v(0)='1' AND wadl_v >= OHRESL THEN o_linf0(wadl_v MOD OHRESM)<=ldw_v; END IF;\n"
    "\t\t\t\tIF wr_v(1)='1' AND wadl_v >= OHRESL THEN o_linf1(wadl_v MOD OHRESM)<=ldw_v; END IF;\n"
    "\t\t\t\tIF wr_v(2)='1' AND wadl_v >= OHRESL THEN o_linf2(wadl_v MOD OHRESM)<=ldw_v; END IF;\n"
    "\t\t\t\tIF wr_v(3)='1' AND wadl_v >= OHRESL THEN o_linf3(wadl_v MOD OHRESM)<=ldw_v; END IF;\n"
    "\t\t\tEND IF;"
)
if olbuf_old not in text:
    sys.exit("missing OLBUF block")
text = text.replace(olbuf_old, olbuf_new, 1)
PAT.write_text(text)
md5 = hashlib.md5(text.encode()).hexdigest()
print(f"patched md5 {md5}")

with PATCH_OUT.open("w") as out:
    subprocess.run(
        [
            "diff",
            "-u",
            "--label",
            "a/ascal.vhd",
            "--label",
            "b/ascal.vhd",
            str(BASE),
            str(PAT),
        ],
        check=False,
        stdout=out,
    )
with PATCH_OUT.open("rb") as pin:
    dry = subprocess.run(
        ["patch", "-p1", "--dry-run", "-d", str(BASE.parent)],
        stdin=pin,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
if dry.returncode != 0:
    sys.exit("generated patch does not apply cleanly")
print(f"wrote {PATCH_OUT}")
