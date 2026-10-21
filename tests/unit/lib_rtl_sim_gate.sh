# Shared helpers for RTL sim unit gates.
# Source from tests/unit/*.sh after setting ROOT.
#
# Contract:
#   - Missing Verilator is NEVER exit 0 (that is a false green).
#   - Default: exit 3 (hard refuse).
#   - Only with ALLOW_MISSING_VERILATOR=1: exit 77 + SKIP-NOT-PASS line.
#   - Compile/elab pin errors come from scripts/run_verilator.sh as rc=2.
#   - A green path must prove the TB binary ran via assert_sim_executed markers.

# shellcheck shell=bash

rtl_sim_require_verilator() {
  local label="${1:-RTL SIM}"
  local run_vl="${RUN_VERILATOR:-${ROOT}/scripts/run_verilator.sh}"
  local ver rc
  if [[ ! -x "$run_vl" ]]; then
    echo "RTL SIM ERROR: Verilator runner not executable: $run_vl" >&2
    exit 3
  fi
  set +e
  ver="$("$run_vl" --version 2>&1)"
  rc=$?
  set -e
  if [[ "$rc" -eq 127 ]]; then
    echo "SKIP-NOT-PASS ${label}: Verilator not found; simulation was NOT run." >&2
    if [[ "${ALLOW_MISSING_VERILATOR:-0}" == "1" ]]; then
      echo "SKIP RTL SIM: ALLOW_MISSING_VERILATOR=1 accepted; this is NOT a pass (exit 77)." >&2
      exit 77
    fi
    echo "RTL SIM ERROR: Verilator not found; refusing PASS without simulation." >&2
    exit 3
  fi
  if [[ "$rc" -ne 0 ]]; then
    echo "RTL SIM ERROR: Verilator probe failed (rc=$rc):" >&2
    printf '%s\n' "$ver" >&2
    exit "$rc"
  fi
  VERILATOR_VERSION="$ver"
  echo "RTL SIM: using $VERILATOR_VERSION" >&2
}

# Require every marker to appear in $1 (log text) or fail rc=2.
assert_sim_executed() {
  local label="$1"
  shift
  local log="$1"
  shift
  local missing=0
  local m
  if [[ -z "${log//[$' \t\r\n']/}" ]]; then
    echo "FAIL $label: empty sim log — compile-only or no-exec is not a pass" >&2
    exit 2
  fi
  for m in "$@"; do
    if ! grep -q -- "$m" <<<"$log"; then
      echo "FAIL $label: sim did not EXECUTE expected marker: $m" >&2
      missing=1
    fi
  done
  if [[ "$missing" -ne 0 ]]; then
    echo "FAIL $label: compile-only or empty run is not a pass (soft-skip≠PASS)" >&2
    exit 2
  fi
}

# Run binary, capture stdout+stderr, require rc==0 and markers.
rtl_sim_run_expect_pass() {
  local label="$1"
  local bin="$2"
  shift 2
  if [[ ! -x "$bin" ]]; then
    echo "FAIL $label: TB binary missing or not executable: $bin" >&2
    exit 2
  fi
  local out rc
  set +e
  out="$("$bin" "$@" 2>&1)"
  rc=$?
  set -e
  printf '%s\n' "$out"
  echo "${label}_sim true rc=$rc"
  if [[ "$rc" -ne 0 ]]; then
    echo "FAIL $label: TB exited rc=$rc (want 0)" >&2
    exit "$rc"
  fi
  if [[ "$#" -gt 0 ]]; then
    assert_sim_executed "$label" "$out" "$@"
  else
    # At least one positive outcome token.
    assert_sim_executed "$label" "$out" "PASS"
  fi
}
