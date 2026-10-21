#!/usr/bin/env bash
# deploy-prodfix.sh -- prod-line daemon (fix/remote-buffer-playbar-1011) on a lab MiSTer over the canonical prod
# (the prod launcher script: v8 core 0x48FF65F2, conf, 7-var env, boot.sh untouched). Staging fills
# @DAEMON@ / @DAEMON_MD5@ / @COMMIT@. Hold the 229 lease first. Rollback: rollback-canonical.sh (or reboot).
#   DIAG=1     adds MPX_COMPANION_POLL_DIAG=1 for this start.
#   RBF=prod   (default) keep the loaded canonical prod core (0x48FF65F2, 3324fcbc at _Utility/Plex.rbf); no core load.
#   RBF=analog push Plex_480p_analog_0x487A1BD1_853265774_s7.rbf (89fd62e2; 0x48FF65F2 + analog VGA restored,
#              prod-480p/analog-vga-1011 853265774) to _Utility/Plex_analog.rbf (new name: Plex.rbf is never
#              touched), load_core it, poll CAPS ~20 s, then require the daemon gate log for 0x487A1BD1.
#              rollback-canonical.sh reloads Plex.rbf (0x48FF65F2).
#   PROXY=off  a true no-proxy run. The daemon reads REMOTE_LADDER_PROXY / MPX_REMOTE_LADDER_PROXY from
#              /media/fat/linux/misterplex.conf ONLY when the MPX_REMOTE_LADDER_PROXY env var is unset
#              (main.cpp), so removing it from the env (B2 1011) fell back to the conf's proxy. PROXY=off
#              therefore SETS it empty (env wins over conf; ladderProxyConfigured() is then false), and
#              the post-start check refuses DEPLOY_OK unless the live environ has it empty and the log
#              shows no conf proxy line.
# Commit: @COMMIT@
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)

# PROXY=off env rewrite (tests/unit/test_deploy_prodfix_proxy_off.sh sources this function).
proxy_off_env() { # <env words> -> the same words with MPX_REMOTE_LADDER_PROXY= (empty), REMOTE_LADDER_PROXY dropped
  local w out=""
  for w in $1; do
    case "$w" in MPX_REMOTE_LADDER_PROXY=*|REMOTE_LADDER_PROXY=*) ;; *) out="$out $w";; esac
  done
  echo "${out# } MPX_REMOTE_LADDER_PROXY=" | sed 's/^ //'
}
[ "${PRODFIX_LIB_ONLY:-0}" = 1 ] && return 0 2>/dev/null

SOT=${PROD_LAUNCHER:?set PROD_LAUNCHER}; [ "$(md5sum < $SOT | cut -c1-32)" = 1d18935a82cc1d0015b4d78325cdb361 ] || { echo "!!! SoT changed since staging (want v8 1d18935a)"; exit 1; }
. $SOT
(cd $HERE && md5sum -c --quiet MD5SUMS) || { echo "!!! staged md5"; exit 1; }
case "${RBF:-prod}" in
  prod)   ARBF=""; AID="";;
  analog) ARBF=Plex_480p_analog_0x487A1BD1_853265774_s7.rbf; ARM5=89fd62e2f76d55349acf9c54bc33a5b6; AID=0x487A1BD1;;
  *) echo "!!! RBF=prod|analog"; exit 1;;
esac
AR=/media/fat/_Utility/Plex_analog.rbf
FD=/media/fat/linux/@DAEMON@; FDM=@DAEMON_MD5@
"${PROD_SSH[@]}" "cat > $FD.new" < $HERE/@DAEMON@
"${PROD_SSH[@]}" "[ \"\$(md5sum < $FD.new | cut -c1-32)\" = $FDM ] && chmod 755 $FD.new && mv $FD.new $FD && echo pushed" | grep -q pushed || { echo "!!! push"; exit 2; }
[ "$("${PROD_SSH[@]}" "md5sum < /media/fat/linux/misterplex.conf" | cut -c1-32)" = $PROD_CONF_MD5 ] || { echo "!!! conf is not $PROD_CONF_MD5"; exit 3; }
if [ -n "$ARBF" ]; then
  "${PROD_SSH[@]}" "cat > $AR.new" < $HERE/$ARBF
  "${PROD_SSH[@]}" "[ \"\$(md5sum < $AR.new | cut -c1-32)\" = $ARM5 ] && mv $AR.new $AR && echo pushed" | grep -q pushed || { echo "!!! push $AR"; exit 2; }
  echo "pushed $AR $ARM5"
