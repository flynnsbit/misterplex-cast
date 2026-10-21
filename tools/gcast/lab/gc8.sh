#!/usr/bin/env bash
# GCAST hold 8 on a lab MiSTer: RSS attribution for the hold 7 soak growth. Same gcast binary:
# A flags OFF Plex-only cast/stop cycles; B flags ON AirPlay-only cycles; C flags ON YouTube-only cycles.
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W; N=${N:-6}
YBIN_SRC=$HOME/gcast/misterplexd-gcast-${GTAG:?}; YBIN=/media/fat/linux/misterplexd-gcast-$GTAG
YSEND="$HOME/gcast/venv312/bin/python $HOME/gcast/ytsrc/tools/gcast/yt_lounge_sender.py"
VID=aqz-KE-bpKQ
plexstop(){ curl -s -m 8 "$PLAYER/player/playback/stop?commandID=$1" >/dev/null; }
sample(){ S "for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd-gcast*)
  echo \"$1 \$(date +%H:%M:%S) rss_kb=\$(awk '/VmRSS/{print \$2}' \$p/status) fds=\$(ls \$p/fd | wc -l) thr=\$(ls \$p/task | wc -l) memavail_kb=\$(awk '/MemAvailable/{print \$2}' /proc/meminfo)\";; esac; done" | tee -a $W/samples.txt; }
log snapshot; S "cat /tmp/CORENAME; echo; md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-;
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd*) echo PID \${p#/proc/} \$e \$(md5sum \$e | cut -c1-8);; esac; done" | tee $W/snapshot.txt
log deploy; "${SCP[@]}" $YBIN_SRC root@192.0.2.228:$YBIN.tmp && "${SCP[@]}" $HOME/gcast/ytsrc/tools/gcast/ca/misterplex-cacert.pem root@192.0.2.228:/tmp/gcast-cacert.pem &&
  S "mv $YBIN.tmp $YBIN; chmod 755 $YBIN; md5sum $YBIN | cut -c1-32" | tee $W/deploy.txt
log A flags OFF Plex only
S "$KILL"; prod_core >/dev/null; mkenv off >/dev/null
S ": > /tmp/mpxd.log"; launch $YBIN eng-off | tee $W/a-launch.txt; sleep 10; sample A0
for c in $(seq 1 $N); do plexcast $W/a$c-plex.log; sleep 8; plexstop 98$c; sleep 3; sample A$c; done
hdmi_png $W/a-last.png
log B flags ON AirPlay only
S "$KILL"; prod_core >/dev/null
mkenv gcast GAIRPLAY=1 GYT=1 GCAST_XCODE=${GCAST_XCODE} MPX_CA_BUNDLE=/tmp/gcast-cacert.pem GYT_SCREEN_ID_FILE=/tmp/yt_screen_id >/dev/null
S ": > /tmp/mpxd.log"; launch $YBIN eng-gcast | tee $W/b-launch.txt; sleep 12; sample B0
for c in $(seq 1 $N); do $SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:16 >$W/b$c-ap.log 2>&1; sleep 3; sample B$c; done
hdmi_png $W/b-last.png
log C flags ON YouTube only, same daemon
for c in $(seq 1 $N); do $YSEND 192.0.2.228 $VID wait:18 stop >$W/c$c-yt.log 2>&1; sleep 3; sample C$c; done
hdmi_png $W/c-last.png
S "cp /tmp/mpxd.log /tmp/gc8.log"; "${SCP[@]}" root@192.0.2.228:/tmp/gc8.log $W/gcast-mpxd-full.log; S "rm -f /tmp/gc8.log"
log restore prod
S "rm -f $YBIN /tmp/gcast-cacert.pem /tmp/yt_screen_id /media/fat/misterplex/airplay_key.pem; ls /media/fat/linux | grep -c gcast"
restore_prod $W
log done-gc8
