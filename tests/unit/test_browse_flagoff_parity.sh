#!/usr/bin/env bash
# Builds and runs test_browse_flagoff_parity.cpp: the shared headers MPX_BROWSE_V2 changed
# (library_browser.hpp, input_mailbox.hpp) against their copies at BASE (default 2ffd5c14 = 2164c4ad + w9 appendTags de8d3d17,
# coregate v2.3), renamed into namespace misterplex_base.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE=${BROWSE_PARITY_BASE:-2ffd5c14}
OUT=${BROWSE_PARITY_OUT:-$ROOT/build/browse_flagoff_parity}
mkdir -p "$OUT/base"
# BASE's own mailbox ABI spec: cb00c3d1 (offsets-only spec) removed the kPlx*Addr names BASE's input_mailbox.hpp
# uses. A plain copy would share namespace mailbox_abi with the tree's spec (redefinition, or BASE binding the
# tree's constants), so it becomes namespace mailbox_abi_base in its own file, and the BASE input_mailbox copy
# includes and names that one (T-PARITY-ABI, review 12:02).
git -C "$ROOT" show "$BASE:host/libmisterplex/mailbox_abi_spec.hpp" |
    sed 's/^namespace mailbox_abi {/namespace mailbox_abi_base {/; s/^} \/\/ namespace mailbox_abi$/} \/\/ namespace mailbox_abi_base/' \
    > "$OUT/base/mailbox_abi_spec_base.hpp"
grep -q '^namespace mailbox_abi_base {' "$OUT/base/mailbox_abi_spec_base.hpp"
grep -q '^} // namespace mailbox_abi_base$' "$OUT/base/mailbox_abi_spec_base.hpp"
for h in library_browser input_mailbox; do
    git -C "$ROOT" show "$BASE:host/libmisterplex/$h.hpp" |
        sed 's/^namespace misterplex {/namespace misterplex_base {/; s/^} \/\/ namespace misterplex$/} \/\/ namespace misterplex_base/;
             s/^#include "mailbox_abi_spec.hpp"$/#include "mailbox_abi_spec_base.hpp"/; s/\bmailbox_abi::/mailbox_abi_base::/g' \
        > "$OUT/base/${h}_base.hpp"
    grep -q '^namespace misterplex_base {' "$OUT/base/${h}_base.hpp"
done
grep -q '^#include "mailbox_abi_spec_base.hpp"$' "$OUT/base/input_mailbox_base.hpp"
if grep -q '\bmailbox_abi::' "$OUT/base/input_mailbox_base.hpp"; then
    echo "test_browse_flagoff_parity: BASE input_mailbox still names the tree's mailbox_abi" >&2
    exit 1
fi
${CXX:-g++} -std=c++17 -O2 -Wall -I"$ROOT/host" -I"$ROOT/host/libmisterplex" -I"$OUT/base" \
    -o "$OUT/test_browse_flagoff_parity" "$ROOT/tests/unit/test_browse_flagoff_parity.cpp"
"$OUT/test_browse_flagoff_parity"
