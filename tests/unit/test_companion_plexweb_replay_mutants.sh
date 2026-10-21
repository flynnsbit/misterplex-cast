#!/usr/bin/env bash
# Teeth for test_companion_plexweb_replay: each mutant of the cast-owner commandID floor
# (temp copy of companion.cpp, whole-line fixed-string edit, count == 1) must make the
# replay fail. Prints CAUGHT/MISSED per mutant; exit 1 if any is missed or invalid.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CXX="${CXX:-g++}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
SRC="$ROOT/arm/misterplexd/companion.cpp"
bad=0
mutant() { # name, anchor line, replacement line
  local name="$1" a="$2" r="$3" n
  cp "$SRC" "$T/companion.cpp"
  n=$(grep -cxF -- "$a" "$T/companion.cpp" || true)
  [[ "$n" == 1 ]] || { echo "MUTANT $name INVALID (anchor count $n)"; bad=1; return; }
  A="$a" R="$r" python3 - "$T/companion.cpp" <<'PY'
import os, sys
p = sys.argv[1]; a, r = os.environ["A"], os.environ["R"]
lines = open(p).read().split("\n")
open(p, "w").write("\n".join(r if l == a else l for l in lines))
PY
  cp "$ROOT"/arm/misterplexd/*.hpp "$T/"
  if ! "$CXX" -std=c++17 -O2 -I"$ROOT/host" -I"$T" -pthread -o "$T/replay" \
       "$ROOT/tests/unit/test_companion_plexweb_replay.cpp" "$T/companion.cpp" 2>"$T/build.err"; then
    echo "MUTANT $name INVALID (build failed)"; bad=1; return
  fi
  if "$T/replay" >"$T/out" 2>&1; then
    echo "MUTANT $name MISSED"; bad=1
  else
    echo "MUTANT $name CAUGHT ($(grep -m1 '^FAIL' "$T/out" | cut -c1-100))"
  fi
}
I='    '
mutant no_poll_floor "${I}if (commandId.empty() && castOwnerSet_) {" "${I}if (false && commandId.empty() && castOwnerSet_) {"
mutant floor_always "${I}if (commandId.empty() && castOwnerSet_) {" "${I}if (castOwnerSet_) {"
mutant no_push_floor "${I}if (own != \"0\" || !castOwnerSet_)" "${I}if (true)"
mutant no_owner_set "${I}${I}${I}${I}setCastOwner(requestControllerId, peerAddress);" "${I}${I}${I}${I}(void)0;"
mutant push_own_ignored "${I}if (own != \"0\" || !castOwnerSet_)" "${I}if (!castOwnerSet_)"
# Stale commandID across sessions (a lab MiSTer glass 1011): the stored value must be the latest, never a max.
mutant high_water "${I}${I}controller->commandId = commandId;" "${I}${I}if (commandIsNewerOrEqual(commandId, controller->commandId)) controller->commandId = commandId;"
mutant unsub_keeps_command "${I}${I}controllerCommands_.erase(" "${I}${I}if (false) controllerCommands_.erase("
exit $bad
