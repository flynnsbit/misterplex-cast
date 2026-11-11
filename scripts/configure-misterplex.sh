#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# configure-misterplex.sh - guided setup for /media/fat/linux/misterplex.conf.
#
# Asks a few questions (or takes flags / environment for a non-interactive run)
# and writes the settings file. It starts from your current misterplex.conf,
# or from the default misterplex.conf.example when there is none (or with
# --fresh). The old file is always backed up first. Burned-in subtitles are
# always left off. Only settings the MiSTerPlex program reads are written.
set -eu

PROG=configure-misterplex.sh
ROOT="${ROOT:-}"
FAT="$ROOT/media/fat"
CONF="${MPX_CONF:-$FAT/linux/misterplex.conf}"
EXAMPLE="${MPX_CONF_EXAMPLE:-}"
DRY=0
ASK=1
FRESH=0

# Answers: flags win over environment. Empty = keep what the file has.
SERVER="${MPX_PLEX_SERVER:-}"
LINK="${MPX_PLEXTV_LINK:-}"
RASTER="${MPX_ANALOG_RASTER:-}"
SUBS="${MPX_SUBTITLES:-}"
SUBLANG="${MPX_SUBTITLE_LANG:-}"
UISIZE="${MPX_UI_SIZE:-}"
MARGIN="${MPX_UI_SAFE_MARGIN:-}"
NAME="${MPX_FRIENDLY_NAME:-}"

usage() {
  cat <<USAGE
Usage: sh $PROG [options]

Guided setup for MiSTerPlex. With no options it asks each question and shows
the current answer in [brackets]; press Enter to keep it.

  -y, --non-interactive  Do not ask; use only the options below.
  --server URL           Plex server, e.g. http://192.0.2.10:32400
                         (several: comma-separated). "none" clears it.
  --link on|off          Sign in on the TV with a plex.tv/link code (default on).
  --raster 480i|240p|auto|default
                         Analog output (MiSTerPlex core). "default" = the core's
                         own F12 setting. 240p needs the 240p-capable core.
  --subtitles on|off     Text subtitles on the TV (on = use --sub-lang).
  --sub-lang CODE        Subtitle language, e.g. en, de, fr.
  --ui-size normal|large On-screen menu size.
  --safe-margin 0-20     Keep menus this many percent inside the screen edge.
  --name NAME            Player name shown in the cast list.
  --conf FILE            Settings file (default: $CONF).
  --example FILE         Default settings to start from.
  --fresh                Start from the default settings, not your current file
                         (the player name is kept).
  --dry-run              Show the result, write nothing.
  -h, --help             Show this help.

Environment (same as the flags): MPX_PLEX_SERVER, MPX_PLEXTV_LINK,
MPX_ANALOG_RASTER, MPX_SUBTITLES, MPX_SUBTITLE_LANG, MPX_UI_SIZE,
MPX_UI_SAFE_MARGIN, MPX_FRIENDLY_NAME, MPX_CONF, MPX_CONF_EXAMPLE.

Burned-in subtitles stay off: every run writes BURN_SUBS_FIX=0 and AUTO_BURN=0,
and SUBTITLES=burn is changed to off.

Next step after setup: start MiSTerPlex (load MiSTerPlex), note the code on the
Ready screen (the row "plex.tv/link ABCD"), then go to https://plex.tv/link,
sign in and enter the code. Until the device is linked, Browse Plex on the TV
shows FETCH FAILED. Casting from the Plex app on your phone works without
linking. Linking is needed once; the sign-in is saved in the settings file.
USAGE
}

next_step() { # $1 = resulting settings file
  if [ -n "$(conf_get PLEXTV_TOKEN "$1")" ]; then
    say "next: this MiSTer is already signed in to Plex; no link code is needed."
  elif [ "$(conf_get PLEXTV_LINK "$1")" = 0 ]; then
    say "next: plex.tv linking is OFF (PLEXTV_LINK=0) and no Plex sign-in is saved."
    say "      Browse Plex on the TV will show FETCH FAILED; casting from the Plex app still works."
    say "      To link: run again with --link on."
  else
    say "next: start MiSTerPlex, note the code on the Ready screen (plex.tv/link ABCD),"
    say "      then link at https://plex.tv/link (sign in, enter the code)."
    say "      Until then Browse Plex on the TV shows FETCH FAILED. Casting from the Plex app works without it."
  fi
}

say()  { printf '%s\n' "$PROG: $*"; }
die()  { printf '%s\n' "$PROG: error: $*" >&2; exit 1; }

