#!/bin/sh
# rc3 boot: after a reboot the MiSTer menu core is up (the FPGA fallback pattern is not
# something to show). By default this launcher leaves the selected core alone,
# paints the "daemon not running"
# card (owenb_idle_card) until the daemon takes over, then starts
# /media/fat/misterplex/bin/misterplexd so the idle screen is the chevron. It does not use misterplexd-boot.sh (that file is pinned to an
# older cand binary and does not load a core) and it does not use the lab-only
# misterplexd-boot-mast.sh.
#
# Sequence
#   1. Read CORENAME (default $ROOT/tmp/CORENAME).
#   2. Only when explicitly opted in with MPX_CORE_MANUAL_ONLY=0 (the Scripts
#      mode launcher default), if it is not Plex or PlexOB, write one
#      `load_core <rbf>` line to
#      MISTER_CMD (default /dev/MiSTer_cmd) and wait until CORENAME changes.
#      RBF order: $PLEX_RBF (empty or unset counts as unset), else
#      $ROOT/media/fat/PlexOB.rbf, else $ROOT/media/fat/_Utility/PlexOB.rbf,
#      else, only if an older install left it, $ROOT/media/fat/_Utility/Plex.rbf
#      (rc3 FPGA pin prefix 999800cf, host/libmisterplex/legacy_core_prefix.hpp).
#      PlexOB is preferred over Plex.rbf: glass Job Q (10-24) cold-booted a card
#      with both and the old order loaded the 480p Plex core, so the PlexOB
#      240p/480i features were absent. The private release ships PlexOB.rbf
#      only. The two PlexOB copies must be the same file; prod load_core
#      prefers /media/fat/PlexOB.rbf.
#   3. Wait up to BOOT_NET_WAIT seconds (default 60) for a default route and
#      a non-169.254 IPv4 on that route, THEN start misterplexd. The wait
#      always runs, including when the name is already in the conf: skipping
#      it launched the daemon before the NIC was up and AirPlay registered
#      once with no local address. The core load above is not delayed. After
#      the bound the daemon still starts. A name already in the conf is
#      kept, byte for byte, including an older MiSTerPlex-<full-ip> value.
#      When the conf has no identity yet, other kept sources are the
#      environment (FRIENDLY_NAME / MPX_ID), --name / --id on a
#      misterplexd-boot.sh or misterplexd_supervise.sh line in
#      user-startup.sh (commented-out lines count), and a persisted identity
#      file. NAME= / ID= in the legacy misterplexd-boot.sh is not a source.
#      With none of those, pick a non-169.254 IPv4 — the default-route
#      source when this wait found one, otherwise any non-link-local — and
#      write FRIENDLY_NAME and PLAYER_ID once as the same string
#      misterplex-<last three octets> (192.0.2.229 is
#      misterplex-0.2.229). A later boot does not rewrite it. Link-local
#      is never used. The no-network fallback (MiSTerPlex / misterplex-1)
#      is this boot only and is never written. Nothing is written at stop
#      or shutdown.
#      $1=stop: SIGINT the daemon, then SIGKILL if it is still alive, and
#      exit. No core load, no identity, no save. Any other argument is a
#      no-op. start boots. An empty argument boots unless a shutdown is
#      already in progress (see below).
#   4. Launch with GAIRPLAY=1 GYT=1, MPX_PREWARM=0 (unless the environment sets
#      it), MPX_LEGACY_CORE_PREFIX=999800cf, MISTERPLEXD_BIN= from the conf when
#      it names an executable, and
#      --conf /media/fat/linux/misterplex.conf (the misterplex/misterplex.conf
#      path only when the linux conf is missing).
#   5. In the background, at most once a day after the clock is plausible
#      (year >= 2020), run update_ytdlp.sh. A bundled yt-dlp zip is unpacked
#      first so a fresh card works offline. Set YTDLP_SELF_UPDATE=0 to skip.
#
# Install: one background line in /media/fat/linux/user-startup.sh (and in
# _user-startup.sh when that file already exists). "$1" is required.
# S99user calls `user-startup.sh stop`; without "$1" the hook never sees
# stop and does a full boot at shutdown.
#   /media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
# `misterplexd-boot-rc3.sh install` writes that line. An existing no-arg
# line is rewritten in place. The edit is idempotent and keeps
# user-startup.sh.bak.<stamp>. Comment out any misterplexd-boot.sh line so
# the cand launcher does not also run.
#
# Shutdown with no argument: MiSTer busybox init runs /etc/init.d/rcK for
# ::shutdown, and rcK is still alive while S99user runs user-startup.sh stop.
# user-startup.sh backgrounds this hook and can exit before the hook reads
# its argument, so a no-arg line races. rcK and S99user invoke the script as
# `/bin/sh <script>`, so argv0 is sh and the script name is argv[1]. Before
# load_core, an empty argument refuses to launch or save when runlevel is 0
# or 6, when a live cmdline's argv0 or argv[1] is rcK, or when argv0 or
# argv[1] is S99user/user-startup.sh and another argv is stop. A daemon pid
# is not the signal: at shutdown the daemon is often already dead, which is
# when the old hook booted again. BOOT_SHUTDOWN=1, BOOT_RUNLEVEL=0|6, and
# BOOT_PROC (a fake /proc) are the test overrides.
#
# Test overrides: ROOT, CORENAME_FILE, MISTER_CMD, PLEX_RBF, MISTERPLEXD_BIN,
# BOOT_CORE_WAIT, BOOT_LOG, BOOT_NET_WAIT, BOOT_IPV4 (stands in for the
# default-route address), BOOT_ROUTE_IPV4, BOOT_NO_NET, BOOT_PROC_ROUTE,
# BOOT_SHUTDOWN, BOOT_RUNLEVEL, BOOT_PROC, BOOT_HOOK_PATH, BOOT_INSTALL_FILES,
# YTDLP_SELF_UPDATE.

ROOT="${ROOT:-}"
export MPX_CORE_MANUAL_ONLY="${MPX_CORE_MANUAL_ONLY:-1}"
CORENAME_FILE="${CORENAME_FILE:-$ROOT/tmp/CORENAME}"
MISTER_CMD="${MISTER_CMD:-/dev/MiSTer_cmd}"
FAT="$ROOT/media/fat"
BIN="${MISTERPLEXD_BIN:-$FAT/misterplex/bin/misterplexd}"
WAIT="${BOOT_CORE_WAIT:-20}"
NET_WAIT="${BOOT_NET_WAIT:-60}"
LOG="${BOOT_LOG:-$ROOT/tmp/misterplexd-boot-rc3.log}"
PREFIX="999800cf"
PORT="${BOOT_PORT:-3005}"

