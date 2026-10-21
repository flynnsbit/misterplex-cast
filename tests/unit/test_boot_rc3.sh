#!/bin/sh
# Boot script against a fake /media/fat. No device, no /dev/MiSTer_cmd.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT="$ROOT/scripts/misterplexd-boot-rc3.sh"
fail() { echo "FAIL $*" >&2; exit 1; }
[ -f "$SCRIPT" ] || fail "missing script"
grep -q 'fsync_path' "$SCRIPT" || fail "conf rewrite is not fsynced"
grep -q 'mpx-tmp' "$SCRIPT" || fail "conf rewrite is not atomic"
grep -q 'last3_name' "$SCRIPT" || fail "last-3 formatter missing"
grep -F 'NAME="MiSTerPlex-$ip"' "$SCRIPT" && fail "full-ip friendly name is still assigned"
grep -F 'ID="misterplex-$ip"' "$SCRIPT" && fail "full-ip player id is still assigned"
grep -q 'legacy_assign' "$SCRIPT" && fail "legacy NAME= import is still a source"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/media/fat/misterplex/bin" "$work/media/fat/linux" "$work/media/fat/_Utility" "$work/tmp" "$work/cmd"
printf '%s\n' '#!/bin/sh' \
  'echo "pid=$$ prefix=${MPX_LEGACY_CORE_PREFIX:-} gair=${GAIRPLAY:-} gyt=${GYT:-}" > "$BOOT_MARK"' \
  'echo "$*" >> "$BOOT_MARK"' \
  'exit 0' \
  > "$work/media/fat/misterplex/bin/misterplexd"
chmod +x "$work/media/fat/misterplex/bin/misterplexd"
echo 'not-a-core' > "$work/media/fat/_Utility/Plex.rbf"
echo MENU > "$work/tmp/CORENAME"
: > "$work/cmd/MiSTer_cmd"

(
  while [ ! -s "$work/cmd/MiSTer_cmd" ]; do sleep 0.05; done
  echo PlexOB > "$work/tmp/CORENAME"
) &
waiter=$!

printf '%s\n' 'FRIENDLY_NAME=MiSTerPlex-192.0.2.12' 'PLAYER_ID=misterplex-192.0.2.12' \
  > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=5 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.11 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot.log" \
  sh "$SCRIPT" || fail "boot rc=$?"
wait "$waiter" || true

grep -q 'load_core .*/_Utility/Plex.rbf' "$work/cmd/MiSTer_cmd" || fail "did not load Plex.rbf"
grep -q 'prefix=999800cf' "$work/tmp/started" || fail "daemon env missing prefix"
grep -q 'gair=1' "$work/tmp/started" || fail "daemon env missing GAIRPLAY=1"
grep -q 'gyt=1' "$work/tmp/started" || fail "daemon env missing GYT=1"
grep -q -- '--conf .*/linux/misterplex.conf' "$work/tmp/started" || fail "launch missing --conf linux"
grep -q -- '--name MiSTerPlex-192.0.2.12' "$work/tmp/started" || fail "name not kept from conf"
grep -q -- '--id misterplex-192.0.2.12' "$work/tmp/started" || fail "id not kept from conf"
grep -q 'MiSTerPlex-192.0.2.11' "$work/tmp/started" && fail "saved name fell back to ipv4"
grep -q 'GAIRPLAY=1' "$work/tmp/boot.log" || fail "log missing GAIRPLAY"
grep -q 'GYT=1' "$work/tmp/boot.log" || fail "log missing GYT"
grep -q 'MPX_LEGACY_CORE_PREFIX=999800cf' "$work/tmp/boot.log" || fail "log missing prefix"

# Already on PlexOB: no second load_core, daemon still starts.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
echo PlexOB > "$work/tmp/CORENAME"
echo 'old' > "$work/media/fat/_Utility/PlexOB.rbf"
rm -f "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.98 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot2.log" \
  sh "$SCRIPT" || fail "second boot rc=$?"
[ ! -s "$work/cmd/MiSTer_cmd" ] || fail "reloaded a core that was already PlexOB"
grep -q 'prefix=999800cf' "$work/tmp/started" || fail "second start missing prefix"
grep -q 'gair=1' "$work/tmp/started" || fail "second start missing GAIRPLAY"
grep -q 'gyt=1' "$work/tmp/started" || fail "second start missing GYT"
grep -q -- '--name misterplex-0.2.98 --id misterplex-0.2.98' "$work/tmp/started" || fail "name not last-3 from ipv4: $(cat "$work/tmp/started")"
grep -q 'FRIENDLY_NAME=misterplex-0.2.98' "$work/media/fat/linux/misterplex.conf" || fail "last-3 name not saved"
grep -q 'PLAYER_ID=misterplex-0.2.98' "$work/media/fat/linux/misterplex.conf" || fail "last-3 id not saved"
grep -q 'MiSTerPlex-192.0.2.98' "$work/tmp/started" && fail "full-ip friendly name was written"
grep -q 'misterplex-192.0.2.98' "$work/tmp/started" && fail "full-ip player id was written"
grep -q -- '--conf .*/linux/misterplex.conf' "$work/tmp/started" || fail "missing conf fell back to the wrong path"

# linux conf absent, misterplex/ conf present: use the fallback path.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/media/fat/linux/misterplex.conf"
mkdir -p "$work/media/fat/misterplex"
printf '%s\n' 'FRIENDLY_NAME=FromFallback' 'PLAYER_ID=id-fallback' \
  > "$work/media/fat/misterplex/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot3.log" \
  sh "$SCRIPT" || fail "fallback boot rc=$?"
grep -q -- '--conf .*/misterplex/misterplex.conf' "$work/tmp/started" || fail "did not fall back to misterplex conf"
grep -q -- '--name FromFallback' "$work/tmp/started" || fail "fallback name"
grep -q 'gair=1' "$work/tmp/started" && grep -q 'gyt=1' "$work/tmp/started" || fail "fallback launch env"

# Old supervise line, commented out, is the saved identity. A live IPv4 must not replace it.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' '# kept from the previous build' \
  '# /media/fat/misterplex/bin/misterplexd --name MiSTerPlex-192.0.2.12 --id misterplex-192.0.2.12 --conf /media/fat/linux/misterplex.conf' \
  > "$work/media/fat/linux/user-startup.sh"
printf '%s\n' '# no name yet' > "$work/media/fat/linux/misterplex.conf"
rm -f "$work/media/fat/misterplex/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.99 \
BOOT_USER_STARTUP="$work/media/fat/linux/user-startup.sh" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot4.log" \
  sh "$SCRIPT" || fail "old-script boot rc=$?"
grep -q -- '--name MiSTerPlex-192.0.2.12' "$work/tmp/started" || fail "old script name not imported"
grep -q -- '--id misterplex-192.0.2.12' "$work/tmp/started" || fail "old script id not imported"
grep -q 'MiSTerPlex-192.0.2.99' "$work/tmp/started" && fail "imported name fell back to ipv4"
grep -q 'FRIENDLY_NAME=MiSTerPlex-192.0.2.12' "$work/media/fat/linux/misterplex.conf" || fail "import did not persist name"
grep -q 'PLAYER_ID=misterplex-192.0.2.12' "$work/media/fat/linux/misterplex.conf" || fail "import did not persist id"

