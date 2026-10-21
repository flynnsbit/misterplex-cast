#!/usr/bin/env python3
"""Curated source-level guard for Quartus SystemVerilog subset pitfalls.

This is not a synthesizer. It catches constructs that Verilator accepts but the
project's Quartus flow rejected during Analysis/Synthesis. The command-line gate
also probes for a real Quartus toolchain before reporting PASS; if the toolchain
is absent, it refuses instead of manufacturing confidence from a static scan.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REFUSE_RC = 4
REJECT_RC = 1


class ToolchainRefused(RuntimeError):
    pass


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def unpacked_arrays(text: str) -> set[str]:
    names: set[str] = set()
    decl = re.compile(
        r"\b(?:input|output|inout|wire|reg|logic)\b"
        r"(?:\s+(?:wire|reg|logic|signed))*"
        r"(?:\s+\[[^\]]+\])*"
        r"\s+([A-Za-z_]\w*)\s*\[[^\]]+\]"
    )
    for m in decl.finditer(text):
        names.add(m.group(1))
    return names


def module_param_blocks(text: str) -> list[tuple[int, str]]:
    blocks: list[tuple[int, str]] = []
    pat = re.compile(r"\bmodule\s+[A-Za-z_]\w*\s*#\s*\(")
    for m in pat.finditer(text):
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        blocks.append((m.end(), text[m.end() : i - 1]))
    return blocks


def function_blocks(text: str) -> list[tuple[int, str]]:
    blocks: list[tuple[int, str]] = []
    pat = re.compile(r"\bfunction\b")
    end_pat = re.compile(r"\bendfunction\b")
    for m in pat.finditer(text):
        end = end_pat.search(text, m.end())
        if end:
            blocks.append((m.start(), text[m.start() : end.end()]))
    return blocks


def line_no(text: str, idx: int) -> int:
    return text.count("\n", 0, idx) + 1


def _run_quartus_version(cmd: list[str], *, input_text: str | None = None) -> str | None:
    try:
        proc = subprocess.run(
            cmd,
            input=input_text,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"{type(e).__name__}: {e}"
    if proc.returncode == 0:
        return None
    return (proc.stdout or f"exit {proc.returncode}").strip()


def probe_quartus_toolchain() -> str:
    """Return a toolchain description or raise ToolchainRefused."""
    if os.environ.get("QUARTUS_SV_SUBSET_DISABLE_LOCAL_PROBE") != "1":
        quartus_map = os.environ.get("QUARTUS_MAP") or shutil.which("quartus_map")
        if quartus_map:
            if not os.access(quartus_map, os.X_OK):
                raise ToolchainRefused(f"QUARTUS_MAP is not executable: {quartus_map}")
            err = _run_quartus_version([quartus_map, "--version"])
            if err is None:
                return f"local:{quartus_map}"
            raise ToolchainRefused(f"local quartus_map probe failed: {err}")

    if os.environ.get("QUARTUS_SV_SUBSET_DISABLE_REMOTE_PROBE") == "1":
        raise ToolchainRefused("local Quartus not found and remote probe disabled")

    ssh_bin = os.environ.get("MISTER_PREFIT_SSH_BIN", "ssh")
    ssh_path = shutil.which(ssh_bin) if "/" not in ssh_bin else ssh_bin
    if not ssh_path or not os.path.exists(ssh_path):
        raise ToolchainRefused(f"ssh client not found: {ssh_bin}")

    host = os.environ.get("MISTER_PREFIT_REMOTE_HOST") or os.environ.get("MISTER_REMOTE_HOST") or "docker"
    remote_dev = os.environ.get("MISTER_PREFIT_REMOTE_DEV") or os.environ.get("MISTER_REMOTE_DEV") or "__DEFAULT_REMOTE_DEV__"
    remote_script = r'''
set -euo pipefail
remote_dev="$1"
if [[ "$remote_dev" == "__DEFAULT_REMOTE_DEV__" ]]; then
  remote_dev_expanded="$HOME/misterfpga-dev"
else
  remote_dev_expanded="$remote_dev"
fi
if [[ ! -f "$remote_dev_expanded/scripts/lib.sh" ]]; then
  echo "Remote mister-dev lib not found at $remote_dev_expanded/scripts/lib.sh" >&2
  exit 4
fi
# shellcheck source=/dev/null
source "$remote_dev_expanded/scripts/lib.sh"
load_env
if [[ -z "${QUARTUS_IMAGE:-}" ]]; then
  echo "QUARTUS_IMAGE is not set after mister-dev load_env" >&2
  exit 4
fi
docker run --rm "$QUARTUS_IMAGE" quartus_map --version >/dev/null
'''
    err = _run_quartus_version([ssh_bin, host, "bash", "-s", "--", remote_dev], input_text=remote_script)
    if err is None:
        return f"remote:{host}"
    raise ToolchainRefused(f"remote Quartus probe failed on {host}: {err}")


def check_file(path: Path) -> list[str]:
    raw = path.read_text()
    text = strip_comments(raw)
    errors: list[str] = []

    for start, block in module_param_blocks(text):
        rel = block.find("localparam")
        if rel >= 0:
            errors.append(
                f"{path}:{line_no(text, start + rel)}: localparam in module parameter list "
                "is rejected by Quartus Analysis/Synthesis; use a normal parameter or move "
                "the localparam into the module body."
            )

    call_select = re.compile(r"\b[A-Za-z_]\w*\s*\([^;\n]*\)\s*\[[^\]]+\]")
    for m in call_select.finditer(text):
        errors.append(
            f"{path}:{line_no(text, m.start())}: part-select on a function-call result "
            "is rejected by Quartus; assign through a typed helper/temp first."
        )

    # Observed Quartus 17.0.2 failure: h264_dpb ref_win[...] was concatenated
    # inside helper functions. Do not generalize this to every unpacked-array
    # concatenation: Quartus accepts some non-function and small helper cases in
    # this tree, and a false-positive "syntax" gate is also a lying instrument.
    arrays = {name for name in unpacked_arrays(text) if name == "ref_win"}
    for fn_start, fn_body in function_blocks(text):
        for name in arrays:
            concat_array = re.compile(r"\{[^{}\n;]*\b" + re.escape(name) + r"\s*\[[^{}\n;]*\][^{}\n;]*\}")
            for m in concat_array.finditer(fn_body):
                errors.append(
                    f"{path}:{line_no(text, fn_start + m.start())}: unpacked array element `{name}[...]` "
                    "inside a function-body concatenation matched the observed Quartus rejection; "
                    "read it into a scalar temp first."
                )

    return errors


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args(argv[1:])

    try:
        toolchain = probe_quartus_toolchain()
    except ToolchainRefused as e:
        print(f"QUARTUS_SV_SUBSET_REFUSED(exit={REFUSE_RC}): {e}", file=sys.stderr)
        return REFUSE_RC

    errors: list[str] = []
    for path in args.files:
        errors.extend(check_file(path))
    if errors:
        print(f"QUARTUS_SV_SUBSET_REJECTED(exit={REJECT_RC}):", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return REJECT_RC
    print(
        f"PASS Quartus SV subset guard: {len(args.files)} file(s); "
        f"toolchain={toolchain}; blind_spot=static_subset_only_no_elaboration_or_inference"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