# PLEX_RBF is read only as ${PLEX_RBF:-}, so the hook is safe under `set -u`.
pick_rbf() {
  if [ -n "${PLEX_RBF:-}" ]; then
    RBF="$PLEX_RBF"
  elif [ -f "$FAT/MiSTerPlex.rbf" ]; then
    RBF="$FAT/MiSTerPlex.rbf"
  elif [ -f "$FAT/_Utility/MiSTerPlex.rbf" ]; then
    RBF="$FAT/_Utility/MiSTerPlex.rbf"
  elif [ -f "$FAT/_Console/MiSTerPlex.rbf" ]; then
    RBF="$FAT/_Console/MiSTerPlex.rbf"
  elif [ -f "$FAT/cores/MiSTerPlex.rbf" ]; then
    RBF="$FAT/cores/MiSTerPlex.rbf"
  elif [ -f "$FAT/PlexOB.rbf" ]; then
    RBF="$FAT/PlexOB.rbf"
  elif [ -f "$FAT/_Utility/PlexOB.rbf" ]; then
    RBF="$FAT/_Utility/PlexOB.rbf"
  elif [ -f "$FAT/_Utility/Plex.rbf" ]; then
    RBF="$FAT/_Utility/Plex.rbf"
  else
    RBF="$FAT/MiSTerPlex.rbf"
  fi
}
pick_rbf

# After a new MiSTerPlex.rbf is on the card and its md5 matches the shipped
# MiSTerPlex.rbf.md5, drop the old filename, including _Console and cores.
# A missing checksum, or a mismatch, leaves every old PlexOB.rbf in place.
# A second successful run finds nothing to remove.
retire_old_plexob() {
  new=
  for n in "$FAT/MiSTerPlex.rbf" "$FAT/_Utility/MiSTerPlex.rbf" \
           "$FAT/_Console/MiSTerPlex.rbf" "$FAT/cores/MiSTerPlex.rbf"; do
    if [ -f "$n" ]; then
      new=$n
      break
    fi
  done
  [ -n "$new" ] || return 0
  want=
  for sum in "${new}.md5" "$FAT/MiSTerPlex.rbf.md5" "$FAT/cores/MiSTerPlex.rbf.md5" \
             "$FAT/misterplex/cores/MiSTerPlex.rbf.md5"; do
    if [ -f "$sum" ]; then
      want=$(awk 'NR==1 { print tolower($1); exit }' "$sum")
      break
    fi
  done
  if [ -z "$want" ]; then
    log "kept old PlexOB.rbf: shipped MiSTerPlex.rbf.md5 not found"
    return 0
  fi
  got=$(md5sum "$new" | awk '{ print tolower($1) }')
  if [ "$got" != "$want" ]; then
    log "kept old PlexOB.rbf: $new md5 $got != $want"
    return 0
  fi
  for old in "$FAT/PlexOB.rbf" "$FAT/_Utility/PlexOB.rbf" \
             "$FAT/_Console/PlexOB.rbf" "$FAT/cores/PlexOB.rbf"; do
    if [ -e "$old" ] && [ ! -d "$old" ]; then
      rm -f "$old" || return 1
      log "removed old core $old"
    fi
  done
  return 0
}

mkdir -p "$(dirname "$LOG")" 2>/dev/null || true
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# Supervisor (misterplexd_supervise.sh), started by this hook unless
# BOOT_SUPERVISE=0. It respawns the daemon after a crash or kill -9. It must
# be stopped BEFORE the daemon or it would respawn the daemon we are stopping.
SUP_PIDFILE="${BOOT_SUP_PID:-$ROOT/tmp/misterplexd-supervise.pid}"
SUP_LOCK="${BOOT_SUP_LOCK:-$ROOT/tmp/misterplexd-supervise.lock}"

stop_supervisor() {
  [ -f "$SUP_PIDFILE" ] || return 0
  spid=$(cat "$SUP_PIDFILE" 2>/dev/null || true)
  case "$spid" in
    ''|*[!0-9]*) ;;
    *)
      if kill -0 "$spid" 2>/dev/null; then
        log "stop: supervisor TERM pid=$spid"
        kill -TERM "$spid" 2>/dev/null || true
        i=0
        # sh runs a trapped TERM only after a foreground sleep ends (<= 60 s
        # backoff), so give it a few seconds and then KILL it. Poll every
        # 0.2 s (same 5 s budget): the supervisor normally exits in well
        # under a second and a whole-second poll was the floor of every stop.
        while [ "$i" -lt 25 ]; do
          kill -0 "$spid" 2>/dev/null || break
          i=$((i + 1))
          sleep 0.2
        done
        if kill -0 "$spid" 2>/dev/null; then
          log "stop: supervisor KILL pid=$spid"
          kill -KILL "$spid" 2>/dev/null || true
        fi
      fi
      ;;
  esac
  rm -f "$SUP_PIDFILE"
  rm -rf "$SUP_LOCK"
}

stop_daemon() {
  stop_supervisor
  pidfile="$ROOT/tmp/misterplexd.pid"
  if [ -f "$pidfile" ]; then
    pid=$(cat "$pidfile" 2>/dev/null || true)
    case "$pid" in
      ''|*[!0-9]*) ;;
      *)
        if kill -0 "$pid" 2>/dev/null; then
          log "stop: INT pid=$pid"
          kill -INT "$pid" 2>/dev/null || true
          i=0
          while [ "$i" -lt 25 ]; do
            kill -0 "$pid" 2>/dev/null || break
            i=$((i + 1))
            sleep 0.2
          done
          if kill -0 "$pid" 2>/dev/null; then
            log "stop: KILL pid=$pid"
            kill -KILL "$pid" 2>/dev/null || true
          fi
        fi
        ;;
    esac
    rm -f "$pidfile"
  fi
  # BOOT_KEEP_HELPER=1 (set by misterplex_mode.sh for an OSD service switch):
  # the AirPlay transcode helper serves both services, so leave it running.
  if [ "${BOOT_KEEP_HELPER:-0}" = 1 ]; then
    log "stop: keeping airplay helper across service switch"
  else
    stop_gcast_helper
  fi
}

# On-card AirPlay helper (airplay_xcode.py; gcast_xcode.py is the compat name).
# Started before the daemon so a fresh card's loopback :18781 is up when the
# ready line is painted. Stopped with the daemon. Without an ffmpeg (libx264)
# the helper is not started and the daemon leaves AirPlay off the ready card.
# Pid files: airplay-xcode.pid is primary. gcast-xcode.pid is written too so an
# older stop, and host tests, still see the helper.
AIRPLAY_HELPER_PID="${BOOT_AIRPLAY_PID:-${BOOT_GCAST_PID:-$ROOT/tmp/airplay-xcode.pid}}"
GCAST_HELPER_PID="${BOOT_GCAST_PID:-$ROOT/tmp/gcast-xcode.pid}"

