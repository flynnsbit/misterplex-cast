#!/bin/sh
# PID lists from /proc are intentionally split into numeric arguments.
# shellcheck disable=SC2086
set -eu

unset MISTERPLEX_EXPLICIT_PLEX
osd_switch=0
case "${1:-}" in
  '') mode=Plex ;;
  --plex) mode=Plex; export MISTERPLEX_EXPLICIT_PLEX=1 ;;
  --spelunkr) mode=Spelunkr ;;
  --osd-plex) mode=Plex; osd_switch=1; export MISTERPLEX_EXPLICIT_PLEX=1 ;;
  --osd-spelunkr) mode=Spelunkr; osd_switch=1 ;;
  *) echo "ERROR: unexpected argument: $1" >&2; exit 1 ;;
esac

fail() { echo "ERROR: $*" >&2; exit 1; }
state=/tmp/misterplex-service-switch.state
# shellcheck disable=SC2329 # Invoked by the EXIT trap.
switch_exit() {
  switch_rc=$?
  if [ "$switch_rc" != 0 ]; then
    printf 'error %s\n' "$mode" > "$state.new" && mv "$state.new" "$state"
    if command -v daemon_pids >/dev/null 2>&1 && [ -z "$(daemon_pids)" ]; then
      painter=/media/fat/misterplex/bin/owenb_idle_card
      if [ -x "$painter" ]; then
        "$painter" --card down </dev/null &
      else
        echo "ERROR: no daemon and no error painter: $painter" >&2
      fi
    fi
  fi
}
if [ "$osd_switch" = 1 ]; then
  export MPX_CORE_MANUAL_ONLY=1
fi
boot=/media/fat/misterplex/bin/misterplexd-boot-rc3.sh
marker=/tmp/misterplex-spelunkr.active
export MISTERPLEXD_BIN=/media/fat/misterplex/bin/misterplexd
export PLEX_RBF=/media/fat/_Utility/MiSTerPlex.rbf
export MPX_PREWARM=0 MPX_INPROC_DECODE=0 MPX_STICK_I420=0
export MPX_CA_BUNDLE=/media/fat/linux/misterplex-cacert.pem
export MPX_CORE_MANUAL_ONLY="${MPX_CORE_MANUAL_ONLY:-0}"
export SPELUNKR_CHANNEL="${SPELUNKR_CHANNEL:-primetime}"
unset SPELUNKR
export BOOT_SUPERVISE=1
if [ "$mode" = Spelunkr ]; then
  export SPELUNKR=1
  export BOOT_SUPERVISE=0
  export MPX_PLEXUI=0 MPX_BROWSE_V2=0 MPX_BROWSE_LIBLIST=0
  export MISTERPLEXD_BIN="${MISTERPLEXD_SPELUNKR_BIN:-/media/fat/misterplex/bin/misterplexd-spelunkr}"
else
  # Let Plex's own configuration win, not the outgoing Spelunkr isolation.
  unset MPX_PLEXUI MPX_BROWSE_V2 MPX_BROWSE_LIBLIST
fi

[ -x "$boot" ] || fail "missing startup script: $boot"
[ -x "$MISTERPLEXD_BIN" ] || fail "missing daemon: $MISTERPLEXD_BIN"
[ -s "$PLEX_RBF" ] || fail "missing core: $PLEX_RBF"
cd /root
exec 9>/tmp/misterplex-mode-switch.lock
flock -w 120 9 || fail "another mode switch is still running"
if [ "$osd_switch" = 1 ]; then
  printf 'pending %s\n' "$mode" > "$state.new" && mv "$state.new" "$state"
  trap switch_exit EXIT
fi

# Speed: no fork per /proc entry. `read` is a shell builtin; cat/readlink/tr/grep
# per process made every scan cost seconds on the MiSTer ARM, and this script
# scans a dozen times. A process whose comm names another program is skipped;
# one with no readable comm still gets the exe check as before.
# Measured per-phase timing for the switch (read by glass): one line per phase on
# the switch log, stamped with /proc/uptime (builtin read, no fork).
phase() {
  up=
  { read -r up _ < /proc/uptime; } 2>/dev/null || up='?'
  echo "mode.sh phase $1 uptime=$up"
}

proc_comm() {
  comm=
  { read -r comm < "$1/comm"; } 2>/dev/null || comm=
}

daemon_pids() {
  for proc in /proc/[0-9]*; do
    proc_comm "$proc"
    case "$comm" in
      misterplexd) echo "${proc##*/}"; continue ;;
      misterplexd*|'') ;;
      *) continue ;;
    esac
    case "$(readlink "$proc/exe" 2>/dev/null)" in
      /media/fat/misterplex/bin/misterplexd|/media/fat/misterplex/bin/misterplexd-spelunkr|\
      "/media/fat/misterplex/bin/misterplexd (deleted)"|\
      "/media/fat/misterplex/bin/misterplexd-spelunkr (deleted)")
        echo "${proc##*/}"
        ;;
    esac
  done
}

