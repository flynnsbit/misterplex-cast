#!/bin/sh
# The seed-10 PlexOB bitstream is the only file the stager and the package
# script will install. cores/PlexOB.rbf.md5 is that pin. A non-matching pin
# or file, including the void seed-5 bitstream, is refused before any copy
# and before the daemon rebuild.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
STAGE_SH="$ROOT/scripts/stage_plexob_yc_rbf.sh"
PKG="$ROOT/scripts/package_release.sh"
PIN_FILE="$ROOT/cores/PlexOB.rbf.md5"
SEED10=c8b346048d680cc410a447490f57a820
fail() { echo "FAIL $*" >&2; exit 1; }
[ -f "$STAGE_SH" ] || fail "missing stager"
[ -f "$PKG" ] || fail "missing package_release.sh"
[ -f "$PIN_FILE" ] || fail "missing cores/PlexOB.rbf.md5"
slot=$(tr -d '[:space:]' < "$PIN_FILE" | tr '[:upper:]' '[:lower:]')
[ "$slot" = "$SEED10" ] || fail "cores/PlexOB.rbf.md5 is $slot"
grep -q "$SEED10" "$PKG" || fail "package_release does not pin the seed-10 md5"
grep -q "$SEED10" "$STAGE_SH" || fail "stager does not name the seed-10 md5"
grep -q 'cores/PlexOB.rbf.md5' "$STAGE_SH" || fail "stager does not check cores/PlexOB.rbf.md5"
grep -q 'not the seed-10' "$STAGE_SH" || fail "stager does not refuse a foreign pin"
grep -q 'PLEXOB_RBF_MD5:-PENDING' "$PKG" && fail "package still defaults the slot to PENDING"
grep -q 'PENDING' "$PKG" && fail "package_release still says PENDING"
grep -q 'PENDING' "$STAGE_SH" && fail "stager still says PENDING"
grep -q 'cores/PlexOB.rbf.md5' "$PKG" || fail "package missing the md5 slot"
grep -q 'PlexOB.rbf md5:' "$PKG" || fail "README missing the md5 line"
grep -q '4D505842/0B59C11A' "$PKG" || fail "package does not name word 88"
grep -q 'OB input region DEAD' "$PKG" || fail "package missing the gate-open note"
grep -q '0x0B59C11A' "$PKG" || fail "package does not name BUILD_ID 0x0B59C11A"
grep -q 'ae7e35a6' "$PKG" || fail "package missing the seed-6 fallback note"
grep -q 'PlexOB.rbf.path' "$PKG" && fail "package still writes the old path placeholder"
grep -q 'RBF pinned separately' "$PKG" && fail "package still uses the old pin note"
grep -q 'stage_plexob_yc_rbf.sh" "$PLEXOB_RBF" "$STAGE" "$PLEXOB_RBF_MD5"' "$PKG" || fail "package does not pass the pinned md5 to the stager"
grep -F -q 'cp -a "\$f" "\$f.bak.\$stamp"' "$PKG" || fail "README install does not back up the existing RBF"
grep -q 'md5sum /tmp/PlexOB.rbf.mpx-tmp' "$PKG" || fail "README install does not check the pinned md5"
grep -q 'cut -d" " -f1' "$PKG" || fail "README md5 check does not use cut"
grep -q 'awk "{print \$1}"' "$PKG" && fail "README install still emits awk print \$1"
grep -q '/media/fat/PlexOB.rbf' "$PKG" || fail "README missing /media/fat/PlexOB.rbf"
grep -q '/media/fat/_Utility/PlexOB.rbf' "$PKG" || fail "README missing the utility install path"
grep -q 'PlexOB.rbf' "$STAGE_SH" || fail "stager missing prod path"
grep -q '_Utility/PlexOB.rbf' "$STAGE_SH" || fail "stager missing utility path"
grep -q 'fsync_path' "$STAGE_SH" || fail "stager does not fsync"
grep -q 'mv -f' "$STAGE_SH" || fail "stager does not rename into place"
grep -q 'mpx-tmp' "$STAGE_SH" || fail "stager does not write a temp file"
grep -q '4D505842/0B59C11A' "$ROOT/docs/release.md" || fail "docs/release.md missing word 88"
grep -q 'OB input region DEAD' "$ROOT/docs/release.md" || fail "docs/release.md missing the gate-open note"

