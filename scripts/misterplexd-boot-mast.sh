#!/bin/sh
# LAB ONLY (MAST-WEDGE launcher, coregate-2164c4ad era): NOT a lab MiSTer prod boot launcher. Never install it on a MiSTer.
# Prod boot = scripts/misterplexd-boot.sh, byte-equal to the live SoT boot (the prod launcher script pins its md5). Kept for the MAST tests.
# misterplexd-boot.sh - on-device boot launcher for the canonical a lab MiSTer PROD misterplexd.
# Install at /media/fat/linux/misterplexd-boot.sh; started in the background from
# /media/fat/linux/user-startup.sh (replaces the dead misterplex_v2 supervise call).
# busybox-ash port of the prod launcher script (md5 d9c2b6e7) prod_kill_strays +
# prod_start_hardened: md5 gate, /proc-exe stray kill (incl. deleted exe), stale-lock
# clear, detached start with the exact prod env/args/log, verify pid/md5/env/cmdline/
# port/lock, 3 tries. One-shot: it does NOT respawn the daemon. Staged 2026-10-04.
# 2026-10-04 coregate v2.3 edit (rel-coregate23): DAEMON/EXPECT_MD5 -> coregate-2164c4ad / 9a94099a (only change).
# 2026-10-04 MAST-WEDGE edit (w9; review 21:14 RCA, release 21:16/21:17/21:43, review 21:15 GO):
#   `misterplexd-boot.sh mast-reload PID REASON MARKER` is the ONLY caller of load_core in the
#   MAST-WEDGE path. The daemon (MPX_MAST_RELOAD_CMD=this script) starts it detached and exits 86.
#   This mode then:
#     - waits <= MR_LOCK_WAIT s for the launcher lock when a start launcher holds it (R1 below);
#     - acts only on marker state=requested; anything else is restart-only, so it never loops;
#     - waits <= MR_EXIT_WAIT s for that PID to be gone;
#     - requires CORENAME == Plex right before the load (review 21:52 M1, lead's product
#       rule: never replace a core or menu the user switched to);
#     - marks the marker attempted;
#     - runs ONE `load_core $PLEX_RBF` through misterplexd-mast-load.sh, which is wrapped by
#       mister-loadcore-guard wrap-script (the 0-misterplexd guard inline, LOADCORE_GUARD_OK/ABORT
#       lines in $BLOG, 97/98 = the guard refused; any other non-zero rc = the load failed past
#       the guard). Inside the guarded load: R6 (review 02:08) waits until the core consumed
#       all that was written (2 matching reads, <= 3 s, else ONE loud 'R6 NOT QUIESCED' and the
#       load still runs), 200 ms quiet, then P-a (the audio commit := 0), then load_core. It
#       appends r6=<outcome> to the marker (MAST_LOAD_MARKER = MARKER). R6 PORT SUSPECT (wrong
#       PLXR/PLXT magic on 2 reads under CORENAME Plex and a known CAPS build): no load, loader
#       rc 96, state=refused why=r6-port-suspect: HPS reboot first;
#     - settles MR_SETTLE s, then falls through into the normal verified start below.
#   Every no-load path for a fresh request writes state=refused why=<why> (M2). The daemon
#   treats refused as terminal: 'no reload ran (<why>)'.
#   A load that fails past the guard writes state=load-failed why=load-failed-rc<rc> and logs
#   'reload attempted, loader rc=<rc>' (review 22:00 N1). The daemon treats it as terminal too.
#   DAEMON/EXPECT_MD5 below keep the deployed v2.3 values; a MAST deploy sets them to the MAST
#   build, as the v2.3 edit did.
#   The restarted daemon re-probes MAST. A second wedge is a terminal cure-failed in the marker,
#   which nothing here or in the daemon loops on.
#   R1 (glass G1 23:49, review 23:53): the gate opens at daemon start, so a boot-time wedge hands
#   off inside the start launcher's START_WAIT verify, while it holds the launcher lock. A hand-off
#   is never dropped:
#     - the start launcher yields to a pending one (marker requested, no older than its own start):
#       no verify, no retry, no daemon start, lock released;
#     - the mast-reload instance waits for the lock and runs the ONE load;
#     - if the lock never frees, it refuses a still-fresh request loudly (why=launcher-busy).
#   review 00:18 P1/P2: the waits are paced by a 100 ms tick (usleep, else fractional sleep, else
#   1 s) and bounded by /proc/uptime, so a box without fractional sleep keeps every wait's length
#   (scripts/misterplexd-tick-precheck.sh checks the box at R4). A hand-off seen only after the last
#   attempt still yields: no PROD START FAILED.
#
# Test-only overrides (all unset at boot -> on-device defaults):
#   DRY_RUN=1 (print kill/rm/start instead of doing them), ROOT=dir (prefix for every path),
#   BOOT_EXPECT_MD5 BOOT_PORT BOOT_PATH_PREFIX BOOT_STARTUP_DELAY BOOT_NET_WAIT
#   BOOT_CORE_WAIT BOOT_NET_IF BOOT_NET_ADDR BOOT_PROC BOOT_MATCH (exe glob, sandbox only)
#   BOOT_TRIES BOOT_START_WAIT BOOT_MR_EXIT_WAIT BOOT_MR_SETTLE BOOT_MR_LOCK_WAIT BOOT_MAST_LOADER
#   BOOT_PLEX_RBF BOOT_MAST_MARKER BOOT_MAST_SELF (the MPX_MAST_RELOAD_CMD the daemon gets)
#   BOOT_LOCK BOOT_CORENAME (the e2e's daemon lock and CORENAME paths)

