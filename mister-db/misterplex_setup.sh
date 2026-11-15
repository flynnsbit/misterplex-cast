#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# misterplex_setup.sh - finish a MisterPlex install done by update_all / Downloader.
#
# Downloader installs every MisterPlex file it is allowed to write (the core, the
# /media/fat/misterplex tree). It refuses paths under /media/fat/linux and it never
# runs scripts, so this step does what install-misterplex.sh does after its copies:
#   - copies the CA bundle and the AirPlay / Cast helpers into /media/fat/linux
#   - creates /media/fat/linux/misterplex.conf from the example ONLY if it is missing
#     (an existing conf, including OVERSCAN and FRIENDLY_NAME, is never touched)
#   - removes the old PlexOB.rbf and a root MiSTerPlex.rbf copy once the
#     _Utility/MiSTerPlex.rbf on the card matches its shipped md5
#   - adds the one boot line to linux/user-startup.sh (via misterplexd-boot-rc3.sh install)
# Safe to run again (after every update_all that changed MisterPlex): nothing is
# added twice and the conf is never replaced. Run it from the MiSTer Scripts menu
# or with: sh /media/fat/Scripts/misterplex_setup.sh
# exFAT: no modes, owners or symlinks; plain copies; scripts are run with sh.
set -eu
PROG=misterplex_setup.sh
ROOT="${ROOT:-}"
FAT="$ROOT/media/fat"
MPX="$FAT/misterplex"
LNX="$FAT/linux"
CONF="$LNX/misterplex.conf"
HOOK="$MPX/bin/misterplexd-boot-rc3.sh"
STAMP=$(date +%Y%m%d-%H%M%S)
say()  { printf '%s\n' "$PROG: $*"; }
warn() { printf '%s\n' "$PROG: warning: $*" >&2; }
die()  { printf '%s\n' "$PROG: error: $*" >&2; exit 1; }
md5of() { md5sum "$1" | cut -d' ' -f1; }
LEDGER="$MPX/.misterplex-created"
created() { mkdir -p "$MPX" && printf '%s\n' "${1#"$FAT"/}" >> "$LEDGER"; }

[ -d "$FAT" ] || die "$FAT not found; run this on the MiSTer"
[ -f "$HOOK" ] || die "$HOOK is missing; run update_all (with the MisterPlex database) first"
SUM="$MPX/cores/MiSTerPlex.rbf.md5"
CORE="$FAT/_Utility/MiSTerPlex.rbf"
[ -f "$SUM" ] || die "$SUM is missing; run update_all again"
want=$(awk 'NR==1 { print tolower($1); exit }' "$SUM")
[ -f "$CORE" ] || die "$CORE is missing; run update_all again"
[ "$(md5of "$CORE")" = "$want" ] || die "$CORE does not match $SUM; run update_all again"

# 1. Stop a running copy (the new files are already in place).
say "stopping a running MisterPlex (if any)"
env ROOT="$ROOT" sh "$HOOK" stop || warn "stop returned an error; continuing"

# 2. /media/fat/linux files (Downloader cannot write there).
mkdir -p "$LNX"
src="$MPX/linux/misterplex-cacert.pem"
if [ -f "$src" ]; then
  if [ -f "$LNX/misterplex-cacert.pem" ] && cmp -s "$src" "$LNX/misterplex-cacert.pem"; then
    say "CA bundle already current"
  else
    if [ -f "$LNX/misterplex-cacert.pem" ]; then
      cp "$LNX/misterplex-cacert.pem" "$LNX/misterplex-cacert.pem.bak.$STAMP"; created "$LNX/misterplex-cacert.pem.bak.$STAMP"
    fi
    cp "$src" "$LNX/misterplex-cacert.pem"; say "installed $LNX/misterplex-cacert.pem"
  fi
fi
for h in airplay gcast; do
  [ -d "$MPX/linux/$h" ] || continue
  say "installing the $h helper -> $LNX/$h"
  mkdir -p "$LNX/$h"
  cp -RL "$MPX/linux/$h/." "$LNX/$h/"
  # Manifest so uninstall-misterplex.sh removes only these files.
  (cd "$MPX/linux/$h" && { find . -type d ! -name . | sed 's|^\./|d |'
    find . \( -type f -o -type l \) | sed 's|^\./|f |'; }) > "$LNX/$h/.misterplex-files"
  if [ -f "$LNX/$h/MD5SUMS" ]; then
    (cd "$LNX/$h" && md5sum -c MD5SUMS >/dev/null) || die "$LNX/$h: MD5SUMS check failed"
  fi
done

# 3. Settings: create once, never replace.
if [ -f "$CONF" ]; then
  say "keeping your settings: $CONF"
elif [ -f "$MPX/conf/misterplex.conf.example" ]; then
  cp "$MPX/conf/misterplex.conf.example" "$CONF"; say "created $CONF from the default settings"
else
  warn "no misterplex/conf/misterplex.conf.example; the daemon will use built-in defaults"
fi

# 4. One core only, in _Utility. Same rules as install-misterplex.sh.
BAKD="$MPX/backup"
for b in "$CORE".bak.[0-9]* "$FAT/MiSTerPlex.rbf".bak.[0-9]*; do
  [ -f "$b" ] || continue; mkdir -p "$BAKD"; mv -f "$b" "$BAKD/"
done
if [ -f "$FAT/MiSTerPlex.rbf" ]; then
  if [ "$(md5of "$FAT/MiSTerPlex.rbf")" = "$want" ]; then
    rm -f -- "$FAT/MiSTerPlex.rbf" && say "removed the root core copy $FAT/MiSTerPlex.rbf"
  else
    mkdir -p "$BAKD"; mv -f "$FAT/MiSTerPlex.rbf" "$BAKD/MiSTerPlex.rbf.root.$STAMP"
    say "moved an older root core copy to $BAKD/MiSTerPlex.rbf.root.$STAMP"
  fi
fi
# The core was renamed from PlexOB.rbf. Only these four exact paths; no globs.
case "$FAT" in ''|/|//*|*\**|*\?*) die "refusing to clean core files under $FAT" ;; esac
for old in "$FAT/PlexOB.rbf" "$FAT/_Utility/PlexOB.rbf" "$FAT/_Console/PlexOB.rbf" "$FAT/cores/PlexOB.rbf"; do
  [ -f "$old" ] || continue
  rm -f -- "$old" && say "removed the old core $old (renamed to MiSTerPlex.rbf)"
done

# 5. Best effort exec bits (ignored on exFAT; everything is started with sh or works without).
chmod +x "$MPX/bin/"*.sh "$MPX/bin/misterplexd" "$MPX/bin/misterplexd-spelunkr" 2>/dev/null || true

# 6. Boot hook: one line in linux/user-startup.sh, never twice.
say "enabling start at boot"
env ROOT="$ROOT" sh "$HOOK" install

say "done. Reboot the MiSTer, then load MiSTerPlex from _Utility. Settings: $CONF"
say "link it: note the code on the Ready screen, go to https://plex.tv/link, sign in and enter it."
say "to change settings: sh $MPX/bin/configure-misterplex.sh"
