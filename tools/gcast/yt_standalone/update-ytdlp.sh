#!/bin/bash
set -euo pipefail
export YTDLP_TAG="${1:-${YTDLP_TAG:-2026.08.19}}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec bash "$SCRIPT_DIR/Install_gcast_tools.sh"
