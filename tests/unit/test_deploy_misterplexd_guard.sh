#!/usr/bin/env bash
# T-DEPLOYD-ARGS (review 19:41/19:52/20:07, H264 19:53/20:05, review 19:41):
# scripts/deploy_misterplexd.sh must refuse (rc 2, a message, and NO external command):
# - any argument;
# - an unset, empty or non-canonical MISTER_HOST: anything but an ASCII IPv4 dotted quad, e.g.
#   user@, a trailing dot, whitespace, a newline, leading zeros, a numeric alias, non-ASCII
#   digits, a hostname.
# Every row runs under LC_ALL=C AND LC_ALL=en_US.UTF-8, where a bash [[ =~ ]] range such as
# [0-9] also matches non-ASCII digits. make/ssh/scp and the other transport tools are PATH stubs
# that log and exit 97, so no row can reach a host; the named host is the unroutable TEST-NET
# 192.0.2.1. A control row (that host, no arguments) shows the guards are not vacuous: it
# reaches the stubbed make, and never ssh.
# Teeth: each guard is replaced in a temp copy (the edited text must occur exactly once) and
# the rows aimed at it must then FAIL.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/scripts/deploy_misterplexd.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
for t in make sshpass ssh scp sftp rsync wget curl; do
  printf '#!/bin/sh\nprintf "%%s %%s\\n" "%s" "$*" >> "$STUB_LOG"\nexit 97\n' "$t" >"$T/bin/$t"
  chmod +x "$T/bin/$t"
done

checks=0
fails=0
fail() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }
UTF8=en_US.UTF-8
LOCALES=(C "$UTF8")
FW2=$'\xef\xbc\x92'   # U+FF12 FULLWIDTH DIGIT TWO
SUP2=$'\xc2\xb2'      # U+00B2 SUPERSCRIPT TWO

# P0, a FAIL and never a skip: the UTF-8 locale exists, and the RANGE form of the octet
# pattern accepts a non-ASCII digit there, so R14/R14b/R15 and M6 test a real hazard.
checks=$((checks + 1))
if ! locale -a 2>/dev/null | grep -qix 'en_US\.utf-\{0,1\}8'; then
  fail "P0: locale $UTF8 is not installed, so the C-2 rows cannot show their teeth here"
elif ! LC_ALL=$UTF8 bash -c 'o="(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])"
    re="^$o\\.$o\\.$o\\.$o\$"; [[ $1 =~ $re ]]' _ "192.0.2.4$FW2"; then
  fail "P0: under $UTF8 the range form rejects 192.0.2.4<U+FF12>, so the hazard does not reproduce here"
fi

R_NAME=()
R_MSG=()
R_HOST=()
R_ARGS=()
row() { R_NAME+=("$1"); R_MSG+=("$2"); R_HOST+=("$3"); R_ARGS+=("${4:-}"); }
row R1 'takes no arguments' 192.0.2.1 '--prebuilt-candidate'
row R1b 'takes no arguments' 192.0.2.1 '--prebuilt-candidate --manifest /nonexistent --mode menu'
row R2 'MISTER_HOST is unset or empty' -unset-
row R2b 'MISTER_HOST is unset or empty' ''
# C-1: any non-canonical host. Examples are TEST-NET and the 10.0.0 test range.
row R5 'is a malformed host' root@192.0.2.1
row R6 'is a malformed host' 192.0.2.1.
row R7 'is a malformed host' ' 192.0.2.1'
row R8 'is a malformed host' '192.0.2.1 '
row R9 'is a malformed host' 192.0.2.042
row R10 'is a malformed host' mister.local
# Numeric aliases are not canonical dotted quads. 3221225985 and 0xc0000201 are 192.0.2.1.
row R11 'is a malformed host' 3221225985
row R12 'is a malformed host' 0xc0000201
row R13 'is a malformed host' 192.0.2.0x1
# C-2 (H264/review 20:07): non-ASCII digits, and an embedded newline.
row R14 'is a malformed host' "192.0.2.4$FW2"
row R14b 'is a malformed host' "192.0.2.4$SUP2"
row R15 'is a malformed host' "1$SUP2.0.0.4"
row R16 'is a malformed host' $'192.0.2.1\n192.0.2.4'

