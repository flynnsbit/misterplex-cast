#!/usr/bin/env python3
"""Check lint primitive interfaces and required full-top elaboration context."""

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import rtl_lint
import check_define_parity


class IntelPllInterfaceTests(unittest.TestCase):
    def test_hps_scope_and_literal_build_date_argument(self):
        project = ROOT / "fpga" / "Plex_MiSTer"
        defines = check_define_parity.verilator_define_args()
        self.assertIn('-DBUILD_DATE="lint"', defines)
        result = subprocess.run(
            [
                str(ROOT / "scripts" / "run_verilator.sh"),
                "--lint-only", "--top-module", "hps_io_elaboration_tb_top",
                "-Wno-fatal", "-Wno-PINMISSING",
                *defines,
                str(project / "sys/hps_io.sv"),
                str(ROOT / "tests/rtl/hps_io_elaboration_tb_top.sv"),
            ],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("%Warning-IMPLICIT:", result.stdout)

    def test_generated_wrapper_interfaces(self):
        project = ROOT / "fpga" / "Plex_MiSTer"
        stub = rtl_lint.write_intel_stubs()
        result = subprocess.run(
            [
                str(ROOT / "scripts" / "run_verilator.sh"),
                "--lint-only", "-Wno-fatal", "-Wno-MULTITOP", "-Wno-PINMISSING",
                f"-I{project}", f"-I{project / 'rtl'}",
                str(stub),
                str(project / "rtl/pll/pll_0002.v"),
                str(project / "sys/pll_audio/pll_audio_0002.v"),
                str(project / "sys/pll_hdmi/pll_hdmi_0002.v"),
            ],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