supervisor_pids() {
  for proc in /proc/[0-9]*; do
    proc_comm "$proc"
    case "$comm" in
      sh|ash|bash|busybox|misterplexd_*|'') ;;
      *) continue ;;
    esac
    [ -r "$proc/cmdline" ] || continue
    if tr '\000' '\n' < "$proc/cmdline" 2>/dev/null |
       grep -q -E '(^|/)misterplexd_supervise\.sh$'; then
      echo "${proc##*/}"
    fi
  done
}

was_spelunkr=0
[ ! -e "$marker" ] || was_spelunkr=1
for pid in $(daemon_pids); do
  if tr '\000' '\n' < "/proc/$pid/environ" | grep -qx 'SPELUNKR=1'; then
    was_spelunkr=1
  fi
done
if [ "$mode" = Plex ] && [ "$was_spelunkr" = 1 ] &&
   [ "${MISTERPLEX_EXPLICIT_PLEX:-0}" != 1 ]; then
  fail "Spelunkr owns the box; use --plex for an explicit operator switch"
fi
if [ "$mode" = Spelunkr ]; then
  : > "$marker"
fi
cfg=/media/fat/config/PlexOB_v1.CFG
if [ "$mode" = Plex ] && [ -e "$cfg.spelunkr-save" ]; then
  [ -s "$cfg.spelunkr-save" ] || fail "empty Spelunkr CFG recovery copy"
fi
if [ "$osd_switch" = 1 ]; then
  saved=$cfg.service-save
  if [ ! -e "$saved" ] && [ "$mode" = Plex ]; then
    echo "WARNING: missing Plex CFG service snapshot; retaining live CFG" >&2
    saved=$cfg.service-live
    source=$cfg
    [ ! -e "$cfg.spelunkr-save" ] || source=$cfg.spelunkr-save
    [ -s "$source" ] || fail "missing live Plex CFG"
    cp "$source" "$saved" || fail "Plex live CFG preservation failed"
  fi
  [ -s "$saved" ] || fail "missing or empty Plex CFG service snapshot"
  if ! cp "$saved" "$cfg.service-tmp" || ! cmp "$saved" "$cfg.service-tmp"; then
    fail "Plex CFG service restore preparation failed"
  fi
fi
phase begin
echo "Switching misterplexd to $mode mode"

signal_pids() {
  signal=$1
  shift
  for pid in "$@"; do
    if ! kill "-$signal" "$pid" 2>/dev/null; then
      if kill -0 "$pid" 2>/dev/null; then
        fail "cannot send $signal to pid $pid"
      fi
    fi
  done
}

# Speed: poll every 0.2 s (fractional sleep is already used below for the Menu
# bounce); the TERM and KILL budgets stay 10 s each.
poll_tick=0.2
poll_max=50
stop_all() {
  finder=$1
  pids=$("$finder")
  [ -n "$pids" ] || return 0
  signal_pids TERM $pids
  attempt=0
  while [ "$attempt" -lt "$poll_max" ]; do
    pids=$("$finder")
    [ -n "$pids" ] || return 0
    sleep "$poll_tick"
    attempt=$((attempt + 1))
  done
  signal_pids KILL $pids
  attempt=0
  while [ "$attempt" -lt "$poll_max" ]; do
    pids=$("$finder")
    [ -n "$pids" ] || return 0
    sleep "$poll_tick"
    attempt=$((attempt + 1))
  done
  fail "$finder still running: $pids"
}

# An OSD service switch keeps the AirPlay transcode helper running: it is the
# same helper for both services, and stopping/copying/restarting it cost
# seconds of the switch (boot script honours BOOT_KEEP_HELPER).
if [ "$osd_switch" = 1 ]; then
  export BOOT_KEEP_HELPER=1
fi
phase boot-stop-begin
sh "$boot" stop 9>&- || fail "startup script stop failed"
phase boot-stop-done
stop_all supervisor_pids
phase supervisor-stopped
stop_all daemon_pids
phase daemon-stopped
# P2-a: helper runs in its own pgroup; kill -9 of misterplexd leaves it.
pkill -f "[y]t_warm_helper\.py" 2>/dev/null || true
sleep 0.3
[ -z "$(supervisor_pids)" ] || fail "supervisor respawned during stop"
[ -z "$(daemon_pids)" ] || fail "daemon respawned during stop"
echo "Verified no misterplexd remains; starting $mode mode"
phase verified
if [ "$mode" = Plex ]; then
  recovery=$cfg.spelunkr-save
  if [ -e "$recovery" ]; then
    cp "$recovery" "$cfg.mpxtmp" || fail "Plex CFG restore copy failed"
    cmp "$recovery" "$cfg.mpxtmp" || fail "Plex CFG restore copy differs"
    mv "$cfg.mpxtmp" "$cfg" || fail "Plex CFG restore failed"
    cmp "$recovery" "$cfg" || fail "Plex CFG restore differs"
    rm "$recovery" || fail "Plex CFG recovery cleanup failed"
  fi
  rm -f "$marker"