# run_case <script> <locale> <host|-unset-> [args...]: sets RC, OUT and CALLS (the stub log).
run_case() {
  local script=$1 loc=$2 host=$3
  shift 3
  local log="$T/calls.$RANDOM$RANDOM"
  : >"$log"
  set +e
  if [[ $host == -unset- ]]; then
    OUT=$(env -u MISTER_HOST LC_ALL="$loc" STUB_LOG="$log" PATH="$T/bin:$PATH" bash "$script" "$@" 2>&1)
  else
    OUT=$(env LC_ALL="$loc" MISTER_HOST="$host" STUB_LOG="$log" PATH="$T/bin:$PATH" bash "$script" "$@" 2>&1)
  fi
  RC=$?
  set -e
  CALLS=$(cat "$log")
}
row_ok() {   # row_ok <script> <locale> <row index>: true when the row's refusal holds
  local i=$3
  # shellcheck disable=SC2086  # R_ARGS holds fixed word lists
  run_case "$1" "$2" "${R_HOST[$i]}" ${R_ARGS[$i]}
  [[ $RC -eq 2 && $OUT == *"${R_MSG[$i]}"* && -z $CALLS ]]
}
row_index() {
  local i
  for i in "${!R_NAME[@]}"; do
    if [[ ${R_NAME[$i]} == "$1" ]]; then
      echo "$i"
      return
    fi
  done
  echo -1
}

for loc in "${LOCALES[@]}"; do
  for i in "${!R_NAME[@]}"; do
    checks=$((checks + 1))
    if ! row_ok "$SCRIPT" "$loc" "$i"; then
      fail "${R_NAME[$i]} [$loc]: want rc 2, '${R_MSG[$i]}' and no external command; got rc=$RC calls='$CALLS' out='$OUT'"
    fi
  done
  # Control: a named host and no arguments passes every guard and reaches (stubbed) make, never ssh.
  checks=$((checks + 1))
  run_case "$SCRIPT" "$loc" 192.0.2.1
  if [[ $RC -ne 97 || $CALLS != make\ * || $CALLS == *ssh* || $CALLS == *scp* ]]; then
    fail "C1 control [$loc]: want the stubbed make only (rc 97); got rc=$RC calls='$CALLS'"
  fi
done

# Teeth: one exact guard text replaced per mutant; every listed row must FAIL in every listed locale.
OCTET_LISTS="octet='(25[012345]|2[01234][0123456789]|1[0123456789][0123456789]|[123456789][0123456789]|[0123456789])'"
OCTET_RANGES="octet='(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])'"
MUT_NAME=(M1 M2 M5 M6)
MUT_TEXT=('if (($#)); then' 'if [[ -z "$HOST" ]]; then' 'if [[ ! $HOST =~ $canonical ]]; then' "$OCTET_LISTS")
MUT_REPL=('if false; then' 'if false; then' 'if false; then' "$OCTET_RANGES")
MUT_ROWS=('R1' 'R2' 'R5 R6 R7 R8 R9 R10 R11 R12 R13 R14 R14b R15 R16' 'R14 R14b R15')
MUT_LOCS=("C $UTF8" "C $UTF8" "C $UTF8" "$UTF8")
src=$(<"$SCRIPT")
mkdir -p "$T/mut/scripts"
MUTANT="$T/mut/scripts/deploy_misterplexd.sh"
for m in "${!MUT_NAME[@]}"; do
  name=${MUT_NAME[$m]}
  needle=${MUT_TEXT[$m]}
  checks=$((checks + 1))
  stripped=${src//"$needle"/}
  if (((${#src} - ${#stripped}) != ${#needle})); then
    fail "$name: vacuous, the text '$needle' is not in the script exactly once"
    continue
  fi
  printf '%s\n' "${src/"$needle"/"${MUT_REPL[$m]}"}" >"$MUTANT"
  for loc in ${MUT_LOCS[$m]}; do
    for target in ${MUT_ROWS[$m]}; do
      i=$(row_index "$target")
      if row_ok "$MUTANT" "$loc" "$i"; then
        fail "$name survived: $target [$loc] still passes with the guard replaced"
      else
        printf '%s killed by %s [%s] (rc=%s calls=%s)\n' "$name" "$target" "$loc" "$RC" "${CALLS%% *}"
      fi
    done
  done
done

if ((fails)); then
  printf 'test_deploy_misterplexd_guard: %d of %d checks FAILED\n' "$fails" "$checks"
  exit 1
fi
printf 'test_deploy_misterplexd_guard: OK (%d checks; under C and %s: args, unset/empty/malformed host refused before any external command; control reaches make only; 4 mutants killed by every row aimed at them)\n' "$checks" "$UTF8"
