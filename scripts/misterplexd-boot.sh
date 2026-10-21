#!/bin/sh
# misterplexd-boot.sh - on-device boot launcher for the canonical a lab MiSTer PROD misterplexd.
# Install at /media/fat/linux/misterplexd-boot.sh; started in the background from
# /media/fat/linux/user-startup.sh (replaces the dead misterplex_v2 supervise call).
# busybox-ash port of the prod launcher script (md5 d9c2b6e7) prod_kill_strays +
# prod_start_hardened: md5 gate, /proc-exe stray kill (incl. deleted exe), stale-lock
# clear, detached start with the exact prod env/args/log, verify pid/md5/env/cmdline/
# port/lock, 3 tries. One-shot: it does NOT respawn the daemon. Staged 2026-10-04.
# 2026-10-04 canon c2aed83b edit (canon-install-1005): DAEMON/EXPECT_MD5 -> misterplexd-canon-c2aed83b / 902a6118.
# 2026-10-06 SoT-bump 9E36 edit (sotbump-1006, packet 2a0981c3): DAEMON/EXPECT_MD5 -> misterplexd-canon-456d0bb3 / 01f530d4; +ENV4 MPX_IDLEFB_PATH=devmem (4-var SoT).
# 2026-10-06 SoT v3 edit (sot-v3, wS for release DEMO step 4): DAEMON/EXPECT_MD5 -> misterplexd-cand-d825aa37 / b1e4a588; +ENV5 MPX_BROWSE_V2=1 +ENV6 MPX_BROWSE_MVPB=1 (6-var SoT).
# 2026-10-06 SoT v3 r2 edit (sot-v3, wS for release 08:54, review 08:51): +ENV7 MPX_MENU_URL=http://192.0.2.21:18769/menu/a1 (7-var SoT: the title-case /menu/a1 render r6-0002 ran); WANT_ENV byte order MVPB<V2<D52<IDLEFB<LEGACY<MENU<REMOTE.
# 2026-10-06 SoT v3b edit (sot-v3, wS for release 08:51, review 08:54, the 416 path): DAEMON/EXPECT_MD5 -> misterplexd-cand-bc679989 / 4c3d3ebf; ENV as v3 r2 (7-var).
#
# Test-only overrides (all unset at boot -> on-device defaults):
#   DRY_RUN=1 (print kill/rm/start instead of doing them), ROOT=dir (prefix for every path),
#   BOOT_EXPECT_MD5 BOOT_PORT BOOT_PATH_PREFIX BOOT_STARTUP_DELAY BOOT_NET_WAIT
#   BOOT_CORE_WAIT BOOT_NET_IF BOOT_NET_ADDR BOOT_PROC BOOT_MATCH (exe glob, sandbox only)

DRY_RUN="${DRY_RUN:-0}"
ROOT="${ROOT:-}"
PROC="${BOOT_PROC:-/proc}"
MATCH="${BOOT_MATCH:-*misterplexd*}"   # exe glob of processes treated as misterplexd
PATH="${BOOT_PATH_PREFIX:+$BOOT_PATH_PREFIX:}/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

