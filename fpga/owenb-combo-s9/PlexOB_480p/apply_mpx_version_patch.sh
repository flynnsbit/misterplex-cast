#!/usr/bin/env bash
# MPX-FORK: build_release.sh requires dev-<slug> on non-release compiles.
set -euo pipefail
EMU="$(cd "$(dirname "$0")" && pwd)/vendor/h264/emu.sv"
SLUG='dev-plexob-p0'
grep -q "\"$SLUG\"" "$EMU" && exit 0
perl -pi -e 's/`define CORE_VERSION "v1\.1\.0"/`define CORE_VERSION "'"$SLUG"'"/' "$EMU"
echo "apply_mpx_version_patch: CORE_VERSION -> $SLUG in emu.sv"
