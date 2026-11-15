#!/bin/bash
ROOT="${GCAST_MISTERPLEX_ROOT:-/media/fat/misterplex}"
export GCAST_MISTERPLEX_ROOT="$ROOT"
export GCAST_XCODE_BIND="${GCAST_XCODE_BIND:-127.0.0.1}"
export GCAST_XCODE_PORT="${GCAST_XCODE_PORT:-18781}"
export GCAST_YTDLP="${GCAST_YTDLP:-/media/fat/linux/misterplex/yt-dlp/yt-dlp_linux_armv7l}"
export MPX_CA_BUNDLE="${MPX_CA_BUNDLE:-$ROOT/ca/misterplex-cacert.pem}"
if [ -z "${GCAST_FFMPEG:-}" ] && [ -x "$ROOT/bin/ffmpeg" ]; then
  export GCAST_FFMPEG="$ROOT/bin/ffmpeg"
fi
if [ -z "${GCAST_FFPROBE:-}" ] && [ -x "$ROOT/bin/ffprobe" ]; then
  export GCAST_FFPROBE="$ROOT/bin/ffprobe"
fi
[ -x "$ROOT/bin/qjs" ] && export GCAST_YT_JS="${GCAST_YT_JS:-$ROOT/bin/qjs}"
[ -x "$ROOT/bin/node" ] && export GCAST_YT_JS="${GCAST_YT_JS:-$ROOT/bin/node}"
exec python3 "$ROOT/gcast_xcode.py" "$@"