DRY_RUN="${DRY_RUN:-0}"
ROOT="${ROOT:-}"
PROC="${BOOT_PROC:-/proc}"
MATCH="${BOOT_MATCH:-*misterplexd*}"   # exe glob of processes treated as misterplexd
PATH="${BOOT_PATH_PREFIX:+$BOOT_PATH_PREFIX:}/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

# ---- canonical prod (keep in sync with the prod launcher script) ----
DAEMON="$ROOT/media/fat/linux/misterplexd-coregate-2164c4ad"   # coregate v2.3 2164c4ad = prod since 2026-10-04 (owner GO via review 13:53); was misterplexd-avsync-dbcc42fa 51c5c450 (boot.sh 6e0682c7)
EXPECT_MD5="${BOOT_EXPECT_MD5:-9a94099a396b5137f953b4bdd024e626}"
CONF="$ROOT/media/fat/linux/misterplex.conf"
NAME="MiSTerPlex-192.0.2.12"
ID="misterplex-192.0.2.12"
PORT="${BOOT_PORT:-3005}"
# Prefix8 of RC RBF 47cb9fb4 (BUILD_ID 0x48004D40). Must match kLegacyCorePrefix
# in host/libmisterplex/legacy_core_prefix.hpp. Retired prod RBF 7cbae324 is not this core.
ENV1="MPX_LEGACY_CORE_PREFIX=999800cf"
ENV2="MPX_REMOTE_LADDER_PROXY="   # product default: direct PMS. Non-empty URL is debug opt-in.
ENV3="MPX_MAST_RELOAD_CMD=${BOOT_MAST_SELF:-/media/fat/linux/misterplexd-boot.sh}"   # MAST-WEDGE hand-off: this script
DPATH="/usr/bin:/bin:/usr/sbin:/sbin:/media/fat/linux:."  # PATH seen in running prod environ
RUNDIR="$ROOT/root"                                      # prod cwd
DLOG="$ROOT/tmp/mpxd.log"                                # prod stdout+stderr (append)
LOCK="${BOOT_LOCK:-$ROOT/tmp/misterplexd.lock}"          # written by the daemon (its pid)
BLOG="$ROOT/tmp/misterplexd-boot.log"
BLOCKD="$ROOT/tmp/misterplexd-boot.lock.d"
FAILF="$ROOT/tmp/misterplexd-boot.FAILED"
CORENAME="${BOOT_CORENAME:-$ROOT/tmp/CORENAME}"
STARTUP_DELAY="${BOOT_STARTUP_DELAY:-3}"
NET_WAIT="${BOOT_NET_WAIT:-30}"
CORE_WAIT="${BOOT_CORE_WAIT:-20}"
NET_IF="${BOOT_NET_IF:-eth0}"
NET_ADDR="${BOOT_NET_ADDR:-192.0.2.228}"
TRIES="${BOOT_TRIES:-3}"
START_WAIT="${BOOT_START_WAIT:-20}"
WANT_ENV="$ENV1 $ENV3 $ENV2"   # sorted (LEGACY < MAST < REMOTE)
MAST_LOADER="${BOOT_MAST_LOADER:-$ROOT/media/fat/linux/misterplexd-mast-load.sh}"
PLEX_RBF="${BOOT_PLEX_RBF:-/media/fat/_Utility/Plex.rbf}"
MR_EXIT_WAIT="${BOOT_MR_EXIT_WAIT:-10}"   # s for the handing-off daemon to be gone
MR_SETTLE="${BOOT_MR_SETTLE:-10}"         # s after the load before the verified start
MR_LOCK_WAIT="${BOOT_MR_LOCK_WAIT:-60}"   # s a hand-off waits for a start launcher to yield the lock
MAST_MARKER="${BOOT_MAST_MARKER:-$ROOT/tmp/misterplexd.mast-reload}"   # the daemon's marker path
WANT_CMD="$DAEMON --name $NAME --id $ID --port $PORT --conf $CONF"