# Link-local is not an identity address.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/media/fat/linux/user-startup.sh"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=169.254.1.1 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot5.log" \
  sh "$SCRIPT" || fail "link-local boot rc=$?"
grep -q -- '--name MiSTerPlex-169.254.1.1' "$work/tmp/started" && fail "link-local became the name"
grep -q -- '--name MiSTerPlex --id misterplex-1' "$work/tmp/started" || fail "link-local did not use the no-address name"
grep -q 'FRIENDLY_NAME=MiSTerPlex$' "$work/media/fat/linux/misterplex.conf" && fail "link-local fallback name was saved"
grep -q 'PLAYER_ID=misterplex-1' "$work/media/fat/linux/misterplex.conf" && fail "link-local fallback id was saved"

# Persisted identity file, no conf keys, no startup line.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
mkdir -p "$work/media/fat/linux/misterplex"
printf '%s\n' 'FRIENDLY_NAME=FromIdentity' 'PLAYER_ID=id-file' \
  > "$work/media/fat/linux/misterplex/identity"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.88 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot6.log" \
  sh "$SCRIPT" || fail "identity-file boot rc=$?"
grep -q -- '--name FromIdentity' "$work/tmp/started" || fail "identity file name"
grep -q -- '--id id-file' "$work/tmp/started" || fail "identity file id"
grep -q 'MiSTerPlex-192.0.2.88' "$work/tmp/started" && fail "identity file fell back to ipv4"
grep -q 'FRIENDLY_NAME=FromIdentity' "$work/media/fat/linux/misterplex.conf" || fail "identity file not persisted"

# Prod path wins over _Utility when both PlexOB rbFs exist.
rm -f "$work/tmp/misterplexd.pid" "$work/cmd/MiSTer_cmd"
echo MENU > "$work/tmp/CORENAME"
echo 'prod' > "$work/media/fat/PlexOB.rbf"
echo 'util' > "$work/media/fat/_Utility/PlexOB.rbf"
(
  while [ ! -s "$work/cmd/MiSTer_cmd" ]; do sleep 0.05; done
  echo PlexOB > "$work/tmp/CORENAME"
) &
plex_waiter=$!
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=5 \
BOOT_NET_WAIT=0 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot7.log" \
  sh "$SCRIPT" || fail "plexob path boot rc=$?"
wait "$plex_waiter" || true
grep -q 'load_core .*/media/fat/PlexOB.rbf' "$work/cmd/MiSTer_cmd" || fail "did not prefer /media/fat/PlexOB.rbf"
grep -q '_Utility/PlexOB.rbf' "$work/cmd/MiSTer_cmd" && fail "used _Utility while prod PlexOB.rbf exists"

# Update stamp is written only after update_ytdlp.sh succeeds.
rm -f "$work/tmp/misterplexd.pid"
upd="$work/upd/update_ytdlp.sh"
mkdir -p "$work/upd" "$work/media/fat/linux/misterplex"
printf '%s\n' '#!/bin/sh' 'exit 1' > "$upd"
chmod +x "$upd"
stamp="$work/media/fat/linux/misterplex/yt-dlp.update-stamp"
rm -f "$stamp"
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
YTDLP_SELF_UPDATE=1 \
YTDLP_UPDATE_FOREGROUND=1 \
BOOT_UPDATE_YTDLP="$upd" \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot8.log" \
  sh "$SCRIPT" || fail "failed-update boot rc=$?"
[ ! -f "$stamp" ] || fail "stamp written after a failed update"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$upd"
rm -f "$work/tmp/misterplexd.pid"
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
YTDLP_SELF_UPDATE=1 \
YTDLP_UPDATE_FOREGROUND=1 \
BOOT_UPDATE_YTDLP="$upd" \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot9.log" \
  sh "$SCRIPT" || fail "ok-update boot rc=$?"
[ -f "$stamp" ] || fail "stamp missing after a successful update"

# Quoted --name keeps its spaces. --id must not match --idle-*.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' '/media/fat/misterplex/bin/misterplexd --idle-card --name "Den TV" --id misterplex-den' \
  > "$work/media/fat/linux/user-startup.sh"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
BOOT_USER_STARTUP="$work/media/fat/linux/user-startup.sh" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-quote.log" \
  sh "$SCRIPT" || fail "quoted-name boot rc=$?"
grep -q -- '--name Den TV --id misterplex-den' "$work/tmp/started" || fail "quoted name was cut: $(cat "$work/tmp/started")"
grep -q 'le-card' "$work/tmp/started" && fail "--id matched --idle-card"
grep -q 'FRIENDLY_NAME=Den TV' "$work/media/fat/linux/misterplex.conf" || fail "quoted name not persisted whole"

# Two boots: no network must not save the fallback; the next networked boot
# derives misterplex-<last three octets> for both keys.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/media/fat/linux/user-startup.sh" \
  "$work/media/fat/linux/misterplex/identity" "$work/media/fat/misterplex/identity"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-nonet.log" \
  sh "$SCRIPT" || fail "no-net boot rc=$?"
grep -q -- '--name MiSTerPlex --id misterplex-1' "$work/tmp/started" || fail "no-net boot did not use the fallback"
grep -q 'FRIENDLY_NAME=' "$work/media/fat/linux/misterplex.conf" && fail "no-net boot saved a name"
grep -q 'PLAYER_ID=' "$work/media/fat/linux/misterplex.conf" && fail "no-net boot saved an id"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.50 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-net.log" \
  sh "$SCRIPT" || fail "networked boot rc=$?"
grep -q -- '--name misterplex-0.2.50 --id misterplex-0.2.50' "$work/tmp/started" || fail "networked boot did not derive last-3: $(cat "$work/tmp/started")"
grep -q 'FRIENDLY_NAME=misterplex-0.2.50' "$work/media/fat/linux/misterplex.conf" || fail "derived name not saved"
grep -q 'PLAYER_ID=misterplex-0.2.50' "$work/media/fat/linux/misterplex.conf" || fail "derived id not saved"
grep -q 'MiSTerPlex-192.0.2.50' "$work/media/fat/linux/misterplex.conf" && fail "full-ip name was saved"
grep -q 'misterplex-192.0.2.50' "$work/media/fat/linux/misterplex.conf" && fail "full-ip id was saved"

# A name already in the conf survives a no-network boot and a later address.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' 'FRIENDLY_NAME=Living Room' 'PLAYER_ID=misterplex-living' \
  > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-saved-nonet.log" \
  sh "$SCRIPT" || fail "saved no-net boot rc=$?"
grep -q -- '--name Living Room' "$work/tmp/started" || fail "saved name lost on the no-network boot"
grep -q -- '--id misterplex-living' "$work/tmp/started" || fail "saved id lost on the no-network boot"
grep -q 'MiSTerPlex --id misterplex-1' "$work/tmp/started" && fail "saved name became the fallback"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.50 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-saved-net.log" \
  sh "$SCRIPT" || fail "saved networked boot rc=$?"