need() { [ $# -ge 2 ] || die "$1 needs a value"; }
while [ $# -gt 0 ]; do
  case "$1" in
    -y|--non-interactive) ASK=0; shift ;;
    --server) need "$@"; SERVER=$2; shift 2 ;;
    --link) need "$@"; LINK=$2; shift 2 ;;
    --raster) need "$@"; RASTER=$2; shift 2 ;;
    --subtitles) need "$@"; SUBS=$2; shift 2 ;;
    --sub-lang) need "$@"; SUBLANG=$2; shift 2 ;;
    --ui-size) need "$@"; UISIZE=$2; shift 2 ;;
    --safe-margin) need "$@"; MARGIN=$2; shift 2 ;;
    --name) need "$@"; NAME=$2; shift 2 ;;
    --conf) need "$@"; CONF=$2; shift 2 ;;
    --example) need "$@"; EXAMPLE=$2; shift 2 ;;
    --fresh) FRESH=1; shift ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done
[ -t 0 ] || ASK=0

if [ -z "$EXAMPLE" ]; then
  here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  for e in "$FAT/misterplex/conf/misterplex.conf.example" "$here/../conf/misterplex.conf.example" \
           "$here/conf/misterplex.conf.example" "$here/../assets/misterplex.conf.example"; do
    if [ -f "$e" ]; then EXAMPLE=$e; break; fi
  done
fi

conf_get() { # last uncommented KEY= value
  [ -f "$2" ] || return 0
  awk -F= -v k="$1" '/^[ \t]*#/ {next} { x=$1; gsub(/^[ \t]+|[ \t]+$/, "", x)
    if (x == k) { v=substr($0, index($0, "=")+1); gsub(/\r/, "", v); gsub(/^[ \t"]+|[ \t"]+$/, "", v); last=v } }
    END { if (last != "") print last }' "$2"
}

# Start file.
if [ "$FRESH" = 0 ] && [ -f "$CONF" ]; then BASE=$CONF
elif [ -n "$EXAMPLE" ] && [ -f "$EXAMPLE" ]; then BASE=$EXAMPLE
else die "no settings file and no misterplex.conf.example found (use --example FILE)"
fi
say "starting from $BASE"

cur() { conf_get "$1" "$BASE"; }
ask() { # ask VAR "question" current
  if [ "$ASK" = 1 ] && eval "[ -z \"\${$1}\" ]"; then
    printf '%s [%s]: ' "$2" "$3"
    # shellcheck disable=SC2034 # ans is read through eval below
    read -r ans || ans=""
    eval "$1=\$ans"
  fi
}

ask SERVER "Plex server address (blank = find it on the network or via plex.tv)" "$(cur PLEX_SERVERS)"
ask LINK "Sign in on the TV with a plex.tv/link code? on/off" "$( [ "$(cur PLEXTV_LINK)" = 0 ] && echo off || echo on)"
ask RASTER "Analog output: 480i, 240p, auto, or default (core's F12 setting)" "$(cur ANALOG_RASTER | sed 's/^$/default/')"
ask SUBS "Text subtitles on? on/off" "$( case "$(cur SUBTITLE_LANG)" in ''|off) echo off ;; *) echo on ;; esac)"
if [ "$SUBS" = on ] || { [ -z "$SUBS" ] && [ -n "$(cur SUBTITLE_LANG)" ] && [ "$(cur SUBTITLE_LANG)" != off ]; }; then
  ask SUBLANG "Subtitle language (en, de, fr, ...)" "$(cur SUBTITLE_LANG | sed 's/^off$/en/;s/^$/en/')"
fi
ask UISIZE "Menu size: normal or large" "$(cur UI_SIZE | sed 's/^$/normal/')"
ask MARGIN "Safe margin percent (CRT: 5-10, HDMI: 0-5)" "$(cur UI_SAFE_MARGIN | sed 's/^$/5/')"
ask NAME "Player name in the cast list (blank = automatic)" "$(cur FRIENDLY_NAME)"

# Validate.
case "$SERVER" in
  ''|none) ;;
  *[!A-Za-z0-9.:/,_\[\]-]*) die "server address has odd characters: $SERVER" ;;
  http://*|https://*) ;;
  *) die "server must start with http:// or https:// (e.g. http://192.0.2.10:32400)" ;;
esac
case "$LINK" in ''|on|off|1|0) ;; *) die "--link must be on or off" ;; esac
case "$RASTER" in ''|480i|240p|auto|default) ;; *) die "--raster must be 480i, 240p, auto or default" ;; esac
case "$SUBS" in ''|on|off) ;; *) die "--subtitles must be on or off" ;; esac
case "$SUBLANG" in ''|*[!A-Za-z_-]*) [ -z "$SUBLANG" ] || die "bad subtitle language: $SUBLANG" ;; esac
case "$UISIZE" in ''|normal|large) ;; *) die "--ui-size must be normal or large" ;; esac
case "$MARGIN" in ''|[0-9]|1[0-9]|20) ;; *) die "--safe-margin must be 0-20" ;; esac
case "$NAME" in ''|*[!A-Za-z0-9._\ -]*) [ -z "$NAME" ] || die "player name: use letters, digits, space, . _ -" ;; esac