_stop_helper_pid() {
  pidfile="$1"
  [ -f "$pidfile" ] || return 0
  pid=$(cat "$pidfile" 2>/dev/null || true)
  case "$pid" in ''|*[!0-9]*) rm -f "$pidfile"; return 0 ;; esac
  if kill -0 "$pid" 2>/dev/null; then
    log "airplay helper: stop pid=$pid"
    kill -INT "$pid" 2>/dev/null || true
    i=0
    while [ "$i" -lt 15 ]; do
      kill -0 "$pid" 2>/dev/null || break
      i=$((i + 1))
      sleep 0.2
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
  rm -f "$pidfile"
}

stop_airplay_helper() {
  _stop_helper_pid "$AIRPLAY_HELPER_PID"
  if [ "$GCAST_HELPER_PID" != "$AIRPLAY_HELPER_PID" ]; then
    _stop_helper_pid "$GCAST_HELPER_PID"
  fi
}
stop_gcast_helper() { stop_airplay_helper; }

# Fixed search list. linux/airplay (flat, then one nested scp) then the compat
# linux/gcast layouts. Nothing else is searched. BOOT_AIRPLAY_DIR / BOOT_GCAST_DIR
# still name one directory (nested gcast/ or airplay/ inside it is the only extra step).
_helper_has_py() {
  [ -f "$1/airplay_xcode.py" ] || [ -f "$1/gcast_xcode.py" ]
}

# Copy only when the destination is missing or differs. Small files are
# compared byte for byte; large binaries (ffmpeg) by size and mtime, because
# re-copying them to the SD on every start cost 2-12 s of a service switch.
_cp_if_changed() {
  s=$1
  d=$2
  if [ -f "$d" ]; then
    ssz=$(stat -c %s "$s" 2>/dev/null || true)
    dsz=$(stat -c %s "$d" 2>/dev/null || true)
    if [ -n "$ssz" ] && [ "$ssz" = "$dsz" ]; then
      if [ "$ssz" -lt 1048576 ]; then
        cmp -s "$s" "$d" && return 0
      elif [ ! "$s" -nt "$d" ]; then
        return 0
      fi
    fi
  fi
  cp -f "$s" "$d"
}

start_airplay_helper() {
  if [ "${BOOT_KEEP_HELPER:-0}" = 1 ]; then
    for kp in "$AIRPLAY_HELPER_PID" "$GCAST_HELPER_PID"; do
      [ -f "$kp" ] || continue
      kold=$(cat "$kp" 2>/dev/null || true)
      case "$kold" in ''|*[!0-9]*) continue ;; esac
      if kill -0 "$kold" 2>/dev/null; then
        log "airplay helper: already running pid=$kold (kept across service switch)"
        return 0
      fi
    done
  fi
  if [ -n "${BOOT_AIRPLAY_DIR:-${BOOT_GCAST_DIR:-}}" ]; then
    src="${BOOT_AIRPLAY_DIR:-$BOOT_GCAST_DIR}"
    if ! _helper_has_py "$src" && _helper_has_py "$src/airplay"; then
      log "airplay helper: using nested layout $src/airplay"
      src="$src/airplay"
    elif ! _helper_has_py "$src" && _helper_has_py "$src/gcast"; then
      log "airplay helper: using nested layout $src/gcast"
      src="$src/gcast"
    fi
  else
    src="$FAT/linux/gcast"
    for cand in \
      "$FAT/linux/airplay" \
      "$FAT/linux/airplay/airplay" \
      "$FAT/linux/gcast" \
      "$FAT/linux/gcast/gcast"
    do
      if _helper_has_py "$cand"; then
        src=$cand
        break
      fi
    done
  fi
  dest="${BOOT_AIRPLAY_ROOT:-${BOOT_GCAST_ROOT:-$FAT/misterplex}}"
  mkdir -p "$dest" "$(dirname "$AIRPLAY_HELPER_PID")" "$(dirname "$GCAST_HELPER_PID")" 2>/dev/null || true
  if _helper_has_py "$src"; then
    if [ -f "$src/airplay_xcode.py" ]; then
      _cp_if_changed "$src/airplay_xcode.py" "$dest/airplay_xcode.py"
      _cp_if_changed "$src/airplay_xcode.py" "$dest/gcast_xcode.py"
    else
      _cp_if_changed "$src/gcast_xcode.py" "$dest/airplay_xcode.py"
      _cp_if_changed "$src/gcast_xcode.py" "$dest/gcast_xcode.py"
    fi
    if [ -f "$src/run-airplay-xcode.sh" ]; then
      _cp_if_changed "$src/run-airplay-xcode.sh" "$dest/run-airplay-xcode.sh"
      chmod +x "$dest/run-airplay-xcode.sh" 2>/dev/null || true
    fi
    if [ -f "$src/run-gcast-xcode.sh" ]; then
      _cp_if_changed "$src/run-gcast-xcode.sh" "$dest/run-gcast-xcode.sh"
      chmod +x "$dest/run-gcast-xcode.sh" 2>/dev/null || true
    fi
    if [ -f "$src/ffmpeg" ]; then
      mkdir -p "$dest/bin"
      _cp_if_changed "$src/ffmpeg" "$dest/bin/ffmpeg"
      chmod +x "$dest/bin/ffmpeg" 2>/dev/null || true
    fi
    if [ -f "$src/ffprobe" ]; then
      mkdir -p "$dest/bin"
      _cp_if_changed "$src/ffprobe" "$dest/bin/ffprobe"
      chmod +x "$dest/bin/ffprobe" 2>/dev/null || true
    fi
  fi
  ca_src="${BOOT_CA_BUNDLE:-$FAT/linux/misterplex-cacert.pem}"
  if [ -s "$ca_src" ]; then
    mkdir -p "$dest/ca"
    _cp_if_changed "$ca_src" "$dest/ca/misterplex-cacert.pem"
  fi
  ff="${AIRPLAY_FFMPEG:-${GCAST_FFMPEG:-}}"
  if [ -z "$ff" ] && [ -x "$dest/bin/ffmpeg" ]; then
    ff="$dest/bin/ffmpeg"
  fi
  if [ -z "$ff" ]; then
    log "airplay helper: no ffmpeg; AirPlay stays off the ready card"
    return 0
  fi
  run=""
  if [ -f "$dest/run-airplay-xcode.sh" ]; then
    run="$dest/run-airplay-xcode.sh"
  elif [ -f "$dest/run-gcast-xcode.sh" ]; then
    run="$dest/run-gcast-xcode.sh"
  fi
  if [ -z "$run" ] || [ ! -f "$dest/gcast_xcode.py" ]; then
    log "airplay helper: script missing under $dest"
    return 0
  fi
  if [ -f "$AIRPLAY_HELPER_PID" ]; then
    old=$(cat "$AIRPLAY_HELPER_PID" 2>/dev/null || true)
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
      log "airplay helper: already running pid=$old"
      return 0
    fi
  fi
  if [ -f "$GCAST_HELPER_PID" ]; then
    old=$(cat "$GCAST_HELPER_PID" 2>/dev/null || true)
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
      log "airplay helper: already running pid=$old"
      return 0
    fi
  fi
  bind="${AIRPLAY_XCODE_BIND:-${GCAST_XCODE_BIND:-127.0.0.1}}"
  port="${AIRPLAY_XCODE_PORT:-${GCAST_XCODE_PORT:-18781}}"
  log "airplay helper: start $run"
  AIRPLAY_MISTERPLEX_ROOT="$dest" GCAST_MISTERPLEX_ROOT="$dest" \
    AIRPLAY_FFMPEG="$ff" GCAST_FFMPEG="$ff" \
    AIRPLAY_XCODE_BIND="$bind" GCAST_XCODE_BIND="$bind" \
    AIRPLAY_XCODE_PORT="$port" GCAST_XCODE_PORT="$port" \
    "$run" >> "$LOG" 2>&1 &
  echo $! > "$AIRPLAY_HELPER_PID"
  echo $! > "$GCAST_HELPER_PID"
  log "airplay helper pid=$(cat "$AIRPLAY_HELPER_PID")"
}
start_gcast_helper() { start_airplay_helper; }

