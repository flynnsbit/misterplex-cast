#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# install-misterplex.sh - one-shot MiSTerPlex installer.
#
# Runs ON the MiSTer from an unpacked release package, or from a PC with
# --remote <mister-ip> (copies the package over scp, then runs itself there
# over ssh). It only writes MiSTerPlex paths:
#   /media/fat/misterplex/                 program, boot scripts, fonts, conf example
#   /media/fat/linux/misterplex.conf       settings (created only if missing)
#   /media/fat/linux/misterplex-cacert.pem, /media/fat/linux/airplay (an old linux/gcast copy is removed)
#   /media/fat/_Utility/MiSTerPlex.rbf     the core, exactly one copy
#   (a /media/fat/MiSTerPlex.rbf root copy or an old PlexOB.rbf from an
#   earlier install is removed once the new core is in place and checked)
#   /media/fat/linux/user-startup.sh       one boot line, via
#                                          misterplexd-boot-rc3.sh install
# Safe to run again: existing files are backed up with a .bak.<time> suffix
# (core backups go to /media/fat/misterplex/backup/, never next to a core)
# and the boot line is never added twice.
# exFAT: /media/fat has no Unix modes, owners or symlinks. Nothing here needs
# them: copies are plain (no cp -a/-p), symlinks are copied as files, chmod is
# best effort, and scripts are run with sh, never relying on the x bit.
set -eu

PROG=install-misterplex.sh
DRY=0
REMOTE=""
RUSER=root
DO_REBOOT=0
PKG=""
ROOT="${ROOT:-}"

usage() {
  cat <<USAGE
Usage:
  On the MiSTer:  sh $PROG [--dry-run] [--reboot] [--package DIR]
  From a PC:      sh $PROG --remote <mister-ip> [--user root] [--dry-run] [--reboot]

Installs MiSTerPlex from an unpacked release package.

  --remote IP     Copy the package to the MiSTer with scp and run the install
                  there with ssh. Needs ssh and scp on this computer.
  --user NAME     SSH user for --remote (default: root).
  --package DIR   Release package folder (default: the folder this script is in,
                  or its parent when run from scripts/).
  --dry-run       Print every step, change nothing.
  --reboot        Reboot the MiSTer when the install finishes.
  -h, --help      Show this help.

Existing files are backed up as <name>.bak.<time>. An existing
misterplex.conf is never replaced, and a FRIENDLY_NAME already in it is kept.
USAGE
}

say()  { printf '%s\n' "$PROG: $*"; }
warn() { printf '%s\n' "$PROG: warning: $*" >&2; }
die()  { printf '%s\n' "$PROG: error: $*" >&2; exit 1; }
run()  {
  if [ "$DRY" = 1 ]; then printf '  [dry-run] %s\n' "$*"; else "$@"; fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --remote) [ $# -ge 2 ] || die "--remote needs an address"; REMOTE=$2; shift 2 ;;
    --user) [ $# -ge 2 ] || die "--user needs a name"; RUSER=$2; shift 2 ;;
    --package) [ $# -ge 2 ] || die "--package needs a folder"; PKG=$2; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --reboot) DO_REBOOT=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done

# N1/N2: a value starting with '-' would turn into an ssh/scp option.
case "$RUSER" in ''|-*|*[!0-9A-Za-z._-]*) die "bad user name: $RUSER" ;; esac
case "$REMOTE" in -*) die "bad address: $REMOTE" ;; esac

# Find the package root (it holds cores/MiSTerPlex.rbf and bin/).
if [ -z "$PKG" ]; then
  here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
  if [ -f "$here/cores/MiSTerPlex.rbf" ]; then PKG=$here
  elif [ -f "$here/../cores/MiSTerPlex.rbf" ]; then PKG=$(CDPATH='' cd -- "$here/.." && pwd)
  else die "cannot find the release package (cores/MiSTerPlex.rbf); use --package DIR"
  fi
fi
[ -d "$PKG/bin" ] || die "$PKG/bin is missing; is this an unpacked release package?"
[ -f "$PKG/cores/MiSTerPlex.rbf" ] || die "$PKG/cores/MiSTerPlex.rbf is missing"
[ -f "$PKG/cores/MiSTerPlex.rbf.md5" ] || die "$PKG/cores/MiSTerPlex.rbf.md5 is missing"
[ -f "$PKG/bin/misterplexd-boot-rc3.sh" ] \
  || die "$PKG/bin/misterplexd-boot-rc3.sh is missing; unpack the release package again"

