#!/usr/bin/env bash
# The rc3 legacy-core prefix has one value. boot.sh, the conf template, and the
# lab env must all assign kLegacyCorePrefix. A second assignment (the retired
# prod RBF 7cbae324, or any other 8 hex digits) fails this test.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HDR=$ROOT/host/libmisterplex/legacy_core_prefix.hpp
IDS=$ROOT/host/libmisterplex/fpga_build_ids.hpp
MAIN=$ROOT/arm/misterplexd/main.cpp
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

PREFIX=$(sed -n 's/.*kLegacyCorePrefix\[\] = "\([0-9a-f][0-9a-f]*\)".*/\1/p' "$HDR")
[ "$PREFIX" = "999800cf" ] || fail "kLegacyCorePrefix is '$PREFIX', want 999800cf (RC 47cb9fb4 / 0x48004D40)"
case "$PREFIX" in
  ????????) ;;
  *) fail "kLegacyCorePrefix '$PREFIX' is not 8 hex digits" ;;
esac

# The constant is the RBF named on the RC row, not the prod F36A row.
rc_line=$(grep -F '0x48004D40u' "$IDS" || true)
echo "$rc_line" | grep -F "$PREFIX" >/dev/null || fail "0x48004D40 row does not name RBF $PREFIX"
echo "$rc_line" | grep -F '7cbae324' >/dev/null && fail "0x48004D40 row names retired prod RBF 7cbae324"
awk '
  /0x4800F36Au/ { n = 3 }
  n && /7cbae324/ { found = 1 }
  n { n--; if (n == 0) exit found ? 0 : 1 }
  END { if (!found) exit 1 }
' "$IDS" || fail "0x4800F36A row no longer names prod RBF 7cbae324"

# Conf must not be able to overwrite the pin. The key stays out of the conf-wins list.
python3 - "$MAIN" <<'PY' || fail "MPX_LEGACY_CORE_PREFIX is back in the conf-wins key list"
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
m = re.search(r'for \(const char\* key : \{(.*?)\}\)', text, re.S)
if not m or "MPX_LEGACY_CORE_PREFIX" in m.group(1):
    raise SystemExit(1)
PY
grep -q 'setenv("MPX_LEGACY_CORE_PREFIX", misterplex::kLegacyCorePrefix, 1)' "$MAIN" || \
  fail "main.cpp does not pin MPX_LEGACY_CORE_PREFIX to kLegacyCorePrefix"
grep -q 'legacy_core_prefix.hpp' "$MAIN" || fail "main.cpp does not include legacy_core_prefix.hpp"

assign='MPX_LEGACY_CORE_PREFIX='
for f in \
  scripts/misterplexd-boot.sh \
  scripts/misterplexd-boot-mast.sh \
  assets/misterplex.conf.example \
  tools/gcast/lab/gc-common.sh \
  tests/unit/test_deploy_prodfix_proxy_off.sh
do
  hits=$(grep -oE "${assign}[0-9a-f]+" "$ROOT/$f" | sort -u)
  [ -n "$hits" ] || fail "$f has no ${assign} assignment"
  while IFS= read -r h; do
    [ "$h" = "${assign}${PREFIX}" ] || fail "$f assigns $h, want ${assign}${PREFIX}"
  done <<<"$hits"
done

# The two boot scripts and the conf template are the pair that disagreed on device.
for f in scripts/misterplexd-boot.sh scripts/misterplexd-boot-mast.sh assets/misterplex.conf.example; do
  grep -q "${assign}${PREFIX}" "$ROOT/$f" || fail "$f missing ${assign}${PREFIX}"
done

if [ "$fails" = 0 ]; then
  echo "test_legacy_core_prefix: PASS ($PREFIX)"
else
  echo "test_legacy_core_prefix: FAIL ($fails)"
  exit 1
fi
