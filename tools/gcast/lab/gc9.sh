#!/usr/bin/env bash
# GCAST memory soak on a lab MiSTer: CYCLES x (YouTube -> AirPlay -> Plex over AirPlay -> stop), RSS/fd/thread samples
# every cycle, full /proc/PID/smaps at start and cycles 5/10/15. ARENA=2 adds MALLOC_ARENA_MAX=2 (+ glibc tunable).
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W; CYCLES=${CYCLES:-15}
YBIN_SRC=$HOME/gcast/misterplexd-gcast-${GTAG:?}; YBIN=/media/fat/linux/misterplexd-gcast-$GTAG
YSEND="$HOME/gcast/venv312/bin/python $HOME/gcast/ytsrc/tools/gcast/yt_lounge_sender.py"
VID=aqz-KE-bpKQ
XENV=""; [ -n "${ARENA:-}" ] && XENV="MALLOC_ARENA_MAX=$ARENA GLIBC_TUNABLES=glibc.malloc.arena_max=$ARENA"
[ -n "${MEMSTAT:-}" ] && XENV="$XENV MPX_MEMSTAT_SEC=$MEMSTAT"
plexstop(){ curl -s -m 8 "$PLAYER/player/playback/stop?commandID=$1" >/dev/null; }
GP='for p in /proc/[0-9]*; do e=$(readlink $p/exe 2>/dev/null) || continue; case $e in *misterplexd-gcast*) echo ${p#/proc/};; esac; done | head -1'
sample(){ S "P=\$($GP); echo \"$1 \$(date +%H:%M:%S) rss_kb=\$(awk '/VmRSS/{print \$2}' /proc/\$P/status) anon_kb=\$(awk '/RssAnon/{print \$2}' /proc/\$P/status) vsz_kb=\$(awk '/VmSize/{print \$2}' /proc/\$P/status) fds=\$(ls /proc/\$P/fd | wc -l) thr=\$(ls /proc/\$P/task | wc -l) maps=\$(wc -l < /proc/\$P/maps) memavail_kb=\$(awk '/MemAvailable/{print \$2}' /proc/meminfo)\"" | tee -a $W/samples.txt
  [ -n "${MEMSTAT:-}" ] && S "grep -a 'MEMSTAT seq' /tmp/mpxd.log | tail -n 1" | sed "s/^/$1 /" | tee -a $W/memstat.txt; }
smaps(){ S "P=\$($GP); cat /proc/\$P/smaps > /tmp/gc9-smaps-$1; cat /proc/\$P/environ | tr '\0' '\n' | grep -E 'MALLOC|TUNABLES' > /tmp/gc9-env-$1"; "${SCP[@]}" root@192.0.2.228:/tmp/gc9-smaps-$1 $W/smaps-$1.txt; "${SCP[@]}" root@192.0.2.228:/tmp/gc9-env-$1 $W/env-$1.txt; S "rm -f /tmp/gc9-smaps-$1 /tmp/gc9-env-$1"; }
log snapshot; S "cat /tmp/CORENAME; echo; md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-;
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd*) echo PID \${p#/proc/} \$e \$(md5sum \$e | cut -c1-8);; esac; done" | tee $W/snapshot.txt
log deploy; "${SCP[@]}" $YBIN_SRC root@192.0.2.228:$YBIN.tmp && "${SCP[@]}" $HOME/gcast/ytsrc/tools/gcast/ca/misterplex-cacert.pem root@192.0.2.228:/tmp/gcast-cacert.pem &&
  S "mv $YBIN.tmp $YBIN; chmod 755 $YBIN; md5sum $YBIN | cut -c1-32" | tee $W/deploy.txt
S "$KILL"; prod_core | tee $W/core.txt
mkenv gcast GAIRPLAY=1 GYT=1 GCAST_XCODE=${GCAST_XCODE} MPX_CA_BUNDLE=/tmp/gcast-cacert.pem GYT_SCREEN_ID_FILE=/tmp/yt_screen_id $XENV | tee $W/env.txt
S ": > /tmp/mpxd.log"; launch $YBIN eng-gcast | tee $W/launch.txt; sleep 12; sample start; smaps c0
for c in $(seq 1 $CYCLES); do
  log "cycle $c"; $YSEND 192.0.2.228 $VID wait:15 stop >$W/c$c-yt.log 2>&1; sleep 1
  $SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:14 >$W/c$c-ap.log 2>&1 &
  AP=$!; sleep 9; plexcast $W/c$c-plex.log; sleep 4
  S "/media/fat/misterplex/bin/set_status --status | head -1 | cut -c1-40" | tee -a $W/plex-status.txt
  wait $AP; plexstop 97$c; sleep 3; sample c$c
  case $c in 5|10|15) smaps c$c;; esac
done
hdmi_png $W/last-idle.png
S "grep -acE 'GCAST .* preempts' /tmp/mpxd.log; grep -aoE 'FPGA_TERMINAL classification=[a-z_]+' /tmp/mpxd.log | sort | uniq -c" | tee $W/soak-counts.txt
S "cp /tmp/mpxd.log /tmp/gc9.log"; "${SCP[@]}" root@192.0.2.228:/tmp/gc9.log $W/gcast-mpxd-full.log; S "rm -f /tmp/gc9.log"
log restore prod
S "rm -f $YBIN /tmp/gcast-cacert.pem /tmp/yt_screen_id /media/fat/misterplex/airplay_key.pem; ls /media/fat/linux | grep -c gcast"
restore_prod $W
log done-gc9
