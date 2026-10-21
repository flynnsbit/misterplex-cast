#!/usr/bin/env bash
# 480p A/V present policy. RED twin: presentLeadMs_ = 0 (HEAD log-lie) must fail.
# GREEN: lead default 40 and hold-only drop when every-decoded.
set -u
set -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HPP="$ROOT/arm/misterplexd/media_player.hpp"
MAINC="$ROOT/arm/misterplexd/main.cpp"
FAB="$ROOT/host/libmisterplex/fabric_direct.hpp"
CLK="$ROOT/host/libmisterplex/av_clock.hpp"
CXX="${CXX:-g++}"
fails=0

fail() {
  echo "FAIL: $*" >&2
  fails=$((fails + 1))
}
ok() { echo "OK: $*"; }

[ -f "$HPP" ] || { echo "missing $HPP" >&2; exit 1; }
[ -f "$MAINC" ] || { echo "missing $MAINC" >&2; exit 1; }

# RED twin: member default 0 is the HEAD lie (log printed 40(default)).
if grep -nE 'int[[:space:]]+presentLeadMs_[[:space:]]*=[[:space:]]*0[[:space:]]*;' "$HPP"; then
  fail "RED: presentLeadMs_ default is 0 (must be 40)"
else
  ok "RED twin: lead default 0 rejected"
fi

# GREEN
if grep -nE 'int[[:space:]]+presentLeadMs_[[:space:]]*=[[:space:]]*40[[:space:]]*;' "$HPP"; then
  ok "GREEN: presentLeadMs_ = 40"
else
  fail "GREEN: presentLeadMs_ = 40 missing"
fi

# Empty-conf path must actually call setters (not log-only 40(default)/80(default)).
if grep -A6 'AV_PRESENT_LEAD_MS' "$MAINC" | grep -q 'setPresentLeadMs'; then
  if grep -nE 'setPresentLeadMs\((40|misterplex::kDefaultPresentLeadMs)\)' "$MAINC"; then
    ok "main.cpp applies present lead default when conf empty"
  else
    fail "main.cpp missing setPresentLeadMs(40|kDefaultPresentLeadMs) on empty conf"
  fi
else
  fail "main.cpp does not set present lead from AV_PRESENT_LEAD_MS"
fi

if grep -A6 'AV_RESYNC_DROP_MS' "$MAINC" | grep -q 'setResyncDropMs'; then
  if grep -nE 'setResyncDropMs\((80|misterplex::MediaPlayer::kDefaultResyncDropMs|kDefaultResyncDropMs)\)' "$MAINC"; then
    ok "main.cpp applies resync drop default when conf empty"
  else
    fail "main.cpp missing setResyncDropMs(kDefaultResyncDropMs) on empty conf"
  fi
else
  fail "main.cpp does not set resync drop from AV_RESYNC_DROP_MS"
fi

HELPER_HDR=""
if [ -f "$FAB" ]; then
  HELPER_HDR="$FAB"
elif [ -f "$CLK" ]; then
  HELPER_HDR="$CLK"
fi
if [ -n "$HELPER_HDR" ]; then
  if grep -q 'kDefaultPresentLeadMs = 40' "$HELPER_HDR"; then
    ok "kDefaultPresentLeadMs = 40 in $(basename "$HELPER_HDR")"
  else
    fail "kDefaultPresentLeadMs = 40 missing from $HELPER_HDR"
  fi
  if grep -q 'avResyncDropMsForPresent' "$HELPER_HDR"; then
    ok "avResyncDropMsForPresent named in $(basename "$HELPER_HDR")"
  else
    fail "avResyncDropMsForPresent missing from $HELPER_HDR"
  fi
else
  fail "no helper header (fabric_direct.hpp / av_clock.hpp)"
fi

mkdir -p "$ROOT/build"
BIN="$ROOT/build/test_cast_av_480p_policy"
SRC="$ROOT/tests/unit/test_cast_av_480p_policy.cpp"
if [ ! -f "$SRC" ]; then
  fail "missing $SRC"
else
  if ! "$CXX" -std=c++17 -Wall -Wextra -I"$ROOT/host" -o "$BIN" "$SRC"; then
    fail "compile $SRC"
  else
    if "$BIN"; then
      ok "helper binary OK"
    else
      fail "helper binary rc=$?"
    fi
  fi
fi

if [ "$fails" -ne 0 ]; then
  echo "test_cast_av_480p_policy: $fails failure(s)" >&2
  exit 1
fi
echo "test_cast_av_480p_policy: OK"
exit 0
