#!/bin/sh
# PID lists from /proc are intentionally split into numeric arguments.
# shellcheck disable=SC2086
set -eu

unset MISTERPLEX_EXPLICIT_PLEX
osd_switch=0
request_switch=0
case "${1:-}" in
  '') mode=Plex ;;
  --plex) mode=Plex; export MISTERPLEX_EXPLICIT_PLEX=1 ;;
  --spelunkr) mode=Spelunkr ;;
  --osd-plex) mode=Plex; osd_switch=1; export MISTERPLEX_EXPLICIT_PLEX=1 ;;
  --osd-spelunkr) mode=Spelunkr; osd_switch=1 ;;
  --request-plex) mode=Plex; request_switch=1; export MISTERPLEX_EXPLICIT_PLEX=1 ;;
  --request-spelunkr) mode=Spelunkr; request_switch=1 ;;
  *) echo "ERROR: unexpected argument: $1" >&2; exit 1 ;;
esac

fail() { echo "ERROR: $*" >&2; exit 1; }

# --- mister_cmd_write begin (bounded write to the Main_MiSTer command FIFO) ---
# /dev/MiSTer_cmd is a FIFO: open(O_WRONLY) BLOCKS until Main_MiSTer has it open for reading. A plain
# `printf ... > /dev/MiSTer_cmd` therefore hangs forever when nothing is reading it at that moment (glass 2026-11-18:
# `mode.sh --plex` blocked in openat on it, core stuck on MENU, in 3 of 4 baseline restores run right after a Spelunkr
# stage was stopped). The write runs in a background subshell that is polled and killed on a timeout; each try is a
# fresh open, so a reader that shows up late still gets the line. Pure POSIX sh (the MiSTer's busybox `timeout`
# flavour differs between builds, so it is not used). MISTER_CMD overrides the path (default /dev/MiSTer_cmd).
#   mister_cmd_write LINE [CORE_REGEX]
#   MISTER_CMD_TIMEOUT  seconds per try (5)    MISTER_CMD_TRIES  tries (3)    MISTER_CMD_READY_WAIT  seconds (10)
#   MISTER_CMD_VERIFY_WAIT  seconds to watch the core name after the last timeout (3)
# Kill-boundary race (review b5501a81): a reader can appear and the line be delivered between the "still blocked"
# check and the kill. So the writer drops a sent-marker after its write completes (best effort: a marker that cannot be
# created, e.g. an unwritable TMPDIR, must never turn a delivered write into a failure; `true`, not `:`, because a failed
# redirection on a POSIX special builtin like `:` exits the shell); after the kill the marker is
# checked first (delivered -> return 0, never re-sent), and CORE_REGEX (a grep -E on the core name, e.g. '^menu$')
# is the backstop for the instant between the write and the marker: a core that already shows the target is
# success, not a retry and not a failure (a Menu bounce must not stop on MENU after the Menu core load landed).
# Returns 0 written/delivered, 124 timed out on every try and the core never showed CORE_REGEX (logged), else the
# write's own error. A path that is not a FIFO (a test file) cannot block and is written directly.
mister_cmd_write() {
  mcw_line=$1
  mcw_expect=${2:-}
  mcw_path=${MISTER_CMD:-/dev/MiSTer_cmd}
  mcw_limit=${MISTER_CMD_TIMEOUT:-5}
  mcw_tries=${MISTER_CMD_TRIES:-3}
  mcw_core=${CORENAME_FILE:-/tmp/CORENAME}
  if [ ! -p "$mcw_path" ]; then
    printf '%s\n' "$mcw_line" > "$mcw_path"
    return $?
  fi
  mcw_mark="${TMPDIR:-/tmp}/.mister-cmd-sent.$$"
  # Core-ready wait: Main_MiSTer publishes /tmp/CORENAME once it is up; do not write before that (bounded).
  mcw_n=0
  while [ ! -s "$mcw_core" ] && [ "$mcw_n" -lt $((${MISTER_CMD_READY_WAIT:-10} * 10)) ]; do
    sleep 0.1
    mcw_n=$((mcw_n + 1))
  done
  [ -s "$mcw_core" ] || echo "WARNING: core name file still empty; writing to $mcw_path anyway" >&2
  mcw_try=1
  while :; do
    rm -f "$mcw_mark"
    ( printf '%s\n' "$mcw_line" > "$mcw_path" && { { true > "$mcw_mark"; } 2>/dev/null; true; } ) &
    mcw_pid=$!
    mcw_ticks=0
    while kill -0 "$mcw_pid" 2>/dev/null; do
      [ "$mcw_ticks" -lt $((mcw_limit * 10)) ] || break
      sleep 0.1
      mcw_ticks=$((mcw_ticks + 1))
    done
    if kill -0 "$mcw_pid" 2>/dev/null; then
      if command -v mcw_hook_before_kill >/dev/null 2>&1; then mcw_hook_before_kill; fi
      kill -9 "$mcw_pid" 2>/dev/null || :
      wait "$mcw_pid" 2>/dev/null || :
      if [ -e "$mcw_mark" ]; then
        rm -f "$mcw_mark"
        echo "NOTE: MiSTer_cmd write completed at the timeout boundary (try $mcw_try/$mcw_tries); not re-sending" >&2
        return 0
      fi
      echo "ERROR: MiSTer_cmd write timed out after ${mcw_limit}s (try $mcw_try/$mcw_tries): nothing is reading $mcw_path" >&2
      if [ -n "$mcw_expect" ] && [ -s "$mcw_core" ] && grep -Eqi -- "$mcw_expect" "$mcw_core" 2>/dev/null; then
        echo "NOTE: core name already matches '$mcw_expect' after the timeout; treating the write as delivered" >&2
        return 0
      fi
      if [ "$mcw_try" -ge "$mcw_tries" ]; then
        # Last try: a delivered core-load command takes a moment to change the core name; watch it before giving up.
        mcw_n=0
        while [ -n "$mcw_expect" ] && [ "$mcw_n" -lt $((${MISTER_CMD_VERIFY_WAIT:-3} * 10)) ]; do
          if [ -s "$mcw_core" ] && grep -Eqi -- "$mcw_expect" "$mcw_core" 2>/dev/null; then
            echo "NOTE: core name now matches '$mcw_expect'; treating the write as delivered" >&2
            return 0
          fi
          sleep 0.1
          mcw_n=$((mcw_n + 1))
        done
        return 124
      fi
      mcw_try=$((mcw_try + 1))
      sleep 1
      continue
    fi
    mcw_rc=0
    wait "$mcw_pid" || mcw_rc=$?
    rm -f "$mcw_mark"
    return "$mcw_rc"
  done
}
# --- mister_cmd_write end ---
state=/tmp/misterplex-service-switch.state
request_dir="${MPX_SERVICE_REQUEST_DIR:-/run/misterplex}"
request="$request_dir/misterplex-service.request"
PLEX_RBF=/media/fat/_Utility/MiSTerPlex.rbf
# 683H N2: the Scripts menu shows a one-shot "Last: <status>" from its previous action (a mode-switch result from an older build).
# Any mode (re)start or switch, OSD ones included (they all run through this script), makes it stale: drop it so
# the menu never reports a mode the device is no longer in. Restart/Repair write theirs again after we return.
rm -f "${MPX_MENU_LOG:-/tmp/misterplex-menu.log}.last" 2>/dev/null || true
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