fi
prod_kill_strays all || { echo "!!! strays"; exit 4; }
CAPS_OK=0
if [ -n "$ARBF" ]; then
  "${PROD_SSH[@]}" "echo 'load_core $AR' > /dev/MiSTer_cmd"; sleep 5
  # CAPS can lag load_core (a lab MiSTer 1011); poll ~20 s, then the daemon gate log is the authority (below).
  for i in $(seq 1 10); do
    ID=$("${PROD_SSH[@]}" "devmem 0x30140070 32; devmem 0x30140074 32" | tr '\n' ' ')
    case "$ID" in *0x4D504334*$AID*) CAPS_OK=1; echo "CAPS $ID (poll $i)"; break;; esac
    sleep 2
  done
  [ $CAPS_OK = 1 ] || echo "... CAPS page still '$ID' after ~25 s; deferring to the daemon's gate log"
fi
X=""; [ "${DIAG:-0}" = 1 ] && X="MPX_COMPANION_POLL_DIAG=1"
case "${PROXY:-on}" in
  on) ;;
  off) PROD_DAEMON_ENV=$(proxy_off_env "$PROD_DAEMON_ENV")
       echo "PROXY=off: env with MPX_REMOTE_LADDER_PROXY= (empty, beats the conf line): $PROD_DAEMON_ENV";;
  *) echo "!!! PROXY=on|off"; exit 7;;
esac
LOG0=$("${PROD_SSH[@]}" "wc -l < /tmp/mpxd.log 2>/dev/null || echo 0")
PROD_DAEMON=$FD PROD_DAEMON_MD5=$FDM prod_start_hardened $X || { echo "!!! start; run rollback-canonical.sh"; exit 6; }
if [ -n "$ARBF" ]; then
  G=""
  for i in $(seq 1 10); do
    G=$("${PROD_SSH[@]}" "tail -n +$((LOG0+1)) /tmp/mpxd.log | grep -aE \"core gate (open|closed|stays closed)\" | tail -1")
    case "$G" in *"core gate open: 'Plex' build $AID"*) break;; esac
    sleep 2
  done
  case "$G" in *"core gate open: 'Plex' build $AID"*) echo "GATE $G";;
    *) echo "!!! analog core $AID not confirmed (CAPS_OK=$CAPS_OK, gate '$G'); run rollback-canonical.sh"; exit 5;; esac
fi
"${PROD_SSH[@]}" "sleep 3; grep -E 'core gate open' /tmp/mpxd.log | tail -2"
if [ "${PROXY:-on}" = off ]; then
  CHK=$("${PROD_SSH[@]}" "P=\$(pidof @DAEMON@); V=\$(tr '\\0' '\\n' < /proc/\$P/environ | grep '^MPX_REMOTE_LADDER_PROXY=' || true); C=\$(tail -n +\$(($LOG0+1)) /tmp/mpxd.log | grep -c 'REMOTE_LADDER_PROXY=http' || true); echo \"env=[\$V] conf_proxy_lines=\$C\"")
  echo "PROXY=off check: $CHK"
  [ "$CHK" = "env=[MPX_REMOTE_LADDER_PROXY=] conf_proxy_lines=0" ] || { echo "!!! PROXY=off did not take (run rollback-canonical.sh)"; exit 8; }
fi
echo "DEPLOY_OK prodfix daemon=$FDM rbf=$( [ -n "$ARBF" ] && echo "$ARM5 ($AID analog)" || echo $PROD_RBF_MD5 ) proxy=${PROXY:-on}"
