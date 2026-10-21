#!/usr/bin/env bash
# GCAST hold 6 on a lab MiSTer: P4 OSD (idle ready line, owner now-playing) + P5 Plex coexistence
# (flags OFF: Plex unchanged, no listeners; flags ON: Plex play, Plex<->AirPlay, Plex<->YouTube), then prod restore.
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W
YBIN_SRC=$HOME/gcast/misterplexd-gcast-9c7859a0; YBIN=/media/fat/linux/misterplexd-gcast-9c7859a0
YSEND="$HOME/gcast/venv312/bin/python $HOME/gcast/ytsrc/tools/gcast/yt_lounge_sender.py"
VID=aqz-KE-bpKQ
plexstop(){ curl -s -m 8 "$PLAYER/player/playback/stop?commandID=$1" >/dev/null; }
log snapshot; S "cat /tmp/CORENAME; echo; md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-;
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd*) echo PID \${p#/proc/} \$e \$(md5sum \$e | cut -c1-8);; esac; done;
  grep -aE 'core gate open' /tmp/mpxd.log | tail -1" | tee $W/snapshot.txt
log deploy; "${SCP[@]}" $YBIN_SRC root@192.0.2.228:$YBIN.tmp && "${SCP[@]}" $HOME/gcast/ytsrc/tools/gcast/ca/misterplex-cacert.pem root@192.0.2.228:/tmp/gcast-cacert.pem &&
  S "mv $YBIN.tmp $YBIN; chmod 755 $YBIN; md5sum $YBIN /tmp/gcast-cacert.pem | cut -c1-32" | tee $W/deploy.txt

log F0 flags OFF: gcast binary, prod env only
S "$KILL"; prod_core | tee $W/f0-core.txt
mkenv off | tee $W/f0-env.txt
S ": > /tmp/mpxd.log"; launch $YBIN eng-off | tee $W/f0-launch.txt; sleep 10
hdmi_png $W/f0-idle.png
S "echo GCAST_LINES=\$(grep -acE 'GCAST|GYT|GAIRPLAY|airplay|yt_fe' /tmp/mpxd.log); netstat -lntu | grep -E ':(7000|7001|7100|8008|1900|5353) ' || echo no-gcast-listeners" | tee $W/f0-up.txt
plexcast $W/f0-cast.log; sleep 8; hdmi_png $W/f0-plex.png
S "/media/fat/misterplex/bin/set_status --status | head -1; grep -aE 'PLXD has_frame' /tmp/mpxd.log | tail -2" | tee $W/f0-plex.txt
plexstop 9910; sleep 5; hdmi_png $W/f0-after-stop.png
S "echo GCAST_LINES=\$(grep -acE 'GCAST|GYT|GAIRPLAY|airplay|yt_fe' /tmp/mpxd.log)" | tee -a $W/f0-up.txt
S "cp /tmp/mpxd.log /tmp/gc6-f0.log"; "${SCP[@]}" root@192.0.2.228:/tmp/gc6-f0.log $W/f0-mpxd-full.log; S "rm -f /tmp/gc6-f0.log"

log F1 flags ON
S "$KILL"; prod_core | tee $W/f1-core.txt
mkenv gcast GAIRPLAY=1 GYT=1 GCAST_XCODE=${GCAST_XCODE} MPX_CA_BUNDLE=/tmp/gcast-cacert.pem GYT_SCREEN_ID_FILE=/tmp/yt_screen_id | tee $W/f1-env.txt
S ": > /tmp/mpxd.log"; launch $YBIN eng-gcast | tee $W/f1-launch.txt; sleep 12
hdmi_png $W/f1-idle-ready.png
S "grep -aE 'ready line|GYT|GAIRPLAY' /tmp/mpxd.log | head -10; netstat -lntu | grep -E ':(7000|8008|1900) '" | tee $W/f1-up.txt

log P1 Plex play with flags on
plexcast $W/p1-cast.log; sleep 8; hdmi_png $W/p1-plex.png
S "/media/fat/misterplex/bin/set_status --status | head -1; grep -aE 'PLXD has_frame' /tmp/mpxd.log | tail -2" | tee $W/p1-plex.txt

log P2 AirPlay preempts Plex
$SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:50 >$W/p2-airplay.log 2>&1 &
AP=$!; sleep 14; hdmi_png $W/p2-airplay-over-plex.png
S "grep -aE 'GCAST|preempt|retire|owner|timeline.*stopped' /tmp/mpxd.log | cut -c1-240 | tail -20" | tee $W/p2-daemon.txt

log P3 Plex preempts AirPlay
plexcast $W/p3-cast.log; sleep 8; hdmi_png $W/p3-plex-over-airplay.png
S "/media/fat/misterplex/bin/set_status --status | head -1; grep -aE 'GCAST|preempt|retire|owner' /tmp/mpxd.log | cut -c1-240 | tail -12" | tee $W/p3-daemon.txt
wait $AP

log P4 YouTube preempts Plex, pause for the OSD now-playing, Plex preempts YouTube
$YSEND 192.0.2.228 $VID wait:22 pause wait:8 play wait:40 stop >$W/p4-yt-sender.log 2>&1 &
YP=$!; sleep 18; hdmi_png $W/p4-yt-over-plex.png
sleep 7; hdmi_png $W/p4-yt-paused-a.png; sleep 3; hdmi_png $W/p4-yt-paused-b.png
S "grep -aE 'GCAST|yt:|preempt|retire|title' /tmp/mpxd.log | cut -c1-240 | tail -20" | tee $W/p4-daemon.txt
sleep 4; plexcast $W/p5-cast.log; sleep 8; hdmi_png $W/p5-plex-over-yt.png
S "/media/fat/misterplex/bin/set_status --status | head -1; grep -aE 'GCAST|yt:|preempt|retire' /tmp/mpxd.log | cut -c1-240 | tail -12" | tee $W/p5-daemon.txt
wait $YP
plexstop 9911; sleep 6; hdmi_png $W/p6-idle-after.png

S "cp /tmp/mpxd.log /tmp/gc6-f1.log"; "${SCP[@]}" root@192.0.2.228:/tmp/gc6-f1.log $W/f1-mpxd-full.log; S "rm -f /tmp/gc6-f1.log"
log restore prod
S "rm -f $YBIN /tmp/gcast-cacert.pem /tmp/yt_screen_id /media/fat/misterplex/airplay_key.pem; ls /media/fat/linux | grep -c gcast"
restore_prod $W
log done-gc6
