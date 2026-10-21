#!/usr/bin/env bash
# PROXY=off in deploy-prodfix.sh (lead a lab MiSTer B2/C2 1011: dropping MPX_REMOTE_LADDER_PROXY from the env let
# the daemon fall back to misterplex.conf's REMOTE_LADDER_PROXY line). Checks:
#   X1 proxy_off_env sets MPX_REMOTE_LADDER_PROXY= (empty), drops any REMOTE_LADDER_PROXY, keeps the rest.
#   X2 the daemon's conf fallback runs only when the env var is unset (main.cpp rule unchanged).
#   X3 ladderProxyConfigured() is false with MPX_REMOTE_LADDER_PROXY= empty, even with the conf value
#      exported the way the daemon would (REMOTE_LADDER_PROXY unset), and true with it set.
#   X4 the script refuses DEPLOY_OK unless the live environ shows the empty var.
# MUT=unset (the B2 script: remove the var) must FAIL at X1.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
S=$ROOT/scripts/deploy/prodfix-1011/deploy-prodfix.sh
fails=0; fail(){ echo "FAIL $*"; fails=$((fails+1)); }
PRODFIX_LIB_ONLY=1 . "$S"
if [ "${MUT:-}" = unset ]; then proxy_off_env(){ echo "$1" | tr ' ' '\n' | grep -v '^MPX_REMOTE_LADDER_PROXY=' | tr '\n' ' ' | sed 's/ *$//'; }; fi
ENV0="MPX_LEGACY_CORE_PREFIX=999800cf MPX_REMOTE_LADDER_PROXY=http://192.0.2.21:18767 MPX_D52_LOG=1 MPX_IDLEFB_PATH=devmem MPX_BROWSE_V2=1 MPX_BROWSE_MVPB=1 MPX_MENU_URL=http://192.0.2.21:18769/menu/a1"
OUT=$(proxy_off_env "$ENV0")
echo "$OUT" | tr ' ' '\n' | grep -qx 'MPX_REMOTE_LADDER_PROXY=' || fail "X1 no empty MPX_REMOTE_LADDER_PROXY= in [$OUT]"
echo "$OUT" | tr ' ' '\n' | grep -q 'LADDER_PROXY=http' && fail "X1 a proxy URL survived in [$OUT]"
for w in MPX_LEGACY_CORE_PREFIX=999800cf MPX_D52_LOG=1 MPX_IDLEFB_PATH=devmem MPX_BROWSE_V2=1 MPX_BROWSE_MVPB=1 MPX_MENU_URL=http://192.0.2.21:18769/menu/a1; do
  echo "$OUT" | tr ' ' '\n' | grep -qxF "$w" || fail "X1 lost $w"
done
OUT2=$(proxy_off_env "$ENV0 REMOTE_LADDER_PROXY=http://x:1")
echo "$OUT2" | grep -q 'REMOTE_LADDER_PROXY=http' && fail "X1 REMOTE_LADDER_PROXY kept"
grep -q 'if (!std::getenv("MPX_REMOTE_LADDER_PROXY")) {' "$ROOT/arm/misterplexd/main.cpp" || fail "X2 main.cpp conf fallback rule changed (env-unset only)"
T=$(mktemp -d); cat > $T/t.cpp <<'CPP'
#include "plex_resolve.hpp"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    (void)argc;
    if (argv[1][0] == 'e') setenv("MPX_REMOTE_LADDER_PROXY", "", 1);
    else setenv("MPX_REMOTE_LADDER_PROXY", "http://192.0.2.21:18765", 1);
    unsetenv("REMOTE_LADDER_PROXY");
    std::printf("%d\n", misterplex::ladderProxyConfigured() ? 1 : 0);
    return 0;
}
CPP
if g++ -std=c++17 -O0 -I$ROOT/arm/misterplexd -I$ROOT/host -I$ROOT -pthread -o $T/t $T/t.cpp $ROOT/arm/misterplexd/plex_resolve.cpp 2>$T/err; then
  [ "$($T/t empty)" = 0 ] || fail "X3 empty env still configures a proxy"
  [ "$($T/t set)" = 1 ] || fail "X3 set env does not configure the proxy"
else fail "X3 build: $(head -3 $T/err)"; fi
rm -rf $T
grep -q 'env=\[MPX_REMOTE_LADDER_PROXY=\] conf_proxy_lines=0' "$S" || fail "X4 no live-environ check"
[ $fails = 0 ] && echo "test_deploy_prodfix_proxy_off: PASS" || { echo "test_deploy_prodfix_proxy_off: FAIL ($fails)"; exit 1; }
