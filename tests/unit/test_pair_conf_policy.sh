#!/usr/bin/env bash
# Pair conf overlay: gold-daemon cores get Star Trek A/V knobs; L4 does not
# inherit the 480p transcode profile.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LIB="$ROOT/scripts/misterplex_pair_conf.sh"
# shellcheck disable=SC1090
. "$LIB"
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails+1)); }
ok() { echo "OK: $*"; }

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
printf 'PLEX_BASE=http://127.0.0.1:32400\nPLEX_TOKEN=keepme\nOSD_CONTROL=1\n' >"$tmp"

misterplex_apply_pair_conf "07f54d9f8f0eda2fe75d9cc314f6de54" "$tmp"
grep -q 'PLEX_BASE=http://127.0.0.1:32400' "$tmp" || fail "clobbered PLEX_BASE"
grep -q 'PLEX_TOKEN=keepme' "$tmp" || fail "clobbered PLEX_TOKEN"
grep -q 'AV_PRESENT_LEAD_MS=40' "$tmp" || fail "480p missing lead 40"
grep -q 'AV_RESYNC_DROP_MS=0' "$tmp" || fail "480p missing drop 0"
grep -q 'FFMPEG_SWS_FLAGS=fast_bilinear' "$tmp" || fail "480p missing fast_bilinear"
grep -q 'FFMPEG_FPS_FILTER=off' "$tmp" || fail "480p missing fps filter off"
grep -q 'OSD_CONTROL=0' "$tmp" || fail "480p should pin OSD_CONTROL=0"
grep -q 'TRANSCODE_PROFILE=480p' "$tmp" || fail "480p profile"
ok "480p gold overlay"

misterplex_apply_pair_conf "03f1b95ac67a568f24193234ea8cb072" "$tmp"
grep -q 'PLEX_BASE=http://127.0.0.1:32400' "$tmp" || fail "L4 clobbered PLEX_BASE"
grep -q 'DECODE=1280x720' "$tmp" || fail "L4 decode bank"
grep -q 'OSD_CONTROL=0' "$tmp" || fail "L4 OSD_CONTROL=0 (leftover F12 480p must not retarget)"
grep -q 'IDLE_SCREEN=logo' "$tmp" || fail "L4 IDLE_SCREEN=logo"
grep -q 'AV_PRESENT_LEAD_MS=40' "$tmp" || fail "L4 lead"
grep -q 'TRANSCODE_PROFILE=720p' "$tmp" || fail "L4 TRANSCODE_PROFILE must be 720p"
grep -q 'DECODE=1280x720' "$tmp" || fail "L4 DECODE=1280x720"
grep -q 'WEAK_BITRATE=8000' "$tmp" || fail "L4 WEAK_BITRATE=8000"
grep -q 'WEAK_QUALITY=40' "$tmp" || fail "L4 WEAK_QUALITY=40"
grep -q 'WEAK_H264_PROFILE=baseline' "$tmp" || fail "L4 baseline"
grep -q 'AV_RESYNC_DROP_MS=0' "$tmp" || fail "L4 hold-only drop 0"
if grep -q 'TRANSCODE_PROFILE=480p' "$tmp"; then
  fail "L4 must not inherit 480p transcode profile"
else
  ok "L4 does not inherit 480p profile"

misterplex_apply_pair_conf "17aa9a7e864555226d3ac313bc50177c" "$tmp"
grep -q 'DECODE=1280x720' "$tmp" || fail "l4-dyn4 17aa9a7e must be L4 720p keys"
grep -q 'TRANSCODE_PROFILE=720p' "$tmp" || fail "l4-dyn4 TRANSCODE_PROFILE=720p"
ok "l4-dyn4 md5 listed as L4"

misterplex_apply_pair_conf "e7097c6c71bfbbf137cb3baf42cb929a" "$tmp"
grep -q 'DECODE=1280x720' "$tmp" || fail "l4-dyn5 e7097c6c must be L4 720p keys"
ok "l4-dyn5 md5 listed as L4"

