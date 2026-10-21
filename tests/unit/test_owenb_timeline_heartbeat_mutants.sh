#!/usr/bin/env bash
# BUG2-1017 teeth: each mutant must make test_owenb_timeline_heartbeat fail.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CXX="${CXX:-g++}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
bad=0

tree_copy() {
  local dest="$1"
  rm -rf "$dest"
  mkdir -p "$dest"
  rsync -a --delete --exclude='.git' --exclude='build/' "$ROOT/" "$dest/"
}

build_test() {
  local tree="$1" out="$2"
  make -C "$tree" -j"$(nproc)" "$tree/build/test_owenb_timeline_heartbeat" \
    >"$T/build.log" 2>&1 || return 1
  cp "$tree/build/test_owenb_timeline_heartbeat" "$out"
}

mutant_line() {
  local name="$1" file="$2" anchor="$3" repl="$4"
  local tree="$T/tree_$name"
  tree_copy "$tree"
  local n
  n=$(grep -cxF -- "$anchor" "$tree/$file" || true)
  if [[ "$n" != 1 ]]; then
    echo "MUTANT $name INVALID (anchor count $n)"
    bad=1
    return
  fi
  A="$anchor" R="$repl" python3 - "$tree/$file" <<'PY'
import os, sys
p = sys.argv[1]
a, r = os.environ["A"], os.environ["R"]
lines = open(p).read().split("\n")
open(p, "w").write("\n".join(r if l == a else l for l in lines))
PY
  if ! build_test "$tree" "$T/bin_$name"; then
    echo "MUTANT $name INVALID (build failed)"
    bad=1
    return
  fi
  if "$T/bin_$name" >"$T/out_$name" 2>&1; then
    echo "MUTANT $name MISSED"
    bad=1
  else
    echo "MUTANT $name CAUGHT ($(grep -m1 '^FAIL' "$T/out_$name" | cut -c1-100))"
  fi
}

mutant_block() {
  local name="$1" file="$2"
  local tree="$T/tree_$name"
  tree_copy "$tree"
  python3 - "$tree/$file" "$3" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
mode = sys.argv[2]
text = p.read_text()
if mode == "M3":
    old = """    // BUG2-1017: OB timeline heartbeat (PMS /:/timeline + companion poll time)
    cb.progress = [this, epoch](int64_t ms, bool) {
        if (playEpoch_.load() != epoch)
            return;
        positionMs_.store(ms);
        companionTimelineTick();
    };
"""
    if text.count(old) != 1:
        sys.exit(2)
    p.write_text(text.replace(old, "", 1))
elif mode == "M4":
    start = "        if (cb.progress && session_ && session_->started()) {"
    if text.count(start) != 1:
        sys.exit(2)
    i = text.index(start)
    j = text.index("        if (cb.position && session_)", i)
    p.write_text(text[:i] + text[j:])
elif mode == "M5":
    old = """        if (playEpoch_.load() != epoch)
            return;
        positionMs_.store(ms);
        companionTimelineTick();"""
    new = """        positionMs_.store(ms);
        companionTimelineTick();"""
    if text.count(old) != 1:
        sys.exit(2)
    p.write_text(text.replace(old, new, 1))
elif mode == "M7":
    old = """        if (cb.ended)
            cb.ended(why, s->error(), s->positionMs());"""
    new = """        if (cb.progress)
            cb.progress(s->positionMs(), s->paused());
        if (cb.ended)
            cb.ended(why, s->error(), s->positionMs());"""
    if text.count(old) != 1:
        sys.exit(2)
    p.write_text(text.replace(old, new, 1))
else:
    sys.exit(2)
PY
  if [[ $? -ne 0 ]]; then
    echo "MUTANT $name INVALID"
    bad=1
    return
  fi
  if ! build_test "$tree" "$T/bin_$name"; then
    echo "MUTANT $name INVALID (build failed)"
    bad=1
    return
  fi
  if "$T/bin_$name" >"$T/out_$name" 2>&1; then
    echo "MUTANT $name MISSED"
    bad=1
  else
    echo "MUTANT $name CAUGHT ($(grep -m1 '^FAIL' "$T/out_$name" | cut -c1-100))"
  fi
}

mutant_line M1_no_cadence_gate host/libmisterplex/owenb_play.cpp \
  '                now - *lastProgress >= std::chrono::milliseconds(1000)) {' \
  '                now - *lastProgress >= std::chrono::milliseconds(0)) {'

mutant_line M2_before_started host/libmisterplex/owenb_play.cpp \
  '        if (cb.progress && session_ && session_->started()) {' \
  '        if (cb.progress && session_) {'

mutant_line M6_paused_false host/libmisterplex/owenb_play.cpp \
  '                cb.progress(session_->positionMs(), session_->paused());' \
  '                cb.progress(session_->positionMs(), false);'

mutant_block M3_no_media_wiring arm/misterplexd/media_player.cpp M3
mutant_block M4_no_progress_hook host/libmisterplex/owenb_play.cpp M4
mutant_block M5_no_epoch_guard arm/misterplexd/media_player.cpp M5
mutant_block M7_progress_after_stop host/libmisterplex/owenb_play.cpp M7

exit $bad