# N4: check the files that are here (bin/misterplexd included) even when the
# package no longer holds everything SHA256SUMS lists (for example no source/).
verify_present() {
  [ -f "$PKG/SHA256SUMS" ] || die "$PKG/SHA256SUMS is missing; cannot verify the package. Download and unpack the release package again"
  command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found; cannot verify the package"
  # N11: accept text-mode ("  file") and binary-mode (" *file") lines.
  [ -f "$PKG/bin/misterplexd" ] && ! grep -Eq '^[0-9A-Fa-f]{64} [ *]bin/misterplexd$' "$PKG/SHA256SUMS" \
    && die "SHA256SUMS does not list bin/misterplexd"
  say "checking package checksums (SHA256SUMS, files present)"
  (cd "$PKG" && awk '{ f=substr($0, 67); if ((getline line < f) >= 0) print; close(f) }' SHA256SUMS \
    | sha256sum -c - >/dev/null) || die "package checksum check failed; download the package again"
}

verify_package() {
  # N10: a missing SHA256SUMS is a hard error, never a skip.
  [ -f "$PKG/SHA256SUMS" ] || die "$PKG/SHA256SUMS is missing; cannot verify the package. Download and unpack the release package again"
  if command -v sha256sum >/dev/null 2>&1; then
    say "checking package checksums (SHA256SUMS)"
    (cd "$PKG" && sha256sum -c SHA256SUMS >/dev/null) || die "package checksum check failed; download the package again"
  else
    [ -n "$REMOTE" ] || die "sha256sum not found; cannot verify the package"
    warn "sha256sum not found on this computer; the MiSTer will check SHA256SUMS during the install"
  fi
}

# ---------------------------------------------------------------- remote mode
if [ -n "$REMOTE" ]; then
  case "$REMOTE" in *[!0-9A-Za-z.:-]*) die "bad address: $REMOTE" ;; esac
  command -v ssh >/dev/null 2>&1 || die "ssh not found on this computer"
  command -v scp >/dev/null 2>&1 || die "scp not found on this computer"
  verify_package
  T="$RUSER@$REMOTE"
  STAGE=/tmp/misterplex-install
  SELF=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/$PROG
  set --
  for i in bin plexui conf cores linux Scripts SHA256SUMS; do
    [ -e "$PKG/$i" ] && set -- "$@" "$i"
  done
  flags=""
  [ "$DRY" = 1 ] && flags="$flags --dry-run"
  [ "$DO_REBOOT" = 1 ] && flags="$flags --reboot"
  say "copying the package to $T:$STAGE (you may be asked for the MiSTer password)"
  if [ "$DRY" = 1 ]; then
    printf '  [dry-run] ssh -- %s "rm -rf %s && mkdir -p %s"\n' "$T" "$STAGE" "$STAGE"
    printf '  [dry-run] (cd "%s" && scp -r --' "$PKG"; printf ' "%s"' "$@" "$SELF"; printf ' "%s:%s/")\n' "$T" "$STAGE"
    printf '  [dry-run] ssh -- %s "sh %s/%s --package %s%s; rm -rf %s"\n' "$T" "$STAGE" "$PROG" "$STAGE" "$flags" "$STAGE"
    exit 0
  fi
  # shellcheck disable=SC2029 # STAGE is a fixed local path, expanded here on purpose
  ssh -- "$T" "rm -rf $STAGE && mkdir -p $STAGE"
  (cd "$PKG" && scp -r -- "$@" "$SELF" "$T:$STAGE/")
  # shellcheck disable=SC2029 # expand on this side on purpose
  ssh -- "$T" "sh $STAGE/$PROG --package $STAGE$flags; rc=\$?; rm -rf $STAGE; exit \$rc"
  exit $?
fi

