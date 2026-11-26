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
exec python3 "$ROOT/gcast_xcode.py" "$@"