# ---- log (kept small: >32 KiB -> last 100 lines) ----
mkdir -p "$ROOT/tmp" 2>/dev/null
if [ -f "$BLOG" ] && [ "$(wc -c < "$BLOG")" -gt 32768 ]; then
  tail -n 100 "$BLOG" > "$BLOG.tmp" 2>/dev/null && mv -f "$BLOG.tmp" "$BLOG"
fi
log() { echo "$(date '+%F %T') up=$(cut -d' ' -f1 /proc/uptime) $*" >> "$BLOG"; [ "$DRY_RUN" = 1 ] && echo "$*"; return 0; }
loud() { log "!!! $*"; echo "$(date '+%F %T') $*" >> "$FAILF"; [ -n "$ROOT" ] || echo "misterplexd-boot: !!! $*" > /dev/console 2>/dev/null; }
act() { if [ "$DRY_RUN" = 1 ]; then log "DRY_RUN would: $*"; else log "do: $*"; "$@"; fi; }
# A 100 ms pacing tick (review 00:18 P1). MiSTer busybox has usleep; elsewhere fractional sleep;
# if neither works, a whole second. Every wait is bounded by the clock (up_cs), never by a count of
# ticks, so a missing fractional sleep cannot shorten MR_LOCK_WAIT or MR_EXIT_WAIT.
tick() { usleep 100000 2>/dev/null || sleep 0.1 2>/dev/null || sleep 1; }
up_cs() { awk '{ printf "%d", $1 * 100 }' /proc/uptime; }        # centiseconds since boot
since() { e=$(( $(up_cs) - $1 )); echo "$((e / 100)).$(((e % 100) / 10))s"; }

MODE=start
case "${1:-start}" in
  start|restart) ;;
  mast-reload) MODE=mast-reload; MR_PID="${2:-}"; MR_REASON="${3:-}"; MR_MARKER="${4:-}" ;;
  *) log "arg '$1' - nothing to do"; exit 0 ;;
esac

# ---- MAST hand-off marker writers (used from the lock wait on) ----
# Every no-load path for a fresh request ends in mr_refuse. It writes state=refused why=<why>,
# which the daemon treats as terminal ('no reload ran (<why>)'; review 21:52 M2, review 21:52).
mr_refuse() { # why detail [what happens instead]
  printf 'state=refused\ntime=%s\nreason=%s\nwhy=%s\n' "$(date +%s)" "$MR_REASON" "$1" > "$MR_MARKER.tmp" &&
    mv -f "$MR_MARKER.tmp" "$MR_MARKER"
  loud "MAST_RELOAD refused ($1): $2 - ${3:-no load, restart only} (marker refused)"
}
# Only the guard's rc 97/98 and R6 PORT SUSPECT's rc 96 are refusals (no load ran). Any other
# non-zero loader rc means a reload was attempted past the guard and failed, so the core state is
# unknown (review 22:00 N1).
mr_loadfail() { # rc
  printf 'state=load-failed\ntime=%s\nreason=%s\nwhy=load-failed-rc%s\n' "$(date +%s)" "$MR_REASON" "$1" > "$MR_MARKER.tmp" &&
    mv -f "$MR_MARKER.tmp" "$MR_MARKER"
  loud "MAST_RELOAD FAILED (load-failed-rc$1): reload attempted, loader rc=$1 (lines in $BLOG) - restart only (marker load-failed)"
}