grep -q -- '--name Living Room' "$work/tmp/started" || fail "saved name lost once the network appeared"
grep -q 'MiSTerPlex-192.0.2.50' "$work/tmp/started" && fail "saved name was replaced by the address"
grep -q 'FRIENDLY_NAME=Living Room' "$work/media/fat/linux/misterplex.conf" || fail "saved name rewritten"

# $1=stop: INT, then KILL if it is still alive. No load, no launch, no save.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd" \
  "$work/media/fat/linux/user-startup.sh" "$work/media/fat/linux/misterplexd-boot.sh"
printf '%s\n' 'FRIENDLY_NAME=Living Room' 'PLAYER_ID=misterplex-living' \
  > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-before-stop"
rm -f "$work/tmp/held.ready"
python3 -c 'import signal,time,sys; signal.signal(signal.SIGINT, signal.SIG_IGN); open(sys.argv[1],"w").write("ready"); time.sleep(30)' "$work/tmp/held.ready" &
held=$!
i=0
while [ ! -s "$work/tmp/held.ready" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || fail "held daemon did not install SIGINT ignore"
  sleep 0.05
done
echo "$held" > "$work/tmp/misterplexd.pid"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-stop.log" \
  sh "$SCRIPT" stop || fail "stop rc=$?"
sleep 0.2
held_state=$(ps -o stat= -p "$held" 2>/dev/null || true)
case "$held_state" in
  ''|Z*) ;;
  *)
    kill -KILL "$held" 2>/dev/null || true
    wait "$held" 2>/dev/null || true
    fail "stop left a daemon that ignored INT (state=$held_state)"
    ;;
esac
wait "$held" 2>/dev/null || true
[ ! -f "$work/tmp/misterplexd.pid" ] || fail "stop left the pidfile"
[ ! -s "$work/tmp/started" ] || fail "stop launched the daemon"
[ ! -s "$work/cmd/MiSTer_cmd" ] || fail "stop loaded a core"
cmp -s "$work/tmp/conf-before-stop" "$work/media/fat/linux/misterplex.conf" || fail "stop rewrote the conf"
grep -q 'stop: INT' "$work/tmp/boot-stop.log" || fail "stop did not INT"
grep -q 'stop: KILL' "$work/tmp/boot-stop.log" || fail "stop did not KILL a daemon that ignored INT"
# A daemon that dies on INT is not KILLed, and the conf stays put.
# Reset SIGINT: a shell background job inherits SIG_IGN, and python leaves that alone.
rm -f "$work/tmp/soft.ready"
python3 -c 'import signal,time,sys; signal.signal(signal.SIGINT, signal.SIG_DFL); open(sys.argv[1],"w").write("ready"); time.sleep(30)' "$work/tmp/soft.ready" &
soft=$!
i=0
while [ ! -s "$work/tmp/soft.ready" ]; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || fail "int daemon did not reset SIGINT"
  sleep 0.05
done
echo "$soft" > "$work/tmp/misterplexd.pid"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-stop-int.log" \
  sh "$SCRIPT" stop || fail "int-stop rc=$?"
sleep 0.2
soft_state=$(ps -o stat= -p "$soft" 2>/dev/null || true)
case "$soft_state" in
  ''|Z*) ;;
  *)
    kill -KILL "$soft" 2>/dev/null || true
    wait "$soft" 2>/dev/null || true
    fail "INT stop left the daemon (state=$soft_state)"
    ;;
esac
wait "$soft" 2>/dev/null || true
grep -q 'stop: KILL' "$work/tmp/boot-stop-int.log" && fail "INT stop also sent KILL"
cmp -s "$work/tmp/conf-before-stop" "$work/media/fat/linux/misterplex.conf" || fail "INT stop rewrote the conf"
# Anything other than start or empty does not boot.
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-ignore.log" \
  sh "$SCRIPT" reload || fail "reload rc=$?"
[ ! -s "$work/tmp/started" ] || fail "reload launched the daemon"
grep -q "ignore argument 'reload'" "$work/tmp/boot-ignore.log" || fail "reload was not ignored"
cmp -s "$work/tmp/conf-before-stop" "$work/media/fat/linux/misterplex.conf" || fail "reload rewrote the conf"

# Legacy misterplexd-boot.sh NAME=/ID= is not a source. An empty conf is
# named from the address, last three octets, same string for both keys.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" \
  "$work/media/fat/linux/misterplex/identity" "$work/media/fat/misterplex/identity"
printf '%s\n' '# NAME=Wrong' 'NAME="MiSTerPlex-192.0.2.12"' 'ID=misterplex-192.0.2.12' \
  > "$work/media/fat/linux/misterplexd-boot.sh"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-legacy.log" \
  sh "$SCRIPT" start || fail "legacy boot rc=$?"
grep -q -- '--name misterplex-0.2.228 --id misterplex-0.2.228' "$work/tmp/started" || fail "legacy NAME= beat last-3: $(cat "$work/tmp/started")"
grep -q 'MiSTerPlex-192.0.2.12' "$work/tmp/started" && fail "legacy NAME= was imported"
grep -q 'misterplex-192.0.2.12' "$work/tmp/started" && fail "legacy ID= was imported"
grep -q 'FRIENDLY_NAME=misterplex-0.2.228' "$work/media/fat/linux/misterplex.conf" || fail "last-3 name not saved over a legacy file"
grep -q 'PLAYER_ID=misterplex-0.2.228' "$work/media/fat/linux/misterplex.conf" || fail "last-3 id not saved over a legacy file"
grep -q 'MiSTerPlex-192.0.2.228' "$work/media/fat/linux/misterplex.conf" && fail "full-ip friendly name was written"
grep -q 'misterplex-192.0.2.228' "$work/media/fat/linux/misterplex.conf" && fail "full-ip player id was written"
grep -q 'MiSTerPlex-192.0.2.12' "$work/media/fat/linux/misterplex.conf" && fail "legacy name was persisted"

# A conf that already has a name, including an old full-IP pair, is left
# byte-identical. Legacy NAME= does not heal it, and a different address
# does not rewrite it.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' 'FRIENDLY_NAME=MiSTerPlex-192.0.2.228' 'PLAYER_ID=misterplex-192.0.2.228' \
  > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-kept-fullip"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.5 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-heal.log" \
  sh "$SCRIPT" || fail "saved-fullip boot rc=$?"
cmp -s "$work/tmp/conf-kept-fullip" "$work/media/fat/linux/misterplex.conf" || fail "saved full-ip conf was rewritten: $(cat "$work/media/fat/linux/misterplex.conf")"
grep -q -- '--name MiSTerPlex-192.0.2.228' "$work/tmp/started" || fail "saved full-ip name was not kept"
grep -q -- '--id misterplex-192.0.2.228' "$work/tmp/started" || fail "saved full-ip id was not kept"
grep -q 'misterplex-0.2.5' "$work/tmp/started" && fail "saved name replaced by last-3"
grep -q 'MiSTerPlex-192.0.2.5' "$work/tmp/started" && fail "old full-ip write path still runs"
grep -q 'MiSTerPlex-192.0.2.12' "$work/tmp/started" && fail "legacy NAME= healed a saved name"
# A conf name that is not an address is left alone.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' 'FRIENDLY_NAME=Living Room' 'PLAYER_ID=misterplex-living' \
  > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-keep-name.log" \
  sh "$SCRIPT" || fail "named-conf boot rc=$?"
