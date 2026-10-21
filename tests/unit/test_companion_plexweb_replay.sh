#!/usr/bin/env bash
# Plex Web 4.162.1 cast-poller replay against the companion httpLoop (host build, loopback).
# See tests/unit/test_companion_plexweb_replay.cpp for the modelled client rules and rows.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CXX="${CXX:-g++}"
mkdir -p "$ROOT/build"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$ROOT/host" -I"$ROOT/arm/misterplexd" -pthread \
  -o "$ROOT/build/test_companion_plexweb_replay" \
  "$ROOT/tests/unit/test_companion_plexweb_replay.cpp" "$ROOT/arm/misterplexd/companion.cpp"
exec "$ROOT/build/test_companion_plexweb_replay"