# Drop the boot line from user-startup.sh. Does not delete cores or the conf.
uninstall_startup_hook() {
  hook="${BOOT_HOOK_PATH:-/media/fat/misterplex/bin/misterplexd-boot-rc3.sh}"
  if [ -n "${BOOT_INSTALL_FILES:-}" ]; then
    files=$BOOT_INSTALL_FILES
  else
    files="$FAT/linux/user-startup.sh"
    if [ -f "$FAT/linux/_user-startup.sh" ]; then
      files="$files $FAT/linux/_user-startup.sh"
    fi
  fi
  for f in $files; do
    [ -f "$f" ] || continue
    tmp="$f.mpx-tmp"
    if ! awk -v hook="$hook" '
      index($0, hook) == 0 { print }
    ' "$f" > "$tmp"; then
      rm -f "$tmp"
      log "uninstall: rewrite failed: $f"
      return 1
    fi
    mv -f "$tmp" "$f"
    log "uninstall: removed hook from $f"
  done
}

# Empty $1 during rcK / runlevel 0 or 6 is shutdown. See the header.
shutdown_in_progress() {
  case "${BOOT_SHUTDOWN:-}" in
    1|true|yes) return 0 ;;
  esac
  case "${BOOT_RUNLEVEL:-}" in
    0|6) return 0 ;;
  esac
  if [ -z "${BOOT_RUNLEVEL+x}" ] && command -v runlevel >/dev/null 2>&1; then
    rl=$(runlevel 2>/dev/null | awk '{print $NF}')
    case "$rl" in
      0|6) return 0 ;;
    esac
  fi
  proc=${BOOT_PROC:-/proc}
  [ -d "$proc" ] || return 1
  for cmdf in "$proc"/[0-9]*/cmdline; do
    [ -r "$cmdf" ] || continue
    args=$(tr '\000' '\n' < "$cmdf" 2>/dev/null || true)
    argv0=$(printf '%s\n' "$args" | head -n 1 || true)
    argv1=$(printf '%s\n' "$args" | sed -n '2p' || true)
    for arg in "$argv0" "$argv1"; do
      case "$arg" in
        rcK|*/rcK|*/init.d/rcK) return 0 ;;
      esac
    done
    for arg in "$argv0" "$argv1"; do
      case "$arg" in
        *S99user*|*user-startup.sh)
          if printf '%s\n' "$args" | grep -qx 'stop'; then
            return 0
          fi
          ;;
      esac
    done
  done
  return 1
}

# Rewrite user-startup.sh so S99user's `stop` reaches this script.
install_startup_hook() {
  hook="${BOOT_HOOK_PATH:-/media/fat/misterplex/bin/misterplexd-boot-rc3.sh}"
  if [ -n "${BOOT_INSTALL_FILES:-}" ]; then
    files=$BOOT_INSTALL_FILES
  else
    files="$FAT/linux/user-startup.sh"
    if [ -f "$FAT/linux/_user-startup.sh" ]; then
      files="$files $FAT/linux/_user-startup.sh"
    fi
  fi
  stamp="${BOOT_INSTALL_STAMP:-$(date +%Y%m%d%H%M%S)}"
  for f in $files; do
    mkdir -p "$(dirname "$f")" 2>/dev/null || true
    [ -f "$f" ] || : > "$f"
    tmp="$f.mpx-tmp"
    if ! awk -v hook="$hook" '
      BEGIN { found = 0 }
      {
        if ($0 ~ /^[ \t]*#/) { print; next }
        if (index($0, hook) == 0) { print; next }
        if (found) next
        print hook " \"$1\" &"
        found = 1
      }
      END { if (!found) print hook " \"$1\" &" }
    ' "$f" > "$tmp"; then
      rm -f "$tmp"
      log "install: rewrite failed: $f"
      return 1
    fi
    if cmp -s "$f" "$tmp"; then
      rm -f "$tmp"
      log "install: $f already has the hook"
      continue
    fi
    cp "$f" "$f.bak.$stamp"   # exFAT: no -a (no modes or owners)
    mv -f "$tmp" "$f"
    log "install: rewrote $f (backup $f.bak.$stamp)"
  done
}

# S99user calls user-startup.sh stop at shutdown. That must not boot again.
# install is the card setup step (not a boot). An empty argument still boots
# unless shutdown_in_progress says rcK is already running.
case "${1:-}" in
  install)
    retire_old_plexob
    install_startup_hook
    exit $?
    ;;
  uninstall)
    log "uninstall: argument"
    stop_daemon
    uninstall_startup_hook
    exit $?
    ;;
  start) ;;
  stop)
    log "stop: argument"
    stop_daemon
    exit 0
    ;;
  '')
    if shutdown_in_progress; then
      log "stop: shutdown in progress (no argument); not launching"
      stop_daemon
      exit 0
    fi
    ;;
  *)
    log "ignore argument '${1}'"
    exit 0
    ;;
esac

if [ "${SPELUNKR:-0}" != 1 ] && [ "${MISTERPLEX_EXPLICIT_PLEX:-0}" != 1 ]; then
  if [ -e "$ROOT/tmp/misterplex-spelunkr.active" ]; then
    log "start refused: Spelunkr owns the box; explicit --plex switch required"
    exit 0
  fi
  for proc in "$ROOT"/proc/[0-9]*; do
    [ -r "$proc/environ" ] || continue
    if tr '\000' '\n' < "$proc/environ" | grep -qx 'SPELUNKR=1'; then
      log "start refused: Spelunkr process owns the box"
      exit 0
    fi
  done
