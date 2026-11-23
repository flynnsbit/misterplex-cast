#!/usr/bin/env bash
# Guard for glass #23 (ARM_PLEXD_AIRPLAY=1 binary 8f12684a SIGSEGV on start).
# Static deps (openssl/libplist/UxPlay/libav) built with glibc>=2.38 headers carry __isoc23_strto*
# references and 64-bit time_t. Linking them into a daemon built by an older ARM toolchain (gcc 10.2, glibc 2.31)
# only "works" if an isoc23 shim hides the missing symbols; the ABI mismatch then crashes at start.
# Usage: check_arm_toolchain_consistency.sh <ARM_CC> <prefix-with-lib/*.a> [more prefixes...]
# Env: GUARD_LIBC_A overrides the libc.a to test against (unit test hook).
set -u
CC=${1:?ARM_CC (arm-none-linux-gnueabihf-gcc)}; shift
NM=${CC%gcc}nm; fail=0
LIBC=${GUARD_LIBC_A:-$("$CC" -print-file-name=libc.a)}
[ -f "$LIBC" ] || { echo "guard: libc.a not found for $CC"; exit 2; }
libc_has_isoc23=0
"$NM" "$LIBC" 2>/dev/null | grep -q ' T __isoc23_strtol$' && libc_has_isoc23=1
want=$("$CC" -dumpfullversion 2>/dev/null || "$CC" -dumpversion)
echo "guard: toolchain gcc=$want libc.a isoc23=$libc_has_isoc23"
for p in "$@"; do
  for a in "$p"/lib/*.a; do
    [ -f "$a" ] || continue
    if [ $libc_has_isoc23 -eq 0 ] && "$NM" "$a" 2>/dev/null | grep -q ' U __isoc23_'; then
      echo "FAIL $a needs __isoc23_* but this toolchain's libc lacks it (built with newer glibc headers)"
      fail=1
    fi
    for v in $(strings -a "$a" | grep -o 'GCC: ([^)]*[^)]*) [0-9][0-9.]*' | grep -o '[0-9][0-9.]*$' | sort -u); do
      [ "${v%%.*}" = "${want%%.*}" ] || { echo "FAIL $a built by gcc $v, toolchain is $want"; fail=1; }
    done
  done
done
[ $fail -eq 0 ] && echo "guard: OK (single consistent toolchain)"
exit $fail