# ---------------------------------------------------------------- local mode
FAT="$ROOT/media/fat"
MPX="$FAT/misterplex"
LNX="$FAT/linux"
CONF="$LNX/misterplex.conf"
STAMP=$(date +%Y%m%d-%H%M%S)
[ "$DRY" = 1 ] && say "dry run: nothing will be changed"
[ -d "$FAT" ] || die "$FAT not found; run this on the MiSTer (or use --remote from a PC)"

# Full check when source/ is here; otherwise check every file that is present.
if [ -d "$PKG/source" ]; then verify_package; else verify_present; fi

KEEP_BAK=${MPX_KEEP_BAK:-3}
prune_bak() { # N5: keep only the newest $KEEP_BAK <path>.bak.<stamp> copies
  [ "$DRY" = 0 ] || return 0
  n=0; for old in "$1".bak.[0-9]*; do [ -e "$old" ] && n=$((n + 1)); done
  for old in "$1".bak.[0-9]*; do   # globs sort; the stamp makes oldest first
    [ "$n" -gt "$KEEP_BAK" ] || break
    [ -e "$old" ] && rm -rf -- "$old"; n=$((n - 1))
  done
}
LEDGER="$MPX/.misterplex-created"
created() { # created PATH: record a file this install made (uninstall removes only these)
  [ "$DRY" = 0 ] || return 0
  mkdir -p "$MPX" && printf '%s\n' "${1#"$FAT"/}" >> "$LEDGER"
}
backup() { # backup FILE-OR-DIR: copy to <path>.bak.<stamp> if it exists
  if [ -e "$1" ]; then
    say "backing up $1 -> $1.bak.$STAMP"
    run cp -RL "$1" "$1.bak.$STAMP"   # exFAT: no -a/-p (no modes or owners)
    created "$1.bak.$STAMP"
    prune_bak "$1"
  fi
}

# 1. Stop a running copy.
HOOK="$MPX/bin/misterplexd-boot-rc3.sh"
if [ -f "$HOOK" ]; then
  say "stopping the running MiSTerPlex"
  run env ROOT="$ROOT" sh "$HOOK" stop || warn "stop returned an error; continuing"
fi

# 2. Program, scripts, docs, fonts.
run mkdir -p "$MPX" "$FAT/_Utility" "$LNX"
LEGACY_WARM="$FAT/_Utilities/misterplex"
if [ -d "$LEGACY_WARM" ]; then
  for entry in "$LEGACY_WARM"/yt-cache "$LEGACY_WARM"/yt_warm_helper.py \
               "$LEGACY_WARM"/yt_warm_helper.py.bak "$LEGACY_WARM"/yt_warm_helper.py.bak.* \
               "$LEGACY_WARM"/yt-warm.sock "$LEGACY_WARM"/yt-warm.sock.lock "$LEGACY_WARM"/yt-warm.sock.log; do
    [ -e "$entry" ] || [ -L "$entry" ] || continue
    dst="$MPX/${entry##*/}"
    if [ -e "$dst" ] || [ -L "$dst" ]; then
      say "removing disposable $entry: $dst already exists"
      run rm -rf -- "$entry" || warn "could not remove $entry; left in place"
    else
      say "moving $entry -> $dst"
      run mv "$entry" "$dst" || warn "could not move $entry -> $dst; left in place"
    fi
  done
  run rmdir "$LEGACY_WARM" "$FAT/_Utilities" 2>/dev/null || true
fi
backup "$MPX/bin"
# The runtime package holds only bin, plexui and conf (packaging/runtime-files.txt). Licences, docs,
# READMEs and source are not copied to the card; they are in the source tarball and the GitHub repo.
for d in bin plexui conf; do
  if [ -d "$PKG/$d" ]; then
    say "installing $d -> $MPX/$d"
    run mkdir -p "$MPX/$d"
    run cp -RL "$PKG/$d/." "$MPX/$d/"
  fi
done
# The one hand-run tool: Settings, Restart, Repair, Uninstall (Plex <-> Live TV switching is core-OSD only).
if [ -f "$PKG/Scripts/configure-misterplex.sh" ]; then
  say "installing the Scripts menu entry -> $FAT/Scripts/configure-misterplex.sh"
  run mkdir -p "$FAT/Scripts"
  run cp "$PKG/Scripts/configure-misterplex.sh" "$FAT/Scripts/configure-misterplex.sh"