grep -q -- '--name Living Room' "$work/tmp/started" || fail "legacy NAME= replaced a real conf name"
grep -q 'FRIENDLY_NAME=Living Room' "$work/media/fat/linux/misterplex.conf" || fail "real conf name rewritten from legacy"

# user-startup.sh --name still beats the legacy file (legacy is the next source).
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' '# /media/fat/misterplex/bin/misterplexd --name Den --id misterplex-den' \
  > "$work/media/fat/linux/user-startup.sh"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
BOOT_USER_STARTUP="$work/media/fat/linux/user-startup.sh" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-script-over-legacy.log" \
  sh "$SCRIPT" || fail "script-over-legacy rc=$?"
grep -q -- '--name Den' "$work/tmp/started" || fail "user-startup name lost to the legacy file"
grep -q 'MiSTerPlex-192.0.2.12' "$work/tmp/started" && fail "legacy name beat user-startup.sh"
rm -f "$work/media/fat/linux/user-startup.sh"

# Two NICs: eth0 is first in ifconfig, wlan0 owns the default route.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" \
  "$work/media/fat/linux/misterplexd-boot.sh" \
  "$work/media/fat/linux/misterplex/identity" "$work/media/fat/misterplex/identity"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
mkdir -p "$work/bin"
cat > "$work/bin/ifconfig" << 'EOF'
#!/bin/sh
cat << 'OUT'
eth0      Link encap:Ethernet
          inet addr:192.0.2.228  Bcast:192.0.2.254  Mask:255.255.255.0
wlan0     Link encap:Ethernet
          inet addr:192.0.2.12  Bcast:192.0.2.255  Mask:255.255.255.0
OUT
EOF
chmod +x "$work/bin/ifconfig"
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'wlan0 00000000 0100000A 0003 0 0 0 00000000 0 0 0' \
  'eth0 0000000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route"
PATH="$work/bin:$PATH" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-route.log" \
  sh "$SCRIPT" start || fail "multi-nic boot rc=$?"
grep -q -- '--name misterplex-0.2.12 --id misterplex-0.2.12' "$work/tmp/started" || fail "default-route last-3 not used: $(cat "$work/tmp/started")"
grep -q 'misterplex-0.2.228' "$work/tmp/started" && fail "first ifconfig address won over the default route"
grep -q 'MiSTerPlex-192.0.2.12' "$work/tmp/started" && fail "full-ip friendly name was written"
grep -q 'misterplex-192.0.2.12' "$work/tmp/started" && fail "full-ip player id was written"
grep -q 'FRIENDLY_NAME=misterplex-0.2.12' "$work/media/fat/linux/misterplex.conf" || fail "route-derived name not saved"
grep -q 'PLAYER_ID=misterplex-0.2.12' "$work/media/fat/linux/misterplex.conf" || fail "route-derived id not saved"

# The yt-dlp refresh is backgrounded, so a slow updater does not hold boot.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/tmp/ytdlp.pid"
printf '%s\n' '#!/bin/sh' 'echo $$ > "$YTDLP_TEST_PID"' 'exec sleep 30' > "$work/bin/slow-ytdlp.sh"
chmod +x "$work/bin/slow-ytdlp.sh"
t0=$(date +%s)
YTDLP_TEST_PID="$work/tmp/ytdlp.pid" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
BOOT_UPDATE_YTDLP="$work/bin/slow-ytdlp.sh" \
YTDLP_SELF_UPDATE=1 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-ytdlp-bg.log" \
  sh "$SCRIPT" || fail "background ytdlp boot rc=$?"
t1=$(date +%s)
[ $((t1 - t0)) -lt 3 ] || fail "yt-dlp updater delayed boot ($((t1 - t0))s)"
if [ -f "$work/tmp/ytdlp.pid" ]; then
  kill -KILL "$(cat "$work/tmp/ytdlp.pid")" 2>/dev/null || true
fi

# Documented hook line passes "$1". install rewrites a no-arg line, once, with a backup.
grep -F 'misterplexd-boot-rc3.sh "$1" &' "$SCRIPT" >/dev/null || fail "hook does not document \"\$1\""
grep -F 'misterplexd-boot-rc3.sh &' "$SCRIPT" >/dev/null && fail "hook still documents the no-arg line"
grep -F 'misterplexd-boot-rc3.sh "$1" &' "$ROOT/README.md" >/dev/null || fail "README dropped \"\$1\""
grep -F 'misterplexd-boot-rc3.sh "$1" &' "$ROOT/docs/release.md" >/dev/null || fail "release notes dropped \"\$1\""
grep -F 'misterplexd-boot-rc3.sh "\$1" &' "$ROOT/scripts/package_release.sh" >/dev/null || fail "package README dropped \"\$1\""
canon="$work/canon-startup.sh"
printf '%s\n' '/media/fat/misterplex/bin/misterplexd-boot-rc3.sh &' > "$canon"
BOOT_HOOK_PATH=/media/fat/misterplex/bin/misterplexd-boot-rc3.sh \
BOOT_INSTALL_FILES="$canon" \
BOOT_INSTALL_STAMP=canon \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
BOOT_LOG="$work/tmp/install-canon.log" \
  sh "$SCRIPT" install || fail "install rc=$?"
grep -F '/media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &' "$canon" >/dev/null || fail "install did not write \"\$1\": $(cat "$canon")"
grep -F '/media/fat/misterplex/bin/misterplexd-boot-rc3.sh &' "$canon" >/dev/null && fail "no-arg line survived install"
[ -f "$canon.bak.canon" ] || fail "install did not back up user-startup.sh"
printf '%s\n' '/media/fat/misterplex/bin/misterplexd-boot-rc3.sh &' > "$work/tmp/canon-expected"
cmp -s "$canon.bak.canon" "$work/tmp/canon-expected" || fail "backup is not the old line"
before_second=$(cat "$canon")
BOOT_HOOK_PATH=/media/fat/misterplex/bin/misterplexd-boot-rc3.sh \
BOOT_INSTALL_FILES="$canon" \
BOOT_INSTALL_STAMP=canon2 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
BOOT_LOG="$work/tmp/install-canon2.log" \
  sh "$SCRIPT" install || fail "second install rc=$?"
[ ! -f "$canon.bak.canon2" ] || fail "idempotent install wrote another backup"
[ "$(cat "$canon")" = "$before_second" ] || fail "idempotent install rewrote the hook line"

# That line, under user-startup.sh stop, must not launch and must not write the conf.
chmod +x "$SCRIPT"
ustart="$work/media/fat/linux/user-startup.sh"
printf '%s\n' "$SCRIPT &" > "$ustart"
BOOT_HOOK_PATH="$SCRIPT" \
BOOT_INSTALL_FILES="$ustart" \
BOOT_INSTALL_STAMP=sim \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
BOOT_LOG="$work/tmp/install-sim.log" \
  sh "$SCRIPT" install || fail "sim install rc=$?"
