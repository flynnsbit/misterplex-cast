#!/bin/sh
# Named Utility RBF identity. Sourced by misterplex_core_watch.sh.
# Do not exec this file (no main).
#
# Every Plex_* sibling reports CORENAME/RBFNAME=Plex. A generic Plex.rbf
# (root or _Utility) therefore always hashes as whichever copy was last
# dropped there — 480p daemon on 720p24 fabric = no chevron, no play.

UTILITY_DIR="${MISTERPLEX_UTILITY:-${UTILITY_DIR:-/media/fat/_Utility}}"
LIVE_RBF="${MISTERPLEX_LIVE_RBF:-${LIVE_RBF:-$UTILITY_DIR/Plex_480p.rbf}}"
RBFNAME_FILE="${RBFNAME_FILE:-/tmp/RBFNAME}"
L4_PLXS_ADDR="${MISTERPLEX_L4_PLXS:-${L4_PLXS_ADDR:-0x3047F100}}"
# True480 PLX pages: the non-L4 pages of the recognised FPGA builds
# (host/libmisterplex/fpga_build_ids.hpp rows; tests/unit/test_plx_page_scripts.sh
# keeps this list equal to that table). Every recognised build publishes at
# 0x300FF000 (PLEX_PRESENT_480P or PLEX_META_DPB).
T480_PAGES="${MISTERPLEX_T480_PAGES:-${T480_PAGES:-0x300FF000}}"

# 720p24 fabric publishes PLXS/PLXJ/PLXD at 0x3047F1xx, true480 cores on their
# T480_PAGES page. Leftover magics survive in HPS DRAM:
# leftover 480p PLXS must not beat a live L4 scanout (28cb5a75 was pinned
# true480 and GLASS-MAX capped 1280x720 → 640x480). A moving high word wins:
# PLXD +0x12C (bank swaps) or PLXS +0x104 (heartbeat seq), on the L4 page and on
# each true480 page. An idle L4 core before its first PLXJ commit only moves PLXS.
# Static leftovers: PLXJ (0x504C584A) is L4-only.
L4_PLXD_ADDR="${MISTERPLEX_L4_PLXD:-${L4_PLXD_ADDR:-0x3047F12C}}"
L4_PLXS_HI_ADDR="${MISTERPLEX_L4_PLXS_HI:-${L4_PLXS_HI_ADDR:-0x3047F104}}"
L4_PLXJ_ADDR="${MISTERPLEX_L4_PLXJ:-${L4_PLXJ_ADDR:-0x3047F130}}"

l4_sample() {
  _l4_d=$("$DEVMEM" "$L4_PLXD_ADDR" 32 2>/dev/null || echo 0)
  _l4_s=$("$DEVMEM" "$L4_PLXS_HI_ADDR" 32 2>/dev/null || echo 0)
  echo "$_l4_d $_l4_s"
}

t480_sample() {
  _t480_out=
  for _t480_p in $T480_PAGES; do
    _t480_s=$("$DEVMEM" "$(printf '0x%08X' $((_t480_p + 0x104)))" 32 2>/dev/null || echo 0)
    _t480_d=$("$DEVMEM" "$(printf '0x%08X' $((_t480_p + 0x12C)))" 32 2>/dev/null || echo 0)
    _t480_out="$_t480_out $_t480_s $_t480_d"
  done
  echo "$_t480_out"
}

t480_plxs_magic() {
  for _t480_p in $T480_PAGES; do
    _t480_v=$("$DEVMEM" "$(printf '0x%08X' $((_t480_p + 0x100)))" 32 2>/dev/null || echo 0)
    case "$_t480_v" in
      *504C5853*|*504c5853*) return 0 ;;
    esac
  done
  return 1
}

live_glass_is_l4() {
  if [ "${MPX_FORCE_LIVE_GLASS:-}" = "L4" ] || [ "${MPX_FORCE_LIVE_GLASS:-}" = "l4" ]; then
    return 0
  fi
  if [ -n "${MPX_FORCE_LIVE_GLASS:-}" ]; then
    return 1
  fi
  DEVMEM="${MISTERPLEX_DEVMEM:-/usr/sbin/devmem}"
  if [ ! -x "$DEVMEM" ]; then
    DEVMEM=$(command -v devmem 2>/dev/null || true)
  fi
  [ -n "${DEVMEM:-}" ] && [ -x "$DEVMEM" ] || return 1
  l4a=$(l4_sample)
  ta=$(t480_sample)
  sleep 0.1
  l4b=$(l4_sample)
  tb=$(t480_sample)
  if [ "$l4a" != "$l4b" ]; then
    return 0
  fi
  if [ "$ta" != "$tb" ]; then
    return 1
  fi
  plxj=$("$DEVMEM" "$L4_PLXJ_ADDR" 32 2>/dev/null || echo 0)
  # case-as-status: grep last-in-function aborts busybox ash on miss.
  case "$plxj" in
    *504C584A*|*504c584a*) return 0 ;;
  esac
  if t480_plxs_magic; then
    return 1
  fi
  l4=$("$DEVMEM" "$L4_PLXS_ADDR" 32 2>/dev/null || echo 0)
  case "$l4" in
    *504C5853*|*504c5853*) return 0 ;;
  esac
  return 1
}

resolve_loaded_rbf() {
  raw=$(tr -d '\000\r\n' <"$RBFNAME_FILE" 2>/dev/null || true)
  base=$(echo "$raw" | sed 's|.*/||; s|\.rbf$||; s|\.RBF$||')
  [ -n "$base" ] || base=Plex
  ut="$UTILITY_DIR"
  case "$base" in
    *[Pp]lex_720*|*[Pp]lex-720*)
      if [ -f "$ut/Plex_720p24.rbf" ]; then echo "$ut/Plex_720p24.rbf"; return 0; fi
      ;;
    *[Pp]lex_240*)
      if [ -f "$ut/Plex_240p15.rbf" ]; then echo "$ut/Plex_240p15.rbf"; return 0; fi
      ;;
    *[Pp]lex_480i*|*[Pp]lex-480i*)
      if [ -f "$ut/Plex_480i.rbf" ]; then echo "$ut/Plex_480i.rbf"; return 0; fi
      ;;
    *[Pp]lex_480p*|*[Pp]lex-480p*)
      if [ -f "$ut/Plex_480p.rbf" ]; then echo "$ut/Plex_480p.rbf"; return 0; fi
      ;;
  esac
  # CORENAME=Plex for every sibling — never hash a generic Plex.rbf.
  if live_glass_is_l4 && [ -f "$ut/Plex_720p24.rbf" ]; then
    echo "$ut/Plex_720p24.rbf"
    return 0
  fi
  if [ -f "$ut/Plex_480p.rbf" ]; then
    echo "$ut/Plex_480p.rbf"
    return 0
  fi
  for cand in "$ut/Plex_240p15.rbf" "$ut/Plex_480i.rbf"; do
    if [ -f "$cand" ]; then
      echo "$cand"
      return 0
    fi
  done
  echo "$LIVE_RBF"
}
