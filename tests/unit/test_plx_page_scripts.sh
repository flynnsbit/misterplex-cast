#!/usr/bin/env bash
# PLX page in the scripts and tools (review 00:55 / release 01:05): the
# watcher's live_glass_is_l4 (misterplex_named_rbf.sh), probe_bitstream_ring.sh
# and input_mailbox_probe read the build row's page and nothing else.
# Needs build/test_plx_page and build/input_mailbox_probe_host (make unit).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
T="$(mktemp -d)"
HB_PID=
stop_hb() {
  if [ -n "$HB_PID" ]; then
    kill "$HB_PID" 2>/dev/null || true
    wait "$HB_PID" 2>/dev/null || true
    HB_PID=
  fi
}
trap 'stop_hb; rm -rf "$T"' EXIT
fails=0
ok() { printf 'PASS %s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }

# --- live_glass_is_l4 on a fake devmem: a word reads value 0 first, value 1 after.
FAKE="$T/devmem"
cat >"$FAKE" <<'SH'
#!/bin/sh
a=$(printf '0x%08X' "$(( $1 ))")
st="$FAKE_DEVMEM_DIR"
n=0
[ -f "$st/$a.n" ] && n=$(cat "$st/$a.n")
echo $((n + 1)) >"$st/$a.n"
if [ "$n" -ge 1 ] && [ -f "$st/$a.1" ]; then cat "$st/$a.1"
elif [ -f "$st/$a.0" ]; then cat "$st/$a.0"
else echo 0x00000000; fi
SH
chmod +x "$FAKE"

# glass_row NAME WANT WORD...: WANT l4|true480; WORD is ADDR=V or ADDR=V0>V1.
glass_row() {
  local name="$1" want="$2" st w a v rc
  shift 2
  st="$T/dev.${name// /_}"
  mkdir -p "$st"
  for w in "$@"; do
    a=$(printf '0x%08X' "$(( ${w%%=*} ))")
    v="${w#*=}"
    if [[ "$v" == *">"* ]]; then
      echo "${v%%>*}" >"$st/$a.0"
      echo "${v#*>}" >"$st/$a.1"
    else
      echo "$v" >"$st/$a.0"
    fi
  done
  rc=0
  (
    set +eu
    unset MPX_FORCE_LIVE_GLASS T480_PAGES MISTERPLEX_T480_PAGES
    export FAKE_DEVMEM_DIR="$st" MISTERPLEX_DEVMEM="$FAKE"
    . "$ROOT/scripts/misterplex_named_rbf.sh"
    live_glass_is_l4
  ) || rc=$?
  local got=true480
  [ "$rc" -eq 0 ] && got=l4
  if [ "$got" = "$want" ]; then ok "live_glass_is_l4: $name -> $got"; else bad "live_glass_is_l4: $name -> $got (want $want)"; fi
}

PLXJ=0x504C584A
PLXS=0x504C5853
glass_row "stale L4 PLXJ + live idle 480P (PLXS heartbeat)" true480 \
  0x3047F130=$PLXJ 0x300FF100=$PLXS "0x300FF104=0x00010000>0x00070000"
glass_row "stale L4 PLXJ + 480P bank swaps (PLXD hi)" true480 \
  0x3047F130=$PLXJ "0x300FF12C=0x00050001>0x00060002"
glass_row "stale L4 PLXJ, nothing live" l4 \
  0x3047F130=$PLXJ 0x300FF100=$PLXS 0x300FF104=0x00420000
glass_row "live L4 over leftover 480P PLXS" l4 \
  "0x3047F12C=0x00010001>0x00020002" 0x300FF100=$PLXS
glass_row "empty DRAM" true480
glass_row "leftover 480P PLXS magic, no PLXJ" true480 0x300FF100=$PLXS
glass_row "stale L4 PLXJ + core live on 0x3007F (no row page)" l4 \
  0x3047F130=$PLXJ 0x3007F100=$PLXS "0x3007F104=0x00010000>0x00070000"
glass_row "static L4 PLXS only" l4 0x3047F100=$PLXS
glass_row "N2: idle L4 before PLXJ, heartbeat only, over leftover 480P PLXS" l4 \
  0x3047F100=$PLXS "0x3047F104=0x00010000>0x00070000" 0x300FF100=$PLXS 0x300FF104=0x00420000
glass_row "static L4 PLXS heartbeat word + live 480P" true480 \
  0x3047F100=$PLXS 0x3047F104=0x00330000 0x300FF100=$PLXS "0x300FF104=0x00010000>0x00070000"

sh_pages=$( (unset T480_PAGES MISTERPLEX_T480_PAGES; . "$ROOT/scripts/misterplex_named_rbf.sh"; echo "$T480_PAGES") )
cpp_pages=$("$BUILD/test_plx_page" --print-true480-pages)
if [ "$sh_pages" = "$cpp_pages" ]; then
  ok "named_rbf T480_PAGES '$sh_pages' == fpga_build_ids.hpp row pages"
else
  bad "named_rbf T480_PAGES '$sh_pages' != fpga_build_ids.hpp row pages '$cpp_pages'"
fi
ring_rows=$(sed -n -E 's/^PLX_ROW_PAGES="(.*)"$/\1/p' "$ROOT/scripts/probe_bitstream_ring.sh")
cpp_rows=$("$BUILD/test_plx_page" --print-row-pages)
if [ -n "$ring_rows" ] && [ "$ring_rows" = "$cpp_rows" ]; then
  ok "probe_bitstream_ring PLX_ROW_PAGES == fpga_build_ids.hpp rows ($(wc -w <<<"$cpp_rows") IDs)"
else
  bad "probe_bitstream_ring PLX_ROW_PAGES '$ring_rows' != fpga_build_ids.hpp rows '$cpp_rows'"
fi

# --- A sparse memory image with a live core on 0x300FF000: PLXS heartbeat moving.
IMG="$T/mem.img"
python3 - "$IMG" <<'PY'
import os, struct, sys
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)
os.ftruncate(fd, 0x30200000)
os.pwrite(fd, struct.pack("<II", 0x504C5853, 0x00420000), 0x300FF100)  # PLXS
os.pwrite(fd, struct.pack("<II", 0x504C5849, 0x00030701), 0x300FF108)  # PLXI playpause
os.pwrite(fd, struct.pack("<II", 0x504C5844, 0x00010006), 0x300FF128)  # PLXD
os.close(fd)
PY
start_hb() {
  python3 - "$IMG" <<'PY' &
import os, struct, sys, time
fd = os.open(sys.argv[1], os.O_RDWR)
n = 0
while True:
    n = (n + 1) & 0xFFFF
    os.pwrite(fd, struct.pack("<I", (n << 16) | 0x0042), 0x300FF104)
    time.sleep(0.005)
PY
  HB_PID=$!
  sleep 0.3
}

