#!/usr/bin/env bash
# Build Plex.rbf using misterfpga-dev Quartus Docker image.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Local fits are banned: they starve this box (~4GB free vs ~85GB remote) and Verilator
# under the resulting memory pressure returns WRONG ANSWERS rather than errors, which
# impersonate the decode bugs we hunt. Remote is also faster (337s vs 660s).
if [[ "${MISTERPLEX_ALLOW_LOCAL_FIT:-0}" != "1" ]]; then
  cat >&2 <<'EOF'
REFUSED: local Quartus fits are not permitted in this project.

Use the remote build farm instead:
    scripts/build_rbf_remote.sh slotN        # slots 1-4

Do NOT override NUM_PARALLEL_PROCESSORS: it changes the RBF and invalidates the
bit-identity proof. For a new configuration with no local reference, fit the same
source in two slots and require the two RBFs to be bit-identical.

Rationale: a local fit starves this machine, and `make unit` run under that pressure
produces untrustworthy results (measured: phantom `recon_sig 0x0`, `mb_exact=292/300`)
that look exactly like real decode failures.
EOF
  exit 3
fi

MISTER_DEV="${MISTER_DEV:-$HOME/misterfpga-dev}"
# Call scripts/mister-dev directly (bin/ symlink breaks SCRIPT_DIR for lib.sh)
MISTER_DEV_BIN="${MISTER_DEV}/scripts/mister-dev"
if [[ ! -x "$MISTER_DEV_BIN" ]]; then
  echo "mister-dev not found at $MISTER_DEV_BIN" >&2
  exit 1
fi
# Auto-find Plex.qpf; pass through extra flags (e.g. --clean)
exec "$MISTER_DEV_BIN" build "$ROOT/fpga/Plex_MiSTer" "$@"
