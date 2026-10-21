#!/usr/bin/env bash
# F1017 prereq (a): CONF_STR core name PlexOB (not upstream H264). Re-run after sync_vendor.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EMU="$HERE/vendor/h264/emu.sv"
[ -f "$EMU" ] || { echo "apply_mpx_conf_str_plexob: missing $EMU (run sync_vendor.sh)" >&2; exit 1; }
if grep -q '"PlexOB;;"' "$EMU"; then
  echo "apply_mpx_conf_str_plexob: already PlexOB"
  exit 0
fi
if ! grep -q '"H264;;"' "$EMU"; then
  echo "apply_mpx_conf_str_plexob: expected \"H264;;\" in CONF_STR" >&2
  exit 1
fi
sed -i 's/"H264;;"/"PlexOB;;"/' "$EMU"
grep -q '"PlexOB;;"' "$EMU" || { echo "apply_mpx_conf_str_plexob: sed failed" >&2; exit 1; }
echo "apply_mpx_conf_str_plexob: CONF_STR renamed H264 -> PlexOB in vendor/h264/emu.sv"