grep -F "$SCRIPT \"\$1\" &" "$ustart" >/dev/null || fail "sim install line: $(cat "$ustart")"
printf '%s\n' 'FRIENDLY_NAME=Living Room' 'PLAYER_ID=misterplex-living' \
  > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-ustart"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd" "$work/tmp/boot-ustart.log"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-ustart.log" \
  sh "$ustart" stop || fail "user-startup stop rc=$?"
i=0
while ! grep -q 'stop: argument' "$work/tmp/boot-ustart.log" 2>/dev/null; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || fail "documented line did not deliver stop: $(cat "$work/tmp/boot-ustart.log" 2>/dev/null || true)"
  sleep 0.05
done
[ ! -s "$work/tmp/started" ] || fail "user-startup stop launched the daemon"
[ ! -s "$work/cmd/MiSTer_cmd" ] || fail "user-startup stop loaded a core"
cmp -s "$work/tmp/conf-ustart" "$work/media/fat/linux/misterplex.conf" || fail "user-startup stop wrote the conf"
grep -q 'load_core' "$work/tmp/boot-ustart.log" && fail "user-startup stop reached load_core"
grep -q 'network ready' "$work/tmp/boot-ustart.log" && fail "user-startup stop waited on the network"

# No argument, but rcK is already running: do not launch and do not save.
mkdir -p "$work/proc/1" "$work/proc/8"
printf '%s' 'init' > "$work/proc/1/cmdline"
printf '%s' '/etc/init.d/rcK' > "$work/proc/8/cmdline"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
cp "$work/tmp/conf-ustart" "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.228 \
BOOT_PROC="$work/proc" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-rck.log" \
  sh "$SCRIPT" || fail "rcK no-arg rc=$?"
grep -q 'shutdown in progress' "$work/tmp/boot-rck.log" || fail "rcK was not treated as shutdown"
[ ! -s "$work/tmp/started" ] || fail "rcK no-arg launched the daemon"
[ ! -s "$work/cmd/MiSTer_cmd" ] || fail "rcK no-arg loaded a core"
cmp -s "$work/tmp/conf-ustart" "$work/media/fat/linux/misterplex.conf" || fail "rcK no-arg wrote the conf"
# A proc that is not shutting down still boots.
echo PlexOB > "$work/tmp/CORENAME"
printf '%s' 'init' > "$work/proc/1/cmdline"
rm -f "$work/proc/8/cmdline"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
BOOT_PROC="$work/proc" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-not-rck.log" \
  sh "$SCRIPT" || fail "idle proc no-arg rc=$?"
[ -s "$work/tmp/started" ] || fail "idle proc refused to boot"

# rcK and S99user run this script as /bin/sh <script>, so argv0 is sh and the
# script name is argv[1]. That no-arg invocation is still shutdown.
mkdir -p "$work/proc/1" "$work/proc/9"
printf '%s' 'init' > "$work/proc/1/cmdline"
rm -f "$work/proc/8/cmdline"
printf '%s\0%s' '/bin/sh' '/etc/init.d/rcK' > "$work/proc/9/cmdline"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
cp "$work/tmp/conf-ustart" "$work/media/fat/linux/misterplex.conf"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=203.0.113.12 \
BOOT_PROC="$work/proc" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-rck-argv1.log" \
  sh "$SCRIPT" || fail "rcK argv1 no-arg rc=$?"
grep -q 'shutdown in progress' "$work/tmp/boot-rck-argv1.log" || fail "rcK argv1 was not treated as shutdown"
[ ! -s "$work/tmp/started" ] || fail "rcK argv1 launched the daemon"
printf '%s\0%s\0%s' '/bin/sh' '/etc/init.d/S99user' 'stop' > "$work/proc/9/cmdline"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=2 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=203.0.113.12 \
BOOT_PROC="$work/proc" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-s99-argv1.log" \
  sh "$SCRIPT" || fail "S99user argv1 no-arg rc=$?"
grep -q 'shutdown in progress' "$work/tmp/boot-s99-argv1.log" || fail "S99user argv1 was not treated as shutdown"
[ ! -s "$work/tmp/started" ] || fail "S99user argv1 launched the daemon"
# /bin/sh user-startup.sh with no stop is a boot, not a shutdown.
printf '%s\0%s' '/bin/sh' '/media/fat/linux/user-startup.sh' > "$work/proc/9/cmdline"
echo PlexOB > "$work/tmp/CORENAME"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
BOOT_PROC="$work/proc" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-sh-ustart.log" \
  sh "$SCRIPT" || fail "sh user-startup no-stop rc=$?"
[ -s "$work/tmp/started" ] || fail "sh user-startup without stop refused to boot"

# No default route: after the AirPlay wait, any non-link-local address is
# the identity. First in ifconfig wins. The old full-IP strings are not written.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" \
  "$work/media/fat/linux/misterplexd-boot.sh" \
  "$work/media/fat/linux/user-startup.sh" \
  "$work/media/fat/linux/misterplex/identity" "$work/media/fat/misterplex/identity"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-noroute"
mkdir -p "$work/bin"
cat > "$work/bin/ifconfig" << 'EOF'
#!/bin/sh
cat << 'OUT'
eth0      Link encap:Ethernet
          inet addr:192.0.2.228  Bcast:192.0.2.254  Mask:255.255.255.0
wlan0     Link encap:Ethernet
          inet addr:192.0.2.12  Bcast:192.0.2.255  Mask:255.255.255.0
OUT
EOF
chmod +x "$work/bin/ifconfig"
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'eth0 0000000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route-none"
PATH="$work/bin:$PATH" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route-none" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-noroute.log" \
  sh "$SCRIPT" || fail "no-route boot rc=$?"
grep -q 'no default-route IPv4' "$work/tmp/boot-noroute.log" || fail "no-route boot treated a NIC as AirPlay-ready: $(cat "$work/tmp/boot-noroute.log")"
grep -q -- '--name misterplex-0.2.228 --id misterplex-0.2.228' "$work/tmp/started" || fail "no-route boot did not use the first non-link-local last-3: $(cat "$work/tmp/started")"
grep -q 'misterplex-0.2.12' "$work/tmp/started" && fail "second NIC won with no default route"
grep -q 'MiSTerPlex-192.0.2.228' "$work/tmp/started" && fail "full-ip friendly name was written"
grep -q 'misterplex-192.0.2.228' "$work/tmp/started" && fail "full-ip player id was written"
grep -q 'MiSTerPlex --id misterplex-1' "$work/tmp/started" && fail "no-route boot used the unsaved fallback"
grep -q 'FRIENDLY_NAME=misterplex-0.2.228' "$work/media/fat/linux/misterplex.conf" || fail "no-route last-3 name not saved"
grep -q 'PLAYER_ID=misterplex-0.2.228' "$work/media/fat/linux/misterplex.conf" || fail "no-route last-3 id not saved"
grep -q 'MiSTerPlex-192.0.2.228' "$work/media/fat/linux/misterplex.conf" && fail "full-ip name saved with no route"
grep -q 'MiSTerPlex-192.0.2.12' "$work/media/fat/linux/misterplex.conf" && fail "second NIC full-ip name saved with no route"

