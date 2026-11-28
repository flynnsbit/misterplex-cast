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
  --safe-margin 0-15     Keep menus this many percent inside the screen edge.
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
# Print the current value, or $2 when conf_get prints nothing (an unset key
# is zero lines, so `value | sed 's/^$/default/'` never runs).
cur_or() { v=$(cur "$1"); if [ -n "$v" ]; then printf '%s\n' "$v"; else printf '%s\n' "$2"; fi; }
# Tests can force the question/form path with MPX_SETTINGS_ASK=1 (stdin need not be a tty).
[ "${MPX_SETTINGS_ASK:-0}" = 1 ] && ASK=1

ask() { # ask VAR "question" current
  if [ "$ASK" = 1 ] && eval "[ -z \"\${$1}\" ]"; then
    printf '%s [%s]: ' "$2" "$3"
    # shellcheck disable=SC2034 # ans is read through eval below
    read -r ans || ans=""
    eval "$1=\$ans"
  fi
}

settings_use_dialog() {
  if type use_dialog >/dev/null 2>&1; then
    use_dialog
    return
  fi
  case "${MPX_MENU_UI:-}" in
    text) return 1 ;;
    dialog) return 0 ;;
  esac
  command -v dialog >/dev/null 2>&1 && [ -t 0 ] && [ -t 1 ]
}

# Fill empty answer vars from the current file (or the shown default) so Save
# and a cancelled field keep what the form displayed.
settings_load_shown() {
  [ -n "$SERVER" ] || SERVER=$(cur PLEX_SERVERS)
  if [ -z "$LINK" ]; then
    if [ "$(cur PLEXTV_LINK)" = 0 ]; then LINK=off; else LINK=on; fi
  fi
  [ -n "$RASTER" ] || RASTER=$(cur_or ANALOG_RASTER default)
  if [ -z "$SUBS" ]; then
    case "$(cur SUBTITLE_LANG)" in ''|off) SUBS=off ;; *) SUBS=on ;; esac
  fi
  [ -n "$SUBLANG" ] || SUBLANG=$(cur_or SUBTITLE_LANG en)
  [ "$SUBLANG" = off ] && SUBLANG=en
  [ -n "$UISIZE" ] || UISIZE=$(cur_or UI_SIZE normal)
  [ -n "$MARGIN" ] || MARGIN=$(cur_or UI_SAFE_MARGIN 5)
  [ -n "$NAME" ] || NAME=$(cur FRIENDLY_NAME)
}

