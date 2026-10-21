#!/usr/bin/env bash
# T-DAEMON-LOCK-SHARED teeth (paper c34923a5, A1 9e3e63c8, fold f1a07c9a; review 00:35/00:45, H264 k1/k2).
# Every daemon here is the REAL host build/misterplexd (MPX_TEST_BUILD) or a test build of set_status,
# each run with a PRIVATE MPX_LOCK_DIR; the prod /tmp lock inodes are checked unchanged at the end.
# Starts real daemons: run it in heavy-sim-slot, in a private PID ns (the lab-pc standing rule).
#   k1  bad dir: the real daemon and set_status exit 2 at static init, the error is the FIRST and
#       ONLY stderr line, and nothing else runs ('main' never logs)
#   n1  MPX_LOCK_DIR=/tmp, /tmp/. and a symlink to /tmp: exit 2 ('is /tmp itself')
#   k2  a symlinked lock dir: the daemon logs the realpath, and lp_expect (which realpaths the
#       script side) passes (POS); a different dir is RED (NEG)
#   T1  two daemons, two dirs: both up, 0 'already running'
#   T2  two daemons, one dir: the second refuses 'already running pid=<first>' (exit 3)
#   T6  the unlink limit, RED BY DESIGN: holder A, its lock file unlinked, B starts beside it
#   T1b-T1f  the baked dir (a probe TU on lock_paths.hpp): baked only -> source=baked; env wins;
#       baked dir removed or read-only -> exit 2; MPX_TEST_LOCK_DIR without MPX_TEST_BUILD -> #error
#   c3  lp_expect NEGs: a missing line (stderr discarded / built without the flags), a /tmp line
#       (default source), one line too many
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/build/lock_paths_teeth
BIN=$ROOT/build/misterplexd
CXX=${CXX:-g++}
make -C "$ROOT" plexd > /dev/null || { echo "test_lock_paths: FAIL (make plexd)"; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"
# Any daemon started without its own MPX_LOCK_DIR below still lands in a private dir (L-3).
export MPX_LOCK_DIR="$OUT/default-locks"
mkdir -p "$MPX_LOCK_DIR"
. "$ROOT/tests/unit/lock_paths_lib.sh"
fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
# Inodes only: another lane's daemon may legitimately rewrite its pid (mtime), but an unlink and
# re-create (the hazard) changes the inode.
prodstat() { stat -c '%n %i' /tmp/misterplexd.lock /tmp/misterplex_spi.lock 2>&1 | tr '\n' ' '; }
P0=$(prodstat)
# b3: no lock is held on this run's private dirs before it starts (and none after it ends).
privlocks() { lslocks -n -o PID,PATH 2>/dev/null | grep -F -- "$OUT/" | tr -s ' ' | tr '\n' ';'; }
PL0=$(privlocks)
echo "prod lock inodes before: $P0; private-dir lslocks before: [${PL0}]"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'; }
start() {  # start <dir> <log>: the real daemon with MPX_LOCK_DIR=<dir>, in the background
    MPX_LOCK_DIR=$1 "$BIN" --name lock-teeth --port "$(port)" > "$2" 2>&1 &
    PIDS+=($!)
    LASTPID=$!
}
up() { sleep 1.5; kill -0 "$1" 2>/dev/null && ! grep -q 'already running' "$2"; }
badrun() {  # badrun <name> <want-msg> <cmd...>: rc 2, the message is the first and only stderr line
    local name=$1 want=$2 rc
    shift 2
    "$@" > "$OUT/$name.out" 2> "$OUT/$name.err"
    rc=$?
    if [ "$rc" = 2 ] && head -n 1 "$OUT/$name.err" | grep -q -F -- "$want" &&
        [ "$(wc -l < "$OUT/$name.err")" = 1 ] && [ ! -s "$OUT/$name.out" ]; then
        ok "$name: rc 2, '$want' is the only stderr line"
    else
        bad "$name: rc $rc, stderr $(head -c 200 "$OUT/$name.err" | tr '\n' '|'), stdout $(wc -c < "$OUT/$name.out") B"
    fi
}

echo "== k1 / n1: the real daemon (build/misterplexd) and a test-build set_status on a bad lock dir"
"$CXX" -std=c++17 -O2 -I"$ROOT/host" -I"$ROOT/arm/misterplexd" -pthread -DMPX_TEST_BUILD \
    -DMPX_DEVMEM_PATH="\"$OUT/fake.mem\"" -o "$OUT/set_status" \
    "$ROOT/tools/set_status.cpp" "$ROOT/arm/misterplexd/fpga_spi.cpp" ||
    { echo "test_lock_paths: FAIL (set_status build)"; exit 1; }
badrun k1-daemon-missing "is not an absolute writable directory" env MPX_LOCK_DIR="$OUT/missing" "$BIN" --port 1
badrun k1-set_status-missing "is not an absolute writable directory" env MPX_LOCK_DIR="$OUT/missing" "$OUT/set_status" --bit 16 1
badrun k1-daemon-relative "is not an absolute writable directory" env MPX_LOCK_DIR=rel "$BIN" --port 1
ln -s /tmp "$OUT/tmplink"
badrun n1-daemon-tmp "is /tmp itself" env MPX_LOCK_DIR=/tmp "$BIN" --port 1
badrun n1-daemon-tmp-dot "is /tmp itself" env MPX_LOCK_DIR=/tmp/. "$BIN" --port 1
badrun n1-daemon-tmp-symlink "is /tmp itself" env MPX_LOCK_DIR="$OUT/tmplink" "$BIN" --port 1
badrun n1-set_status-tmp "is /tmp itself" env MPX_LOCK_DIR=/tmp "$OUT/set_status" --bit 16 1

echo "== k2: a symlinked lock dir is logged as its realpath; lp_expect realpaths the script side"
mkdir -p "$OUT/k2-real"
ln -s "$OUT/k2-real" "$OUT/k2-link"
start "$OUT/k2-link" "$OUT/k2.log"
MPX_LOCK_DIR=$OUT/k2-link
lp_init
if lp_expect "$OUT/k2.log" 1 2> "$OUT/k2.err"; then ok "k2 POS: one line, $LP_WANT"; else bad "k2 POS: $(cat "$OUT/k2.err")"; fi
MPX_LOCK_DIR=$OUT
lp_init
if lp_expect "$OUT/k2.log" 1 2> /dev/null; then bad "k2 NEG: a different dir passed"; else ok "k2 NEG: a different dir is RED"; fi
kill "$LASTPID" 2>/dev/null
wait "$LASTPID" 2>/dev/null

echo "== T1 / T2: two dirs -> both up; one dir -> the second refuses"
mkdir -p "$OUT/t1-a" "$OUT/t1-b"
start "$OUT/t1-a" "$OUT/t1-a.log"
A=$LASTPID
start "$OUT/t1-b" "$OUT/t1-b.log"
B=$LASTPID
if up "$A" "$OUT/t1-a.log" && up "$B" "$OUT/t1-b.log"; then ok "T1: both daemons up in their own dirs"; else bad "T1: $(grep -h 'already running' "$OUT"/t1-*.log | head -n 1)"; fi
start "$OUT/t1-a" "$OUT/t2.log"
C=$LASTPID
wait "$C"
rc=$?
if [ "$rc" = 3 ] && grep -q "already running pid=$A lock=$(realpath "$OUT/t1-a")/misterplexd.lock" "$OUT/t2.log"; then
    ok "T2: the same dir refuses (rc 3, pid=$A)"
else
    bad "T2: rc $rc, $(grep -m1 'already running' "$OUT/t2.log" || echo 'no refusal')"
fi
kill "$A" "$B" 2>/dev/null
wait "$A" "$B" 2>/dev/null

echo "== T6: the unlink limit (RED by design, documented): the lock file unlinked under a live holder"
t6_fds() {  # t6_fds <A> <B> <lock>: poll (<= 10 s) until A holds <lock> unlinked and B holds the new <lock>
    local i f
    for i in $(seq 1 100); do
        fa=0 fb=0
        for f in /proc/"$1"/fd/*; do [ "$(readlink "$f" 2>/dev/null)" = "$3 (deleted)" ] && fa=$((fa + 1)); done
        for f in /proc/"$2"/fd/*; do [ "$(readlink "$f" 2>/dev/null)" = "$3" ] && fb=$((fb + 1)); done
        [ "$fa" = 1 ] && [ "$fb" = 1 ] && return 0
        sleep 0.1
    done
    return 1
}
mkdir -p "$OUT/t6"
start "$OUT/t6" "$OUT/t6-a.log"
A=$LASTPID
sleep 1
rm -f "$OUT/t6/misterplexd.lock"  # a PRIVATE dir: this is the limit's demonstration, never a prod path
start "$OUT/t6" "$OUT/t6-b.log"
B=$LASTPID
# b2: the ONLY accepted outcome is the documented one, by pattern: A and B both up, A's lock fd on
# the unlinked inode ('(deleted)'), B's on the new file. Anything else (B refused, A gone, other
# fds) is an unexpected RED, a FAIL. H264 k1: the fds are POLLED (<= 10 s, 0.1 s steps) after up(), so a
# loaded host cannot catch B before it opens its lock; n2: each fd is matched by exact readlink equality.
T6L="$(realpath "$OUT/t6")/misterplexd.lock"
if up "$A" "$OUT/t6-a.log" && up "$B" "$OUT/t6-b.log" && t6_fds "$A" "$B" "$T6L"; then
    ok "T6 EXPECTED-RED: holder A pid=$A on the unlinked inode, B pid=$B on a new inode (two holders; the documented limit)"
else
    bad "T6 unexpected outcome (a different RED): A up=$(kill -0 "$A" 2>/dev/null && echo 1 || echo 0) fd-deleted=$fa, B fd-new=$fb, B: $(grep -m1 'already running' "$OUT/t6-b.log" || echo 'no refusal')"
fi
kill "$A" "$B" 2>/dev/null
wait "$A" "$B" 2>/dev/null

echo "== T6 sampler teeth (H264 k1): the poll waits for a slow B, and still fails a B that never opens"
mkdir -p "$OUT/t6t"
TL="$(realpath "$OUT/t6t")/misterplexd.lock"  # a PRIVATE dir, never a prod path
: > "$TL"
( exec 9<> "$TL"; sleep 30 ) &
FA=$!
sleep 0.3
rm -f "$TL"
: > "$TL"
( sleep 3; exec 9<> "$TL"; sleep 30 ) &
FB=$!
( sleep 30 ) &
FN=$!
if t6_fds "$FA" "$FB" "$TL"; then ok "T6 sampler: a B that opens its lock after 3 s is seen"; else bad "T6 sampler: missed a B that opened after 3 s (fa=$fa fb=$fb)"; fi
if t6_fds "$FA" "$FN" "$TL"; then bad "T6 sampler: passed a B that never opened its lock"; else ok "T6 sampler: a B that never opens its lock is RED (fa=$fa fb=$fb)"; fi
kill "$FA" "$FB" "$FN" 2>/dev/null
wait "$FA" "$FB" "$FN" 2>/dev/null

echo "== T1b-T1f: the baked dir (probe TU on lock_paths.hpp)"
printf '#include "lock_paths.hpp"\n#include <cstdio>\nint main(){std::printf("%%s %%s\\n", misterplex::daemonLockPath(), misterplex::spiLockPath());}\n' > "$OUT/probe.cpp"
"$CXX" -std=c++17 -I"$ROOT/arm/misterplexd" -DMPX_TEST_BUILD -DMPX_TEST_LOCK_DIR="\"$OUT/baked\"" -o "$OUT/probe" "$OUT/probe.cpp" ||
    { echo "test_lock_paths: FAIL (probe build)"; exit 1; }
mkdir -p "$OUT/baked" "$OUT/env2"
o=$(env -i "$OUT/probe" 2>&1)
case $o in *"source=baked daemon=$(realpath "$OUT/baked")/misterplexd.lock"*) ok "T1b: env -i, baked only -> source=baked" ;; *) bad "T1b: $o" ;; esac
o=$(env -i MPX_LOCK_DIR="$OUT/env2" "$OUT/probe" 2>&1)
case $o in *"source=env daemon=$(realpath "$OUT/env2")/misterplexd.lock"*) ok "T1c: the env wins over the baked dir" ;; *) bad "T1c: $o" ;; esac
chmod 555 "$OUT/baked"
badrun T1e-baked-read-only "is not an absolute writable directory" env -i "$OUT/probe"
chmod 755 "$OUT/baked"
rmdir "$OUT/baked"
badrun T1d-baked-removed "is not an absolute writable directory" env -i "$OUT/probe"
if "$CXX" -std=c++17 -fsyntax-only -I"$ROOT/arm/misterplexd" -DMPX_TEST_LOCK_DIR="\"$OUT/baked\"" \
    "$OUT/probe.cpp" 2> "$OUT/t1f.err"; then
    bad "T1f: MPX_TEST_LOCK_DIR without MPX_TEST_BUILD compiled"
else
    grep -q 'honoured only in an MPX_TEST_BUILD' "$OUT/t1f.err" && ok "T1f: MPX_TEST_LOCK_DIR without MPX_TEST_BUILD -> #error" ||
        bad "T1f: failed for another reason: $(head -n 1 "$OUT/t1f.err")"
fi

echo "== c3: lp_expect NEGs (lock_paths_lib.sh)"
MPX_LOCK_DIR=$OUT/env2
lp_init
: > "$OUT/c3-none.log"
echo "mpx-lock-paths: source=default daemon=/tmp/misterplexd.lock spi=/tmp/misterplex_spi.lock" > "$OUT/c3-tmp.log"
{ echo "mpx-lock-paths: source=env $LP_WANT"; echo "mpx-lock-paths: source=env $LP_WANT"; } > "$OUT/c3-two.log"
echo "mpx-lock-paths: source=baked $LP_WANT" > "$OUT/c3-good.log"
lp_expect "$OUT/c3-none.log" 1 2> /dev/null && bad "c3: a missing line passed" || ok "c3: a missing line (discarded stderr / no flags) is RED"
lp_expect "$OUT/c3-tmp.log" 1 2> /dev/null && bad "c3: a /tmp line passed" || ok "c3: a /tmp (default) line is RED"
lp_expect "$OUT/c3-two.log" 1 2> /dev/null && bad "c3: two lines passed for one" || ok "c3: one line too many is RED"
lp_expect "$OUT/c3-good.log" 1 2> /dev/null && ok "c3: POS, one baked line naming the dir" || bad "c3: the POS line failed"

P1=$(prodstat)
if [ "$P0" = "$P1" ]; then ok "the prod lock inodes are unchanged: $P1"; else bad "the prod lock inodes CHANGED: $P0 -> $P1"; fi
cleanup
PIDS=()
PL1=$(privlocks)
if [ -z "$PL0" ] && [ -z "$PL1" ]; then ok "private-dir lslocks before [] and after []"; else bad "private-dir lslocks before [$PL0] after [$PL1]"; fi
if [ "$fails" = 0 ]; then echo "test_lock_paths: OK"; else echo "test_lock_paths: FAIL ($fails)"; exit 1; fi
