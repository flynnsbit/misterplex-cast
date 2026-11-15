#!/bin/bash
# On-card / computer AirPlay helper. GCAST_* names are read-compat aliases.
ROOT="${AIRPLAY_MISTERPLEX_ROOT:-${GCAST_MISTERPLEX_ROOT:-/media/fat/misterplex}}"
export AIRPLAY_MISTERPLEX_ROOT="$ROOT"
export GCAST_MISTERPLEX_ROOT="$ROOT"
export AIRPLAY_XCODE_BIND="${AIRPLAY_XCODE_BIND:-${GCAST_XCODE_BIND:-127.0.0.1}}"
export GCAST_XCODE_BIND="$AIRPLAY_XCODE_BIND"
export AIRPLAY_XCODE_PORT="${AIRPLAY_XCODE_PORT:-${GCAST_XCODE_PORT:-18781}}"
export GCAST_XCODE_PORT="$AIRPLAY_XCODE_PORT"
export AIRPLAY_YTDLP="${AIRPLAY_YTDLP:-${GCAST_YTDLP:-/media/fat/linux/misterplex/yt-dlp/yt-dlp_linux_armv7l}}"
export GCAST_YTDLP="$AIRPLAY_YTDLP"
export MPX_CA_BUNDLE="${MPX_CA_BUNDLE:-$ROOT/ca/misterplex-cacert.pem}"
ff="${AIRPLAY_FFMPEG:-${GCAST_FFMPEG:-}}"
if [ -z "$ff" ] && [ -x "$ROOT/bin/ffmpeg" ]; then
  ff="$ROOT/bin/ffmpeg"
fi
if [ -n "$ff" ]; then
  export AIRPLAY_FFMPEG="$ff"
  export GCAST_FFMPEG="$ff"
fi
fp="${AIRPLAY_FFPROBE:-${GCAST_FFPROBE:-}}"
if [ -z "$fp" ] && [ -x "$ROOT/bin/ffprobe" ]; then
  fp="$ROOT/bin/ffprobe"
fi
if [ -n "$fp" ]; then
  export AIRPLAY_FFPROBE="$fp"
  export GCAST_FFPROBE="$fp"
fi
[ -x "$ROOT/bin/qjs" ] && export AIRPLAY_YT_JS="${AIRPLAY_YT_JS:-${GCAST_YT_JS:-$ROOT/bin/qjs}}"
[ -x "$ROOT/bin/node" ] && export AIRPLAY_YT_JS="${AIRPLAY_YT_JS:-${GCAST_YT_JS:-$ROOT/bin/node}}"
[ -n "${AIRPLAY_YT_JS:-}" ] && export GCAST_YT_JS="${GCAST_YT_JS:-$AIRPLAY_YT_JS}"
if [ -f "$ROOT/airplay_xcode.py" ]; then
  exec python3 "$ROOT/airplay_xcode.py" "$@"
fi
exec python3 "$ROOT/gcast_xcode.py" "$@"
