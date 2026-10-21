#!/usr/bin/env bash
# Refuse to produce untrustworthy test results.
#
# WHY THIS EXISTS
# ---------------
# Verilator under memory pressure returns WRONG ANSWERS, not errors, and those
# answers impersonate the exact decode bugs this project hunts (recon_sig 0x0 is
# both the real divergence signature and the phantom-red signature). A suite that
# reports a confident red it cannot reproduce is worse than a suite that refuses
# to run.
#
# WHAT IT MEASURES
# ----------------
# 1. Local Quartus processes. This is the *proven cause* of every phantom red
#    observed so far, not a proxy for it. A local fit peaks around 2 GB RSS on an
#    8-core/16 GB machine and reliably pushes the box into paging.
# 2. Paging RATE (pswpin/pswpout deltas over a sample window), NOT SwapFree.
#    SwapFree is a record of PAST pressure: once the kernel has swapped anything
#    out it stays low indefinitely even when the system is completely idle, and
#    reclaiming it needs swapoff, which needs unavailable sudo. Gating on
#    SwapFree therefore refuses forever, which trains everyone to set the
#    override and makes the gate worse than absent.
# 3. Absolute headroom floor, calibrated to this 16 GB fleet host. The
#    deterministic merge adjudication ran cleanly at ~5.4-6.1 GB available, and
#    sibling workers routinely validate around ~4.7 GB. Refuse below 4 GB rather
#    than a round 8 GB so the gate catches real starvation without becoming the
#    default outcome.
# 4. Swap exhaustion. The documented phantom-red incidents correlated with swap
#    at 100% full; low swap *history* is not enough, but no remaining swap slack
#    plus Verilator is unsafe.
#
# Override with MISTERPLEX_ALLOW_LOW_MEMORY_TESTS=1 (announces itself loudly).
set -uo pipefail

SAMPLE_SECONDS="${MISTERPLEX_PREFLIGHT_SAMPLE_SECONDS:-2}"
MIN_AVAIL_MB="${MISTERPLEX_PREFLIGHT_MIN_AVAIL_MB:-4096}"
MIN_SWAPFREE_MB="${MISTERPLEX_PREFLIGHT_MIN_SWAPFREE_MB:-32}"
VMSTAT_FILE="${MISTERPLEX_PREFLIGHT_VMSTAT:-/proc/vmstat}"
MEMINFO_FILE="${MISTERPLEX_PREFLIGHT_MEMINFO:-/proc/meminfo}"
# Pages per second. A quiet machine reads 0; sustained paging runs orders of
# magnitude above this.
MAX_PAGING_PPS="${MISTERPLEX_PREFLIGHT_MAX_PAGING_PPS:-200}"

fail() {
  echo "PREFLIGHT REFUSED: $*" >&2
  echo "  Results produced under this condition are not trustworthy." >&2
  echo "  Fix the cause, or set MISTERPLEX_ALLOW_LOW_MEMORY_TESTS=1 to override." >&2
  if [[ "${MISTERPLEX_ALLOW_LOW_MEMORY_TESTS:-0}" == "1" ]]; then
    echo "  OVERRIDE ACTIVE: continuing anyway. Any red from this run is SUSPECT." >&2
    exit 0
  fi
  exit 3
}

# --- 1. local Quartus ---------------------------------------------------------
# Match the binaries, not a shell pattern that would also match this script.
quartus_procs="$(ps -eo pid,rss,comm 2>/dev/null \
  | awk '$3 ~ /^quartus_(fit|sh|sta|map|asm)$/ {printf "    pid=%s rss=%sKB %s\n", $1, $2, $3}')"
if [[ -n "$quartus_procs" ]]; then
  echo "Local Quartus processes detected:" >&2
  echo "$quartus_procs" >&2
  fail "a local Quartus fit is running (fits are remote-only: scripts/build_rbf_remote.sh slotN)"
fi

# --- 2. paging rate -----------------------------------------------------------
read_paging() {
  awk '/^pswpin |^pswpout /{s+=$2} END{print s+0}' "$VMSTAT_FILE"
}
if [[ -r "$VMSTAT_FILE" ]]; then
  before="$(read_paging)"
  sleep "$SAMPLE_SECONDS"
  after="$(read_paging)"
  pps=$(( (after - before) / SAMPLE_SECONDS ))
  echo "preflight: paging ${pps} pages/s over ${SAMPLE_SECONDS}s (limit ${MAX_PAGING_PPS})"
  if (( pps > MAX_PAGING_PPS )); then
    fail "system is actively paging at ${pps} pages/s"
  fi
fi

# --- 3. available memory floor ------------------------------------------------
if [[ ! -r "$MEMINFO_FILE" ]]; then
  fail "cannot read memory information from ${MEMINFO_FILE}"
fi
avail_mb="$(awk '/^MemAvailable:/{print int($2/1024)}' "$MEMINFO_FILE")"
if [[ -z "$avail_mb" ]]; then
  fail "MemAvailable is missing from ${MEMINFO_FILE}"
fi
echo "preflight: MemAvailable ${avail_mb}MB (floor ${MIN_AVAIL_MB}MB)"
if (( avail_mb < MIN_AVAIL_MB )); then
  fail "only ${avail_mb}MB available for Verilator, below ${MIN_AVAIL_MB}MB absolute headroom floor"
fi

# --- 4. swap exhaustion -------------------------------------------------------
swapfree_mb="$(awk '/^SwapFree:/{print int($2/1024)}' "$MEMINFO_FILE")"
if [[ -n "$swapfree_mb" ]]; then
  echo "preflight: SwapFree ${swapfree_mb}MB (floor ${MIN_SWAPFREE_MB}MB)"
  if (( swapfree_mb < MIN_SWAPFREE_MB )); then
    fail "only ${swapfree_mb}MB swap free, below ${MIN_SWAPFREE_MB}MB exhaustion guard"
  fi
fi

echo "preflight OK: no local Quartus, no active paging, ${avail_mb}MB available, ${swapfree_mb:-unknown}MB swap free"
exit 0