# ---- single instance of this launcher ----
# Glass G1 23:49 (review 23:53 R1): the gate opens at daemon start, so a boot-time wedge hands
# off while the start launcher still holds this lock in its START_WAIT verify. A mast-reload
# instance therefore never drops its hand-off on a busy lock: it waits (MR_LOCK_WAIT) while the
# start launcher yields to it (mr_yield). If the lock never frees, a still-fresh request is
# refused loudly (why=launcher-busy), so the daemon logs 'no reload ran'.
BOOT_T0=$(date +%s)
lw_t0=$(up_cs)
lw_end=$((lw_t0 + MR_LOCK_WAIT * 100))
waited=0
while ! mkdir "$BLOCKD" 2>/dev/null; do
  op=$(cat "$BLOCKD/pid" 2>/dev/null)
  if [ -n "$op" ] && [ "$op" != "$$" ] && kill -0 "$op" 2>/dev/null; then
    [ "$MODE" = mast-reload ] || { log "another misterplexd-boot pid=$op running - exit"; exit 0; }
    [ "$waited" = 0 ] && log "MAST_RELOAD: launcher lock held by misterplexd-boot pid=$op; waiting up to ${MR_LOCK_WAIT}s for it to yield to this hand-off"
    waited=1
    if [ "$(up_cs)" -ge "$lw_end" ]; then
      st=$(sed -n 's/^state=//p' "$MR_MARKER" 2>/dev/null | head -n 1)
      if [ "$st" = requested ]; then
        mr_refuse launcher-busy "misterplexd-boot pid=$op still holds the launcher lock after ${MR_LOCK_WAIT}s" "no load; the lock holder restarts the daemon"
      else
        loud "MAST_RELOAD: launcher lock still held by pid=$op after ${MR_LOCK_WAIT}s; marker state=[$st] is not a fresh request - no load"
      fi
      exit 0
    fi
  else
    rm -rf "$BLOCKD"   # stale: its holder is gone
  fi
  tick
  [ "$(up_cs)" -le $((lw_end + 1000)) ] || { loud "launcher lock $BLOCKD unusable - exit"; exit 1; }
