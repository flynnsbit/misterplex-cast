#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# misterplex_setup.sh - finish a MisterPlex install done by update_all / Downloader.
#
# Downloader installs every MisterPlex file it is allowed to write (the core, the
# /media/fat/misterplex tree). It refuses paths under /media/fat/linux and it never
# runs scripts, so this step (Scripts/configure-misterplex.sh runs it on first use and from
# Repair / Reinstall) does what install-misterplex.sh does after its copies:
#   - copies the CA bundle and the AirPlay / Cast helpers into /media/fat/linux
#   - creates /media/fat/linux/misterplex.conf from the example ONLY if it is missing
#     (an existing conf, including OVERSCAN and FRIENDLY_NAME, is never touched)
#   - removes the old PlexOB.rbf and a root MiSTerPlex.rbf copy once the
#     _Utility/MiSTerPlex.rbf on the card matches its shipped md5
#   - adds the one boot line to linux/user-startup.sh (via misterplexd-boot-rc3.sh install)
# Safe to run again (after every update_all that changed MisterPlex): nothing is
# added twice and the conf is never replaced. Run it from Scripts > configure-misterplex
# (Repair / Reinstall) or with: bash /media/fat/Scripts/configure-misterplex.sh setup
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
    cp "$src" "$LNX/misterplex-cacert.pem" || die "could not install $LNX/misterplex-cacert.pem"
    say "installed $LNX/misterplex-cacert.pem"
  fi
fi
# Install both airplay and gcast helper trees (if staged under misterplex/linux/).
for h in airplay gcast; do
  [ -d "$MPX/linux/$h" ] || continue
  say "installing the $h helper -> $LNX/$h"
  mkdir -p "$LNX/$h" || die "could not create $LNX/$h"
  cp -RL "$MPX/linux/$h/." "$LNX/$h/" || die "could not install the $h helper to $LNX/$h"
  # Manifest so uninstall-misterplex.sh removes only these files.
  (cd "$MPX/linux/$h" && { find . -type d ! -name . | sed 's|^\./|d |'
    find . \( -type f -o -type l \) | sed 's|^\./|f |'; }) > "$LNX/$h/.misterplex-files"
  if [ -f "$LNX/$h/MD5SUMS" ]; then
    (cd "$LNX/$h" && md5sum -c MD5SUMS >/dev/null) || die "$LNX/$h: MD5SUMS check failed"
  fi
done
# Dest helpers must be present, or restorable from misterplex/linux/ staging.
# A tar install now leaves that staging; an older tar card without it cannot be
# silently reported as repaired.
if [ ! -f "$LNX/misterplex-cacert.pem" ]; then
  die "CA bundle is missing ($LNX/misterplex-cacert.pem) and there is no copy at $MPX/linux/misterplex-cacert.pem to restore"
fi
if [ ! -f "$LNX/airplay/airplay_xcode.py" ]; then
  die "AirPlay helper is missing ($LNX/airplay/airplay_xcode.py) and there is no copy under $MPX/linux/airplay to restore"
fi

# 2b. Clean up what older installs put on the card and the runtime set no longer ships
# (v1.0.0 package install or an older db). Same list as scripts/install-misterplex.sh 2b.
# Fixed paths only. Never touches linux/misterplex.conf, OVERSCAN, backup/, keys, caches or logs.
for d in "$MPX/licenses" "$MPX/docs" "$MPX/scripts"; do
  [ -d "$d" ] || continue
  rm -rf -- "$d" && say "removed $d (not needed on the MiSTer)"
done
for f in LICENSE NOTICE README.md README.txt RELEASE-NOTES.md \
         bin/misterplex_core_watch.sh bin/misterplex_pair_conf.sh bin/misterplex_named_rbf.sh \
         bin/push_frame bin/set_status plexui/LICENSE-Roboto.txt run-gcast-xcode.sh \
         bin/install-misterplex.sh bin/configure-misterplex.sh bin/uninstall-misterplex.sh \
         bin/misterplex_plex.sh bin/misterplex_spelunkr.sh bin/misterplex_restart_manual.sh; do
  [ -f "$MPX/$f" ] || continue
  rm -f -- "$MPX/$f" && say "removed $MPX/$f (not needed on the MiSTer)"
