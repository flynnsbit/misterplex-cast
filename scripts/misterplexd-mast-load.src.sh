#!/bin/sh
# misterplexd-mast-load.sh RBF - the MAST-WEDGE hand-off's ONE core load (source; install the
# copy that `mister-loadcore-guard wrap-script` produces, which inlines the 0-misterplexd guard
# before the load and ends in its digest line). Called only by `misterplexd-boot.sh mast-reload`
# after the handing-off misterplexd has exited.
# P-a (review 01:19/01:25/01:28): right after the guard and right before load_core (the same
# guarded line below), withdraw any stale audio command from the DDR control page, so the new core
# never boots into a valid stale MACT/MACC (a candidate silent-core trigger, glass G2 01:03).
# Hygiene, never a second gate: the ONE load always follows.
# The write: busybox devmem with width 32 (O_SYNC /dev/mem, one mapped page) makes two stores to
# the 64-bit commit word only, 0x30140128-0x3014012F: 0x30140128 := 0 first (the 'MACC' magic half,
# which alone invalidates the command), then 0x3014012C := 0 (the publication half). Nothing else
# is written. Both halves are read back; a non-zero read-back logs ONE loud 'withdraw failed' line.
# R6 (review 02:08/02:17; H264 review/rca/r6-quiesce-input-1005.md §2-§5; RCA §11/§13): no DPB
# write traffic may be in flight when Main's fpga_core_reset(1) lands (fpga_io.cpp:474, before
# do_bridge :489 and the load :490). The feed is already stopped: the launcher runs this only after
# the handing-off misterplexd is gone (MR_EXIT_WAIT), and it holds its single-instance lock for its
# whole run, so no start launcher can start another daemon meanwhile. A daemon started by hand
# outside the launcher is caught by the guard's 0-misterplexd scan right before the load_core write
# (after the drain, the quiet wait and P-a): the guard then refuses the load (rc 97). The guard
# checks placed before the r6_quiesce and pa_withdraw calls are static (these are script
# functions, not loaders): they do not scan. Before P-a, the §5 raw path (reads only, 32-bit
# devmem): quiesced = PLXB 0x30140000 and
# PLXR 0x30140008 magics valid, written [62:32] == consumed [62:32], PLXT 0x30140018 magic valid
# with level [63:32] == 0, the same on 2 reads 50 ms apart. Then a 200 ms quiet wait timed from
# that second read (consumed is not decoded: the encoded-AU buffer can still hold an AU being
# decoded and committed), then P-a, then load_core. Not quiesced within MAST_LOAD_DRAIN_MS (3000):
# the load still happens, with ONE loud 'R6 NOT QUIESCED' line (boot log and FAILED file). No
# PLXB: nothing was ever fed, nothing to drain.
# R6 PORT SUSPECT (review 02:17): with CORENAME Plex and a CAPS row naming a known build (MPC4
# 0x30140070, the build at 0x30140074 in kRecognisedFpgaBuilds), a PLXR or PLXT magic that is
# wrong on 2 reads is the offset-port signature (RCA §12/§13). The loader then logs ONE loud line
# and the D10 0x300FF100-13F and 0x30140000-01F words, still withdraws (P-a), and does NOT load
# (rc 96: the launcher marks refused why=r6-port-suspect, reboot first). Each outcome goes to the
# reload marker as r6=... before the load, so a later silent core is attributed (the R5a line).
# Test-only overrides (unset on the box): MAST_LOAD_FAILED, MAST_LOAD_DRAIN_MS. Tests put a fake
# devmem first in PATH. From the launcher: MAST_LOAD_MARKER (its MARKER), MAST_LOAD_CORENAME.
RBF=$1
[ -n "$RBF" ] || { echo "misterplexd-mast-load: no RBF" >&2; exit 2; }
R6_DRAIN_MS=${MAST_LOAD_DRAIN_MS:-3000}
R6_MARKER=${MAST_LOAD_MARKER:-/tmp/misterplexd.mast-reload}
R6_CORENAME=${MAST_LOAD_CORENAME:-/tmp/CORENAME}
# kRecognisedFpgaBuilds (host/libmisterplex/fpga_build_ids.hpp); a launcher row keeps them equal.
R6_BUILDS="0x4800F36A 0x48004D57 0x48004D40 0x48005D40 0x48005D45 0x48005D50 0x48005D52 0x4800A1FB 0x4800A152 0x48009E36 0x48002731 0x4800C88F 0x48003ECB 0x480058BC 0x480094DD 0x48018AE0 0x48FF65F2 0x487A1BD1 0x48C5385C 0x48C51FF1 0x48CFEE31 0x48A64AED 0x48110015 0x48032F84 0x48038D9A 0x4800F40A 0x48006A7E 0x2400F36A 0x2400F40B 0x24003997 0x24F1A2C4 0x24A10026 0x24C07821 0x4801F40A 0x48F14A0B 0x48C078E2"
# One function of allowlisted commands only (devmem, read, cat, usleep, sleep, echo, printf, date):
# the guard wrap puts a check before every call of a script function, so nothing in here calls
# one. Returns 96 on R6 PORT SUSPECT (no load), else 0 after the 200 ms quiet wait.
r6_quiesce() {
  R6=
  if [ "$((0xFFFFFFFF))" != 4294967295 ]; then
    R6="not-quiesced (this shell's math is not 64-bit: nothing was judged)"
  fi
  cn=$(cat "$R6_CORENAME" 2>/dev/null)
  b0=$(devmem 0x30140070 32 2>/dev/null)
  b1=$(devmem 0x30140074 32 2>/dev/null)
  gate=0
  if [ "$cn" = Plex ] && [ "$b0" = 0x4D504334 ]; then
    case " $R6_BUILDS " in *" $b1 "*) gate=1 ;; esac
  fi
  read -r u _ < /proc/uptime
  cs=${u#*.}
  case $cs in 0?) cs=${cs#0} ;; esac
  t0=$((${u%.*} * 100 + cs))
  prev=
  bad=0
  while [ -z "$R6" ]; do
    c0=$(devmem 0x30140000 32 2>/dev/null)
    c1=$(devmem 0x30140004 32 2>/dev/null)
    r0=$(devmem 0x30140008 32 2>/dev/null)
    r1=$(devmem 0x3014000C 32 2>/dev/null)
    l0=$(devmem 0x30140018 32 2>/dev/null)
    l1=$(devmem 0x3014001C 32 2>/dev/null)
    read -r u _ < /proc/uptime
    cs=${u#*.}
    case $cs in 0?) cs=${cs#0} ;; esac
    ms=$(((${u%.*} * 100 + cs - t0) * 10))
    s="PLXB $c0:$c1 PLXR $r0:$r1 PLXT $l0:$l1"
    if [ "$c0" != 0x504C5842 ]; then
      R6="no-feed (CTRL [$c0] is not PLXB: nothing was fed, nothing to drain)"
      break
    fi
    if [ "$r0" != 0x504C5852 ] || [ "$l0" != 0x504C5854 ]; then
      bad=$((bad + 1))
      if [ "$gate" = 1 ] && [ "$bad" -ge 2 ]; then
        R6="port-suspect (PLXR/PLXT magic wrong on 2 reads, CORENAME Plex, CAPS build $b1: $s)"
        break
      fi
    else
      bad=0
    fi
    key=
    if [ "$r0" = 0x504C5852 ] && [ "$l0" = 0x504C5854 ] &&
      [ $((c1 & 0x7FFFFFFF)) -eq $((r1 & 0x7FFFFFFF)) ] && [ $((l1)) -eq 0 ]; then
      key="$c1:$r1"
    fi
    if [ -n "$key" ] && [ "$key" = "$prev" ]; then
      R6="quiesced (consumed == written, level 0: $s, on 2 reads after $ms ms)"
      break
    fi
    prev=$key
    if [ "$ms" -ge "$R6_DRAIN_MS" ]; then
      R6="not-quiesced (consumed != written or level != 0 within $R6_DRAIN_MS ms: $s)"
      break
    fi
    usleep 50000 2>/dev/null || sleep 0.05 2>/dev/null || sleep 1
  done
  if [ -f "$R6_MARKER" ]; then printf 'r6=%s\n' "$R6" >> "$R6_MARKER"; fi
  case $R6 in
    port-suspect*)
      m="MAST_LOAD R6 PORT SUSPECT: $R6 - NO core load: HPS reboot first (runbook; R6 cannot heal an offset port)"
      echo "!!! $m"
      echo "$(date '+%F %T') $m" >> "${MAST_LOAD_FAILED:-/tmp/misterplexd-boot.FAILED}"
      w=
      for a in 0x300FF100 0x300FF104 0x300FF108 0x300FF10C 0x300FF110 0x300FF114 0x300FF118 \
        0x300FF11C 0x300FF120 0x300FF124 0x300FF128 0x300FF12C 0x300FF130 0x300FF134 0x300FF138 \
        0x300FF13C; do
        w="$w $(devmem "$a" 32 2>/dev/null)"
      done
      echo "MAST_LOAD R6 D10 0x300FF100-13F:$w"
      w=
      for a in 0x30140000 0x30140004 0x30140008 0x3014000C 0x30140010 0x30140014 0x30140018 \
        0x3014001C; do
        w="$w $(devmem "$a" 32 2>/dev/null)"
      done
      echo "MAST_LOAD R6 0x30140000-01F:$w"
      return 96 ;;
    not-quiesced*)
      m="MAST_LOAD R6 NOT QUIESCED: $R6 - the ONE load proceeds"
      echo "!!! $m"
      echo "$(date '+%F %T') $m" >> "${MAST_LOAD_FAILED:-/tmp/misterplexd-boot.FAILED}" ;;
    *) echo "MAST_LOAD: R6 $R6; 200 ms quiet, then P-a and the ONE load" ;;
  esac
  usleep 200000 2>/dev/null || sleep 0.2 2>/dev/null || sleep 1
  return 0
}
pa_withdraw() {
  devmem 0x30140128 32 0
  devmem 0x3014012C 32 0
  lo=$(devmem 0x30140128 32 2>/dev/null)
  hi=$(devmem 0x3014012C 32 2>/dev/null)
  if [ "$lo" = 0x00000000 ] && [ "$hi" = 0x00000000 ]; then
    echo "MAST_LOAD: audio command withdrawn before load_core (commit 0x30140128-0x3014012F := 0, read back 0)"
  else
    m="MAST_LOAD withdraw failed: commit 0x30140128 read back lo=[$lo] hi=[$hi] - the ONE load proceeds"
    echo "!!! $m"
    echo "$(date '+%F %T') $m" >> "${MAST_LOAD_FAILED:-/tmp/misterplexd-boot.FAILED}"
  fi
  return 0
}
# One guarded call: the guard runs before pa_load (it writes load_core), so the R6 quiesce (drain
# and 200 ms quiet wait) and the withdraw follow the guard and precede the load, in that order. On
# R6 PORT SUSPECT the withdraw still runs and nothing is loaded (rc 96).
pa_load() {
  r6_quiesce
  r6rc=$?
  pa_withdraw
  if [ "$r6rc" = 96 ]; then return 96; fi
  echo "load_core $1" > /dev/MiSTer_cmd
}
pa_load "$RBF"