fi
if [ "$osd_switch" = 1 ]; then
  # The OSD path never bounces a core. Restore saved video settings live,
  # and retain the complete original CFG including unknown trailing bytes.
  : > /tmp/misterplex-service.ready
  if ! mv "$cfg.service-tmp" "$cfg" || ! cmp "$saved" "$cfg"; then
    fail "Plex CFG service restore failed"
  fi
fi
if [ "$mode" = Plex ] && [ "$was_spelunkr" = 1 ] && [ "$MPX_CORE_MANUAL_ONLY" != 1 ]; then
  # Reload saved OSD settings, not the source-specific Refresh/Video Output
  # left live by Spelunkr. A same-core daemon restart does not restore them.
  [ -s /media/fat/menu.rbf ] || fail "missing Menu core for video-settings restore"
  printf '%s\n' 'load_core /media/fat/menu.rbf' > /dev/MiSTer_cmd
  attempt=0
  while ! grep -qi '^menu$' /tmp/CORENAME; do
    [ "$attempt" -lt 100 ] || fail "Menu bounce did not finish"
    sleep 0.1
    attempt=$((attempt + 1))
  done
  printf 'load_core %s\n' "$PLEX_RBF" > /dev/MiSTer_cmd
  attempt=0
  while ! grep -qi '^plexob$' /tmp/CORENAME; do
    [ "$attempt" -lt 200 ] || fail "Plex video-settings restore did not finish"
    sleep 0.1
    attempt=$((attempt + 1))
  done
  echo "Restored saved Plex video settings through one Menu bounce"
fi
phase boot-start-begin
sh "$boot" start 9>&- || fail "startup script start failed"
phase boot-start-done
# Ready wait: 60 s wall clock, as before. The 0.25 s poll alone would let a slow
# curl stretch the old 60-iteration bound four-fold, so also bound by uptime.
wait_start=
{ read -r wait_start _ < /proc/uptime; } 2>/dev/null || wait_start=
wait_start=${wait_start%%.*}
attempt=0
while [ "$attempt" -lt 240 ]; do
  if [ -n "$wait_start" ]; then
    wait_now=
    { read -r wait_now _ < /proc/uptime; } 2>/dev/null || wait_now=
    wait_now=${wait_now%%.*}
    [ -z "$wait_now" ] || [ "$wait_now" -lt $((wait_start + 60)) ] || break
  fi
  pids=$(daemon_pids)
  set -- $pids
  if [ "$#" -eq 1 ] &&
     curl -fsS --max-time 2 http://127.0.0.1:3005/resources >/dev/null 2>&1; then
    pid=$1
    [ "$(readlink "/proc/$pid/exe")" = "$MISTERPLEXD_BIN" ] ||
      fail "pid $pid is not running $MISTERPLEXD_BIN"
    if [ "$mode" = Spelunkr ]; then
      tr '\000' '\n' < "/proc/$pid/environ" | grep -qx 'SPELUNKR=1' ||
        fail "pid $pid is not in Spelunkr mode"
    elif tr '\000' '\n' < "/proc/$pid/environ" | grep -q '^SPELUNKR='; then
      fail "SPELUNKR is still set for pid $pid"
    fi
    if [ "$mode" = Spelunkr ]; then
      [ -z "$(supervisor_pids)" ] || fail "supervisor running in Spelunkr mode"
      tr '\000' '\n' < "/proc/$pid/environ" | grep -qx 'BOOT_SUPERVISE=0' ||
        fail "Spelunkr supervisor isolation missing"
      for key in MPX_PLEXUI MPX_BROWSE_V2 MPX_BROWSE_LIBLIST; do
        tr '\000' '\n' < "/proc/$pid/environ" | grep -qx "$key=0" ||
          fail "Spelunkr $key isolation missing"
      done
    fi
    if [ "$osd_switch" != 1 ] ||
       [ "$(cat /tmp/misterplex-service.ready 2>/dev/null)" = "$pid $mode" ]; then
      if [ "$osd_switch" = 1 ]; then
        cmp "$saved" "$cfg" || fail "Plex CFG changed during startup"
      fi
      if [ "$mode" = Plex ] && [ -e "$cfg.service-save" ]; then
        mv "$cfg.service-save" "$cfg.service-restored" ||
          fail "Plex CFG service snapshot retirement failed"
      fi
      if [ "$osd_switch" = 1 ]; then
        printf 'ready %s\n' "$mode" > "$state.new" && mv "$state.new" "$state"
      fi
      phase ready
      echo "misterplexd mode: $mode; one daemon pid=$pid; /resources and mode ready"
      exit 0
    fi
  fi
  sleep 0.25
  attempt=$((attempt + 1))
done
fail "$mode startup did not become ready with exactly one daemon; see /tmp/misterplexd-boot-rc3.log"