fi

# Explicit switches retain their snapshot until mode.sh verifies readiness.
# Ordinary Plex boot starts a new session, including after a power cycle.
if [ "${SPELUNKR:-0}" != 1 ] && [ "${MISTERPLEX_EXPLICIT_PLEX:-0}" != 1 ]; then
  saved="$FAT/config/PlexOB_v1.CFG.service-save"
  if [ -e "$saved" ]; then
    if ! mv "$saved" "$FAT/config/PlexOB_v1.CFG.service-restored"; then
      log "Plex boot: service snapshot retirement failed; leaving snapshot at $saved and continuing daemon startup"
    else
      log "Plex boot: retired previous service snapshot"
    fi
  fi
fi

if [ "$MPX_CORE_MANUAL_ONLY" != 1 ]; then
  retire_old_plexob
fi
pick_rbf
name=$(cat "$CORENAME_FILE" 2>/dev/null || true)
case "$name" in
  Plex|PLEX|PlexOB|PLEXOB)
    log "core already $name"
    ;;
  *)
    if [ "$MPX_CORE_MANUAL_ONLY" = 1 ]; then
      log "manual-only: leaving core '${name:-empty}' loaded; companion only"
    else
    log "load_core $RBF (CORENAME='${name:-empty}' prefix=$PREFIX)"
    if [ ! -e "$MISTER_CMD" ] && [ ! -c "$MISTER_CMD" ]; then
      # A test file may not exist yet. On the device /dev/MiSTer_cmd does.
      mkdir -p "$(dirname "$MISTER_CMD")" 2>/dev/null || true
    fi
    if ! printf 'load_core %s\n' "$RBF" > "$MISTER_CMD"; then
      log "load_core write failed: $MISTER_CMD"
      exit 1
    fi
    i=0
    while [ "$i" -lt "$WAIT" ]; do
      name=$(cat "$CORENAME_FILE" 2>/dev/null || true)
      case "$name" in
        Plex|PLEX|PlexOB|PLEXOB) break ;;
      esac
      i=$((i + 1))
      sleep 1
    done
    case "$name" in
      Plex|PLEX|PlexOB|PLEXOB) ;;
      *)
        log "core did not become Plex/PlexOB (still '${name:-empty}')"
        exit 1
        ;;
    esac
    fi
    ;;
esac

install_plexui_fonts() {
  dest="$FAT/misterplex/plexui"
  mkdir -p "$dest" 2>/dev/null || true
  if [ -s "$dest/Roboto-Regular.ttf" ] && [ -s "$dest/Roboto-Bold.ttf" ]; then
    return 0
  fi
  self_dir=$(cd "$(dirname "$0")" 2>/dev/null && pwd || true)
  for src in "$FAT/linux/misterplex/plexui" "$self_dir/../plexui" "$FAT/misterplex/plexui"; do
    [ -n "$src" ] || continue
    if [ -s "$src/Roboto-Regular.ttf" ] && [ -s "$src/Roboto-Bold.ttf" ]; then
      if [ "$src" != "$dest" ]; then
        cp -f "$src/Roboto-Regular.ttf" "$src/Roboto-Bold.ttf" "$dest/" 2>/dev/null || true
        [ -f "$src/LICENSE-Roboto.txt" ] && cp -f "$src/LICENSE-Roboto.txt" "$dest/" 2>/dev/null || true
      fi
      [ -s "$dest/Roboto-Regular.ttf" ] && log "plexui fonts: $dest"
      return 0
    fi
  done
  log "plexui fonts: missing (built-in bitmap fallback)"
}
# idle-card:boot -- the core is loaded and no daemon is up yet: paint "daemon not running" (not colorbars) until the
# daemon's own "Ready to cast" card replaces it (the daemon releases this painter first thing). Resident, detached,
# no network, no video_mode. Before the net wait below, which this does not move. Missing binary: no card, no error.
IDLE_CARD="${MISTERPLEX_IDLE_CARD:-$(dirname "$BIN")/owenb_idle_card}"
if [ -x "$IDLE_CARD" ]; then
  if [ "${SPELUNKR:-0}" = 1 ]; then
    # Spelunkr owns the idle chrome (Open the Guide / last channel). Do not
    # launch the Plex "Ready to cast" / daemon-down painter — a stuck
    # --card down process keeps that card on screen over Spelunkr.
    "$IDLE_CARD" --release || {
      log "Spelunkr startup refused: previous idle painter did not release"
      exit 1
    }
    log "idle card: Spelunkr — released prior painter; no boot card"
  else
    log "idle card: $IDLE_CARD --card down"
    "$IDLE_CARD" --card down </dev/null >/dev/null 2>&1 &
  fi
fi

if [ ! -x "$BIN" ]; then
  log "daemon missing: $BIN"
  exit 1
fi
install_plexui_fonts

# One daemon. A pidfile is optional; pgrep is not required.
if [ -f "$ROOT/tmp/misterplexd.pid" ]; then
  old=$(cat "$ROOT/tmp/misterplexd.pid" 2>/dev/null || true)
  if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
    log "daemon already running pid=$old"
    exit 0
  fi
fi

# One supervisor too. While it sleeps between respawns the daemon pid above is
# dead, so the supervisor pidfile is the only sign that a start is in hand.
if [ -f "$SUP_PIDFILE" ]; then
  oldsup=$(cat "$SUP_PIDFILE" 2>/dev/null || true)
  case "$oldsup" in
    ''|*[!0-9]*) ;;
    *)
      if kill -0 "$oldsup" 2>/dev/null; then
        log "supervisor already running pid=$oldsup"
        exit 0
      fi
      ;;
  esac
fi

CONF_LINUX="$FAT/linux/misterplex.conf"
CONF_FALLBACK="$FAT/misterplex/misterplex.conf"
STARTUP="${BOOT_USER_STARTUP:-$FAT/linux/user-startup.sh}"
CONF="$CONF_LINUX"

conf_get() {
  key=$1
  file=$2
  [ -f "$file" ] || return 0
  awk -F= -v k="$key" '
    $0 ~ /^[ \t]*#/ { next }
    {
      key=$1
      gsub(/^[ \t]+|[ \t]+$/, "", key)
      if (key == k) {
        v=substr($0, index($0, "=")+1)
        gsub(/\r/, "", v)
        gsub(/^[ \t"]+|[ \t"]+$/, "", v)
      }
    }
    END { printf "%s", v }
  ' "$file"
}

ipv4_usable() {
  case "$1" in
    ''|127.*|169.254.*) return 1 ;;
  esac
  return 0
}