else
  warn "no Scripts/configure-misterplex.sh in the package; the Scripts menu entry is not installed"
fi
run chmod +x "$MPX/bin/"*.sh 2>/dev/null || true

# 2b. Clean up what older packages put on the card and the runtime package no longer ships.
# Fixed paths only (no globs, nothing outside these lists). Never touches linux/misterplex.conf,
# OVERSCAN or any other setting, the backup/ folder, keys, caches or logs. A path that is already
# gone is a no-op, so re-runs are safe.
for d in "$MPX/licenses" "$MPX/docs" "$MPX/scripts"; do
  case "$d" in "$MPX/licenses"|"$MPX/docs"|"$MPX/scripts") ;; *) die "refusing unexpected cleanup path: $d" ;; esac
  [ -d "$d" ] || continue
  say "removing $d (not needed on the MiSTer)"
  run rm -rf -- "$d" || warn "could not remove $d; left in place"
done
# Same list as mister-db/misterplex_setup.sh (keep the two in step).
for f in LICENSE NOTICE README.md README.txt RELEASE-NOTES.md \
         bin/misterplex_core_watch.sh bin/misterplex_pair_conf.sh bin/misterplex_named_rbf.sh \
         bin/push_frame bin/set_status plexui/LICENSE-Roboto.txt run-gcast-xcode.sh \
         bin/install-misterplex.sh bin/configure-misterplex.sh bin/uninstall-misterplex.sh \
         bin/misterplex_plex.sh bin/misterplex_spelunkr.sh bin/misterplex_restart_manual.sh; do
  [ -f "$MPX/$f" ] || continue
  # A file the runtime package still ships is never removed.
  [ -f "$PKG/$f" ] && continue
  say "removing $MPX/$f (not needed on the MiSTer)"
  run rm -f -- "$MPX/$f" || warn "could not remove $MPX/$f; left in place"
done

# 3. Settings: create once, never replace.
if [ -f "$CONF" ]; then
  say "keeping your settings: $CONF"
  backup "$CONF"
elif [ -f "$PKG/conf/misterplex.conf.example" ]; then
  say "creating $CONF from the default settings"
  run cp "$PKG/conf/misterplex.conf.example" "$CONF"
else
  warn "no conf/misterplex.conf.example in the package; the daemon will use built-in defaults"
fi

# 4. The core, checked before it is copied.
want=$(cut -d" " -f1 "$PKG/cores/MiSTerPlex.rbf.md5" | tr 'A-F' 'a-f')
got=$(md5sum "$PKG/cores/MiSTerPlex.rbf" | cut -d" " -f1)
[ -n "$want" ] && [ "$want" = "$got" ] || die "cores/MiSTerPlex.rbf checksum does not match cores/MiSTerPlex.rbf.md5"
# Pin list: the release core 96eb13d2 (BUILD_ID 0x0B1101C5, RTL 5588134, seed 10) and the
# seed-12 core 3ed684ce (BUILD_ID 0x0B111401, RTL 2b6570a8) and the v1.0.1 core 042f8617
# (BUILD_ID 0x0B111401, RTL 20469332, seed 10) and the seed-14 core 7f048508
# (BUILD_ID 0x0B112501, RTL 544565ba, seed 14); scripts/plexob_pin_identity.sh. A host test under a fake ROOT may set
# MPX_TEST_RBF_PIN. Honor it only when the operator sets both ROOT (a fake
# card) and MPX_TEST_RBF_PIN, the pin is a lowercase 32-digit hex md5, and
# the target FAT is not the live /media/fat (ROOT=/ would install there).
# Ignored when ROOT is unset.
RBF_PINS="96eb13d2cfc966fa56528703d0b542cf 3ed684ceb7d60fbbd0c498380cfaa870 042f86171b562a61694da46860b89d89 7f048508861967f857a5aacff79bfb6f"
rbf_test_pin_ok() {
  [ -n "${ROOT:-}" ] && [ -n "${MPX_TEST_RBF_PIN:-}" ] || return 1
  case "$ROOT" in /|"") return 1 ;; esac
  pin=$(printf '%s' "$MPX_TEST_RBF_PIN" | tr 'A-F' 'a-f')
  case "$pin" in *[!0-9a-f]*) return 1 ;; esac
  [ ${#pin} -eq 32 ] || return 1
  fat=$(CDPATH='' cd -- "$ROOT/media/fat" 2>/dev/null && pwd -P || printf '%s' "$ROOT/media/fat")
  [ "$fat" != "/media/fat" ]
}
if rbf_test_pin_ok; then
  pin=$(printf '%s' "$MPX_TEST_RBF_PIN" | tr 'A-F' 'a-f')
  [ "$MPX_TEST_RBF_PIN" = "$pin" ] || die "MPX_TEST_RBF_PIN must be a lowercase 32-digit hex md5"
  RBF_PINS=$pin
fi
case " $RBF_PINS " in
  *" $got "*) ;;
  *) die "cores/MiSTerPlex.rbf md5 $got is not on the pin list (release 96eb13d2, seed-12 3ed684ce, v1.0.1 042f8617, s14 7f048508)" ;;
