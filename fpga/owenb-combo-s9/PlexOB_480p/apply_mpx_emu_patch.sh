#!/usr/bin/env bash
# MPX-FORK: instantiate mpx_plexob_core_hook in vendor emu.sv (re-run after sync_vendor).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EMU="$HERE/vendor/h264/emu.sv"
MARK="u_mpx_plexob_core_hook"
grep -q "$MARK" "$EMU" && exit 0

python3 - "$EMU" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
needle = "assign LED_USER  = dec_alive;"
insert = """// MPX-FORK PlexOB scaffold (private; see fpga/PlexOB_480p/CAPS_BRIDGE.md)
wire mpx_plexob_led;
wire [31:0] mpx_status_build_id, mpx_caps_build_id;
mpx_plexob_core_hook #(
`ifdef MPX_PLEXOB_BUILD_ID
    .BUILD_ID(`MPX_PLEXOB_BUILD_ID)
`else
    .BUILD_ID(32'h4F800001)
`endif
) u_mpx_plexob_core_hook (
    .clk(clk_sys),
    .rst_n(~reset_req),
    .probe_valid(1'b0),
    .probe_nonce(64'd0),
    .led_mpx(mpx_plexob_led),
    .status_build_id(mpx_status_build_id),
    .caps_build_id(mpx_caps_build_id)
);

"""
if needle not in text:
    sys.exit("apply_mpx_emu_patch: LED_USER anchor missing in emu.sv")
text = text.replace(needle, insert + needle.replace("dec_alive", "dec_alive | mpx_plexob_led"))
open(path, "w").write(text)
PY
echo "apply_mpx_emu_patch: inserted $MARK into vendor/h264/emu.sv"