# inet address on one interface. Busybox `inet addr:10.0.0.1` and iproute
# `inet 10.0.0.1/24` both count. The interface is the first field of a line
# that does not start with whitespace.
iface_ipv4() {
  want=$1
  [ -n "$want" ] || return 0
  command -v ifconfig >/dev/null 2>&1 || return 0
  ifconfig 2>/dev/null | awk -v want="$want" '
    /^[^ \t]/ { iface=$1; sub(/:$/, "", iface) }
    /inet / {
      ip=""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^addr:/) { split($i, a, ":"); ip=a[2] }
        if ($i == "inet" && $(i+1) ~ /^[0-9]/) ip=$(i+1)
      }
      sub(/\/.*/, "", ip)
      if (iface == want && ip != "" && ip !~ /^127\./ && ip !~ /^169\.254\./) {
        print ip
        exit
      }
    }'
}

# Interface of the default route (UG, destination 00000000). Not the first
# ifconfig address: another NIC can hold an address that is not the LAN.
default_route_iface() {
  route=${BOOT_PROC_ROUTE:-/proc/net/route}
  [ -f "$route" ] || return 0
  awk 'NR > 1 && $2 == "00000000" && $4 ~ /[13579bdfBDF]$/ { print $1; exit }' "$route"
}

primary_ipv4() {
  # BOOT_NO_NET=1 is the no-address boot (tests, and a card with no DHCP yet).
  if [ -n "${BOOT_NO_NET:-}" ]; then
    return 0
  fi
  if [ -n "${BOOT_IPV4:-}" ]; then
    if ipv4_usable "$BOOT_IPV4"; then
      printf '%s\n' "$BOOT_IPV4"
    fi
    return 0
  fi
  if [ -n "${BOOT_ROUTE_IPV4:-}" ]; then
    if ipv4_usable "$BOOT_ROUTE_IPV4"; then
      printf '%s\n' "$BOOT_ROUTE_IPV4"
    fi
    return 0
  fi
  via=$(default_route_iface)
  if [ -n "$via" ]; then
    ip=$(iface_ipv4 "$via")
    if [ -n "$ip" ]; then
      printf '%s\n' "$ip"
      return 0
    fi
    # The default-route NIC has no usable address. That is not "network
    # ready" for AirPlay. Do not report another NIC from this wait.
    return 0
  fi
  # No default route yet. Another NIC is not ready for AirPlay. Identity
  # may use one after this wait gives up. BOOT_IPV4 / BOOT_ROUTE_IPV4 are
  # the test stand-ins for the default-route address.
}

# First non-loopback, non-link-local address on any interface. Identity
# uses this only when the default route contributed nothing and the conf
# has no name yet.
any_usable_ipv4() {
  command -v ifconfig >/dev/null 2>&1 || return 0
  ifconfig 2>/dev/null | awk '
    /inet / {
      ip=""
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^addr:/) { split($i, a, ":"); ip=a[2] }
        if ($i == "inet" && $(i+1) ~ /^[0-9]/) ip=$(i+1)
      }
      sub(/\/.*/, "", ip)
      if (ip != "" && ip !~ /^127\./ && ip !~ /^169\.254\./) {
        print ip
        exit
      }
    }'
}

# 192.0.2.229 -> misterplex-0.2.229. FRIENDLY_NAME and PLAYER_ID share it.
last3_name() {
  case "$1" in
    *.*.*.*) ;;
    *) return 1 ;;
  esac
  rest=${1#*.}
  case "$rest" in
    *.*.*) printf 'misterplex-%s' "$rest" ;;
    *) return 1 ;;
  esac
}

