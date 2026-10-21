#!/usr/bin/env bash
# Run Verilator from a scoped userspace OSS CAD Suite install without sourcing its
# environment globally or changing the caller's shell/toolchain PATH.
set -euo pipefail

if [[ -n "${VERILATOR:-}" ]]; then
  if [[ ! -x "$VERILATOR" ]]; then
    echo "run_verilator: VERILATOR is not executable: $VERILATOR" >&2
    exit 2
  fi
  exec "$VERILATOR" "$@"
fi

OSS_CAD_SUITE="${OSS_CAD_SUITE:-$HOME/.local/oss-cad-suite}"
if [[ -x "$OSS_CAD_SUITE/bin/verilator" ]]; then
  PATH="$OSS_CAD_SUITE/bin:$PATH" exec "$OSS_CAD_SUITE/bin/verilator" "$@"
fi

if command -v verilator >/dev/null 2>&1; then
  exec "$(command -v verilator)" "$@"
fi

echo "run_verilator: Verilator not found. Install oss-cad-suite under ~/.local/oss-cad-suite or set VERILATOR=/path/to/verilator." >&2
exit 127