done
[ "$MODE" = mast-reload ] && [ "$waited" = 1 ] && log "MAST_RELOAD: launcher lock taken after $(since "$lw_t0")"
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
    log "DRY_RUN would: cd $RUNDIR; env -i HOME=/root PATH=$DPATH SHELL=/bin/sh USER=root LOGNAME=root $ENV1 $ENV2 $ENV3 setsid nohup $WANT_CMD >> $DLOG 2>&1 </dev/null &"
    return 0
  fi
  log "start: cd $RUNDIR; env $ENV1 $ENV2 $ENV3 setsid nohup $WANT_CMD >> $DLOG 2>&1 </dev/null &"
  ( cd "$RUNDIR" 2>/dev/null || cd /
    exec env -i HOME=/root PATH="$DPATH" SHELL=/bin/sh USER=root LOGNAME=root "$ENV1" "$ENV2" "$ENV3" \
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

# MAST-WEDGE hand-off (see the header). Restart-only on anything but a fresh request. The marker
# writers mr_refuse and mr_loadfail sit above the lock wait.
# R1 (glass G1 23:49, review 23:53): a daemon this start launcher started can hand a reload off
# inside START_WAIT; its mast-reload instance then waits for our lock. A hand-off is pending when
# the marker says requested and is no older than this launcher. The start launcher then stops:
# no verify, no retry, no daemon start, and its exit releases the lock to the mast-reload
# instance, which runs the ONE load and the verified start.
mr_pending() {
  [ "$MODE" = start ] || return 1
  [ "$(sed -n 's/^state=//p' "$MAST_MARKER" 2>/dev/null | head -n 1)" = requested ] || return 1
  t=$(sed -n 's/^time=//p' "$MAST_MARKER" 2>/dev/null | head -n 1)
  [ -n "$t" ] && [ "$t" -ge "$BOOT_T0" ] 2>/dev/null
}
mr_yield() { # where
  mr_pending || return 0
  log "MAST_HANDOFF pending at $1 (marker $MAST_MARKER requested at $(sed -n 's/^time=//p' "$MAST_MARKER" | head -n 1), this launcher started $BOOT_T0, reason=[$(sed -n 's/^reason=//p' "$MAST_MARKER" | head -n 1)]): start launcher yields - no verify, no retry, no daemon start; the lock goes to the mast-reload instance"
  exit 0
}
mast_reload() {
  log "MAST_RELOAD request daemon_pid=$MR_PID reason=[$MR_REASON] marker=$MR_MARKER rbf=$PLEX_RBF"
  st=$(sed -n 's/^state=//p' "$MR_MARKER" 2>/dev/null | head -n 1)
  if [ "$st" != requested ]; then
    loud "MAST_RELOAD refused: marker state=[$st] is not 'requested' (one reload per episode, never a loop) - restart only"
    return 0
  fi
  t0=$(up_cs)
  while [ -n "$MR_PID" ] && kill -0 "$MR_PID" 2>/dev/null && [ "$(up_cs)" -lt $((t0 + MR_EXIT_WAIT * 100)) ]; do
    tick
  done
  if [ -n "$MR_PID" ] && kill -0 "$MR_PID" 2>/dev/null; then
    mr_refuse "daemon-alive" "misterplexd pid=$MR_PID still running after ${MR_EXIT_WAIT}s"
    return 0
  fi
  log "MAST_RELOAD: daemon pid=$MR_PID gone after $(since "$t0")"
  # M1 (review 21:52, lead's product rule): never replace a core or menu the user switched to.
  cn=$(cat "$CORENAME" 2>/dev/null)
  if [ "$cn" != Plex ]; then
    mr_refuse "core-switched:$cn" "CORENAME=[$cn] is not Plex right before the load: the auto reload never replaces a core or menu the user switched to"
    return 0
  fi
  printf 'state=attempted\ntime=%s\nreason=%s\n' "$(date +%s)" "$MR_REASON" > "$MR_MARKER.tmp" &&
    mv -f "$MR_MARKER.tmp" "$MR_MARKER"
  log "MAST_RELOAD: CORENAME=[Plex]; marker attempted; ONE guarded load_core $PLEX_RBF via $MAST_LOADER"
  if [ "$DRY_RUN" = 1 ]; then log "DRY_RUN would: sh $MAST_LOADER $PLEX_RBF"; return 0; fi
  [ -f "$MAST_LOADER" ] || { mr_refuse "loader-missing" "guarded loader $MAST_LOADER missing"; return 0; }
  MAST_LOAD_MARKER="$MR_MARKER" MAST_LOAD_CORENAME="$CORENAME" sh "$MAST_LOADER" "$PLEX_RBF" >> "$BLOG" 2>&1
  rc=$?
  case $rc in
    0) ;;
    96) mr_refuse "r6-port-suspect" "R6 PORT SUSPECT: the core's PLXR/PLXT ring words read with a wrong magic before the load (an offset f2sdram port, RCA §12/§13; D10 and 0x30140000-01F words in $BLOG)" "no load, restart only: HPS reboot first (R6 cannot heal an offset port)"; return 0 ;;
    97|98) mr_refuse "guard-rc$rc" "LOADCORE_GUARD refused the load (loader rc=$rc; LOADCORE_GUARD lines in $BLOG)"; return 0 ;;
    *) mr_loadfail "$rc"; sleep "$MR_SETTLE"; return 0 ;;
  esac
  sleep "$MR_SETTLE"
  log "MAST_RELOAD: loaded; CORENAME=[$(cat "$CORENAME" 2>/dev/null)] after ${MR_SETTLE}s settle; verified start follows"
}

# ---- main ----
log "=== misterplexd-boot start pid=$$ arg=${1:-start} DRY_RUN=$DRY_RUN ROOT=${ROOT:-/}"
[ "$MODE" = mast-reload ] && mast_reload

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
mr_yield "CORENAME wait"

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
  mr_yield "attempt $n"
  log "--- attempt $n"
  if kill_all && clean_lock_port; then
    start_daemon
    if [ "$DRY_RUN" = 1 ]; then
      log "DRY_RUN would verify: one pid exe=$DAEMON md5=$EXPECT_MD5 env=[$WANT_ENV] cmd=[$WANT_CMD] port=$PORT lock=pid"
      exit 0
    fi
    i=0
    while [ $i -lt $START_WAIT ]; do
      mr_yield "START_WAIT ${i}s"
      P=$(canon_pids); [ -n "$P" ] && [ "$(port_owner)" = "$P" ] && break
      sleep 1; i=$((i+1))
    done
    sleep 2
    mr_yield "verify"
    if verify; then log "PROD_START_OK attempt $n pid=$P"; rm -f "$FAILF"; exit 0; fi
    log "PROD_START_BAD attempt $n"
  else
    log "attempt $n: strays/lock/port not clean"
  fi
  n=$((n+1)); sleep 2
done
mr_yield "final"   # a hand-off after the last verify is no start failure (review 00:18 P2)
loud "PROD START FAILED after $TRIES attempts ($DAEMON, env $WANT_ENV) - see $BLOG and $DLOG"
exit 1
