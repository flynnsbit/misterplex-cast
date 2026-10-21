#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$ROOT/build/audio-session-compiler"
export TMPDIR="$ROOT/build/audio-session-compiler"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -pthread \
  -I"$ROOT/host" -I"$ROOT/arm/misterplexd" -DMPX_CORE_GATE_START_OPEN \
  "$ROOT/tests/unit/test_audio_session_host.cpp" "$ROOT/arm/misterplexd/fpga_spi.cpp" \
  -Wl,--wrap=open -Wl,--wrap=read -Wl,--wrap=close \
  -o "$ROOT/build/test_audio_session_host"
"$ROOT/build/test_audio_session_host"
