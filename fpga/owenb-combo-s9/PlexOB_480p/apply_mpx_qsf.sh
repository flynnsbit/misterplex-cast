#!/usr/bin/env bash
# MPX-FORK: append our RTL to the vendor qsf after sync_vendor (vendor/ is git-ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QSF="$HERE/vendor/H264.qsf"
append_sv() {
  local f="$1"
  grep -q "$f" "$QSF" || echo "set_global_assignment -name SYSTEMVERILOG_FILE ../$f" >> "$QSF"
}
grep -q "MPX_PLEXOB_BUILD_ID" "$QSF" || {
  echo "" >> "$QSF"
  echo "# MPX-FORK $(date -u +%F): private PlexOB scaffold (not upstream)" >> "$QSF"
}
append_sv "mpx_plexob_status.sv"
append_sv "mpx_mister_caps_shim.sv"
append_sv "mpx_plexob_core_hook.sv"
grep -q "MPX_PLEXOB_BUILD_ID" "$QSF" || \
  echo 'set_global_assignment -name VERILOG_MACRO "MPX_PLEXOB_BUILD_ID=32'\''h4F800001"' >> "$QSF"