# Last --flag value on a misterplexd / boot / supervise line, including a
# commented-out line. A quoted value may contain spaces. --id does not match
# --idle-*. Quotes around the value are stripped.
old_launch_arg() {
  file=$1
  flag=$2
  [ -f "$file" ] || return 0
  awk -v flag="$flag" '
    function emit(v) {
      gsub(/^[ \t]+|[ \t]+$/, "", v)
      if (v != "") last = v
    }
    {
      s = $0
      sub(/^[ \t]*#+[ \t]*/, "", s)
      if (s !~ /misterplexd/) next
      flen = length(flag)
      n = length(s)
      i = 1
      while (i <= n) {
        p = index(substr(s, i), flag)
        if (p == 0) break
        p = i + p - 1
        if (p > 1 && substr(s, p - 1, 1) !~ /[ \t]/) { i = p + 1; continue }
        after = p + flen
        if (after <= n && substr(s, after, 1) !~ /[ \t=]/) { i = p + 1; continue }
        j = after
        while (j <= n && substr(s, j, 1) ~ /[ \t]/) j++
        if (j <= n && substr(s, j, 1) == "=") {
          j++
          while (j <= n && substr(s, j, 1) ~ /[ \t]/) j++
        }
        if (j > n) break
        q = substr(s, j, 1)
        if (q == "\"" || q == "\047") {
          rest = substr(s, j + 1)
          k = index(rest, q)
          if (k == 0) { emit(rest); break }
          emit(substr(rest, 1, k - 1))
          i = j + 1 + k
        } else {
          k = j
          while (k <= n && substr(s, k, 1) !~ /[ \t]/) k++
          emit(substr(s, j, k - j))
          i = k
        }
      }
    }
    END { printf "%s", last }
  ' "$file"
}

fsync_path() {
  target=$1
  [ -n "$target" ] || return 0
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import os,sys
p=sys.argv[1]
flags=os.O_RDONLY
if os.path.isdir(p):
    flags |= getattr(os, "O_DIRECTORY", 0)
fd=os.open(p, flags)
os.fsync(fd)
os.close(fd)' "$target"
    return $?
  fi
  if [ -d "$target" ]; then
    sync
    return 0
  fi
  dd if="$target" of="$target" conv=notrunc,fsync status=none 2>/dev/null || sync
}

persist_kv() {
  file=$1
  key=$2
  val=$3
  [ -n "$file" ] && [ -n "$key" ] && [ -n "$val" ] || return 0
  # A later boot that already has this value must not rewrite the file.
  if [ -f "$file" ]; then
    cur=$(conf_get "$key" "$file")
    if [ "$cur" = "$val" ]; then
      return 0
    fi
  fi
  mkdir -p "$(dirname "$file")" 2>/dev/null || true
  touch "$file"
  tmp="$file.mpx-tmp"
  if ! awk -v k="$key" -v v="$val" '
    BEGIN { done = 0 }
    {
      if ($0 ~ /^[ \t]*#/) { print; next }
      key = $0
      sub(/=.*/, "", key)
      gsub(/^[ \t]+|[ \t]+$/, "", key)
      if (key == k) {
        if (!done) print k "=" v
        done = 1
        next
      }
      print
    }
    END { if (!done) print k "=" v }
  ' "$file" > "$tmp"; then
    rm -f "$tmp"
    return 1
  fi
  fsync_path "$tmp" || { rm -f "$tmp"; return 1; }
  mv -f "$tmp" "$file"
  fsync_path "$(dirname "$file")"
}

script_conf=$(old_launch_arg "$STARTUP" --conf)
if [ -n "$script_conf" ] && [ -f "$script_conf" ]; then
  CONF="$script_conf"
elif [ -f "$CONF_LINUX" ]; then
  CONF="$CONF_LINUX"
elif [ -n "$script_conf" ]; then
  CONF="$script_conf"
  log "conf from old launch line: $CONF"
elif [ -f "$CONF_FALLBACK" ]; then
  CONF="$CONF_FALLBACK"
  log "linux conf missing; using $CONF"
else
  CONF="$CONF_LINUX"
fi

script_name=$(old_launch_arg "$STARTUP" --name)
script_id=$(old_launch_arg "$STARTUP" --id)

NAME="${FRIENDLY_NAME:-}"
ID="${MPX_ID:-}"
name_src=env
id_src=env
[ -n "$NAME" ] || name_src=
[ -n "$ID" ] || id_src=

if [ -z "$NAME" ]; then
  NAME=$(conf_get FRIENDLY_NAME "$CONF")
  [ -z "$NAME" ] && NAME=$(conf_get NAME "$CONF")
  [ -n "$NAME" ] && name_src=conf
fi
if [ -z "$ID" ]; then
  ID=$(conf_get PLAYER_ID "$CONF")
  [ -z "$ID" ] && ID=$(conf_get ID "$CONF")
  [ -z "$ID" ] && ID=$(conf_get MPX_ID "$CONF")
  [ -n "$ID" ] && id_src=conf
fi
if [ -z "$NAME" ] && [ -n "$script_name" ]; then
  NAME=$script_name
  name_src=script
fi
if [ -z "$ID" ] && [ -n "$script_id" ]; then
  ID=$script_id
  id_src=script
fi
take_ident() {
  f=$1
  [ -f "$f" ] || return 0
  if [ -z "$NAME" ]; then
    NAME=$(conf_get FRIENDLY_NAME "$f")
    [ -z "$NAME" ] && NAME=$(conf_get NAME "$f")
    [ -n "$NAME" ] && name_src=file
  fi
  if [ -z "$ID" ]; then
    ID=$(conf_get PLAYER_ID "$f")
    [ -z "$ID" ] && ID=$(conf_get ID "$f")
    [ -z "$ID" ] && ID=$(conf_get MPX_ID "$f")
    [ -n "$ID" ] && id_src=file
  fi
}
if [ -z "$NAME" ] || [ -z "$ID" ]; then
  if [ -n "${BOOT_IDENTITY_FILE:-}" ]; then
    take_ident "$BOOT_IDENTITY_FILE"
  fi
  take_ident "$FAT/linux/misterplex/identity"
  take_ident "$FAT/misterplex/identity"
fi

# Always wait, even when the name is already known. AirPlay registers from
# the daemon, and a launch before the default route left it with no address.
ip=""
n=0
while [ "$n" -le "$NET_WAIT" ]; do
  ip=$(primary_ipv4)
  if [ -n "$ip" ]; then
    log "network ready $ip after ${n}s"
    break
  fi
  if [ "$n" -ge "$NET_WAIT" ]; then
    log "no default-route IPv4 after ${NET_WAIT}s"
    break
  fi
  n=$((n + 1))
  sleep 1
done
# Prefer the default-route address the wait accepted. Otherwise any
# non-link-local. BOOT_IPV4 / BOOT_ROUTE_IPV4 / BOOT_NO_NET already decided
# the address for tests; do not scan the host ifconfig behind those.
ident_ip=$ip
if [ -z "$ident_ip" ] && [ -z "${BOOT_NO_NET:-}" ] && [ -z "${BOOT_IPV4:-}" ] && [ -z "${BOOT_ROUTE_IPV4:-}" ]; then
  ident_ip=$(any_usable_ipv4)
fi
if [ -z "$NAME" ] && [ -z "$ID" ]; then
  formatted=
  if [ -n "$ident_ip" ]; then
    formatted=$(last3_name "$ident_ip") || formatted=
  fi
  if [ -n "$formatted" ]; then
    log "identity from $ident_ip after ${n}s ($formatted)"
    NAME=$formatted
    ID=$formatted
    name_src=ip
    id_src=ip
  else
    log "no usable IPv4 after ${NET_WAIT}s; fallback identity for this boot only (not saved)"
    NAME="MiSTerPlex"
    ID="misterplex-1"
    name_src=fallback
    id_src=fallback
  fi
else
  # Conf, env, user-startup, or the identity file already named the box.
  # Do not overwrite those keys, and do not invent a last-3 for a missing
  # half (that would write a different string than the one already saved).
  log "identity kept ($name_src/$id_src)"
  if [ -z "$NAME" ]; then
    NAME="MiSTerPlex"
    name_src=fallback
  fi
  if [ -z "$ID" ]; then
    ID="misterplex-1"
    id_src=fallback
  fi
fi

# conf and fallback are not written. An address name is written once, and
# both keys are the same misterplex-<last3> string. Stop and shutdown
# return before this.
case "$name_src" in
  ip) persist_kv "$CONF" FRIENDLY_NAME "$NAME" ;;
  ''|conf|fallback) ;;
  *) persist_kv "$CONF" FRIENDLY_NAME "$NAME" ;;
esac
case "$id_src" in
  ip) persist_kv "$CONF" PLAYER_ID "$ID" ;;
  ''|conf|fallback) ;;
  *) persist_kv "$CONF" PLAYER_ID "$ID" ;;
esac

# MISTERPLEXD_BIN= in the conf names the daemon to run (prod keeps a cand build beside the shipped one). The
# environment override (BIN above) is the default; an executable conf value wins, as in prod 7cca970e.
if [ "${SPELUNKR:-0}" != 1 ]; then
  plexbin=$(conf_get MISTERPLEXD_BIN "$CONF")
  [ -n "$plexbin" ] && [ -x "$plexbin" ] && BIN="$plexbin"
fi

# MPX_PREWARM is OFF for the boot launch (prod 7cca970e sets it on the daemon line); an explicit environment value
# is honoured. The two-GET path is the one proven on glass (plex_resolve.cpp MPX_PREWARM).
MPX_PREWARM="${MPX_PREWARM:-0}"
export MPX_PREWARM

# Default CA is the bundle package_release.sh installs. An operator override
# in the environment is left alone.
CA_DEFAULT="$FAT/linux/misterplex-cacert.pem"
if [ -z "${MPX_CA_BUNDLE:-}" ] && [ -s "$CA_DEFAULT" ]; then
  MPX_CA_BUNDLE="$CA_DEFAULT"
  export MPX_CA_BUNDLE
fi