esac
# Exactly one copy, in _Utility. The menu lists a root copy as a second core,
# so no root MiSTerPlex.rbf is written and an old one is removed below.
BAKD="$MPX/backup"
core_aside() { # core_aside FILE TAG: move a core out of the menu folders
  run mkdir -p "$BAKD"
  say "moving $1 -> $BAKD/MiSTerPlex.rbf.$2.$STAMP"
  run mv -f "$1" "$BAKD/MiSTerPlex.rbf.$2.$STAMP"
}
dst="$FAT/_Utility/MiSTerPlex.rbf"
if [ -f "$dst" ] && [ "$(md5sum "$dst" | cut -d" " -f1)" = "$want" ]; then
  say "core already current: $dst"
else
  [ -f "$dst" ] && core_aside "$dst" utility
  say "installing the core -> $dst"
  run cp "$PKG/cores/MiSTerPlex.rbf" "$dst"
fi
for b in "$dst".bak.[0-9]* "$FAT/MiSTerPlex.rbf".bak.[0-9]*; do  # older installers left these
  [ -f "$b" ] || continue; run mkdir -p "$BAKD"; run mv -f "$b" "$BAKD/"
done
if [ -f "$FAT/MiSTerPlex.rbf" ]; then
  if [ "$DRY" = 1 ]; then
    say "would remove the root core copy $FAT/MiSTerPlex.rbf"
  elif [ -f "$dst" ] && [ "$(md5sum "$dst" | cut -d" " -f1)" = "$want" ]; then
    if [ "$(md5sum "$FAT/MiSTerPlex.rbf" | cut -d" " -f1)" = "$want" ]; then
      rm -f -- "$FAT/MiSTerPlex.rbf" && say "removed the root core copy $FAT/MiSTerPlex.rbf"
    else
      core_aside "$FAT/MiSTerPlex.rbf" root
    fi
  else
    warn "kept $FAT/MiSTerPlex.rbf: $dst is missing or does not match its md5"
  fi
fi
# The core was renamed from PlexOB.rbf. Remove the old file only at these four
# exact paths, and only after a MiSTerPlex.rbf on the card matches its md5.
# No globs. A path outside this list is never passed to rm. A file that is
# already gone is a no-op, so re-runs are safe. A miss leaves the old file.
case "$FAT" in
  ''|/|//*|*\**|*\?*) die "refusing to clean core files under $FAT" ;;
esac
[ -d "$FAT" ] || die "install directory is not a directory: $FAT"
verified=
for cand in "$FAT/MiSTerPlex.rbf" "$FAT/_Utility/MiSTerPlex.rbf" \
            "$FAT/_Console/MiSTerPlex.rbf" "$FAT/cores/MiSTerPlex.rbf"; do
  if [ -f "$cand" ] && [ "$(md5sum "$cand" | cut -d" " -f1)" = "$want" ]; then
    verified=$cand
    break
  fi
