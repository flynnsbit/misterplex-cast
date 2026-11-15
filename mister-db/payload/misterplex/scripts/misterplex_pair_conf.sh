#!/bin/sh
# Upsert misterplex.conf keys for the live RBF pair.
# Does not touch PLEX_BASE / PLEX_TOKEN / PLEX_SERVERS.
#
# Gold-daemon cores (480p / 240p15 / 480i, bytes acfa03d7):
#   lead 40, drop 0, fast_bilinear, no fps= filter, OSD_CONTROL=0
#   (leftover 720p OSD was retargeting the 480p ladder).
# L4 720p24: 720p ladder + OSD_CONTROL=0 (leftover F12 480p bits must not
# retarget DECODE to 640x480 on this RBF). Same lead/drop (safe). Do not
# force the 480p transcode profile onto this RBF.

CONF="${MISTERPLEX_CONF:-/media/fat/misterplex/misterplex.conf}"

# $1 = live rbf md5
misterplex_pair_conf_keys() {
  sum=$1
  case "$sum" in
    03f1b95ac67a568f24193234ea8cb072|17aa9a7e864555226d3ac313bc50177c|e7097c6c71bfbbf137cb3baf42cb929a|ca7aff2848038e0f620fc4cb49e46a93|ad57fe6682ba79b1820890a000af4208|4184a3b05eafb14d14fd4ac9d00c5545|28cb5a75ac6f2477112fb94095306568|9eacd6d26d9cb2a5904b7b923a5efa19|786d10f1fe3abca8851cbf3da9355dc8|2ac1e69f82a6de18ff21bc58e3ae02eb|0eea3580a5a0dacf60bf65c50499bcc9)
      echo "CONTENT_RES=720p"
      echo "DISPLAY_RES=720p"
      echo "DECODE=1280x720"
      echo "TRANSCODE_PROFILE=720p"
      # Dual-A9 24p ladder (plexTranscodeProfiles 720p). osd_menu 20 Mbps
      # must not overwrite this — WEAK_BITRATE is explicit.
      echo "WEAK_BITRATE=8000"
      echo "WEAK_QUALITY=40"
      echo "WEAK_H264_PROFILE=baseline"
      echo "OSD_CONTROL=0"
      echo "IDLE_SCREEN=logo"
      echo "AV_PRESENT_LEAD_MS=40"
      echo "AV_RESYNC_DROP_MS=0"
      echo "FFMPEG_SWS_FLAGS=fast_bilinear"
      echo "FFMPEG_FPS_FILTER=off"
      ;;
    4d6efef954acf7b33747f35ac2878c1b)
      echo "CONTENT_RES=240p"
      echo "DISPLAY_RES=480p"
      echo "DECODE=320x240"
      echo "TRANSCODE_PROFILE=240p"
      echo "OSD_CONTROL=0"
      echo "IDLE_SCREEN=logo"
      echo "AV_PRESENT_LEAD_MS=40"
      echo "AV_RESYNC_DROP_MS=0"
      echo "FFMPEG_SWS_FLAGS=fast_bilinear"
      echo "FFMPEG_FPS_FILTER=off"
      ;;
    61db00e7d54efad7c1a456b127b798bd)
      echo "CONTENT_RES=480p"
      echo "DISPLAY_RES=480p"
      echo "DECODE=640x480"
      echo "TRANSCODE_PROFILE=480p"
      echo "OSD_CONTROL=0"
      echo "IDLE_SCREEN=logo"
      echo "AV_PRESENT_LEAD_MS=40"
      echo "AV_RESYNC_DROP_MS=0"
      echo "FFMPEG_SWS_FLAGS=fast_bilinear"
      echo "FFMPEG_FPS_FILTER=off"
      ;;
    07f54d9f8f0eda2fe75d9cc314f6de54|*)
      # Default / Plex_480p gold
      echo "CONTENT_RES=480p"
      echo "DISPLAY_RES=480p"
      echo "DECODE=640x480"
      echo "TRANSCODE_PROFILE=480p"
      echo "OSD_CONTROL=0"
      echo "IDLE_SCREEN=logo"
      echo "AV_PRESENT_LEAD_MS=40"
      echo "AV_RESYNC_DROP_MS=0"
      echo "FFMPEG_SWS_FLAGS=fast_bilinear"
      echo "FFMPEG_FPS_FILTER=off"
      ;;
  esac
}

# $1 = md5  $2 = conf path (optional)
misterplex_apply_pair_conf() {
  sum=$1
  dest=${2:-$CONF}
  keys=$(misterplex_pair_conf_keys "$sum") || return 1
  mkdir -p "$(dirname "$dest")"
  touch "$dest"
  _pair_conf_tmp="$dest.pairconf.$$"
  printf '%s\n' "$keys" | awk -v dest="$dest" '
    BEGIN {
      while ((getline line < dest) > 0) {
        n++
        raw[n] = line
        if (line ~ /^[[:space:]]*#/ || line ~ /^[[:space:]]*$/) continue
        split(line, a, "=")
        k = a[1]
        gsub(/[[:space:]]/, "", k)
        have[k] = n
      }
      close(dest)
    }
    {
      split($0, a, "=")
      k = a[1]
      if (k in have) raw[have[k]] = $0
      else { n++; raw[n] = $0 }
    }
    END {
      for (i = 1; i <= n; i++) print raw[i]
    }
  ' >"$_pair_conf_tmp" && mv -f "$_pair_conf_tmp" "$dest"
}