misterplex_apply_pair_conf "9eacd6d26d9cb2a5904b7b923a5efa19" "$tmp"
grep -q 'DECODE=1280x720' "$tmp" || fail "l4-dyn10 9eacd6d2 must be L4 720p keys"
grep -q 'TRANSCODE_PROFILE=720p' "$tmp" || fail "l4-dyn10 TRANSCODE_PROFILE=720p"
ok "l4-dyn10 md5 listed as L4"

misterplex_apply_pair_conf "0eea3580a5a0dacf60bf65c50499bcc9" "$tmp"
grep -q 'DECODE=1280x720' "$tmp" || fail "live 0eea3580 must be L4 720p keys"
grep -q 'TRANSCODE_PROFILE=720p' "$tmp" || fail "live 0eea3580 TRANSCODE_PROFILE=720p"
if grep -q 'TRANSCODE_PROFILE=480p' "$tmp"; then
  fail "live 0eea3580 must not inherit 480p transcode profile"
fi
ok "live 0eea3580 does not inherit 480p profile"
fi

misterplex_apply_pair_conf "4d6efef954acf7b33747f35ac2878c1b" "$tmp"
grep -q 'TRANSCODE_PROFILE=240p' "$tmp" || fail "240p15 profile"
grep -q 'DECODE=320x240' "$tmp" || fail "240p15 decode"
grep -q 'OSD_CONTROL=0' "$tmp" || fail "240p15 OSD_CONTROL=0"
ok "240p15 overlay"

misterplex_apply_pair_conf "61db00e7d54efad7c1a456b127b798bd" "$tmp"
grep -q 'TRANSCODE_PROFILE=480p' "$tmp" || fail "480i profile"
grep -q 'FFMPEG_SWS_FLAGS=fast_bilinear' "$tmp" || fail "480i sws"
ok "480i overlay"

# Unknown RBF md5 currently falls through to 480p DECODE=640x480. A new L4
# BUILD_OK must be listed next to 03f1b95a before first play (else 720p24
# HDMI + 480p decode). Twin: this md5 is NOT a live L4 key.
misterplex_apply_pair_conf "2372248aed5c8b65fbe89ec4629b89e3" "$tmp"
grep -q 'DECODE=640x480' "$tmp" || fail "unknown md5 must fall through to 480p until listed"
if grep -q 'DECODE=1280x720' "$tmp"; then
  fail "unlisted l4-dyn1 md5 2372248a must not get L4 keys (STA-red, 20 MHz PLL)"
fi
ok "unknown md5 is 480p fallthrough (list new L4 md5 before play)"

grep -q 'misterplex_pair_conf.sh' "$ROOT/scripts/misterplex_core_watch.sh" || \
  fail "watch does not source pair_conf"
grep -q 'misterplex_apply_pair_conf' "$ROOT/scripts/misterplex_core_watch.sh" || \
  fail "watch does not apply pair_conf"
# 28cb5a75 must pin L4 from pairs col3 — hashing only 03f1b95a as L4
# GLASS-MAX'd live 720p24 to bank=640x480.
WATCH="$ROOT/scripts/misterplex_core_watch.sh"
NAMED="$ROOT/scripts/misterplex_named_rbf.sh"
if grep -A8 'select_bin_for_live_rbf' "$WATCH" | grep -q '03f1b95a.*then'; then
  fail "watch must not pin L4 from the 03f1b95a md5 only"
fi
grep -q 'glass_col' "$WATCH" || fail "watch must pin live-glass from pairs col3"
grep -q '0x3047F12C' "$NAMED" || fail "named_rbf must sample L4 PLXD (not leftover 480p PLXS first)"
grep -q '504C584A' "$NAMED" || fail "named_rbf must treat PLXJ as L4-only leftover discriminator"
ok "watch pins L4 from pairs col3; named_rbf prefers live L4 PLXD/PLXJ"

if [ "$fails" -ne 0 ]; then
  echo "test_pair_conf_policy: $fails failure(s)" >&2
  exit 1
fi
echo "test_pair_conf_policy: OK"
exit 0