done
for old in "$FAT/PlexOB.rbf" "$FAT/_Utility/PlexOB.rbf" \
           "$FAT/_Console/PlexOB.rbf" "$FAT/cores/PlexOB.rbf"; do
  case "$old" in
    "$FAT/PlexOB.rbf"|"$FAT/_Utility/PlexOB.rbf"|"$FAT/_Console/PlexOB.rbf"|"$FAT/cores/PlexOB.rbf") ;;
    *) die "refusing unexpected cleanup path: $old" ;;
  esac
  [ -f "$old" ] || continue
  if [ "$DRY" = 1 ]; then
    say "would remove the old core $old (renamed to MiSTerPlex.rbf)"; continue
  fi
  if [ -n "$verified" ]; then
    rm -f -- "$old" && say "removed the old core $old (renamed to MiSTerPlex.rbf)"
  else
    warn "kept $old: MiSTerPlex.rbf is missing or does not match its md5"
  fi
done

# The boot hook reads this md5 slot before it retires an old PlexOB.rbf (misterplexd-boot-rc3.sh:137).
run mkdir -p "$MPX/cores"
run cp "$PKG/cores/MiSTerPlex.rbf.md5" "$MPX/cores/MiSTerPlex.rbf.md5"

# 5. Casting helpers and the CA bundle.
if [ -f "$PKG/linux/misterplex/yt_warm_helper.py" ]; then
  # Like the core and CA bundle, uninstall removes this explicitly, not via the backup ledger.
  # A pre-existing directory at the helper path makes cp fail with "cannot overwrite directory".
  if [ -d "$MPX/yt_warm_helper.py" ]; then
    say "removing unexpected directory $MPX/yt_warm_helper.py so the helper can be installed"
    run rm -rf -- "$MPX/yt_warm_helper.py" || warn "could not remove $MPX/yt_warm_helper.py; left in place"
  fi
  if [ "$DRY" != 1 ] && [ -d "$MPX/yt_warm_helper.py" ]; then
    warn "skipped helper install: $MPX/yt_warm_helper.py is a directory"
  else
    run cp "$PKG/linux/misterplex/yt_warm_helper.py" "$MPX/"
  fi
fi
if [ -f "$PKG/linux/misterplex-cacert.pem" ]; then
  backup "$LNX/misterplex-cacert.pem"
  # Also leave a copy under misterplex/linux/ so Repair can restore linux/
  # destinations (same staging layout the update_all db uses).
  run mkdir -p "$MPX/linux"
  run cp "$PKG/linux/misterplex-cacert.pem" "$LNX/"
  run cp "$PKG/linux/misterplex-cacert.pem" "$MPX/linux/"
fi
for h in airplay gcast; do   # install both airplay and gcast helper trees
  if [ -d "$PKG/linux/$h" ]; then
    say "installing the $h helper -> $LNX/$h"
    run mkdir -p "$LNX/$h" "$MPX/linux/$h"
    run cp -RL "$PKG/linux/$h/." "$LNX/$h/"
    run cp -RL "$PKG/linux/$h/." "$MPX/linux/$h/"
    # Manifest of what this install put here, so uninstall removes only these
    # files (never the whole folder, which may hold the user's own files).
    if [ "$DRY" = 0 ]; then
      (cd "$PKG/linux/$h" && { find . -type d ! -name . | sed 's|^\./|d |'
        find . \( -type f -o -type l \) | sed 's|^\./|f |'; }) > "$LNX/$h/.misterplex-files"
    else
      printf '  [dry-run] write %s\n' "$LNX/$h/.misterplex-files"
    fi
    if [ "$DRY" = 0 ] && [ -f "$LNX/$h/MD5SUMS" ]; then
      (cd "$LNX/$h" && md5sum -c MD5SUMS >/dev/null) || die "$LNX/$h: MD5SUMS check failed"
    fi
  fi
done
# The old separate update_all Scripts entry; its setup is part of Scripts/configure-misterplex.sh now.
# Same as mister-db/misterplex_setup.sh. Removed even when this upgrade does not run first-run setup.
if [ -f "$FAT/Scripts/misterplex_setup.sh" ] || [ -L "$FAT/Scripts/misterplex_setup.sh" ]; then
  say "removing the old $FAT/Scripts/misterplex_setup.sh (Scripts > configure-misterplex replaces it)"
  run rm -f -- "$FAT/Scripts/misterplex_setup.sh" || warn "could not remove $FAT/Scripts/misterplex_setup.sh"