ring() {
  env -u MISTER_HOST -u PLX_BUILD -u PLX_PAGE FORCE_LOCAL=1 PROBE_MEM="$IMG" SAMPLE_SLEEP_S=0.3 \
    PUSH_FRAME="$T/no_push_frame" BITSTREAM_STATUS_FILE="$T/no_status" "$@" \
    "$ROOT/scripts/probe_bitstream_ring.sh" 2>&1
}

# ring_row NAME WANT ENV...: WANT is live (PLXS/PLXD on 0x300FF000, heartbeat live) or
# noread; NOTE (optional, env RING_NOTE) must appear as the "PLX page:" line.
ring_row() {
  local name="$1" want="$2" out good=1
  shift 2
  out=$(ring "$@")
  if [ "$want" = live ]; then
    grep -q 'PLXS@0x300FF100 magic=.*(0x504c5853)' <<<"$out" && grep -q 'heartbeat=live' <<<"$out" &&
      grep -q 'PLXD@0x300FF128 magic=.*(0x504c5844)' <<<"$out" || good=0
  else
    ! grep -q 'PLXS@0x\|PLXD@0x' <<<"$out" || good=0
  fi
  if [ -n "${RING_NOTE:-}" ]; then
    grep -q -F "PLX page: $RING_NOTE" <<<"$out" || good=0
  fi
  grep -q '^ring_triage=' <<<"$out" || good=0
  if [ "$good" -eq 1 ]; then
    ok "probe_bitstream_ring $name -> $want"
  else
    bad "probe_bitstream_ring $name (want $want): $(grep -E 'PLX|ring_triage' <<<"$out" | tr '\n' '|')"
  fi
}

