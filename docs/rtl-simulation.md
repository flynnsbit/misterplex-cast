# RTL simulation

MiSTerPlex has a userspace Verilator path for behavioural RTL checks before paying for a Quartus fit. The default local install is outside the repo:

```bash
export OSS_CAD_SUITE="$HOME/.local/oss-cad-suite"
scripts/run_verilator.sh --version
```

Do not source `~/.local/oss-cad-suite/environment` globally and do not prepend the suite to your login `PATH`; that can shadow normal host/Quartus tools. `scripts/run_verilator.sh` scopes `PATH="$OSS_CAD_SUITE/bin:$PATH"` to the single Verilator process only. The unit harness uses that wrapper, which checks `$VERILATOR`, then `$OSS_CAD_SUITE/bin/verilator`, then `verilator` on the existing `PATH`.

## Built-in H.264 IQ/IDCT/recon simulation

Run just the RTL sim:

```bash
make rtl-sim
# or
VERILATOR=$HOME/.local/oss-cad-suite/bin/verilator tests/unit/test_p3_idct_rtl_sim.sh
scripts/run_verilator.sh --version
```

`make unit` invokes the same script. If Verilator is absent it prints a loud `SKIP RTL SIM` notice and exits 0 so non-simulator hosts keep their host tests usable; that skip is not a hardware-quality pass.

The current test elaborates `fpga/Plex_MiSTer/rtl/h264_iq_idct_4x4.sv` with Verilator and drives all 16 luma 4x4 blocks from `tests/fixtures/p3_host_recon/mb0_luma_v1.json`. It compares actual RTL `dequant`, `idct`, and `recon` outputs against the checked-in golden JSON.

## Simulating another module

Create a C++ testbench under `tests/rtl/` that includes the generated `V<top>.h`, drives ports, calls `eval()`, and checks outputs. Then run:

```bash
ROOT=$PWD
TOP=my_top_module
mkdir -p build/verilator/$TOP
scripts/run_verilator.sh --cc --exe --build \
  --Mdir build/verilator/$TOP \
  --top-module $TOP \
  -CFLAGS "-std=c++17 -O2" \
  fpga/Plex_MiSTer/rtl/my_top_module.sv tests/rtl/my_top_module_tb.cpp
build/verilator/$TOP/V$TOP
```

For multi-file RTL, list every required `.sv` on the Verilator command line or use `-f filelist.f`. Keep generated outputs under `build/verilator/`.

## Whole-project RTL lint baseline

Run the Verilator parse/lint gate without starting Quartus:

```bash
make rtl-lint
# or update the checked-in baseline intentionally after triage:
scripts/rtl_lint.py --write-baseline
```

`rtl-lint` parses `fpga/Plex_MiSTer/Plex.qsf` plus sourced `.qip`/`.tcl` assignments, injects the active Quartus product macros into Verilator, runs Verilator on owned RTL, and reports only warnings physically located in MiSTerPlex-owned sources. Vendor/generated context (`sys/`, `rtl/pll/`, Intel primitive stubs) is excluded from the ranked counts so it does not bury project warnings. This is not a Quartus synthesis/buildability check.

The checked-in baseline is `tests/fixtures/rtl_lint_baseline.json`. Existing `WIDTHTRUNC`, `WIDTHEXPAND`, `WIDTH`, `UNSIGNED`, and `IMPLICIT*` counts are allowed; any count above baseline fails. The baseline stores both per-file/type counts and `warning_details` entries with line/message text so a diff shows which warning moved or appeared. `make unit` runs this gate after the RTL simulations. If Verilator is absent, the target refuses with `RTL LINT REFUSED(exit=3)` rather than silently passing.

Run the curated Quartus subset guard before requesting a full fit:

```bash
make define-parity
make quartus-sv-subset
```

`define-parity` prints the raw Quartus/Verilator macro table and refuses if the
product Quartus macro set diverges from the Verilator/lint macro set. Test-only
fault macros are accepted only when declared in
`tests/fixtures/define_parity_allowlist.json`.

`quartus-sv-subset` first proves a real Quartus toolchain is reachable, then scans the product Quartus file list for observed Quartus 17.0.2 SystemVerilog subset hazards that Verilator accepted: function-result part-selects, the observed `ref_win[...]` function-body concatenation pattern, and `localparam` declarations in module parameter lists. If Quartus is absent it refuses with `QUARTUS_SV_SUBSET_REFUSED(exit=4)`. This is still a static curated guard, not Analysis & Elaboration; unsupported inference, generate/parameter scoping, latch inference, and other elaboration-only Quartus failures can still reach a fit unless caught by a real Quartus analysis pass.

After a remote fit, run the hierarchy resource guard against copied Quartus reports:

```bash
make post-fit-hierarchy FIT_RPT=fpga/Plex_MiSTer/remote_out/<slot>/Plex.fit.rpt \
  MAP_RPT=fpga/Plex_MiSTer/remote_out/<slot>/Plex.map.rpt \
  COMPILE_LOG=fpga/Plex_MiSTer/remote_out/<slot>/compile.log
```

`post-fit-hierarchy` prints the fitted hierarchy resources for critical modules
declared in `tests/fixtures/critical_fit_hierarchy.json` and refuses if one is
missing, optimized down below the declared resource floor, or has removal/tie-off
warnings in the compile log. `scripts/build_rbf_remote.sh` copies the map report
and compile log and runs this check automatically when copy-back is enabled.
