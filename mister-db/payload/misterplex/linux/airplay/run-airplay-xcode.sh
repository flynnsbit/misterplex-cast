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
# Same DVB SDT charset aliases as kGconvStubModules in
# arm/misterplexd/airplay/yt_live.hpp:520-546. Write atomically so a
# concurrent helper start cannot observe a partial gconv-modules file.
# MPX_GCONV_STUB_DIR overrides the directory; "off" disables the stub.
gconv_stub="${MPX_GCONV_STUB_DIR:-/tmp/mpx-gconv-stub}"
if [ "$gconv_stub" != "off" ]; then
  case "$gconv_stub" in
    /*)
      if mkdir -p "$gconv_stub" 2>/dev/null; then
        gconv_tmp="$gconv_stub/.gconv-modules.$$"
        if cat > "$gconv_tmp" <<'GCONV_EOF'
# MisterPlex: route DVB SDT string charsets to the built-in ASCII converter (no gconv dlopen; see yt_live.hpp)
alias	ISO6937//	ANSI_X3.4-1968//
alias	ISO-6937//	ANSI_X3.4-1968//
alias	ISO_6937//	ANSI_X3.4-1968//
alias	ISO6937-2//	ANSI_X3.4-1968//
alias	ISO-6937-2//	ANSI_X3.4-1968//
alias	ISO_6937-2//	ANSI_X3.4-1968//
alias	UTF-16BE//	ANSI_X3.4-1968//
alias	UTF-16//	ANSI_X3.4-1968//
alias	GB2312//	ANSI_X3.4-1968//
alias	BIG5//	ANSI_X3.4-1968//
alias	EUC-KR//	ANSI_X3.4-1968//
alias	ISO-8859-1//	ANSI_X3.4-1968//
alias	ISO-8859-2//	ANSI_X3.4-1968//
alias	ISO-8859-3//	ANSI_X3.4-1968//
alias	ISO-8859-4//	ANSI_X3.4-1968//
alias	ISO-8859-5//	ANSI_X3.4-1968//
alias	ISO-8859-6//	ANSI_X3.4-1968//
alias	ISO-8859-7//	ANSI_X3.4-1968//
alias	ISO-8859-8//	ANSI_X3.4-1968//
alias	ISO-8859-9//	ANSI_X3.4-1968//
alias	ISO-8859-10//	ANSI_X3.4-1968//
alias	ISO-8859-11//	ANSI_X3.4-1968//
alias	ISO-8859-13//	ANSI_X3.4-1968//
alias	ISO-8859-14//	ANSI_X3.4-1968//
alias	ISO-8859-15//	ANSI_X3.4-1968//
GCONV_EOF
        then
          if mv -f "$gconv_tmp" "$gconv_stub/gconv-modules"; then
            export GCONV_PATH="$gconv_stub"
          else
            rm -f "$gconv_tmp"
          fi
        else
          rm -f "$gconv_tmp"
        fi
      fi
      ;;
  esac
fi
if [ -f "$ROOT/airplay_xcode.py" ]; then
  exec python3 "$ROOT/airplay_xcode.py" "$@"
fi
exec python3 "$ROOT/gcast_xcode.py" "$@"