# Real default-route identity is written once. The next boot leaves the conf bytes alone.
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'wlan0 00000000 0100000A 0003 0 0 0 00000000 0 0 0' \
  'eth0 0000000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route-once"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
PATH="$work/bin:$PATH" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route-once" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-once.log" \
  sh "$SCRIPT" || fail "route-once boot rc=$?"
grep -q 'FRIENDLY_NAME=misterplex-0.2.12' "$work/media/fat/linux/misterplex.conf" || fail "default-route name was not saved once"
grep -q 'PLAYER_ID=misterplex-0.2.12' "$work/media/fat/linux/misterplex.conf" || fail "default-route id was not saved once"
grep -q 'MiSTerPlex-192.0.2.12' "$work/media/fat/linux/misterplex.conf" && fail "full-ip name was saved once"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-once"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
PATH="$work/bin:$PATH" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route-once" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-once2.log" \
  sh "$SCRIPT" || fail "route-twice boot rc=$?"
cmp -s "$work/tmp/conf-once" "$work/media/fat/linux/misterplex.conf" || fail "second boot rewrote a saved default-route identity"
grep -q -- '--name misterplex-0.2.12 --id misterplex-0.2.12' "$work/tmp/started" || fail "second boot lost the saved name"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_NO_NET=1 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-once-nonet.log" \
  sh "$SCRIPT" || fail "saved identity no-net rc=$?"
cmp -s "$work/tmp/conf-once" "$work/media/fat/linux/misterplex.conf" || fail "no-net reboot rewrote the saved identity"
grep -q -- '--name misterplex-0.2.12 --id misterplex-0.2.12' "$work/tmp/started" || fail "saved identity lost when the network was down"

# Name already in the conf: still wait for the default route before launching.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
printf '%s\n' 'FRIENDLY_NAME=Living Room' 'PLAYER_ID=misterplex-living' \
  > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-wait"
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  > "$work/tmp/route-delay"
(
  sleep 1
  printf '%s\n' 'wlan0 00000000 0100000A 0003 0 0 0 00000000 0 0 0' >> "$work/tmp/route-delay"
) &
delay_writer=$!
t0=$(python3 -c 'import time; print("%.3f" % time.time())')
PATH="$work/bin:$PATH" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=5 \
BOOT_PROC_ROUTE="$work/tmp/route-delay" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-wait-known.log" \
  sh "$SCRIPT" || fail "known-name wait rc=$?"
t1=$(python3 -c 'import time; print("%.3f" % time.time())')
wait "$delay_writer" || true
python3 -c 'import sys; d=float(sys.argv[2])-float(sys.argv[1]); assert d >= 0.8, d' "$t0" "$t1" \
  || fail "known name skipped the network wait ($(python3 -c "print(float('$t1')-float('$t0'))")s)"
grep -q 'network ready 192.0.2.12' "$work/tmp/boot-wait-known.log" || fail "known-name boot did not log the route: $(cat "$work/tmp/boot-wait-known.log")"
grep -q 'identity kept' "$work/tmp/boot-wait-known.log" || fail "known name was not kept"
grep -q -- '--name Living Room' "$work/tmp/started" || fail "known name was replaced after the wait"
cmp -s "$work/tmp/conf-wait" "$work/media/fat/linux/misterplex.conf" || fail "known-name wait rewrote the conf"

# No default route. After the AirPlay wait, the first non-link-local address
# is the identity (last three octets). The old MiSTerPlex-<full-ip> strings
# for 192.0.2.228 and 192.0.2.5 are not written.
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" \
  "$work/media/fat/linux/misterplexd-boot.sh" \
  "$work/media/fat/linux/user-startup.sh" \
  "$work/media/fat/linux/misterplex/identity" "$work/media/fat/misterplex/identity"
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
mkdir -p "$work/bin" "$work/tmp"
cat > "$work/bin/ifconfig" << 'EOF'
#!/bin/sh
cat "${BOOT_IFCONFIG_OUT:?}"
EOF
chmod +x "$work/bin/ifconfig"
cat > "$work/tmp/ifconfig-b4" << 'EOF'
eth0      Link encap:Ethernet
          inet addr:192.0.2.228  Bcast:192.0.2.254  Mask:255.255.255.0
eth1      Link encap:Ethernet
          inet addr:192.0.2.5  Bcast:192.0.2.255  Mask:255.255.255.0
EOF
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'eth0 0000000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  'eth1 0500000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route-b4-none"
PATH="$work/bin:$PATH" \
BOOT_IFCONFIG_OUT="$work/tmp/ifconfig-b4" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route-b4-none" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-b4-none.log" \
  sh "$SCRIPT" || fail "no-default-route boot rc=$?"
grep -q 'no default-route IPv4' "$work/tmp/boot-b4-none.log" || fail "no-route NIC was treated as AirPlay-ready: $(cat "$work/tmp/boot-b4-none.log")"
grep -q -- '--name misterplex-0.2.228 --id misterplex-0.2.228' "$work/tmp/started" || fail "no-route did not use first last-3: $(cat "$work/tmp/started")"
grep -q 'misterplex-0.2.5' "$work/tmp/started" && fail "no-route skipped the first address for 192.0.2.5"
grep -q 'MiSTerPlex-192.0.2.228' "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "old MiSTerPlex-192.0.2.228 path"
grep -q 'MiSTerPlex-192.0.2.5' "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "old MiSTerPlex-192.0.2.5 path"
grep -q 'misterplex-192.0.2.228' "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "old misterplex-192.0.2.228 path"
grep -q 'misterplex-192.0.2.5' "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "old misterplex-192.0.2.5 path"
grep -q 'FRIENDLY_NAME=misterplex-0.2.228' "$work/media/fat/linux/misterplex.conf" || fail "no-route name not saved"
grep -q 'PLAYER_ID=misterplex-0.2.228' "$work/media/fat/linux/misterplex.conf" || fail "no-route id not the same string"

# Default route exists, but that NIC is link-local. AirPlay does not call
# that ready. Identity then uses the other NIC: 192.0.2.5 -> misterplex-0.2.5.
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
cat > "$work/tmp/ifconfig-b4" << 'EOF'
eth0      Link encap:Ethernet
          inet addr:169.254.12.34  Bcast:169.254.255.255  Mask:255.255.0.0
eth1      Link encap:Ethernet
          inet addr:192.0.2.5  Bcast:192.0.2.255  Mask:255.255.255.0
          inet addr:192.0.2.228  Bcast:192.0.2.254  Mask:255.255.255.0
EOF
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'eth0 00000000 0100000A 0003 0 0 0 00000000 0 0 0' \
  'eth1 0500000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route-b4-ll"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
PATH="$work/bin:$PATH" \
BOOT_IFCONFIG_OUT="$work/tmp/ifconfig-b4" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route-b4-ll" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-b4-ll.log" \
  sh "$SCRIPT" || fail "link-local default route boot rc=$?"
