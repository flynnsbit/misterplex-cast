#!/usr/bin/env bash
# GCAST hold 5 on a lab MiSTer: YouTube (DIAL + Lounge) milestone with the simulated YouTube-app sender,
# controls, AirPlay<->YouTube preemption, then prod restore + play check.
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W
YBIN_SRC=$HOME/gcast/misterplexd-gcast-1b22d925; YBIN=/media/fat/linux/misterplexd-gcast-1b22d925
YSEND="$HOME/gcast/venv312/bin/python $HOME/gcast/ytsrc/tools/gcast/yt_lounge_sender.py"
VID=aqz-KE-bpKQ
log snapshot; S "cat /tmp/CORENAME; echo; md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-;
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd*) echo PID \${p#/proc/} \$e \$(md5sum \$e | cut -c1-8);; esac; done;
  grep -aE 'core gate open' /tmp/mpxd.log | tail -1" | tee $W/snapshot.txt
log P0-yt; S 'echo DNS: $(nslookup www.youtube.com 2>&1 | tail -2 | tr "\n" " "); echo ROUTE: $(ip route | head -2 | tr "\n" " ");
  netstat -lntu 2>/dev/null | grep -E ":(8008|1900) " || echo no-8008-1900' | tee $W/p0-yt.txt
log deploy; "${SCP[@]}" $YBIN_SRC root@192.0.2.228:$YBIN.tmp && "${SCP[@]}" $HOME/gcast/ytsrc/tools/gcast/ca/misterplex-cacert.pem root@192.0.2.228:/tmp/gcast-cacert.pem &&
  S "mv $YBIN.tmp $YBIN; chmod 755 $YBIN; md5sum $YBIN /tmp/gcast-cacert.pem | cut -c1-32" | tee $W/deploy.txt
S "$KILL"; prod_core | tee $W/core.txt
mkenv gcast GAIRPLAY=1 GYT=1 GCAST_XCODE=${GCAST_XCODE} MPX_CA_BUNDLE=/tmp/gcast-cacert.pem GYT_SCREEN_ID_FILE=/tmp/yt_screen_id | tee $W/env.txt
S ": > /tmp/mpxd.log"; launch $YBIN eng-gcast | tee $W/launch.txt; sleep 10
S "grep -aE 'GYT|yt:|GAIRPLAY|gcast' /tmp/mpxd.log | head -20; netstat -lntu | grep -E ':(8008|1900|7000) '" | tee $W/yt-up.txt

log Y1 YouTube play + controls: 25s play, pause 8s, play 16s, seek 120, play 20s, stop
$YSEND 192.0.2.228 $VID wait:25 pause wait:8 play wait:16 seek:120 wait:20 stop >$W/y1-sender.log 2>&1 &
SP=$!; sleep 20; hdmi_png $W/y1-t20.png; sleep 10; hdmi_png $W/y1-paused-a.png; sleep 4; hdmi_png $W/y1-paused-b.png
sleep 8; hdmi_png $W/y1-resumed-a.png; sleep 6; hdmi_png $W/y1-resumed-b.png
S "grep -aE 'yt:|GCAST|seek|pause|resume|FPGA_TERMINAL classification' /tmp/mpxd.log | cut -c1-240 | tail -30" | tee $W/y1-mid-daemon.txt
sleep 14; hdmi_png $W/y1-after-seek-a.png; sleep 5; hdmi_png $W/y1-after-seek-b.png
wait $SP; sleep 3; hdmi_png $W/y1-after-stop.png
S "grep -aE 'yt:|GCAST|seek|pause|resume|FPGA_TERMINAL classification|ladder' /tmp/mpxd.log | cut -c1-300 | tail -60" | tee $W/y1-daemon.txt

log Y3 YouTube preempts AirPlay
$SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:40 >$W/y3-airplay.log 2>&1 &
AP=$!; sleep 12; hdmi_png $W/y3-airplay.png
$YSEND 192.0.2.228 $VID wait:24 stop >$W/y3-sender.log 2>&1 &
YP=$!; sleep 16; hdmi_png $W/y3-yt-over-airplay.png
wait $YP; wait $AP
S "grep -aE 'yt:|GCAST|airplay|preempt' /tmp/mpxd.log | cut -c1-240 | tail -30" | tee $W/y3-daemon.txt
S "cp /tmp/mpxd.log /tmp/gcast-hold5-mpxd.log"; "${SCP[@]}" root@192.0.2.228:/tmp/gcast-hold5-mpxd.log $W/gcast-mpxd-full.log; S "rm -f /tmp/gcast-hold5-mpxd.log"

log restore prod; S "grep -a . /tmp/mpxd.log | tail -500" > $W/gcast-mpxd-tail.log
S "rm -f $YBIN /tmp/gcast-cacert.pem /tmp/yt_screen_id /media/fat/misterplex/airplay_key.pem; ls /media/fat/linux | grep -c gcast"
restore_prod $W
log done-gc2