done
# The old separate db Scripts entry; setup now lives in Scripts/configure-misterplex.sh.
[ ! -f "$FAT/Scripts/misterplex_setup.sh" ] || { rm -f -- "$FAT/Scripts/misterplex_setup.sh" && say "removed the old $FAT/Scripts/misterplex_setup.sh"; }
# linux/gcast, once linux/airplay is in place: the files in its .misterplex-files manifest, else the
# files its MD5SUMS lists, and always the five files every older package put there, so nothing is left
# behind. Other files there are the user's and are kept. Same rules as scripts/install-misterplex.sh.
GC="$LNX/gcast"
gc_safe() { case "$1" in ''|/*|.|..|./*|../*|*/./*|*/../*|*/.|*/..) return 1 ;; esac; }
gc_rm() { gc_safe "$1" || return 0; { [ -f "$GC/$1" ] || [ -L "$GC/$1" ]; } || return 0; rm -f -- "$GC/$1"; }
if [ ! -d "$MPX/linux/gcast" ] && [ -d "$GC" ] && [ ! -L "$GC" ] && [ -f "$LNX/airplay/airplay_xcode.py" ]; then
  gc_dirs=""; gc_list=""
  if [ -f "$GC/.misterplex-files" ] && [ ! -L "$GC/.misterplex-files" ]; then
    gc_list=$(sed -n 's/^f //p' "$GC/.misterplex-files")
    gc_dirs=$(awk '$1 == "d" { print length($0) "\t" substr($0, 3) }' "$GC/.misterplex-files" | sort -rn | cut -f2-)
  elif [ -f "$GC/MD5SUMS" ] && [ ! -L "$GC/MD5SUMS" ]; then
    gc_list=$(sed -n 's/^[0-9a-fA-F]\{32\}[ *][ *]*//p' "$GC/MD5SUMS")
  fi
  printf '%s\n' "$gc_list" | while IFS= read -r rel; do gc_rm "$rel"; done
  for f in MD5SUMS ffmpeg ffprobe gcast_xcode.py run-gcast-xcode.sh .misterplex-files; do gc_rm "$f"; done
  printf '%s\n' "$gc_dirs" | while IFS= read -r rel; do gc_safe "$rel" && rmdir -- "$GC/$rel" 2>/dev/null || true; done
  if rmdir -- "$GC" 2>/dev/null; then say "removed the old helper copy $GC"; else say "kept $GC: it holds files MisterPlex did not put there"; fi
fi

# 2c. yt-dlp (YouTube): replace a missing or older card copy with the shipped zip; a
# matching or newer copy stays (update_ytdlp.sh YTDLP_IF_OLDER). The boot script only
# unpacks when none is there, so an old copy used to stay forever. No shipped zip: an
# online update in the background (log /tmp/misterplex-ytdlp.log).
YUPD="$MPX/bin/update_ytdlp.sh"; YZIP="$MPX/bin/yt-dlp_linux_armv7l.zip"; YDEST="$LNX/misterplex/yt-dlp"
if [ -f "$YUPD" ] && [ -f "$YZIP" ]; then
  say "checking yt-dlp against the shipped copy"
  env YTDLP_IF_OLDER=1 YTDLP_LOCAL_ZIP="$YZIP" YTDLP_DEST="$YDEST" MPX_YT_CACHE_DIR="$MPX/yt-cache" sh "$YUPD" \
    || warn "yt-dlp refresh failed; YouTube may not play until the daily online update"
elif [ -f "$YUPD" ] && [ "${MPX_YTDLP_ONLINE:-1}" = 1 ]; then
  say "no shipped yt-dlp; updating it online in the background (log: $ROOT/tmp/misterplex-ytdlp.log)"
  mkdir -p "$ROOT/tmp" 2>/dev/null || true
  ( env YTDLP_DEST="$YDEST" MPX_CA_BUNDLE="$LNX/misterplex-cacert.pem" MPX_YT_CACHE_DIR="$MPX/yt-cache" \
      sh "$YUPD" >> "$ROOT/tmp/misterplex-ytdlp.log" 2>&1 < /dev/null & )
fi

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
say "to change settings, repair or uninstall: Scripts > configure-misterplex"
