#!/usr/bin/env bash
# GCAST hold 7 on a lab MiSTer: P6 soak (YouTube -> AirPlay -> Plex cycles, RSS/fd/thread samples, HDMI per leg)
# then sender-cap probes (gc_caps.py), then prod restore.
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W; CYCLES=${CYCLES:-7}
YBIN_SRC=$HOME/gcast/misterplexd-gcast-${GTAG:?}; YBIN=/media/fat/linux/misterplexd-gcast-$GTAG
YSEND="$HOME/gcast/venv312/bin/python $HOME/gcast/ytsrc/tools/gcast/yt_lounge_sender.py"
VID=aqz-KE-bpKQ
plexstop(){ curl -s -m 8 "$PLAYER/player/playback/stop?commandID=$1" >/dev/null; }
sample(){ S "for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd-gcast*)
  echo \"$1 \$(date +%H:%M:%S) pid=\${p#/proc/} rss_kb=\$(awk '/VmRSS/{print \$2}' \$p/status) fds=\$(ls \$p/fd | wc -l) thr=\$(ls \$p/task | wc -l) memavail_kb=\$(awk '/MemAvailable/{print \$2}' /proc/meminfo)\";; esac; done" | tee -a $W/samples.txt; }
log snapshot; S "cat /tmp/CORENAME; echo; md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-;
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd*) echo PID \${p#/proc/} \$e \$(md5sum \$e | cut -c1-8);; esac; done" | tee $W/snapshot.txt
log deploy; "${SCP[@]}" $YBIN_SRC root@192.0.2.228:$YBIN.tmp && "${SCP[@]}" $HOME/gcast/ytsrc/tools/gcast/ca/misterplex-cacert.pem root@192.0.2.228:/tmp/gcast-cacert.pem &&
  S "mv $YBIN.tmp $YBIN; chmod 755 $YBIN; md5sum $YBIN | cut -c1-32" | tee $W/deploy.txt
S "$KILL"; prod_core | tee $W/core.txt
mkenv gcast GAIRPLAY=1 GYT=1 GCAST_XCODE=${GCAST_XCODE} MPX_CA_BUNDLE=/tmp/gcast-cacert.pem GYT_SCREEN_ID_FILE=/tmp/yt_screen_id | tee $W/env.txt
S ": > /tmp/mpxd.log"; launch $YBIN eng-gcast | tee $W/launch.txt; sleep 12; sample start
for c in $(seq 1 $CYCLES); do
  log "cycle $c YouTube"; $YSEND 192.0.2.228 $VID wait:22 stop >$W/c$c-yt.log 2>&1; sleep 2; sample c$c-yt
  [ $c = 1 ] || [ $c = $CYCLES ] && hdmi_png $W/c$c-after-yt.png
  log "cycle $c AirPlay"; $SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:18 >$W/c$c-ap.log 2>&1 &
  AP=$!; sleep 12; [ $c = 1 ] || [ $c = $CYCLES ] && hdmi_png $W/c$c-airplay.png
  log "cycle $c Plex over AirPlay"; plexcast $W/c$c-plex.log; sleep 6; [ $c = 1 ] || [ $c = $CYCLES ] && hdmi_png $W/c$c-plex.png
  S "/media/fat/misterplex/bin/set_status --status | head -1 | cut -c1-40" | tee -a $W/plex-status.txt
  wait $AP; plexstop 99$c; sleep 3; sample c$c-plex
done
S "grep -acE 'GCAST .* preempts' /tmp/mpxd.log; grep -acE 'FPGA_TERMINAL|wedge|abort|SIGSEGV' /tmp/mpxd.log" | tee $W/soak-counts.txt
log caps probes; python3 ~/gcast/lab/gc_caps.py 192.0.2.228 8008 7000 2>&1 | tee $W/caps.txt
sample after-caps
S "grep -aE 'DIAL connection dropped|DIAL POST' /tmp/mpxd.log | sort | uniq -c | sort -rn | head -5" | tee $W/caps-daemon.txt
S "cp /tmp/mpxd.log /tmp/gc7.log"; "${SCP[@]}" root@192.0.2.228:/tmp/gc7.log $W/gcast-mpxd-full.log; S "rm -f /tmp/gc7.log"
log restore prod
S "rm -f $YBIN /tmp/gcast-cacert.pem /tmp/yt_screen_id /media/fat/misterplex/airplay_key.pem; ls /media/fat/linux | grep -c gcast"
restore_prod $W
log done-gc7
