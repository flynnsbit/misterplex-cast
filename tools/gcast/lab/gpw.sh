#!/usr/bin/env bash
# PAUSE-WATCHDOG on a lab MiSTer: Plex cast, 10 s pause, resume, on b89bc987 (pre-release) then e9b526b5 (fix).
set -uo pipefail
. "${LAB_COMMON:?set LAB_COMMON}"; . "$(dirname "$0")/gc-common.sh"
W=${W:?}; mkdir -p $W
B=${OWENB_BUILD_DIR:?set OWENB_BUILD_DIR}
log snapshot; S "cat /tmp/CORENAME; echo; md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-;
  for p in /proc/[0-9]*; do e=\$(readlink \$p/exe 2>/dev/null) || continue; case \$e in *misterplexd*) echo PID \${p#/proc/} \$e;; esac; done" | tee $W/snapshot.txt
for leg in b89bc987.db80132a e9b526b5.0e8dd29a; do
  L=$W/$leg; mkdir -p $L; D=/media/fat/linux/misterplexd-pw-$leg
  log "leg $leg deploy"; "${SCP[@]}" $B/misterplexd.$leg root@192.0.2.228:$D.tmp && S "mv $D.tmp $D; chmod 755 $D; md5sum $D | cut -c1-32" | tee $L/deploy.txt
  S "$KILL"; prod_core | tee $L/core.txt; mkenv lab >/dev/null
  S ": > /tmp/mpxd.log"; launch $D eng-lab | tee $L/launch.txt; sleep 8
  log "leg $leg plex cast"; plexcast $L/plexcast.log; sleep 10; hdmi_png $L/t-play.png
  curl -s -m 5 "$PLAYER/player/timeline/poll?wait=0&commandID=9911" > $L/tl-before.xml
  log "leg $leg pause 10 s"; curl -s -m 8 "$PLAYER/player/playback/pause?commandID=9912" > $L/pause.xml
  sleep 5; hdmi_png $L/t-paused.png; sleep 5
  log "leg $leg resume"; curl -s -m 8 "$PLAYER/player/playback/play?commandID=9913" > $L/play.xml
  sleep 3; hdmi_png $L/t-resume3.png; sleep 7; hdmi_png $L/t-resume10.png
  curl -s -m 5 "$PLAYER/player/timeline/poll?wait=0&commandID=9914" > $L/tl-after.xml
  S "grep -aE 'stalled picture|boundedReopen|picture stall|FPGA_TERMINAL classification|compressed seek|pause|resume|load_core|PLAY ' /tmp/mpxd.log | cut -c1-260 | tail -40" | tee $L/daemon.txt
  S "cp /tmp/mpxd.log /tmp/pw.log"; "${SCP[@]}" root@192.0.2.228:/tmp/pw.log $L/mpxd-full.log; S "rm -f /tmp/pw.log"
  curl -s -m 8 "$PLAYER/player/playback/stop?commandID=9915" >/dev/null; sleep 3
done
log restore prod; S "rm -f /media/fat/linux/misterplexd-pw-*; ls /media/fat/linux | grep -c misterplexd-pw"
restore_prod $W
log done-gpw