# ---- canonical prod (keep in sync with the prod launcher script) ----
DAEMON="$ROOT/media/fat/linux/misterplexd-cand-bc679989"   # bc679989 = a lab MiSTer prod at SoT v3b (d825aa37 + the 345a4c9b pick, T-E2E-416); ROLLBACK is canon 456d0bb3 01f530d4 (boot 3e178196)
EXPECT_MD5="${BOOT_EXPECT_MD5:-4c3d3ebf0b0576ce20c899dec7cdb1c5}"
CONF="$ROOT/media/fat/linux/misterplex.conf"
NAME="MiSTerPlex-192.0.2.12"
ID="misterplex-192.0.2.12"
PORT="${BOOT_PORT:-3005}"
# Prefix8 of RC RBF 47cb9fb4 (BUILD_ID 0x48004D40). Must match kLegacyCorePrefix
# in host/libmisterplex/legacy_core_prefix.hpp. Retired prod RBF 7cbae324 is not this core.
ENV1="MPX_LEGACY_CORE_PREFIX=999800cf"
ENV2="MPX_REMOTE_LADDER_PROXY="   # product default: direct PMS. Non-empty URL is debug opt-in.
DPATH="/usr/bin:/bin:/usr/sbin:/sbin:/media/fat/linux:."  # PATH seen in running prod environ
RUNDIR="$ROOT/root"                                      # prod cwd
DLOG="$ROOT/tmp/mpxd.log"                                # prod stdout+stderr (append)
LOCK="$ROOT/tmp/misterplexd.lock"                        # written by the daemon (its pid)
BLOG="$ROOT/tmp/misterplexd-boot.log"
BLOCKD="$ROOT/tmp/misterplexd-boot.lock.d"
FAILF="$ROOT/tmp/misterplexd-boot.FAILED"
CORENAME="$ROOT/tmp/CORENAME"
STARTUP_DELAY="${BOOT_STARTUP_DELAY:-3}"
NET_WAIT="${BOOT_NET_WAIT:-30}"
CORE_WAIT="${BOOT_CORE_WAIT:-20}"
NET_IF="${BOOT_NET_IF:-eth0}"
NET_ADDR="${BOOT_NET_ADDR:-192.0.2.228}"
TRIES=3
START_WAIT=20
ENV3="MPX_D52_LOG=1"
ENV4="MPX_IDLEFB_PATH=devmem"
ENV5="MPX_BROWSE_V2=1"
ENV6="MPX_BROWSE_MVPB=1"
ENV7="${MPX_MENU_URL:+MPX_MENU_URL=$MPX_MENU_URL}"  # set MPX_MENU_URL for lab browse; no hardcoded IP
WANT_ENV="$ENV6 $ENV5 $ENV3 $ENV4 $ENV1 $ENV7 $ENV2"   # sorted
WANT_CMD="$DAEMON --name $NAME --id $ID --port $PORT --conf $CONF"

# ---- log (kept small: >32 KiB -> last 100 lines) ----
mkdir -p "$ROOT/tmp" 2>/dev/null
if [ -f "$BLOG" ] && [ "$(wc -c < "$BLOG")" -gt 32768 ]; then
  tail -n 100 "$BLOG" > "$BLOG.tmp" 2>/dev/null && mv -f "$BLOG.tmp" "$BLOG"
fi
log() { echo "$(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) $*" >> "$BLOG"; [ "$DRY_RUN" = 1 ] && echo "$*"; return 0; }
loud() { log "!!! $*"; echo "$(date '+%F %T') $*" >> "$FAILF"; [ -n "$ROOT" ] || echo "misterplexd-boot: !!! $*" > /dev/console 2>/dev/null; }
act() { if [ "$DRY_RUN" = 1 ]; then log "DRY_RUN would: $*"; else log "do: $*"; "$@"; fi; }

case "${1:-start}" in
  start|restart) ;;
  *) log "arg '$1' - nothing to do"; exit 0 ;;
esac

# ---- single instance of this launcher ----
if ! mkdir "$BLOCKD" 2>/dev/null; then
  op=$(cat "$BLOCKD/pid" 2>/dev/null)
  if [ -n "$op" ] && [ "$op" != "$$" ] && kill -0 "$op" 2>/dev/null; then log "another misterplexd-boot pid=$op running - exit"; exit 0; fi
  rm -rf "$BLOCKD"; mkdir "$BLOCKD" 2>/dev/null
fi
echo $$ > "$BLOCKD/pid"
trap 'rm -rf "$BLOCKD"' EXIT
trap 'exit 1' INT TERM HUP