fi
# The boot hook reads linux/airplay first and linux/gcast only as a fallback. When this package has
# no gcast copy and the airplay helper is in place, remove the old copy in linux/gcast: the files in
# its .misterplex-files manifest, else the files its MD5SUMS lists, and always the five files every
# older package put there (MD5SUMS, ffmpeg, ffprobe, gcast_xcode.py, run-gcast-xcode.sh), so an upgrade
# leaves none of them behind. Other files there are the user's and are kept. Same rules as
# mister-db/misterplex_setup.sh (keep the two in step).
GC="$LNX/gcast"
gc_safe() { case "$1" in ''|/*|.|..|./*|../*|*/./*|*/../*|*/.|*/..) return 1 ;; esac; }
gc_rm() { # gc_rm REL: remove one old gcast file (unlink a link, never follow it)
  gc_safe "$1" || return 0
  { [ -f "$GC/$1" ] || [ -L "$GC/$1" ]; } || return 0
  run rm -f -- "$GC/$1" || warn "could not remove $GC/$1"
}
if [ ! -d "$PKG/linux/gcast" ] && [ -d "$GC" ] && [ ! -L "$GC" ] && [ -f "$LNX/airplay/airplay_xcode.py" ]; then
  gc_dirs=""
  if [ -f "$GC/.misterplex-files" ] && [ ! -L "$GC/.misterplex-files" ]; then
    gc_list=$(sed -n 's/^f //p' "$GC/.misterplex-files")
    gc_dirs=$(awk '$1 == "d" { print length($0) "\t" substr($0, 3) }' "$GC/.misterplex-files" | sort -rn | cut -f2-)
  elif [ -f "$GC/MD5SUMS" ] && [ ! -L "$GC/MD5SUMS" ]; then
    gc_list=$(sed -n 's/^[0-9a-fA-F]\{32\}[ *][ *]*//p' "$GC/MD5SUMS")
  else
    gc_list=""
  fi
  gc_any=0
  for f in .misterplex-files MD5SUMS ffmpeg ffprobe gcast_xcode.py run-gcast-xcode.sh; do
    { [ -f "$GC/$f" ] || [ -L "$GC/$f" ]; } && gc_any=1
  done
  [ -n "$gc_list" ] && gc_any=1
  if [ "$gc_any" = 1 ]; then
    say "removing the old helper copy $GC (linux/airplay replaces it)"
    printf '%s\n' "$gc_list" | while IFS= read -r rel; do gc_rm "$rel"; done
    for f in MD5SUMS ffmpeg ffprobe gcast_xcode.py run-gcast-xcode.sh .misterplex-files; do gc_rm "$f"; done
    if [ "$DRY" = 0 ]; then
      printf '%s\n' "$gc_dirs" | while IFS= read -r rel; do gc_safe "$rel" && rmdir -- "$GC/$rel" 2>/dev/null || true; done
      rmdir -- "$GC" 2>/dev/null || say "kept $GC: it still holds files this installer did not put there"
    fi
  fi
fi

# 6. Player name: misterplex-<last 3 octets>, only when the conf has none.
conf_get() { # conf_get KEY FILE: last uncommented KEY= value
  [ -f "$2" ] || return 0
  awk -F= -v k="$1" '/^[ \t]*#/ {next} { x=$1; gsub(/^[ \t]+|[ \t]+$/, "", x)
    if (x == k) { v=substr($0, index($0, "=")+1); gsub(/\r/, "", v); gsub(/^[ \t"]+|[ \t"]+$/, "", v); last=v } }
    END { if (last != "") print last }' "$2"
}
lan_ipv4() {
  if [ -n "${MPX_TEST_IP:-}" ]; then printf '%s\n' "$MPX_TEST_IP"; return 0; fi
  ip -4 -o addr show 2>/dev/null | awk '{ split($4, a, "/"); ip=a[1]
    if (ip !~ /^127\./ && ip !~ /^169\.254\./) { print ip; exit } }'
}
name=$(conf_get FRIENDLY_NAME "$CONF")
pid=$(conf_get PLAYER_ID "$CONF")
if [ -n "$name" ] || [ -n "$pid" ]; then
  say "keeping the player name already set (${name:-$pid})"
