#!/usr/bin/env python3
"""Stub after 26b05aeef: the fpga_audio_routing Verilator bench and native_de RTL are not on canon."""
import sys

print(
    "SKIP: test_fpga_audio_routing.py bench absent on canon "
    "(full test dropped 26b05aeef; unit-host excludes this step)",
    file=sys.stderr,
)
sys.exit(0)