# The void seed-5 file, when present, must not be named by packaging or docs.
# The hash is computed here so the tree does not have to store it.
VOID=/tmp/plexob-yc-s5/PlexOB.rbf
void=""
if [ -f "$VOID" ]; then
  void=$(md5sum "$VOID" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')
  [ "$void" != "$SEED10" ] || fail "void fixture hashed to the seed-10 pin"
  for f in "$STAGE_SH" "$PKG" "$PIN_FILE" "$ROOT/docs/release.md" \
    "$ROOT/fpga/owenb-combo-s9/PlexOB_480p/fit/yc/TIMING-CLOSURE.md"; do
    if grep -q "$void" "$f"; then
      fail "void md5 is still in $f"
    fi
  done
  if grep -R -I -q "$void" "$ROOT/scripts" "$ROOT/docs" "$ROOT/cores" "$ROOT/README.md" 2>/dev/null; then
    fail "void md5 is still in scripts, docs, or allowed-md5 lists"
  fi
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/out/_Utility"
printf '%s\n' 'keep-me' > "$work/out/PlexOB.rbf"
printf '%s\n' 'keep-util' > "$work/out/_Utility/PlexOB.rbf"
if sh "$STAGE_SH" "$work/missing.rbf" "$work/out" >/tmp/plexob-stage.out 2>&1; then
  fail "a missing source was accepted"
fi
[ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "missing source clobbered DEST/PlexOB.rbf"
[ "$(cat "$work/out/_Utility/PlexOB.rbf")" = "keep-util" ] || fail "missing source clobbered utility RBF"

printf '%s\n' 'caller-supplied-core' > "$work/in.rbf"
if sh "$STAGE_SH" "$work/in.rbf" "$work/out" >/tmp/plexob-stage.out 2>&1; then
  fail "a file that is not the seed-10 bitstream was accepted"
fi
grep -q 'seed-10' /tmp/plexob-stage.out || fail "foreign file did not say so: $(cat /tmp/plexob-stage.out)"
[ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "foreign file clobbered DEST/PlexOB.rbf"
pin=$(md5sum "$work/in.rbf" | awk '{print $1}')
mkdir -p "$work/out/cores"
printf '%s\n' 'PENDING' > "$work/out/cores/PlexOB.rbf.md5"
if sh "$STAGE_SH" "$work/in.rbf" "$work/out" >/tmp/plexob-stage.out 2>&1; then
  fail "a non-md5 cores/PlexOB.rbf.md5 was accepted as a pin"
fi
grep -q 'pinned md5' /tmp/plexob-stage.out || fail "non-md5 pin did not say so: $(cat /tmp/plexob-stage.out)"
[ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "non-md5 pin clobbered the prod RBF"
printf '%s\n' '00000000000000000000000000000000' > "$work/out/cores/PlexOB.rbf.md5"
if sh "$STAGE_SH" "$work/in.rbf" "$work/out" >/tmp/plexob-stage.out 2>&1; then
  fail "a mismatched cores/PlexOB.rbf.md5 was accepted"
fi
[ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "mismatched pin clobbered the prod RBF"
if sh "$STAGE_SH" "$work/in.rbf" "$work/out" "$pin" >/tmp/plexob-stage.out 2>&1; then
  fail "a caller pin other than the seed-10 md5 was accepted"
fi
grep -q 'not the seed-10' /tmp/plexob-stage.out || fail "foreign pin did not say so: $(cat /tmp/plexob-stage.out)"
[ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "foreign pin clobbered the prod RBF"
printf '%s\n' "$SEED10" > "$work/out/cores/PlexOB.rbf.md5"
if sh "$STAGE_SH" "$work/in.rbf" "$work/out" >/tmp/plexob-stage.out 2>&1; then
  fail "seed-10 slot accepted a file with a different md5"
fi
[ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "mismatched file clobbered the prod RBF"

echo not-a-dir > "$work/notadir"
if sh "$STAGE_SH" "$work/in.rbf" "$work/notadir" "$SEED10" >/tmp/plexob-stage.out 2>&1; then
  fail "DEST_ROOT that is a file was accepted"
fi
mkdir -p "$work/squat/PlexOB.rbf"
if sh "$STAGE_SH" "$work/in.rbf" "$work/squat" "$SEED10" >/tmp/plexob-stage.out 2>&1; then
  fail "a directory squatting on PlexOB.rbf was accepted"
fi
[ -d "$work/squat/PlexOB.rbf" ] || fail "squatting directory was replaced"
find "$work/squat" -type f | grep -q . && fail "stager wrote into the squatting directory"

if [ -n "$void" ]; then
  printf '%s\n' 'keep-me' > "$work/out/PlexOB.rbf"
  printf '%s\n' 'keep-util' > "$work/out/_Utility/PlexOB.rbf"
  if sh "$STAGE_SH" "$VOID" "$work/out" "$void" >/tmp/plexob-stage.out 2>&1; then
    fail "void seed-5 pin was accepted"
  fi
  grep -q 'not the seed-10' /tmp/plexob-stage.out || fail "void pin did not say so: $(cat /tmp/plexob-stage.out)"
  [ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "void pin clobbered DEST/PlexOB.rbf"
  [ "$(cat "$work/out/_Utility/PlexOB.rbf")" = "keep-util" ] || fail "void pin clobbered the utility RBF"
  if PLEXOB_RBF_MD5="$void" sh "$STAGE_SH" "$VOID" "$work/out" >/tmp/plexob-stage.out 2>&1; then
    fail "void seed-5 env pin was accepted"
  fi
  [ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "void env pin clobbered DEST/PlexOB.rbf"
  if sh "$STAGE_SH" "$VOID" "$work/out" "$SEED10" >/tmp/plexob-stage.out 2>&1; then
    fail "void file was accepted under the seed-10 pin"
  fi
  [ "$(cat "$work/out/PlexOB.rbf")" = "keep-me" ] || fail "void file clobbered DEST/PlexOB.rbf"
  if PLEXOB_RBF_MD5="$void" bash "$PKG" >/tmp/plexob-pkg-void.out 2>&1; then
    fail "package_release accepted a void pin"
  fi
  grep -q 'refusing PlexOB pin' /tmp/plexob-pkg-void.out || fail "package did not refuse the pin: $(cat /tmp/plexob-pkg-void.out)"
  grep -q 'Building arm' /tmp/plexob-pkg-void.out && fail "void pin reached the daemon build"
  if env -u PLEXOB_RBF_MD5 -u MPX_SOURCE_MODE -u MPX_SOURCE_CONTACT -u MPX_SOURCE_INTERNAL \
      PLEXOB_RBF="$VOID" bash "$PKG" >/tmp/plexob-pkg-void.out 2>&1; then
    fail "package_release accepted the void bitstream"
  fi
  grep -q 'refusing PlexOB.rbf' /tmp/plexob-pkg-void.out || fail "package did not refuse the file: $(cat /tmp/plexob-pkg-void.out)"
  grep -q 'Building arm' /tmp/plexob-pkg-void.out && fail "void bitstream reached the daemon build"
  if grep -q "$void" /tmp/plexob-pkg-void.out /tmp/plexob-stage.out; then
    fail "refusal output echoed the void md5"
  fi
fi

# Both install paths, when the cleared bitstream is on disk.
src=""
for cand in \
  "$ROOT/release_artifacts/plexob-yc-0b59c11a/PlexOB.rbf" \
  "${PLEXOB_RBF:-}" \
  "${PLEXOB_RBF_GOOD:-}"
do
  [ -n "$cand" ] && [ -f "$cand" ] || continue
  got=$(md5sum "$cand" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')
  if [ "$got" = "$SEED10" ]; then
    src=$cand
    break
  fi
done
if [ -n "$src" ]; then
  mkdir -p "$work/good/_Utility"
  printf '%s\n' 'keep' > "$work/good/PlexOB.rbf"
  printf '%s\n' 'keep' > "$work/good/_Utility/PlexOB.rbf"
  sh "$STAGE_SH" "$src" "$work/good" >/tmp/plexob-stage.out 2>&1 || fail "seed-10 file was refused: $(cat /tmp/plexob-stage.out)"
  a=$(md5sum "$work/good/PlexOB.rbf" | awk '{print $1}')
  b=$(md5sum "$work/good/_Utility/PlexOB.rbf" | awk '{print $1}')
  [ "$a" = "$SEED10" ] && [ "$b" = "$SEED10" ] || fail "installed md5 is not the seed-10 pin"
  find "$work/good" -name '*.mpx-tmp' | grep -q . && fail "temp file left after a good stage"
  [ ! -e "$work/good/menu.rbf" ] || fail "stager wrote menu.rbf"
else
  echo "test_package_plexob_rbf: seed-10 bitstream not on disk; install path not exercised" >&2
fi

echo "test_package_plexob_rbf: ok"