# Build the new file: KEY=VAL edits replace the last uncommented KEY= line,
# or append at the end. KEY with an empty VAL deletes uncommented KEY= lines.
# N9: TMP and OUT can hold the Plex tokens. umask 077 makes them 0600 from
# the moment they are created, and the trap removes them on exit or signal.
umask 077
TMP="${CONF}.mpx-cfg.$$"
OUT=""
trap 'rm -f -- "$TMP" ${OUT:+"$OUT"} ${OUT:+"$OUT.n"}' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# N12: no predictable /tmp fallback; mktemp is required.
OUT=$(mktemp) || die "mktemp failed; cannot create a private temp file"
cp "$BASE" "$OUT"
setkv() {
  awk -v k="$1" -v v="$2" -v del="$3" '
    { lines[NR]=$0; s=$0; sub(/^[ \t]+/, "", s)
      if (s !~ /^#/) { split(s, a, "="); x=a[1]; gsub(/[ \t]+$/, "", x); if (x == k && index(s, "=")) hit[NR]=1; lastk = (x == k && index(s, "=")) ? NR : lastk } }
    END { done=0
      for (i=1; i<=NR; i++) {
        if (hit[i]) { if (del) continue; if (i == lastk) { print k "=" v; done=1 }; continue }
        print lines[i] }
      if (!del && !done) print k "=" v }' "$OUT" > "$OUT.n" && mv "$OUT.n" "$OUT"
}
set_key() { setkv "$1" "$2" ""; }
del_key() { setkv "$1" "" 1; }

case "$SERVER" in none) del_key PLEX_SERVERS ;; '') ;; *) set_key PLEX_SERVERS "$SERVER" ;; esac
case "$LINK" in on|1) set_key PLEXTV_LINK 1 ;; off|0) set_key PLEXTV_LINK 0 ;; esac
case "$RASTER" in default) del_key ANALOG_RASTER ;; '') ;; *) set_key ANALOG_RASTER "$RASTER" ;; esac
case "$SUBS" in
  off) set_key SUBTITLE_LANG off ;;
  on) set_key SUBTITLE_LANG "${SUBLANG:-en}" ;;
  '') [ -z "$SUBLANG" ] || set_key SUBTITLE_LANG "$SUBLANG" ;;
esac
[ -z "$UISIZE" ] || set_key UI_SIZE "$UISIZE"
[ -z "$MARGIN" ] || set_key UI_SAFE_MARGIN "$MARGIN"
# --fresh: carry the player name/id and the Plex sign-in over, so the
# device stays linked (values are copied file to file, never printed).
if [ "$FRESH" = 1 ] && [ -f "$CONF" ]; then
  for k in FRIENDLY_NAME PLAYER_ID PLEXTV_TOKEN PLEX_TOKEN PLEX_HOME_ID PLEX_HOME_TITLE PLEX_HOME_TOKEN; do
    old=$(conf_get "$k" "$CONF"); [ -z "$old" ] || set_key "$k" "$old"
  done
fi
[ -z "$NAME" ] || set_key FRIENDLY_NAME "$NAME"
# Burned-in subtitles stay off: opt-in only, by hand, for servers you own.
set_key BURN_SUBS_FIX 0
set_key AUTO_BURN 0
[ "$(conf_get SUBTITLES "$OUT")" != burn ] || set_key SUBTITLES off

if [ "$DRY" = 1 ]; then
  say "dry run: $CONF would get these settings:"
  for k in PLEX_SERVERS PLEXTV_LINK ANALOG_RASTER SUBTITLES SUBTITLE_LANG UI_SIZE UI_SAFE_MARGIN FRIENDLY_NAME PLAYER_ID BURN_SUBS_FIX AUTO_BURN; do
    printf '  %s=%s\n' "$k" "$(conf_get "$k" "$OUT")"
  done
  next_step "$OUT"
  exit 0
fi

mkdir -p "$(dirname "$CONF")"
if [ -f "$CONF" ]; then
  bak="$CONF.bak.$(date +%Y%m%d-%H%M%S)"
  cp -a "$CONF" "$bak"
  say "backed up the old settings to $bak"
  # Keep only the newest ${MPX_KEEP_BAK:-3} backups.
  n=0; for old in "$CONF".bak.[0-9]*; do [ -e "$old" ] && n=$((n + 1)); done
  for old in "$CONF".bak.[0-9]*; do   # globs sort; the stamp makes oldest first
    [ "$n" -gt "${MPX_KEEP_BAK:-3}" ] || break
    [ -e "$old" ] && rm -f -- "$old"; n=$((n - 1))
  done
fi
cp "$OUT" "$TMP" && mv -f "$TMP" "$CONF"
say "wrote $CONF"
say "restart to apply: sh $FAT/misterplex/bin/misterplexd-boot-rc3.sh stop; sh $FAT/misterplex/bin/misterplexd-boot-rc3.sh start"
next_step "$CONF"
