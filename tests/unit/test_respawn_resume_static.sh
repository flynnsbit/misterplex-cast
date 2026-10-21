#!/usr/bin/env bash
# Static regression: respawn-resume v3 (review-269 BLOCK).
#
# lastPosMs_ is a MediaPlayer session field updated from the player clock and
# zeroed on user stop(). resumeAfterRespawn uses lastPosMs_ and the seek-safe
# URL rewrite. Companion holds resumeAfterRespawn_ as a session field applied
# through scrubTargetMs_. User stop and ordinary playMedia?offset=0 must not
# seek from a process-global /tmp last-pos file.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MP="$ROOT/arm/misterplexd/media_player.cpp"
MH="$ROOT/arm/misterplexd/media_player.hpp"
CC="$ROOT/arm/misterplexd/companion.cpp"
CH="$ROOT/arm/misterplexd/companion.hpp"
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "OK $*"; }

[[ -f "$MP" && -f "$MH" && -f "$CC" && -f "$CH" ]] || fail "missing media_player/companion sources"

# --- Phase 1/2: lastPosMs_ clock; resumeAfterRespawn uses it + seek-safe URL ---
grep -qE 'std::atomic<int64_t>[[:space:]]+lastPosMs_\{0\}' "$MH" \
  || fail "media_player.hpp missing std::atomic<int64_t> lastPosMs_{0}"
pass "lastPosMs_{0} declared in media_player.hpp"

grep -q 'lastPosMs() const' "$MH" \
  || fail "media_player.hpp missing lastPosMs() const"
pass "lastPosMs() const declared in media_player.hpp"

store_count="$(grep -c 'lastPosMs_\.store' "$MP" || true)"
[[ "$store_count" -ge 8 ]] || fail "media_player.cpp lastPosMs_.store count=$store_count want>=8"
pass "lastPosMs_.store appears $store_count times in media_player.cpp (>=8 clock sites)"

grep -q 'lastPosMs_.store(0)' "$MP" \
  || fail "media_player.cpp stop() must zero lastPosMs_ (user stop must not auto-resume)"
pass "lastPosMs_.store(0) present (user-stop path)"

grep -q 'resumeAfterRespawn' "$MH" \
  || fail "media_player.hpp missing resumeAfterRespawn"
grep -q 'resumeAfterRespawn' "$MP" \
  || fail "media_player.cpp missing resumeAfterRespawn"
pass "resumeAfterRespawn appears in media_player.hpp and media_player.cpp"

grep -q 'withSeekSafeTranscodeParams(urlOrPath, lastStoppedMs)' "$MP" \
  || fail "media_player.cpp resumeAfterRespawn must wrap URL with withSeekSafeTranscodeParams(urlOrPath, lastStoppedMs)"
pass "withSeekSafeTranscodeParams(urlOrPath, lastStoppedMs) present in media_player.cpp"

if grep -q 'MediaPlayer::respawnResume' "$MP"; then
  fail "media_player.cpp must not define undeclared MediaPlayer::respawnResume"
fi
pass "MediaPlayer::respawnResume absent from media_player.cpp"

if grep -q 'MediaPlayer::resumeOnRespawn' "$MP"; then
  fail "media_player.cpp must not define undeclared MediaPlayer::resumeOnRespawn"
fi
pass "MediaPlayer::resumeOnRespawn absent from media_player.cpp"

if grep -q 'misterplex_last_pos_ms' "$MP" "$MH"; then
  fail "media_player.cpp/hpp must not use /tmp/misterplex_last_pos_ms"
fi
pass "misterplex_last_pos_ms absent from media_player.cpp and media_player.hpp"

# --- Phase 3/4: session resumeAfterRespawn_; no global /tmp / offset=0 hijack ---
grep -q 'resumeAfterRespawn_{0}' "$CH" \
  || fail "companion.hpp missing resumeAfterRespawn_{0} session field"
pass "resumeAfterRespawn_{0} declared in companion.hpp"

grep -q 'armResumeAfterRespawn' "$CH" \
  || fail "companion.hpp missing armResumeAfterRespawn"
pass "armResumeAfterRespawn declared in companion.hpp"

grep -q 'if (resumeAfterRespawn_ > 0)' "$CC" \
  || fail "companion.cpp must apply resumeAfterRespawn_ through the playMedia scrub plant"
pass "if (resumeAfterRespawn_ > 0) present in companion.cpp"

grep -q 'resumeAfterRespawn_ = 0' "$CC" \
  || fail "companion.cpp must clear resumeAfterRespawn_ (clearMedia / consume)"
pass "resumeAfterRespawn_ = 0 present in companion.cpp"

if grep -q 'g_resumeAfterRespawn_' "$CC" "$CH"; then
  fail "companion.cpp/hpp must not use process-global g_resumeAfterRespawn_"
fi
pass "g_resumeAfterRespawn_ absent from companion.cpp and companion.hpp"

if grep -q 'misterplex_last_pos_ms' "$CC" "$CH" "$MP" "$MH"; then
  fail "sources must not read/write /tmp/misterplex_last_pos_ms"
fi
pass "misterplex_last_pos_ms absent from companion and media_player"

if grep -q 'if (saved > 0 && off == 0)' "$CC"; then
  fail "companion.cpp must not hijack every offset=0 playMedia via saved last-pos"
fi
pass "if (saved > 0 && off == 0) absent from companion.cpp"

echo "test_respawn_resume_static: OK"
exit 0
