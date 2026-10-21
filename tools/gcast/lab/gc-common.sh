# GCAST a lab MiSTer lab helpers (sourced after $LAB_COMMON)
GBIN_SRC=$HOME/gcast/misterplexd-gcast-82855645
GBIN=/media/fat/linux/misterplexd-gcast-82855645
PRODD=/media/fat/linux/misterplexd-prodfix-4fdf97e0
# Lab endpoints via env (no hardcoded defaults in product code). Override on the lab host.
: "${MPX_LADDER_URL:=${MPX_REMOTE_LADDER_PROXY:-}}"
: "${MPX_MENU_URL:=}"
: "${GCAST_XCODE:=}"
: "${GCAST_LAB_MEDIA:=}"
PENV="MPX_LEGACY_CORE_PREFIX=999800cf MPX_D52_LOG=1 MPX_IDLEFB_PATH=devmem MPX_BROWSE_V2=1 MPX_BROWSE_MVPB=1"
[ -n "$MPX_LADDER_URL" ] && PENV="$PENV MPX_LADDER_URL=$MPX_LADDER_URL MPX_REMOTE_LADDER_PROXY=$MPX_LADDER_URL"
[ -n "$MPX_MENU_URL" ] && PENV="$PENV MPX_MENU_URL=$MPX_MENU_URL"
SENDER="$HOME/gcast/venv312/bin/python $HOME/gcast/harness/tools/gcast/ios_like_sender.py"
MEDIA="${GCAST_LAB_MEDIA:-}"
# write /tmp/eng-$1.environ + .cmdline on a lab MiSTer (prod env + extra vars $2...)
mkenv(){ local PF=$1; shift
  S "{ for v in PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME=/root $PENV $*; do printf '%s\0' \$v; done; } >/tmp/eng-$PF.environ
     printf '%s\0' X --name $NAME --id misterplex-192.0.2.12 --port 3005 --conf $CONF >/tmp/eng-$PF.cmdline
     echo ENV_$PF: \$(tr '\0' ' ' </tmp/eng-$PF.environ)"
}
prod_core(){ S "{ echo load_core /media/fat/_Utility/Plex.rbf > /dev/MiSTer_cmd; for i in \$(seq 1 20); do sleep 2; [ \"\$(cat /tmp/CORENAME)\" = Plex ] && break; done; }; echo CORE=\$(cat /tmp/CORENAME)"; }
plexcast(){ # $1 = log
  PLEX_WEB_BASE=http://198.51.100.153:32400 PLEX_METADATA_KEY=/library/metadata/40780 PLAYER=$PLAYER PLEX_CAST_WAIT_PLAY_S=60 PLEX_WEB_CLIENT_ID=mpx-eng-gcast \
    timeout -k 10 80 python3 "${LAB_WAN_CAST:?set LAB_WAN_CAST to v2-lab-wan-cast.py}" >"$1" 2>&1
}
restore_prod(){ # $1 = W
  S "$KILL" ; prod_core | tee $1/restore-core.txt
  mkenv prod >/dev/null; launch $PRODD eng-prod | tee $1/restore-daemon.txt; sleep 8
  S "md5sum /media/fat/_Utility/Plex.rbf /media/fat/linux/misterplex.conf | cut -c1-8,33-; grep -aE 'core gate open|MAST' /tmp/mpxd.log | tail -3" | tee $1/restore-verify.txt
  plexcast $1/prod-cast.log; sleep 8; hdmi_png $1/prod-playcheck.png
  S "/media/fat/misterplex/bin/set_status --status | head -1; grep -aE 'PLXD has_frame' /tmp/mpxd.log | tail -2" | tee $1/prod-playcheck.txt
  curl -s -m 8 "$PLAYER/player/playback/stop?commandID=9902" >/dev/null
}