# Blue dialog form: a menu of items, each opens an inputbox or radiolist.
# Cancel returns 75 (no write). Save leaves the answer vars set and returns 0.
settings_form() {
  settings_load_shown
  while :; do
    sl_lab=$SUBLANG
    [ "$SUBS" = off ] && sl_lab="(off)"
    if type use_dialog >/dev/null 2>&1 && use_dialog; then
      # dlg (menu head) draws on the terminal and returns the pick on stdout, so this
      # capture keeps the blue dialog (the old 3>&1 1>&2 2>&3 form needed a tty stdout).
      _ch=$(dlg --title "Settings" --no-tags --cancel-label "Cancel" \
        --menu "Choose a setting, then Save." 20 72 12 \
        server "Plex server [$SERVER]" \
        link "plex.tv link [$LINK]" \
        raster "Analog output [$RASTER]" \
        subs "Text subtitles [$SUBS]" \
        sublang "Subtitle language [$sl_lab]" \
        uisize "Menu size [$UISIZE]" \
        margin "Safe margin [$MARGIN]" \
        name "Player name [$NAME]" \
        save "Save") || return 75
    else
      printf '\nSettings\n\n' >&2
      printf '  1) Plex server [%s]\n' "$SERVER" >&2
      printf '  2) plex.tv link [%s]\n' "$LINK" >&2
      printf '  3) Analog output [%s]\n' "$RASTER" >&2
      printf '  4) Text subtitles [%s]\n' "$SUBS" >&2
      printf '  5) Subtitle language [%s]\n' "$sl_lab" >&2
      printf '  6) Menu size [%s]\n' "$UISIZE" >&2
      printf '  7) Safe margin [%s]\n' "$MARGIN" >&2
      printf '  8) Player name [%s]\n' "$NAME" >&2
      printf '  9) Save\n  0) Cancel\n\nChoose 0-9: ' >&2
      read -r _n || return 75
      case "$_n" in
        1) _ch="server" ;; 2) _ch="link" ;; 3) _ch="raster" ;; 4) _ch="subs" ;;
        5) _ch="sublang" ;; 6) _ch="uisize" ;; 7) _ch="margin" ;; 8) _ch="name" ;;
        9) _ch="save" ;; *) return 75 ;;
      esac
    fi
    case "$_ch" in
      server)
        if type menu_inputbox >/dev/null 2>&1; then
          _v=$(menu_inputbox "Plex server address (blank = find it on the network or via plex.tv)" "$SERVER") && SERVER=$_v
        else
          printf 'Plex server address [%s]: ' "$SERVER" >&2; read -r _v || _v=$SERVER; SERVER=$_v
        fi ;;
      link)
        _on=off; _off=on; [ "$LINK" = on ] && _on=on && _off=off
        if type menu_radiolist >/dev/null 2>&1; then
          _v=$(menu_radiolist "Sign in on the TV with a plex.tv/link code?" on "on" "$_on" off "off" "$_off") && LINK=$_v
        else
          printf 'plex.tv link on/off [%s]: ' "$LINK" >&2; read -r _v || _v=$LINK; [ -n "$_v" ] && LINK=$_v
        fi ;;
      raster)
        _d=off _480=off _240=off _a=off
        case "$RASTER" in default) _d=on ;; 480i) _480=on ;; 240p) _240=on ;; auto) _a=on ;; *) _d=on ;; esac
        if type menu_radiolist >/dev/null 2>&1; then
          _v=$(menu_radiolist "Analog output (default = the core's F12 setting)" \
            default "default" "$_d" 480i "480i" "$_480" 240p "240p" "$_240" auto "auto" "$_a") && RASTER=$_v
        else
          printf 'Analog output 480i/240p/auto/default [%s]: ' "$RASTER" >&2; read -r _v || _v=$RASTER; [ -n "$_v" ] && RASTER=$_v
        fi ;;
      subs)
        _on=off; _off=on; [ "$SUBS" = on ] && _on=on && _off=off
        if type menu_radiolist >/dev/null 2>&1; then
          _v=$(menu_radiolist "Text subtitles?" on "on" "$_on" off "off" "$_off") && SUBS=$_v
        else
          printf 'Text subtitles on/off [%s]: ' "$SUBS" >&2; read -r _v || _v=$SUBS; [ -n "$_v" ] && SUBS=$_v
        fi ;;
      sublang)
        if type menu_inputbox >/dev/null 2>&1; then
          _v=$(menu_inputbox "Subtitle language (en, de, fr, ...)" "$SUBLANG") && SUBLANG=$_v
        else
          printf 'Subtitle language [%s]: ' "$SUBLANG" >&2; read -r _v || _v=$SUBLANG; [ -n "$_v" ] && SUBLANG=$_v
        fi ;;
      uisize)
        _n=off; _l=off
        case "$UISIZE" in large) _l=on ;; *) _n=on ;; esac
        if type menu_radiolist >/dev/null 2>&1; then
          _v=$(menu_radiolist "Menu size" normal "normal" "$_n" large "large" "$_l") && UISIZE=$_v
        else
          printf 'Menu size normal/large [%s]: ' "$UISIZE" >&2; read -r _v || _v=$UISIZE; [ -n "$_v" ] && UISIZE=$_v
        fi ;;
      margin)
        if type menu_inputbox >/dev/null 2>&1; then
          _v=$(menu_inputbox "Safe margin percent 0-15 (CRT: 5-10, HDMI: 0-5)" "$MARGIN") && MARGIN=$_v
        else
          printf 'Safe margin percent 0-15 [%s]: ' "$MARGIN" >&2; read -r _v || _v=$MARGIN; [ -n "$_v" ] && MARGIN=$_v
        fi ;;
      name)
        if type menu_inputbox >/dev/null 2>&1; then
          _v=$(menu_inputbox "Player name in the cast list (blank = automatic)" "$NAME") && NAME=$_v
        else
          printf 'Player name [%s]: ' "$NAME" >&2; read -r _v || _v=$NAME; NAME=$_v
        fi ;;
      save) break ;;
      *) return 75 ;;
    esac
  done
}

if [ "$ASK" = 1 ] && settings_use_dialog; then
  # Cancel: exit 0 when run by itself; the Scripts menu passes MPX_SETTINGS_CANCEL_RC=75 so
  # it goes straight back to the main menu with no Done box.
  settings_form || { say "settings cancelled"; exit "${MPX_SETTINGS_CANCEL_RC:-0}"; }
elif [ "$ASK" = 1 ]; then
  # Text questions (no dialog): show the same defaults as the form.
  ask SERVER "Plex server address (blank = find it on the network or via plex.tv)" "$(cur PLEX_SERVERS)"
  ask LINK "Sign in on the TV with a plex.tv/link code? on/off" "$( [ "$(cur PLEXTV_LINK)" = 0 ] && echo off || echo on)"
  ask RASTER "Analog output: 480i, 240p, auto, or default (core's F12 setting)" "$(cur_or ANALOG_RASTER default)"
  ask SUBS "Text subtitles on? on/off" "$( case "$(cur SUBTITLE_LANG)" in ''|off) echo off ;; *) echo on ;; esac)"
  if [ "$SUBS" = on ] || { [ -z "$SUBS" ] && [ -n "$(cur SUBTITLE_LANG)" ] && [ "$(cur SUBTITLE_LANG)" != off ]; }; then
    sl=$(cur SUBTITLE_LANG)
    case "$sl" in ''|off) sl=en ;; esac
    ask SUBLANG "Subtitle language (en, de, fr, ...)" "$sl"
  fi
  ask UISIZE "Menu size: normal or large" "$(cur_or UI_SIZE normal)"
  ask MARGIN "Safe margin percent 0-15 (CRT: 5-10, HDMI: 0-5)" "$(cur_or UI_SAFE_MARGIN 5)"
  ask NAME "Player name in the cast list (blank = automatic)" "$(cur FRIENDLY_NAME)"
fi

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
case "$MARGIN" in ''|[0-9]|1[0-5]) ;; *) die "--safe-margin must be 0-15" ;; esac
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