else
  ipaddr=$(lan_ipv4 || true)
  case "$ipaddr" in
    *.*.*.*)
      new="misterplex-${ipaddr#*.}"
      say "setting the player name to $new"
      if [ "$DRY" = 1 ]; then
        printf '  [dry-run] append FRIENDLY_NAME=%s and PLAYER_ID=%s to %s\n' "$new" "$new" "$CONF"
      else
        printf '\n# Player name set by %s\nFRIENDLY_NAME=%s\nPLAYER_ID=%s\n' "$PROG" "$new" "$new" >> "$CONF"
      fi ;;
    *) warn "no network address yet; the player name will be set on first boot" ;;
  esac
fi

# 6b. yt-dlp (YouTube): replace a missing or older card copy with the shipped zip; a
# matching or newer copy stays (update_ytdlp.sh YTDLP_IF_OLDER). The boot script only
# unpacks when none is there, so an old copy used to stay forever. No shipped zip: an
# online update in the background (log /tmp/misterplex-ytdlp.log).
YUPD="$MPX/bin/update_ytdlp.sh"; YZIP="$MPX/bin/yt-dlp_linux_armv7l.zip"; YDEST="$LNX/misterplex/yt-dlp"
if [ -f "$YUPD" ] && [ -f "$YZIP" ]; then
  say "checking yt-dlp against the shipped copy"
  run env YTDLP_IF_OLDER=1 YTDLP_LOCAL_ZIP="$YZIP" YTDLP_DEST="$YDEST" MPX_YT_CACHE_DIR="$MPX/yt-cache" sh "$YUPD" \
    || warn "yt-dlp refresh failed; YouTube may not play until the daily online update"
elif [ -f "$YUPD" ] && [ "${MPX_YTDLP_ONLINE:-1}" = 1 ] && [ "$DRY" = 0 ]; then
  say "no shipped yt-dlp; updating it online in the background (log: $ROOT/tmp/misterplex-ytdlp.log)"
  mkdir -p "$ROOT/tmp" 2>/dev/null || true
  ( env YTDLP_DEST="$YDEST" MPX_CA_BUNDLE="$LNX/misterplex-cacert.pem" MPX_YT_CACHE_DIR="$MPX/yt-cache" \
      sh "$YUPD" >> "$ROOT/tmp/misterplex-ytdlp.log" 2>&1 < /dev/null & )
fi

# 7. Boot hook (adds one line to user-startup.sh; never twice).
say "enabling start at boot"
if [ "$DRY" = 0 ] && [ ! -f "$HOOK" ]; then   # run with sh: no x bit needed (exFAT)
  die "$HOOK is missing; nothing was enabled (re-run the installer)"
fi
us_baks() { ls -d "$LNX"/user-startup.sh.bak.* "$LNX"/_user-startup.sh.bak.* 2>/dev/null || true; }
pre_baks=$(us_baks)
run env ROOT="$ROOT" sh "$HOOK" install
for b in $(us_baks); do   # the hook's user-startup .bak copies from this run
  case "
$pre_baks
" in *"
$b
"*) ;; *) created "$b" ;; esac
done

if [ "$DO_REBOOT" = 1 ]; then
  say "done. Settings: $CONF"
  say "rebooting"
  run reboot
elif [ "$DRY" = 0 ] && [ -z "$ROOT" ] && [ "${MPX_INSTALL_NO_START:-0}" != 1 ]; then
  say "starting MiSTerPlex companion"
  if sh "$HOOK" start; then
    say "done. MiSTerPlex companion is running; load MiSTerPlex from _Utility (no reboot needed). Settings: $CONF"
  else
    warn "could not start the companion immediately; reboot the MiSTer or run: sh $HOOK start"
    say "done. Load MiSTerPlex from _Utility after rebooting. Settings: $CONF"
  fi
else
  say "done. Load MiSTerPlex from _Utility after the reboot. Settings: $CONF"
  say "reboot the MiSTer: the companion starts at boot; then load MiSTerPlex from _Utility"
fi
say "then link it: note the code on the Ready screen (plex.tv/link ABCD), go to https://plex.tv/link, sign in and enter it."
say "until it is linked, Browse Plex on the TV shows FETCH FAILED."
say "to change settings, repair or uninstall: Scripts > configure-misterplex (or sh $FAT/Scripts/configure-misterplex.sh)"