# ---- helpers ----
list() { # "pid|exe" for every process whose exe contains misterplexd (deleted exe reads '<path> (deleted)')
  for p in "$PROC"/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null) || continue
    case "$e" in $MATCH) echo "${p##*/}|$e" ;; esac
  done
}
pids_all() { list | cut -d'|' -f1 | tr '\n' ' ' | sed 's/ *$//'; }
canon_pids() { list | while IFS='|' read -r pid e; do [ "$e" = "$DAEMON" ] && echo "$pid"; done | tr '\n' ' ' | sed 's/ *$//'; }
is_mpxd() { case "$(readlink "$PROC/$1/exe" 2>/dev/null)" in $MATCH) return 0 ;; esac; return 1; }
port_inodes() { # inodes of LISTEN sockets on $PORT (tcp + tcp6)
  hp=$(printf ':%04X' "$PORT")
  cat "$PROC/net/tcp" "$PROC/net/tcp6" 2>/dev/null | awk -v hp="$hp" '$4=="0A" && substr($2, length($2)-4)==hp {print $10}'
}
port_owner() { # misterplexd pid owning the $PORT listener; "other" if something else holds it; empty if free
  ino=$(port_inodes | head -n 1)
  [ -n "$ino" ] || return 0
  for pid in $(pids_all); do
    for f in "$PROC/$pid"/fd/*; do
      [ "$(readlink "$f" 2>/dev/null)" = "socket:[$ino]" ] && { echo "$pid"; return 0; }
    done
  done
  echo other
}

kill_all() { # stop EVERY misterplexd* process (canonical too) for a fresh start = prod_kill_strays all
  T=$(pids_all)
  [ -z "$T" ] && return 0
  log "kill TERM: $(list | tr '\n' ' ')"
  act kill -TERM $T
  [ "$DRY_RUN" = 1 ] && return 0
  i=0; while [ -n "$(pids_all)" ] && [ $i -lt 6 ]; do sleep 1; i=$((i+1)); done
  T=$(pids_all)
  if [ -n "$T" ]; then log "kill KILL: $T"; kill -KILL $T 2>/dev/null; sleep 1; fi
  T=$(pids_all)
  [ -z "$T" ] || { loud "STRAYS_LEFT $T"; return 3; }
}

clean_lock_port() { # clear lock only if no live misterplexd holds it; require :PORT free
  LIVE=$(pids_all)
  if [ -e "$LOCK" ]; then
    L=$(tr -dc 0-9 2>/dev/null < "$LOCK")
    if [ -n "$L" ] && is_mpxd "$L"; then
      if [ "$DRY_RUN" = 1 ]; then log "DRY_RUN lock held by live pid=$L (would be gone after kill)"
      else loud "LOCK_HELD_BY_LIVE pid=$L $(readlink "$PROC/$L/exe")"; return 4; fi
    elif [ -z "$LIVE" ]; then
      log "stale lock (pid=$L not a live misterplexd) -> clear"; act rm -f "$LOCK"
    elif [ "$DRY_RUN" = 1 ]; then log "DRY_RUN lock pid=$L stale, live misterplexd [$LIVE] (would be gone after kill; then clear)"
    else loud "lock pid=$L stale but live misterplexd present: $LIVE"; return 4; fi
  fi
  O=$(port_owner)
  if [ -n "$O" ]; then
    if [ "$DRY_RUN" = 1 ] && [ "$O" != other ]; then log "DRY_RUN port $PORT held by misterplexd pid=$O (would be freed by kill)"
    else loud "PORT_${PORT}_BUSY owner=$O"; return 5; fi
  fi
  log "clean live=[$LIVE] lock=$(cat "$LOCK" 2>/dev/null) port$PORT=${O:-free}"
}

start_daemon() { # detached, exact prod env + args, append to prod log
  if [ "$DRY_RUN" = 1 ]; then
    log "DRY_RUN would: cd $RUNDIR; env -i HOME=/root PATH=$DPATH SHELL=/bin/sh USER=root LOGNAME=root $ENV1 $ENV2 $ENV3 $ENV4 $ENV5 $ENV6 $ENV7 setsid nohup $WANT_CMD >> $DLOG 2>&1 </dev/null &"
    return 0
  fi
  log "start: cd $RUNDIR; env $ENV1 $ENV2 $ENV3 $ENV4 $ENV5 $ENV6 $ENV7 setsid nohup $WANT_CMD >> $DLOG 2>&1 </dev/null &"
  ( cd "$RUNDIR" 2>/dev/null || cd /
    exec env -i HOME=/root PATH="$DPATH" SHELL=/bin/sh USER=root LOGNAME=root "$ENV1" "$ENV2" "$ENV3" "$ENV4" "$ENV5" "$ENV6" "$ENV7" \
      setsid nohup "$DAEMON" --name "$NAME" --id "$ID" --port "$PORT" --conf "$CONF" >> "$DLOG" 2>&1 < /dev/null
  ) &
}

verify() { # one misterplexd only; canonical exe + md5; exact MPX_* env; exact cmdline; owns :PORT and the lock
  A=$(pids_all); P=$(canon_pids); X=; EV=; CL=; PO=
  if [ -n "$P" ] && [ "$(echo $P | wc -w)" = 1 ]; then
    X=$(md5sum "$PROC/$P/exe" 2>/dev/null | cut -c1-32)
    EV=$(tr '\0' '\n' 2>/dev/null < "$PROC/$P/environ" | grep '^MPX_' | sort | tr '\n' ' ' | sed 's/ *$//')
    CL=$(tr '\0' ' ' 2>/dev/null < "$PROC/$P/cmdline" | sed 's/ *$//')
    PO=$(port_owner)
  fi
  LK=$(tr -dc 0-9 2>/dev/null < "$LOCK")
  log "verify all=[$A] pid=[$P] md5=$X env=[$EV] cmd=[$CL] port$PORT=$PO lock=$LK"
  [ -n "$P" ] && [ "$A" = "$P" ] && [ "$X" = "$EXPECT_MD5" ] && [ "$EV" = "$WANT_ENV" ] \
    && [ "$CL" = "$WANT_CMD" ] && [ "$PO" = "$P" ] && [ "$LK" = "$P" ]
}

# ---- main ----
log "=== misterplexd-boot start pid=$$ arg=${1:-start} DRY_RUN=$DRY_RUN ROOT=${ROOT:-/}"

# 1. startup delay, then wait for network (user-startup.sh adds the eth0 ladder addr right
#    after launching us; wlan0 DHCP gives the default route) and for MiSTer main's CORENAME.
sleep "$STARTUP_DELAY"
i=0
while [ $i -lt "$NET_WAIT" ]; do
  ip -4 addr show "$NET_IF" 2>/dev/null | grep -q "inet $NET_ADDR/" && ip route 2>/dev/null | grep -q '^default ' && break
  sleep 1; i=$((i+1))
done
log "net after ${i}s: $NET_IF=[$(ip -4 addr show "$NET_IF" 2>/dev/null | awk '/inet /{printf "%s ", $2}')] default=[$(ip route 2>/dev/null | awk '/^default/{print $3" "$5; exit}')]"
[ $i -ge "$NET_WAIT" ] && log "WARN network not fully up after ${NET_WAIT}s - starting anyway"
i=0; while [ ! -s "$CORENAME" ] && [ $i -lt "$CORE_WAIT" ]; do sleep 1; i=$((i+1)); done
log "CORENAME=[$(cat "$CORENAME" 2>/dev/null)] after ${i}s"

# 2. md5 gate: wrong/missing binary -> loud, no start
[ -x "$DAEMON" ] || { loud "DAEMON MISSING/NOT EXECUTABLE $DAEMON - NOT STARTING"; exit 2; }
M=$(md5sum "$DAEMON" 2>/dev/null | cut -c1-32)
[ "$M" = "$EXPECT_MD5" ] || { loud "DAEMON MD5 MISMATCH $DAEMON md5=$M want=$EXPECT_MD5 - NOT STARTING"; exit 2; }
log "md5 ok $DAEMON $M"

# 3. canonical prod already healthy (manual re-run, S99user restart)? leave it alone.
if verify; then log "PROD_ALREADY_OK pid=$P - nothing to do"; rm -f "$FAILF"; exit 0; fi

# 4. up to 3 tries: kill all misterplexd* (incl. deleted exe), clear stale lock, :PORT free, start, verify
n=1
while [ $n -le $TRIES ]; do
  log "--- attempt $n"
  if kill_all && clean_lock_port; then
    start_daemon
    if [ "$DRY_RUN" = 1 ]; then
      log "DRY_RUN would verify: one pid exe=$DAEMON md5=$EXPECT_MD5 env=[$WANT_ENV] cmd=[$WANT_CMD] port=$PORT lock=pid"
      exit 0
    fi
    i=0
    while [ $i -lt $START_WAIT ]; do
      P=$(canon_pids); [ -n "$P" ] && [ "$(port_owner)" = "$P" ] && break
      sleep 1; i=$((i+1))
    done
    sleep 2
    if verify; then log "PROD_START_OK attempt $n pid=$P"; rm -f "$FAILF"; exit 0; fi
    log "PROD_START_BAD attempt $n"
  else
    log "attempt $n: strays/lock/port not clean"
  fi
  n=$((n+1)); sleep 2
done
loud "PROD START FAILED after $TRIES attempts ($DAEMON, env $WANT_ENV) - see $BLOG and $DLOG"
exit 1
