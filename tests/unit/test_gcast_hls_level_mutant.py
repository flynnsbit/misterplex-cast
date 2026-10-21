#!/usr/bin/env python3
"""The HLS level check must be load-bearing: GCAST_MUT=hlslevel has to fail the xcode test."""
import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
env = os.environ.copy()
env["GCAST_MUT"] = "hlslevel"
r = subprocess.run([sys.executable, str(root / "tools/gcast/test_gcast_xcode.py")], env=env)
if r.returncode == 0:
    print("FAIL hlslevel mutant survived")
    sys.exit(1)
print("ok hlslevel mutant failed")