if [ "$request_switch" = 1 ]; then
  exec 9>/tmp/misterplex-mode-switch.lock
  flock -w 120 9 || fail "another mode switch is still running"
  if [ -z "$(daemon_pids)" ]; then
    echo "No misterplexd running; falling back to direct mode switch (no Plex CFG snapshot)."
    if [ "$mode" = Plex ]; then
      exec "$0" --plex
    else
      exec "$0" --spelunkr
    fi
  fi
  if ! curl -fsS --max-time 2 http://127.0.0.1:3005/resources >/dev/null 2>&1; then
    echo "Companion not ready (core closed?); falling back to direct mode switch (no Plex CFG snapshot)."
    if [ "$mode" = Plex ]; then
      exec "$0" --plex
    else
      exec "$0" --spelunkr
    fi
  fi
  mkdir -m 0700 -p "$request_dir" 2>/dev/null || fail "service request dir unavailable"
  rm -f "$state" "$state.new"
  : > /tmp/misterplex-service.ready
  _umask=$(umask)
  umask 077
  printf '%s\n' "$mode" > "$request.new" || fail "service request write failed"
  chmod 600 "$request.new" 2>/dev/null || true
  mv "$request.new" "$request" || fail "service request publish failed"
  umask "$_umask"
  flock -u 9
  saw_pending=0
  attempt=0
  while [ "$attempt" -lt 80 ]; do
    if [ -r "$state" ]; then
      read -r phase target_mode detail _ < "$state" || phase=
      case "$phase" in
        pending)
          if [ "$target_mode" = "$mode" ]; then
            saw_pending=1
          fi
          ;;
        ready)
          if [ "$target_mode" = "$mode" ] && [ "$saw_pending" = 1 ]; then
            echo "misterplexd service switch via daemon: $mode (CFG snapshot path)"
            loaded=0
            if [ "$mode" = Plex ] && [ "${MISTERPLEX_EXPLICIT_PLEX:-0}" = 1 ]; then
              cur=
              { read -r cur < /tmp/CORENAME || [ -n "$cur" ]; } 2>/dev/null || cur=
              cur=$(echo "$cur" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
              if [ "$cur" = menu ] && [ -s "$PLEX_RBF" ]; then
                mister_cmd_write "load_core $PLEX_RBF" '^plexob$' || fail "explicit Plex from MENU: MiSTer_cmd write failed (rc=$?)"
                loaded=1
              fi
            fi
            # 683G N2: the daemon marks a cross-mode Begin with the core gate closed "ready <mode> snapshot-only":
            # there was no OSD to switch from, it only saved the CFG snapshot. Unless we just loaded the core
            # (H2), nothing switched, so say so and fail instead of reporting success.
            if [ "$detail" = snapshot-only ] && [ "$loaded" != 1 ]; then
              echo "ERROR: --request-$(echo "$mode" | tr '[:upper:]' '[:lower:]') did not switch: the core OSD gate is closed (no OSD to switch from), the daemon only saved the CFG snapshot and NOTHING switched. Use the core OSD Service switch." >&2
              exit 3 # 683G-N2
            fi
            exit 0
          fi
          ;;
        error)
          fail "daemon service switch failed for $mode"
          ;;
      esac
    fi
    sleep 0.25
    attempt=$((attempt + 1))
  done
  if [ "$saw_pending" = 1 ]; then
    cur=
    { read -r cur < /tmp/CORENAME || [ -n "$cur" ]; } 2>/dev/null || cur=
    cur=$(echo "$cur" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
    if [ -n "$cur" ] && [ "$cur" != plexob ] && [ -s "$PLEX_RBF" ]; then
      mister_cmd_write "load_core $PLEX_RBF" '^plexob$' || echo "WARNING: PlexOB reload write failed (rc=$?); continuing to the switch failure" >&2
    fi
    rm -f "$request"
    fail "daemon service switch for $mode did not reach ready within 20s"
  fi
  echo "Daemon did not accept the service request; falling back to direct mode switch (no Plex CFG snapshot)."
  rm -f "$request"
  for pid in $(daemon_pids); do
    ch=$(tr '\000' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n 's/^SPELUNKR_CHANNEL=//p' | head -n1)
    mo=$(tr '\000' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n 's/^MPX_CORE_MANUAL_ONLY=//p' | head -n1)
    [ -n "$ch" ] && export SPELUNKR_CHANNEL="$ch"
    [ -n "$mo" ] && export MPX_CORE_MANUAL_ONLY="$mo"
    break
  done
  export BOOT_KEEP_HELPER=1
  cur=
  { read -r cur < /tmp/CORENAME || [ -n "$cur" ]; } 2>/dev/null || cur=
  cur=$(echo "$cur" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
  if [ -n "$cur" ] && [ "$cur" != plexob ] && [ -s "$PLEX_RBF" ]; then
    mister_cmd_write "load_core $PLEX_RBF" '^plexob$' || fail "PlexOB reload before retry: MiSTer_cmd write failed (rc=$?)"
  fi
  if [ "$mode" = Plex ]; then
    exec "$0" --plex
  else
    exec "$0" --spelunkr
  fi
fi

boot=/media/fat/misterplex/bin/misterplexd-boot-rc3.sh
marker=/tmp/misterplex-spelunkr.active
export MISTERPLEXD_BIN=/media/fat/misterplex/bin/misterplexd
export PLEX_RBF="${PLEX_RBF:-/media/fat/_Utility/MiSTerPlex.rbf}"
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

# /tmp/CORENAME as written by Main_MiSTer has NO trailing newline ("PlexOB"): read -r returns 1 at EOF but still fills
# the variable, so every read below keeps the value when it is non-empty (a bare `|| var=` would blank it).
corename_norm() {
  line=
  { read -r line < /tmp/CORENAME || [ -n "$line" ]; } 2>/dev/null || line=
  echo "$line" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]'
}

# After a MiSTer_cmd core load: if CORENAME already read as $want, wait until
# CORENAME shows a different label once (real transition) before accepting $want.
wait_corename() {
  want=$1
  max=$2
  stale=$3
  if [ "$stale" = 1 ]; then
    attempt=0
    left=0
    while [ "$attempt" -lt 150 ]; do
      cur=$(corename_norm)
      if [ -n "$cur" ] && [ "$cur" != "$want" ]; then
        left=1
        break
      fi
      sleep 0.1
      attempt=$((attempt + 1))
    done
    [ "$left" = 1 ] || fail "core '$want' did not leave stale CORENAME before reload"
  fi
  attempt=0
  while [ "$attempt" -lt "$max" ]; do
    [ "$(corename_norm)" = "$want" ] && return 0
    sleep 0.1
    attempt=$((attempt + 1))
  done
  return 1
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
  if [ ! -e "$saved" ]; then
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
  before_menu=$(corename_norm)
  stale_menu=0
  [ "$before_menu" = menu ] && stale_menu=1
  mister_cmd_write 'load_core /media/fat/menu.rbf' '^menu$' || fail "Menu bounce: MiSTer_cmd write failed (rc=$?)"
  wait_corename menu 100 "$stale_menu" || fail "Menu bounce did not finish"
  plex_retry=0
  while :; do
    before_plex=$(corename_norm)
    stale_plex=0
    [ "$before_plex" = plexob ] && stale_plex=1
    mister_cmd_write "load_core $PLEX_RBF" '^plexob$' || fail "Plex video-settings restore: MiSTer_cmd write failed (rc=$?)"
    if wait_corename plexob 200 "$stale_plex"; then
      break
    fi
    [ "$plex_retry" -lt 1 ] || fail "Plex video-settings restore did not finish"
    plex_retry=$((plex_retry + 1))
  done
  echo "Restored saved Plex video settings through one Menu bounce"
fi
# H2: explicit --plex/--request-plex fallback from MENU must load the Plex
# RBF once before boot wait; manual-only blocks the Spelunkr menu-bounce path above.
if [ "$mode" = Plex ] && [ "${MISTERPLEX_EXPLICIT_PLEX:-0}" = 1 ] && [ "$osd_switch" != 1 ]; then
  cur=$(corename_norm)
  if [ "$cur" = menu ] && [ -s "$PLEX_RBF" ]; then
    mister_cmd_write "load_core $PLEX_RBF" '^plexob$' || fail "explicit Plex from MENU: MiSTer_cmd write failed (rc=$?)"
  fi
fi
phase boot-start-begin
sh "$boot" start 9>&- || fail "startup script start failed"
phase boot-start-done
switch_boot_started=1
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
    _svc_ready=0
    if [ -r "$state" ]; then
      _svc_phase= _svc_target=
      read -r _svc_phase _svc_target < "$state" || true
      if [ "$_svc_phase" = ready ] && [ "$_svc_target" = "$mode" ]; then
        _svc_ready=1
      fi
    fi
    if [ "$osd_switch" != 1 ] ||
       [ "$(cat /tmp/misterplex-service.ready 2>/dev/null)" = "$pid $mode" ] ||
       [ "$_svc_ready" = 1 ]; then
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
cur=$(corename_norm)
if [ "$osd_switch" = 1 ] && [ "${switch_boot_started:-0}" = 1 ] &&
   [ "$cur" = menu ] && [ -s "$PLEX_RBF" ]; then
  # A failed write (124) goes straight to the startup failure: waiting 20 s for a core load that never went out is pointless.
  if mister_cmd_write "load_core $PLEX_RBF" '^plexob$'; then
    wait_corename plexob 200 0 || true
  else
    echo "WARNING: PlexOB load write failed (rc=$?); continuing to the startup failure" >&2
  fi
fi
fail "$mode startup did not become ready with exactly one daemon; see /tmp/misterplexd-boot-rc3.log"