start_hb
RING_NOTE="page 0x300FF000 from build 0x48005D45 row" ring_row "PLX_BUILD=0x48005D45 (prod 480p row)" live PLX_BUILD=0x48005D45
RING_NOTE="page 0x300FF000 from build 0x2400F40B row" ring_row "PLX_BUILD=0x2400f40b (240p META_DPB row, lower case)" live PLX_BUILD=0x2400f40b
RING_NOTE="build 0xDEADBEEF has no fpga_build_ids.hpp row: no PLX page, no PLX reads" ring_row "PLX_BUILD=0xDEADBEEF (no row)" noread PLX_BUILD=0xDEADBEEF
RING_NOTE="build 0x4801F36A has no fpga_build_ids.hpp row: no PLX page, no PLX reads" ring_row "PLX_BUILD=0x4801F36A (retired)" noread PLX_BUILD=0x4801F36A
RING_NOTE="PLX_PAGE=0x3007F000 disagrees with build 0x48005D45 row page 0x300FF000: no PLX reads" ring_row "PLX_BUILD=0x48005D45 + PLX_PAGE=0x3007F000 (disagree)" noread PLX_BUILD=0x48005D45 PLX_PAGE=0x3007F000
RING_NOTE="page 0x300FF000 from build 0x48005D45 row" ring_row "PLX_BUILD=0x48005D45 + PLX_PAGE=0x300FF000 (agree)" live PLX_BUILD=0x48005D45 PLX_PAGE=0x300FF000
RING_NOTE="PLX_BUILD=5d45 is not a number: no PLX reads" ring_row "PLX_BUILD=5d45 (not a number)" noread PLX_BUILD=5d45
RING_NOTE="PLX_BUILD=09 is not a number: no PLX reads" ring_row "PLX_BUILD=09 (leading-zero decimal, review 04:05 N1)" noread PLX_BUILD=09
RING_NOTE="PLX_PAGE=0777 is not a number: no PLX reads" ring_row "PLX_PAGE=0777 (leading-zero decimal)" noread PLX_PAGE=0777
RING_NOTE="page 0x300FF000 from PLX_PAGE (override, not from a build row)" ring_row "PLX_PAGE=0x300FF000 alone (override)" live PLX_PAGE=0x300FF000
out=$(ring PLX_PAGE=0x300FF000)
if grep -q 'PLXS@0x300FF100 magic=.*(0x504c5853)' <<<"$out" && grep -q 'heartbeat=live' <<<"$out" &&
   grep -q 'PLXD@0x300FF128 magic=.*(0x504c5844)' <<<"$out"; then
  ok "probe_bitstream_ring PLX_PAGE=0x300FF000: PLXS/PLXD on the page, heartbeat live"
else
  bad "probe_bitstream_ring PLX_PAGE=0x300FF000: $(grep -E 'PLX' <<<"$out" | tr '\n' '|')"
fi
out=$(ring PLX_PAGE=)
if grep -q 'PLX page unset: PLXS/PLXD not read' <<<"$out" && ! grep -q 'PLXS@0x' <<<"$out"; then
  ok "probe_bitstream_ring without PLX_PAGE: no PLX reads, says so"
else
  bad "probe_bitstream_ring without PLX_PAGE: $(grep -E 'PLX' <<<"$out" | tr '\n' '|')"
fi
out=$(ring PLX_PAGE=0x3007F000)
if grep -q 'PLXS@0x3007F100' <<<"$out" && grep -q 'heartbeat=NOT MOVING' <<<"$out"; then
  ok "probe_bitstream_ring on a dead page: heartbeat NOT MOVING, not a silent absent"
else
  bad "probe_bitstream_ring on a dead page: $(grep -E 'PLX' <<<"$out" | tr '\n' '|')"
fi

PROBE="$BUILD/input_mailbox_probe_host"
rc=0
out=$("$PROBE" --build 0x48005D45 --once --mem "$IMG" 2>&1) || rc=$?
if [ "$rc" -eq 0 ] && grep -q 'page=0x300FF000 build=0x48005D45' <<<"$out" &&
   grep -q 'magic=PLXI seq=3 cmd_seq=7 cmd=playpause' <<<"$out"; then
  ok "input_mailbox_probe --build 0x48005D45: PLXI read on its row page 0x300FF000"
else
  bad "input_mailbox_probe --build 0x48005D45 rc=$rc: $(tr '\n' '|' <<<"$out")"
fi
rc=0
out=$("$PROBE" --build 0xDEADBEEF --once --mem "$IMG" 2>&1) || rc=$?
if [ "$rc" -eq 6 ] && grep -q 'no PLX page, no PLX reads' <<<"$out"; then
  ok "input_mailbox_probe unknown build: refused, no PLX reads"
else
  bad "input_mailbox_probe unknown build rc=$rc: $(tr '\n' '|' <<<"$out")"
fi
stop_hb
rc=0
out=$("$PROBE" --build 0x4801F40A --once --mem "$IMG" 2>&1) || rc=$?
if [ "$rc" -eq 5 ] && grep -q 'PLXS heartbeat (+0x104) not moving' <<<"$out"; then
  ok "input_mailbox_probe on a page without a live heartbeat: refused (rc 5)"
else
  bad "input_mailbox_probe without heartbeat rc=$rc: $(tr '\n' '|' <<<"$out")"
fi

if [ "$fails" -ne 0 ]; then
  echo "test_plx_page_scripts: $fails failure(s)"
  exit 1
fi
echo "test_plx_page_scripts: PASS"
