#!/usr/bin/env bash
# Plex loaded = unchanged, at the hardware-write level.
#
# Builds test_core_gate.cpp's open battery (idle paint into the bank + doorbell,
# input mailbox poll, a status bit over SPI, a ring flush) twice against a fake
# /dev/mem and the same fake Plex core (fake_plex_core.hpp: SPI handshake and AU-ring
# replies): once with this tree's fpga_spi.cpp and once with the base tree's
# fpga_spi.cpp (default dbcc42fa, the prod daemon), only its "/dev/mem" opens pointed
# at the fake file. This tree runs the daemon's managed gate, opened the way the daemon
# opens it (settle, then one CAPS probe answered with 0x48005D45), before the sentinel
# fill and the battery. Each prints a hash of every DDR byte and FPGA-manager register
# byte written plus the number of SPI strobes the fake FPGA answered. Both must match,
# and each must match itself on a second run.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE_REF=${CORE_GATE_BASE_REF:-dbcc42fa}
OUT=$ROOT/build/core_gate_base_diff
mkdir -p "$OUT"
CXX=${CXX:-g++}

git -C "$ROOT" show "$BASE_REF:arm/misterplexd/fpga_spi.hpp" > "$OUT/fpga_spi.hpp"
git -C "$ROOT" show "$BASE_REF:arm/misterplexd/fpga_spi.cpp" |
    sed 's#::open("/dev/mem", #::open(MPX_DEVMEM_PATH, #' > "$OUT/fpga_spi.cpp"
opens=$(grep -c 'MPX_DEVMEM_PATH' "$OUT/fpga_spi.cpp")
if [ "$opens" != 4 ]; then
    echo "FAIL: expected 4 /dev/mem opens in $BASE_REF fpga_spi.cpp, patched $opens" >&2
    exit 1
fi

"$CXX" -std=c++17 -O2 -I"$OUT" -I"$ROOT/arm/misterplexd" -I"$ROOT/host" -pthread \
    -include "$ROOT/tests/unit/core_gate_base_legacy_names.hpp" \
    -DMPX_CORE_GATE_HASH_ONLY -DMPX_DEVMEM_PATH="\"$OUT/base.fakemem\"" \
    -o "$OUT/base_hash" "$ROOT/tests/unit/test_core_gate.cpp" "$OUT/fpga_spi.cpp"
"$CXX" -std=c++17 -O2 -I"$ROOT/arm/misterplexd" -I"$ROOT/host" -pthread \
    -DMPX_DEVMEM_PATH="\"$OUT/gate.fakemem\"" \
    -o "$OUT/gate_hash" "$ROOT/tests/unit/test_core_gate.cpp" "$ROOT/arm/misterplexd/fpga_spi.cpp"

b1=$("$OUT/base_hash" | grep '^battery_hash=')
b2=$("$OUT/base_hash" | grep '^battery_hash=')
g1=$("$OUT/gate_hash" --battery-hash | grep '^battery_hash=')
g2=$("$OUT/gate_hash" --battery-hash | grep '^battery_hash=')
echo "core gate base diff: $BASE_REF [$b1] [$b2], this tree [$g1] [$g2]"
if [ -z "$b1" ] || [ "$b1" != "$b2" ] || [ "$g1" != "$g2" ] || [ "$b1" != "$g1" ]; then
    echo "FAIL: Plex-loaded hardware writes or SPI strobe count differ from $BASE_REF" >&2
    exit 1
fi
echo "test_core_gate_base_diff: OK (Plex loaded: same bytes written and same SPI strobes as $BASE_REF)"