grep -q 'no default-route IPv4' "$work/tmp/boot-b4-ll.log" || fail "link-local route was treated as ready: $(cat "$work/tmp/boot-b4-ll.log")"
grep -q -- '--name misterplex-0.2.5 --id misterplex-0.2.5' "$work/tmp/started" || fail "link-local route did not use the other NIC last-3: $(cat "$work/tmp/started")"
grep -q '169.254' "$work/tmp/started" && fail "link-local address became the name"
grep -q 'MiSTerPlex-192.0.2.5' "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "old MiSTerPlex-192.0.2.5 path"
grep -q 'MiSTerPlex-192.0.2.228' "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "old MiSTerPlex-192.0.2.228 path"
grep -q 'FRIENDLY_NAME=misterplex-0.2.5' "$work/media/fat/linux/misterplex.conf" || fail "link-local-route name not saved"
grep -q 'PLAYER_ID=misterplex-0.2.5' "$work/media/fat/linux/misterplex.conf" || fail "link-local-route id not the same string"

# The real default-route address shows up during the wait. Save that last-3,
# not the addresses that were already up.
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
cat > "$work/tmp/ifconfig-b4" << 'EOF'
eth0      Link encap:Ethernet
          inet addr:192.0.2.228  Bcast:192.0.2.254  Mask:255.255.255.0
eth1      Link encap:Ethernet
          inet addr:192.0.2.5  Bcast:192.0.2.255  Mask:255.255.255.0
EOF
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'eth0 0000000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route-b4-wait"
(
  sleep 1
  cat > "$work/tmp/ifconfig-b4" << 'EOF'
eth0      Link encap:Ethernet
          inet addr:192.0.2.228  Bcast:192.0.2.254  Mask:255.255.255.0
eth1      Link encap:Ethernet
          inet addr:192.0.2.5  Bcast:192.0.2.255  Mask:255.255.255.0
wlan0     Link encap:Ethernet
          inet addr:192.0.2.50  Bcast:192.0.2.255  Mask:255.255.255.0
EOF
  printf '%s\n' 'wlan0 00000000 0100000A 0003 0 0 0 00000000 0 0 0' >> "$work/tmp/route-b4-wait"
) &
b4_writer=$!
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
t0=$(python3 -c 'import time; print("%.3f" % time.time())')
PATH="$work/bin:$PATH" \
BOOT_IFCONFIG_OUT="$work/tmp/ifconfig-b4" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=5 \
BOOT_PROC_ROUTE="$work/tmp/route-b4-wait" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-b4-wait.log" \
  sh "$SCRIPT" || fail "delayed default-route boot rc=$?"
t1=$(python3 -c 'import time; print("%.3f" % time.time())')
wait "$b4_writer" || true
python3 -c 'import sys; d=float(sys.argv[2])-float(sys.argv[1]); assert d >= 0.8, d' "$t0" "$t1" \
  || fail "delayed route did not wait ($(python3 -c "print(float('$t1')-float('$t0'))")s)"
grep -q 'FRIENDLY_NAME=misterplex-0.2.50' "$work/media/fat/linux/misterplex.conf" || fail "real default-route name was not saved: $(cat "$work/media/fat/linux/misterplex.conf")"
grep -q 'PLAYER_ID=misterplex-0.2.50' "$work/media/fat/linux/misterplex.conf" || fail "real default-route id was not saved"
grep -q 'MiSTerPlex-192.0.2.50' "$work/media/fat/linux/misterplex.conf" && fail "full-ip default-route name was saved"
grep -q '192.0.2.228' "$work/media/fat/linux/misterplex.conf" && fail "delayed route saved 192.0.2.228"
grep -q '192.0.2.5' "$work/media/fat/linux/misterplex.conf" && fail "delayed route saved 192.0.2.5"
grep -q 'MiSTerPlex-192.0.2.228' "$work/media/fat/linux/misterplex.conf" && fail "old MiSTerPlex-192.0.2.228 path"
grep -q 'MiSTerPlex-192.0.2.5' "$work/media/fat/linux/misterplex.conf" && fail "old MiSTerPlex-192.0.2.5 path"
grep -q -- '--name misterplex-0.2.50 --id misterplex-0.2.50' "$work/tmp/started" || fail "delayed route did not launch the default-route name: $(cat "$work/tmp/started")"
grep -q '192.0.2.228' "$work/tmp/started" && fail "delayed route launched 192.0.2.228"
grep -q '192.0.2.5' "$work/tmp/started" && fail "delayed route launched 192.0.2.5"

# Only loopback and link-local: nothing to name, and nothing is written.
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-ll-only"
cat > "$work/tmp/ifconfig-b4" << 'EOF'
lo        Link encap:Local Loopback
          inet addr:127.0.0.1  Mask:255.0.0.0
eth0      Link encap:Ethernet
          inet addr:169.254.9.9  Bcast:169.254.255.255  Mask:255.255.0.0
EOF
printf '%s\n' \
  'Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT' \
  'eth0 0000000A 00000000 0001 0 0 0 00FFFFFF 0 0 0' \
  > "$work/tmp/route-ll-only"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
PATH="$work/bin:$PATH" \
BOOT_IFCONFIG_OUT="$work/tmp/ifconfig-b4" \
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_PROC_ROUTE="$work/tmp/route-ll-only" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-ll-only.log" \
  sh "$SCRIPT" || fail "link-local-only boot rc=$?"
grep -q -- '--name MiSTerPlex --id misterplex-1' "$work/tmp/started" || fail "link-local-only did not use the unsaved fallback: $(cat "$work/tmp/started")"
cmp -s "$work/tmp/conf-ll-only" "$work/media/fat/linux/misterplex.conf" || fail "link-local-only wrote the conf: $(cat "$work/media/fat/linux/misterplex.conf")"
grep -q '169.254' "$work/tmp/started" && fail "link-local-only used 169.254"

# Last-3 formatting for several addresses. Both keys match. Full-IP writes are gone.
rm -f "$work/media/fat/linux/user-startup.sh" \
  "$work/media/fat/linux/misterplexd-boot.sh" \
  "$work/media/fat/linux/misterplex/identity" "$work/media/fat/misterplex/identity"
for pair in \
  '192.0.2.12 misterplex-0.2.12' \
  '192.0.2.5 misterplex-0.2.5' \
  '192.0.2.228 misterplex-0.2.228' \
  '192.0.2.84 misterplex-0.2.84'
