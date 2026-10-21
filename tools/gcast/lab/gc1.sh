#!/usr/bin/env bash
# GCAST hold 1 on a lab MiSTer: P0 read-only checks + AirPlay URL milestone + preemption both ways, then prod restore.
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W
log P0; S 'echo LISTEN:; netstat -lntu 2>/dev/null | grep -E ":(7000|7001|7100|5353|8008|1900) " || echo none-of-7000/7001/7100/5353/8008/1900;
  echo UDP_RAW:; awk "NR>1{print \$2}" /proc/net/udp | grep -iE ":(14E9|076C)$" || echo no-5353-1900-udp;
  echo AVAHI: $(ls /usr/sbin/avahi-daemon /etc/init.d/S*avahi* 2>/dev/null || echo absent);
  echo CA: $(ls -la /etc/ssl/certs/ca-certificates.crt 2>&1 | head -1) $(ls /etc/ssl/certs 2>/dev/null | wc -l) files;
  echo IFACES:; ip -4 -o addr 2>/dev/null | awk "{print \$2, \$4}";
  echo FREE:; free -m | head -2; echo DF:; df -h /media/fat /tmp | tail -2' | tee $W/p0-229.txt
log deploy; "${SCP[@]}" $GBIN_SRC root@192.0.2.228:$GBIN.tmp && S "mv $GBIN.tmp $GBIN; chmod 755 $GBIN; md5sum $GBIN | cut -c1-32" | tee $W/deploy.txt
S "$KILL"; prod_core | tee $W/core.txt
mkenv gcast GAIRPLAY=1 | tee $W/env.txt
S ": > /tmp/mpxd.log"; launch $GBIN eng-gcast | tee $W/launch.txt; sleep 6
S "grep -aE 'GAIRPLAY|airplay|gcast' /tmp/mpxd.log | head -20; netstat -lntu | grep -E ':(7000|7100|5353) '" | tee $W/gcast-up.txt
timeout 12 avahi-browse -rpt _airplay._tcp 2>/dev/null | grep -i 229 | head -4 | tee $W/mdns-airplay.txt
timeout 12 avahi-browse -rpt _raop._tcp 2>/dev/null | grep -i 229 | head -2 | tee -a $W/mdns-airplay.txt

log A1 play test.mp4 from 0, hold 45s
$SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:45 >$W/a1-sender.log 2>&1 &
SP=$!; sleep 14; hdmi_png $W/a1-t14.png; sleep 10; hdmi_png $W/a1-t24.png
S "grep -aE 'gcast|airplay|cast' /tmp/mpxd.log | tail -25; /media/fat/misterplex/bin/set_status --status | head -1" | tee $W/a1-daemon.txt
wait $SP; sleep 3; hdmi_png $W/a1-after-stop.png
S "grep -aE 'gcast|airplay|cast|stop' /tmp/mpxd.log | tail -8" | tee $W/a1-after.txt

log A2 controls: start 5, pause, resume, scrub 30, stop
$SENDER 192.0.2.228 7000 $MEDIA 5 MobileSafari controls >$W/a2-sender.log 2>&1 &
SP=$!; sleep 9; hdmi_png $W/a2-t9.png; wait $SP
S "grep -aE 'gcast|airplay|seek|pause|resume' /tmp/mpxd.log | tail -20" | tee $W/a2-daemon.txt

log P1 preempt AirPlay to Plex: AirPlay holds 70s, Plex cast at t=12
$SENDER 192.0.2.228 7000 $MEDIA 0 MobileSafari hold:70 >$W/p1-sender.log 2>&1 &
SP=$!; sleep 12; hdmi_png $W/p1-airplay.png
plexcast $W/p1-plexcast.log; sleep 8; hdmi_png $W/p1-plex-after.png
S "grep -aE 'gcast|airplay|retire|playMedia|cast' /tmp/mpxd.log | tail -25; /media/fat/misterplex/bin/set_status --status | head -1" | tee $W/p1-daemon.txt
wait $SP; grep -E 'playback-info|ended|connection' $W/p1-sender.log | tail -6 > $W/p1-sender-tail.txt

log P2 preempt Plex to AirPlay: Plex playing, AirPlay play
$SENDER 192.0.2.228 7000 $MEDIA 20 MobileSafari hold:25 >$W/p2-sender.log 2>&1 &
SP=$!; sleep 14; hdmi_png $W/p2-airplay-over-plex.png
S "grep -aE 'gcast|airplay|retire|closedStop|timeline|cast' /tmp/mpxd.log | tail -25" | tee $W/p2-daemon.txt
curl -s -m 5 "$PLAYER/player/timeline/poll?wait=0&commandID=9903" > $W/p2-plex-timeline.xml
wait $SP; sleep 2; hdmi_png $W/p2-after-stop.png

log restore prod; S "grep -a . /tmp/mpxd.log | tail -400" > $W/gcast-mpxd-tail.log
S "rm -f $GBIN /media/fat/misterplex/airplay_key.pem; ls /media/fat/linux | grep -c gcast"
restore_prod $W
log done-gc1
