#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# install-misterplex.sh - one-shot MiSTerPlex installer.
#
# Runs ON the MiSTer from an unpacked release package, or from a PC with
# --remote <mister-ip> (copies the package over scp, then runs itself there
# over ssh). It only writes MiSTerPlex paths:
#   /media/fat/misterplex/                 program, scripts, docs, fonts
#   /media/fat/linux/misterplex.conf       settings (created only if missing)
#   /media/fat/linux/misterplex-cacert.pem, /media/fat/linux/airplay, gcast
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
  for i in bin scripts docs licenses plexui conf cores linux LICENSE NOTICE README.md SHA256SUMS; do
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
  for entry in "$LEGACY_WARM"/* "$LEGACY_WARM"/.[!.]* "$LEGACY_WARM"/..?*; do
    [ -e "$entry" ] || [ -L "$entry" ] || continue
    dst="$MPX/${entry##*/}"
    if [ -e "$dst" ] || [ -L "$dst" ]; then
      warn "kept $entry: $dst already exists"
    else
      say "moving $entry -> $dst"
      run mv "$entry" "$dst"
    fi
  done
  run rmdir "$LEGACY_WARM" "$FAT/_Utilities" 2>/dev/null || true
fi
backup "$MPX/bin"
for d in bin scripts docs licenses plexui conf; do
  if [ -d "$PKG/$d" ]; then
    say "installing $d -> $MPX/$d"
    run mkdir -p "$MPX/$d"
    run cp -RL "$PKG/$d/." "$MPX/$d/"
  fi
done
for f in install-misterplex.sh configure-misterplex.sh misterplex_mode.sh misterplex_restart_manual.sh misterplex_plex.sh misterplex_spelunkr.sh; do
  for src in "$PKG/$f" "$PKG/scripts/$f"; do
    if [ -f "$src" ]; then run cp "$src" "$MPX/bin/$f"; break; fi
  done
done
run chmod +x "$MPX/bin/"*.sh 2>/dev/null || true

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
# Pin list: the release core 96eb13d2 (BUILD_ID 0x0B1101C5, RTL 5588134, seed 10;
# scripts/plexob_pin_identity.sh). A host test under a fake ROOT may set
# MPX_TEST_RBF_PIN. Honor it only when the operator sets both ROOT (a fake
# card) and MPX_TEST_RBF_PIN, the pin is a lowercase 32-digit hex md5, and
# the target FAT is not the live /media/fat (ROOT=/ would install there).
# Ignored when ROOT is unset.
RBF_PINS="96eb13d2cfc966fa56528703d0b542cf"
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
  *) die "cores/MiSTerPlex.rbf md5 $got is not on the pin list (release 96eb13d2)" ;;
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

# 5. Casting helpers and the CA bundle.
if [ -f "$PKG/linux/misterplex/yt_warm_helper.py" ]; then
  # Like the core and CA bundle, uninstall removes this explicitly, not via the backup ledger.
  run cp "$PKG/linux/misterplex/yt_warm_helper.py" "$MPX/"
fi
if [ -f "$PKG/linux/misterplex-cacert.pem" ]; then
  backup "$LNX/misterplex-cacert.pem"
  run cp "$PKG/linux/misterplex-cacert.pem" "$LNX/"
fi
for h in airplay gcast; do
  if [ -d "$PKG/linux/$h" ]; then
    say "installing the $h helper -> $LNX/$h"
    run mkdir -p "$LNX/$h"
    run cp -RL "$PKG/linux/$h/." "$LNX/$h/"
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

say "done. Load MiSTerPlex from _Utility after the reboot. Settings: $CONF"
say "then link it: note the code on the Ready screen (plex.tv/link ABCD), go to https://plex.tv/link, sign in and enter it."
say "until it is linked, Browse Plex on the TV shows FETCH FAILED."
say "to change settings: sh $MPX/bin/configure-misterplex.sh"
if [ "$DO_REBOOT" = 1 ]; then
  say "rebooting"
  run reboot
else
  say "reboot the MiSTer to start MiSTerPlex"
fi