do
  ip=${pair%% *}
  want=${pair#* }
  rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
  printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
  BOOT_MARK="$work/tmp/started" \
  BOOT_CORE_WAIT=1 \
  BOOT_NET_WAIT=0 \
  BOOT_IPV4="$ip" \
  YTDLP_SELF_UPDATE=0 \
  ROOT="$work" \
  CORENAME_FILE="$work/tmp/CORENAME" \
  MISTER_CMD="$work/cmd/MiSTer_cmd" \
  BOOT_LOG="$work/tmp/boot-last3.log" \
    sh "$SCRIPT" || fail "last3 $ip rc=$?"
  grep -q -- "--name $want --id $want" "$work/tmp/started" || fail "last3 $ip launched $(cat "$work/tmp/started")"
  grep -q "FRIENDLY_NAME=$want" "$work/media/fat/linux/misterplex.conf" || fail "last3 $ip name not saved"
  grep -q "PLAYER_ID=$want" "$work/media/fat/linux/misterplex.conf" || fail "last3 $ip id not saved"
  grep -q "MiSTerPlex-$ip" "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "full friendly name written for $ip"
  grep -q "misterplex-$ip" "$work/tmp/started" "$work/media/fat/linux/misterplex.conf" && fail "full player id written for $ip"
  cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-last3"
  rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started"
  BOOT_MARK="$work/tmp/started" \
  BOOT_CORE_WAIT=1 \
  BOOT_NET_WAIT=0 \
  BOOT_IPV4=1.2.3.4 \
  YTDLP_SELF_UPDATE=0 \
  ROOT="$work" \
  CORENAME_FILE="$work/tmp/CORENAME" \
  MISTER_CMD="$work/cmd/MiSTer_cmd" \
  BOOT_LOG="$work/tmp/boot-last3-again.log" \
    sh "$SCRIPT" || fail "last3 rewrite $ip rc=$?"
  cmp -s "$work/tmp/conf-last3" "$work/media/fat/linux/misterplex.conf" || fail "second boot rewrote saved last-3 for $ip"
done

# Stop and shutdown write nothing, including when the conf has no identity yet.
printf '%s\n' '# empty' > "$work/media/fat/linux/misterplex.conf"
cp "$work/media/fat/linux/misterplex.conf" "$work/tmp/conf-empty-stop"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.12 \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-empty-stop.log" \
  sh "$SCRIPT" stop || fail "empty-conf stop rc=$?"
cmp -s "$work/tmp/conf-empty-stop" "$work/media/fat/linux/misterplex.conf" || fail "stop wrote an identity into an empty conf"
grep -q 'misterplex-0.2.12' "$work/media/fat/linux/misterplex.conf" && fail "stop saved last-3"
[ ! -s "$work/tmp/started" ] || fail "empty-conf stop launched the daemon"
mkdir -p "$work/proc/1" "$work/proc/8"
printf '%s' 'init' > "$work/proc/1/cmdline"
printf '%s' '/etc/init.d/rcK' > "$work/proc/8/cmdline"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.5 \
BOOT_PROC="$work/proc" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-empty-rck.log" \
  sh "$SCRIPT" || fail "empty-conf rcK rc=$?"
cmp -s "$work/tmp/conf-empty-stop" "$work/media/fat/linux/misterplex.conf" || fail "shutdown wrote an identity into an empty conf"
grep -q 'misterplex-0.2.5' "$work/media/fat/linux/misterplex.conf" && fail "shutdown saved last-3"
grep -q 'MiSTerPlex-192.0.2.5' "$work/media/fat/linux/misterplex.conf" && fail "shutdown wrote the old full-ip name"
[ ! -s "$work/tmp/started" ] || fail "empty-conf shutdown launched the daemon"
# Item 17: the hook starts the on-card helper, and the daemon's loopback probe finds it.
hdir="$work/gcast-src"
mkdir -p "$hdir"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$hdir/ffmpeg"
chmod +x "$hdir/ffmpeg"
printf '%s\n' '# stub' > "$hdir/gcast_xcode.py"
cat > "$hdir/run-gcast-xcode.sh" << 'EOF'
#!/bin/sh
exec python3 -c '
import os, socket
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(os.environ.get("GCAST_XCODE_PORT", "18781"))))
s.listen(4)
while True:
    c, _ = s.accept()
    c.recv(512)
    c.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok")
    c.close()
'
EOF
chmod +x "$hdir/run-gcast-xcode.sh"
printf '%s\n' 'test-ca' > "$work/tmp/ca.pem"
rm -f "$work/tmp/misterplexd.pid" "$work/tmp/gcast-xcode.pid" "$work/tmp/started" "$work/cmd/MiSTer_cmd"
printf '%s\n' Plex > "$work/tmp/CORENAME"
BOOT_MARK="$work/tmp/started" \
BOOT_CORE_WAIT=1 \
BOOT_NET_WAIT=0 \
BOOT_IPV4=192.0.2.123 \
BOOT_GCAST_DIR="$hdir" \
BOOT_CA_BUNDLE="$work/tmp/ca.pem" \
GCAST_FFMPEG="$hdir/ffmpeg" \
YTDLP_SELF_UPDATE=0 \
ROOT="$work" \
CORENAME_FILE="$work/tmp/CORENAME" \
MISTER_CMD="$work/cmd/MiSTer_cmd" \
BOOT_LOG="$work/tmp/boot-helper.log" \
  sh "$SCRIPT" start || fail "helper boot rc=$?"
[ -s "$work/tmp/started" ] || fail "helper boot did not start the daemon"
for i in 1 2 3 4 5 6 7 8 9 10; do
  [ -s "$work/tmp/gcast-xcode.pid" ] && break
  sleep 0.1
done
[ -s "$work/tmp/gcast-xcode.pid" ] || fail "hook did not start the helper"
cmp -s "$work/tmp/ca.pem" "$work/media/fat/misterplex/ca/misterplex-cacert.pem" || fail "hook did not place the CA bundle"
g++ -std=c++17 -O2 -pthread -I"$ROOT/arm/misterplexd" -o "$work/tmp/probe" -x c++ - << 'EOF'
#include "gcast/cast_resolve.hpp"
#include <cstdio>
int main() {
    const bool up = misterplex::gcast::cachedLoopbackHelperHealth();
    const std::string base = misterplex::gcast::effectiveXcodeBase(false, up);
    if (!up || base != misterplex::gcast::kLoopbackHelperBase) {
        std::printf("up=%d base=%s\n", up ? 1 : 0, base.c_str());
        return 1;
    }
    std::printf("probe ok %s\n", base.c_str());
    return 0;
}
EOF
"$work/tmp/probe" || fail "daemon loopback probe did not find the helper"
hpid=$(cat "$work/tmp/gcast-xcode.pid")
BOOT_LOG="$work/tmp/boot-helper-stop.log" \
ROOT="$work" \
  sh "$SCRIPT" stop || fail "helper stop rc=$?"
sleep 0.2
if kill -0 "$hpid" 2>/dev/null; then
  fail "helper still running after stop"
fi
# Item 18: uninstall removes the boot line and does not launch.
ustart="$work/media/fat/linux/user-startup.sh"
printf '%s\n' '/media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &' > "$ustart"
rm -f "$work/tmp/started"
BOOT_HOOK_PATH="/media/fat/misterplex/bin/misterplexd-boot-rc3.sh" \
BOOT_INSTALL_FILES="$ustart" \
BOOT_LOG="$work/tmp/boot-uninstall.log" \
ROOT="$work" \
  sh "$SCRIPT" uninstall || fail "uninstall rc=$?"
grep -q 'misterplexd-boot-rc3.sh' "$ustart" && fail "uninstall left the hook line"
[ ! -s "$work/tmp/started" ] || fail "uninstall launched the daemon"
echo "test_boot_rc3: ok"