start_gcast_helper
log "start $BIN --name $NAME --id $ID --port $PORT --conf $CONF MPX_PREWARM=$MPX_PREWARM GAIRPLAY=1 GYT=1 MPX_LEGACY_CORE_PREFIX=$PREFIX"
# BOOT_SUPERVISE=0 (env, or BOOT_SUPERVISE=0 in the conf) is the rollback to
# the plain one-shot start below. Default is on for Plex. Spelunkr must never
# inherit an unset BOOT_SUPERVISE and start the supervisor — mode.sh exports 0,
# but ad-hoc SPELUNKR=1 boot starts (the 03:31/04:34/05:09 mid-watch flips) did not.
if [ -z "${BOOT_SUPERVISE+x}" ] && [ "${SPELUNKR:-0}" = 1 ]; then
  BOOT_SUPERVISE=0
fi
SUPERVISE="${BOOT_SUPERVISE:-$(conf_get BOOT_SUPERVISE "$CONF")}"
SUP_SCRIPT="${BOOT_SUPERVISE_SCRIPT:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)/misterplexd_supervise.sh}"
[ -f "$SUP_SCRIPT" ] || SUP_SCRIPT="$FAT/misterplex/bin/misterplexd_supervise.sh"
SUP_READY="$(dirname "$SUP_SCRIPT")/misterplex_cast_ready.sh"
if [ "$SUPERVISE" != "0" ] && [ -f "$SUP_SCRIPT" ] && [ -f "$SUP_READY" ]; then
  log "start supervised $SUP_SCRIPT bin=$BIN --name $NAME --id $ID --port $PORT --conf $CONF GAIRPLAY=1 GYT=1 MPX_LEGACY_CORE_PREFIX=$PREFIX"
  # The supervisor defaults MPX_INPROC_DECODE=1 and MPX_STICK_I420=1. The plain
  # start never set them (both default off in the daemon), so pass 0 to keep
  # the boot behaviour the same; an explicit value in the environment wins.
  # The HTTP watchdog is made slower than the supervisor defaults (4 s to bind,
  # kill after 2 failed polls) so a slow bind or a busy playback poll does not
  # get the daemon killed.
  GAIRPLAY=1 GYT=1 MPX_LEGACY_CORE_PREFIX="$PREFIX" \
  MPX_INPROC_DECODE="${MPX_INPROC_DECODE:-0}" MPX_STICK_I420="${MPX_STICK_I420:-0}" \
  MISTERPLEXD_BIN="$BIN" MISTERPLEX_NAME="$NAME" MISTERPLEX_ID="$ID" \
  MISTERPLEX_PORT="$PORT" PORT="$PORT" MISTERPLEX_CONF="$CONF" MISTERPLEX_LOG="$LOG" \
  MISTERPLEX_SUPLOG="${BOOT_SUP_LOG:-$ROOT/tmp/misterplexd_supervise.log}" \
  MISTERPLEX_SUP_LOCK="$SUP_LOCK" MISTERPLEX_CAST_READY="$SUP_READY" \
  MISTERPLEX_CHILD_PIDFILE="$ROOT/tmp/misterplexd.pid" \
  MISTERPLEX_HTTP_WAIT_TRIES="${MISTERPLEX_HTTP_WAIT_TRIES:-60}" \
  MISTERPLEX_HTTP_FAIL_LIMIT="${MISTERPLEX_HTTP_FAIL_LIMIT:-6}" \
    sh "$SUP_SCRIPT" >> "$LOG" 2>&1 &
  echo $! > "$SUP_PIDFILE"
  log "supervisor pid=$(cat "$SUP_PIDFILE")"
else
  [ "$SUPERVISE" = "0" ] || log "supervisor script or cast_ready missing; plain start"
  GAIRPLAY=1 GYT=1 MPX_LEGACY_CORE_PREFIX="$PREFIX" \
    "$BIN" --name "$NAME" --id "$ID" --port "$PORT" --conf "$CONF" >> "$LOG" 2>&1 &
  echo $! > "$ROOT/tmp/misterplexd.pid"
  log "daemon pid=$(cat "$ROOT/tmp/misterplexd.pid")"
fi

# yt-dlp: unpack a bundled zip immediately (offline), then self-update at most
# once a day after the clock is plausible. Does not delay the daemon.
if [ "${YTDLP_SELF_UPDATE:-1}" != "0" ]; then
  UPD="${BOOT_UPDATE_YTDLP:-}"
  if [ -z "$UPD" ]; then
    UPD="$(CDPATH= cd -- "$(dirname "$0")" && pwd)/update_ytdlp.sh"
  fi
  if [ ! -f "$UPD" ]; then
    UPD="$FAT/misterplex/bin/update_ytdlp.sh"
  fi
  if [ -f "$UPD" ]; then
    (
      DEST="${YTDLP_DEST:-$FAT/linux/misterplex/yt-dlp}"
      if [ ! -e "$DEST/yt-dlp_linux_armv7l" ]; then
        for z in "$FAT/linux/yt-dlp_linux_armv7l.zip" \
                 "$(dirname "$UPD")/yt-dlp_linux_armv7l.zip" \
                 "$FAT/misterplex/bin/yt-dlp_linux_armv7l.zip"; do
          if [ -f "$z" ]; then
            echo "yt-dlp: unpack bundled $z"
            YTDLP_LOCAL_ZIP="$z" YTDLP_DEST="$DEST" sh "$UPD" || echo "yt-dlp: bundled unzip failed"
            break
          fi
        done
      fi
      i=0
      while [ "$i" -lt 90 ]; do
        now=$(date +%s 2>/dev/null || echo 0)
        if [ "$now" -ge 1577836800 ]; then
          break
        fi
        i=$((i + 1))
        sleep 2
      done
      now=$(date +%s 2>/dev/null || echo 0)
      if [ "$now" -lt 1577836800 ]; then
        echo "yt-dlp update skipped: clock still implausible"
        exit 0
      fi
      stamp="$FAT/linux/misterplex/yt-dlp.update-stamp"
      mkdir -p "$(dirname "$stamp")" 2>/dev/null || true
      if [ -f "$stamp" ]; then
        prev=$(cat "$stamp" 2>/dev/null || echo 0)
        case "$prev" in ''|*[!0-9]*) prev=0 ;; esac
        if [ $((now - prev)) -lt 86400 ]; then
          echo "yt-dlp update skipped: already ran within a day"
          exit 0
        fi
      fi
      if YTDLP_DEST="$DEST" sh "$UPD"; then
        echo "$now" > "$stamp"
      else
        echo "yt-dlp update failed"
      fi
    ) >> "$LOG" 2>&1 &
  if [ "${YTDLP_UPDATE_FOREGROUND:-0}" = 1 ]; then
    wait || true
  fi
  fi
fi
exit 0
