#!/usr/bin/env bash
# Phase 1a RBF luck-reload ban.
#
# Given an md5/sha256 prefix (argv or env), exit 2 if banned, 0 if not.
# No prefix → unit self-test. Sourceable: play/deploy can `source` later.
#
#   ./tests/unit/test_phase1a_rbf_ban.sh 4bb74dc4          # exit 2
#   ./tests/unit/test_phase1a_rbf_ban.sh deadbeef          # exit 0
#   RBF_HASH=4bb74dc4 ./tests/unit/test_phase1a_rbf_ban.sh
#
# Does not delete RBFs. Does not SSH. Does not deploy. Soft-skip ≠ PASS.
# deploy_plex_core.sh has no hash hook — this file is the standalone gate.

# When sourced, only define helpers (do not set -euo or run the unit).
_phase1a_rbf_ban_this="${BASH_SOURCE[0]:-}"
_phase1a_rbf_ban_dir="$(cd "$(dirname "$_phase1a_rbf_ban_this")" && pwd)"

phase1a_rbf_ban_list_file() {
  if [[ -n "${PHASE1A_RBF_BAN_LIST:-}" ]]; then
    printf '%s\n' "$PHASE1A_RBF_BAN_LIST"
    return 0
  fi
  printf '%s\n' "$_phase1a_rbf_ban_dir/phase1a_rbf_ban.txt"
}

phase1a_rbf_ban_normalize() {
  local raw="${1:-}"
  raw="${raw#0x}"
  raw="${raw#0X}"
  printf '%s' "$raw" | tr -d ' \t\n\r' | tr 'A-F' 'a-f' | tr -cd '0-9a-f'
}

phase1a_rbf_ban_tokens() {
  local list
  list="$(phase1a_rbf_ban_list_file)"
  if [[ ! -f "$list" ]]; then
    echo "phase1a_rbf_ban: missing list $list" >&2
    return 1
  fi
  # shellcheck disable=SC2013
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    line="$(printf '%s' "$line" | tr -d ' \t\r')"
    [[ -z "$line" ]] && continue
    line="$(phase1a_rbf_ban_normalize "$line")"
    [[ ${#line} -lt 8 ]] && continue
    printf '%s\n' "$line"
  done <"$list"
}

# Return 2 if banned, 0 if not, 1 on usage/list error.
# Prints BANNED / OK on stdout. Safe to call from a sourced deploy/play.
phase1a_rbf_ban_check() {
  local cand tok tokens
  cand="$(phase1a_rbf_ban_normalize "${1:-}")"
  if [[ ${#cand} -lt 8 ]]; then
    echo "phase1a_rbf_ban: need ≥8 hex chars (argv or RBF_HASH / PHASE1A_RBF_HASH)" >&2
    return 1
  fi
  if ! tokens="$(phase1a_rbf_ban_tokens)"; then
    return 1
  fi
  while IFS= read -r tok || [[ -n "$tok" ]]; do
    [[ -z "$tok" ]] && continue
    if [[ "$cand" == "$tok"* || "$tok" == "$cand"* ]]; then
      echo "BANNED prefix=${tok:0:8} cand=${cand:0:8} (glass-FAIL / thrash — not product)"
      return 2
    fi
  done <<<"$tokens"
  echo "OK not-banned cand=${cand:0:8}"
  return 0
}

phase1a_rbf_ban_resolve_hash() {
  if [[ -n "${1:-}" ]]; then
    printf '%s\n' "$1"
    return 0
  fi
  if [[ -n "${RBF_HASH:-}" ]]; then
    printf '%s\n' "$RBF_HASH"
    return 0
  fi
  if [[ -n "${PHASE1A_RBF_HASH:-}" ]]; then
    printf '%s\n' "$PHASE1A_RBF_HASH"
    return 0
  fi
  return 1
}

_phase1a_rbf_ban_selftest() {
  local self list fail=0 rc out p
  self="$_phase1a_rbf_ban_this"
  list="$(phase1a_rbf_ban_list_file)"

  # Living glass-FAIL + historical thrash set. Do not drop any of these.
  local required=(
    4bb74dc4 285b7ca0
    da54cbb6 2b2c1eeb
    01aabf3d fad71435
    de1847f3 718fddae
    6faba2ac
    8832824e 75da8bb1 4d6ee356 4deaf6cc dabdaeb0
  )

  if [[ ! -f "$list" ]]; then
    echo "FAIL: missing $list" >&2
    return 1
  fi
  for p in "${required[@]}"; do
    if ! grep -qiE "^${p}" "$list"; then
      echo "FAIL: list dropped required prefix $p" >&2
      fail=1
    fi
  done

  expect_check() {
    local want="$1" hash="$2"
    set +e
    out="$(RBF_HASH= PHASE1A_RBF_HASH= "$self" "$hash" 2>&1)"
    rc=$?
    set -e
    echo "$out"
    if [[ "$rc" -ne "$want" ]]; then
      echo "FAIL: prefix $hash exit $rc want $want" >&2
      fail=1
    fi
  }

  echo "test_phase1a_rbf_ban: known FAIL prefixes must exit 2"
  for p in "${required[@]}"; do
    expect_check 2 "$p"
  done

  echo "test_phase1a_rbf_ban: fake deadbeef must exit 0"
  expect_check 0 deadbeef
  expect_check 0 DEADBEEF
  # Full living fit17 md5 still hits the prefix8 row.
  expect_check 2 4bb74dc4b4b42779e104ebe8d94d5b0e

  set +e
  out="$(RBF_HASH=4bb74dc4 PHASE1A_RBF_HASH= "$self" 2>&1)"
  rc=$?
  set -e
  echo "$out"
  if [[ "$rc" -ne 2 ]]; then
    echo "FAIL: RBF_HASH=4bb74dc4 exit $rc want 2" >&2
    fail=1
  fi

  set +e
  out="$(RBF_HASH=deadbeef PHASE1A_RBF_HASH= "$self" 2>&1)"
  rc=$?
  set -e
  echo "$out"
  if [[ "$rc" -ne 0 ]]; then
    echo "FAIL: RBF_HASH=deadbeef exit $rc want 0" >&2
    fail=1
  fi

  if [[ "$fail" -ne 0 ]]; then
    echo "test_phase1a_rbf_ban: FAIL" >&2
    return 1
  fi
  echo "test_phase1a_rbf_ban: OK (banned exit 2; deadbeef exit 0; list kept historical)"
  return 0
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  set -euo pipefail
  if hash="$(phase1a_rbf_ban_resolve_hash "${1:-}")"; then
    set +e
    phase1a_rbf_ban_check "$hash"
    rc=$?
    set -e
    exit "$rc"
  fi
  _phase1a_rbf_ban_selftest
fi
