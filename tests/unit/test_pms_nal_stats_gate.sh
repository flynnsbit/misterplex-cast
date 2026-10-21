#!/usr/bin/env bash
# Command-level error propagation; shared AnnexBFramer/ABI tests belong to the coordinator.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
make --quiet "$ROOT/build/pms_nal_stats"
python3 - "$ROOT" <<'PY'
from pathlib import Path
import os
import subprocess
import sys

root = Path(sys.argv[1])
work = root/"build/pms-nal-stats-gate"
work.mkdir(exist_ok=True)
probe = root/"build/pms_nal_stats"
first = b"\x00\x00\x00\x01\x67\x11\x80"
second = b"\x00\x00\x01\x68\x22\x80"
third = b"\x00\x00\x01\x65\x33\x80"
env = os.environ.copy()
for key in ("PLEX_BASE", "PLEX_TOKEN", "MISTERPLEX_BASELINE_KEY", "PLEX_KEY"):
    env.pop(key, None)

def run(name, data, *options, red=None, partial=False):
    path = work/f"{name}.264"
    path.write_bytes(data)
    result = subprocess.run([str(probe), "--annexb", str(path), *options],
                            capture_output=True, text=True, env=env, timeout=20)
    output = result.stdout+result.stderr
    if red is None:
        assert result.returncode == 0 and "origin=offline" in output, output
        assert "nal_bytes="+str(len(data)) in output, output
    else:
        assert result.returncode != 0 and red in output, output
        assert "PMS_NAL_STATS " not in output, "partial samples became successful statistics"
        if partial:
            assert "partial_samples=1" in output, output

run("eof-flush", first+second)
run("overflow-after-good", first+b"\x00\x00\x01\x65"+b"\x55"*200,
    "--max-nal-bytes", "32", red="buffer limit", partial=True)
run("malformed-eof-after-good", first+b"\x00\x00\x01",
    red="malformed or truncated", partial=True)
run("consumer-reject-during-push", first+second+third, "--max-samples", "1",
    red="sample limit", partial=True)
run("consumer-reject-during-finish", first+second, "--max-samples", "1",
    red="sample limit", partial=True)
run("empty", b"", red="no Annex-B")
run("malformed-leading-data", b"\x12"+first, red="malformed or truncated")
print("test_pms_nal_stats_gate: OK command nonzero for push/finish/consumer failures after good samples")
PY
