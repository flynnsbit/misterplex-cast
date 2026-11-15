#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# uninstall-misterplex.sh - undo install-misterplex.sh on a MiSTer.
#
# Runs ON the MiSTer (or against a fake tree with ROOT=<dir> for tests).
#   1. Runs misterplexd-boot-rc3.sh uninstall: stops misterplexd and removes
#      its line from /media/fat/linux/user-startup.sh.
#   2. Deletes what the installer created: the core (_Utility and any root
#      copy), linux/misterplex-cacert.pem, the program files in
#      /media/fat/misterplex/ (bin scripts docs licenses plexui conf, warm helper
#      and its cache/endpoint files) and the
#      .bak.<time> copies listed in the install ledger misterplex/.misterplex-created.
#      In linux/airplay and linux/gcast only the files listed in the install
#      manifest (.misterplex-files, or MD5SUMS from older installs) are removed;
#      other files there, and the folders holding them, are kept.
#   3. Keeps your settings and all other user/runtime data under misterplex/
#      (airplay_key.pem, yt_screen_id, reports/, ca/, ...) unless --purge:
#      /media/fat/linux/misterplex.conf, /media/fat/misterplex/misterplex.conf
#      (fallback conf), /media/fat/misterplex/identity and
#      /media/fat/misterplex/backup/ (cores the installer moved aside).
#      Lines other tools wrote to user-startup.sh are untouched.
# exFAT safe: no chmod, chown or symlinks; the boot script is run with sh.
set -eu

PROG=uninstall-misterplex.sh
DRY=0
KEEP_CORE=0
PURGE=0
ROOT="${ROOT:-}"

usage() {
  cat <<USAGE
Usage: sh $PROG [--dry-run] [--keep-core] [--purge]

Turns off MiSTerPlex at boot and removes everything the installer created.

  --dry-run         Print every step, change nothing.
  --keep-core       Leave MiSTerPlex.rbf in place.
  --purge           Also remove the settings and all of misterplex/ (identity,
                    backup/, keys, reports) and the user-startup.sh .bak copies
                    the installer made.
  --remove-program  Accepted for older notes; the program is always removed.
  -h, --help        Show this help.

Settings, identity and moved-aside cores are kept unless --purge.
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
    --dry-run) DRY=1; shift ;;
    --keep-core) KEEP_CORE=1; shift ;;
    --purge) PURGE=1; shift ;;
    --remove-program) shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done

FAT="$ROOT/media/fat"
[ -d "$FAT" ] || die "$FAT not found (is this a MiSTer?)"
case "$FAT" in *\**|*\?*) die "refusing to clean under $FAT" ;; esac
LNX="$FAT/linux"
MPX="$FAT/misterplex"
HOOK="$MPX/bin/misterplexd-boot-rc3.sh"
gone() { # gone PATH...: delete what install-misterplex.sh created (dirs too)
  for x in "$@"; do
    [ -e "$x" ] || [ -L "$x" ] || continue
    say "removing $x"
    run rm -rf -- "$x"
  done
}

# 1. Stop the daemon and remove the boot line (run with sh: no x bit on exFAT).
if [ -f "$HOOK" ]; then
  say "stopping MiSTerPlex and removing its boot line"
  run env ROOT="$ROOT" sh "$HOOK" uninstall || warn "boot script uninstall returned an error; continuing"
else
  warn "$HOOK not found; the MiSTerPlex line in $LNX/user-startup.sh stays (remove it by hand)"
fi

# 2. The core (both places older installers used) and the backups next to it.
if [ "$KEEP_CORE" = 0 ]; then
  gone "$FAT/MiSTerPlex.rbf" "$FAT/_Utility/MiSTerPlex.rbf"   # .bak copies only via the ledger
fi

# 3. The CA bundle, and in airplay/gcast only the files install put there.
gone "$LNX/misterplex-cacert.pem"
# P2: the Spelunkr header (moov) cache -- pure performance cache, never user data, always
# removed (not gated by --purge).
gone "$LNX/misterplex-cache"
helper_files() { # helper_files DIR: remove only installer-listed files in DIR
  d=$1; m="$d/.misterplex-files"
  [ -d "$d" ] || return 0
  if [ -L "$d" ]; then warn "$d is a symlink; left as is"; return 0; fi
  base=$(readlink -f -- "$d") || { warn "cannot resolve $d; left as is"; return 0; }
  if [ -f "$m" ] && [ ! -L "$m" ]; then
    list=$(sed -n 's/^f //p' "$m"); dirs=$(sed -n 's/^d //p' "$m" | sort -r)
  elif [ -f "$d/MD5SUMS" ] && [ ! -L "$d/MD5SUMS" ]; then
    list=$(sed -n 's/^[0-9a-fA-F]\{32\}[ *][ *]*//p' "$d/MD5SUMS"); dirs=""
  else
    warn "$d has no install manifest; left as is"; return 0
  fi
  printf '%s\n' "$list" | while IFS= read -r f; do
    safe_entry "$d" "$base" "$f" || continue
    p="$d/$f"
    if [ -L "$p" ]; then say "removing link $p"; run rm -f -- "$p"   # unlink only, never follow
    elif [ -f "$p" ]; then say "removing $p"; run rm -f -- "$p"
    elif [ -e "$p" ]; then warn "skipped $p: not a file"
    fi
  done
  run rm -f -- "$m" "$d/MD5SUMS"
  printf '%s\n' "$dirs" | while IFS= read -r f; do
    safe_entry "$d" "$base" "$f" || continue
    [ -L "$d/$f" ] && { warn "skipped $d/$f: symlink, not a folder"; continue; }
    [ "$DRY" = 1 ] || rmdir -- "$d/$f" 2>/dev/null || true
  done
  if [ "$DRY" = 0 ] && rmdir -- "$d" 2>/dev/null; then say "removed empty $d"
  elif [ -d "$d" ]; then say "kept $d (holds files the installer did not create)"; fi
}
safe_entry() { # safe_entry DIR REALDIR ENTRY: 0 if DIR/ENTRY stays inside DIR
  [ -n "$3" ] || return 1
  case "$3" in /*) warn "skipped manifest entry '$3' in $1: absolute path"; return 1 ;; esac
  case "/$3/" in
    */../*|*/./*) warn "skipped manifest entry '$3' in $1: '..' or '.' component"; return 1 ;;
  esac
  par=$(dirname -- "$1/$3")
  [ -e "$1/$3" ] || [ -L "$1/$3" ] || return 1
  rp=$(readlink -f -- "$par" 2>/dev/null) || { warn "skipped $1/$3: cannot resolve its folder"; return 1; }
  case "$rp/" in
    "$2"/*) return 0 ;;
    *) warn "skipped $1/$3: its folder resolves outside $1 ($rp)"; return 1 ;;
  esac
}
helper_files "$LNX/airplay"; helper_files "$LNX/gcast"

# Shipped worker plus the warm-helper runtime dir (cache, abstract marker, lock, log).
LEGACY_WARM="$FAT/_Utilities/misterplex"
for warm in "$MPX" "$LEGACY_WARM"; do
  gone "$warm/yt_warm_helper.py" "$warm/yt-warm.sock" \
       "$warm/yt-warm.sock.lock" "$warm/yt-warm.sock.log" "$warm/yt-cache"
done
if [ "$DRY" = 0 ]; then
  rmdir "$LEGACY_WARM" "$FAT/_Utilities" 2>/dev/null || true
fi

# 4. Backups this install made, from its ledger (misterplex/.misterplex-created). Nothing
#    else named .bak is touched. user-startup .bak copies go only with --purge.
# #353: suffix after .bak. is only YYYYMMDD-HHMMSS or 1-3 digits (N<=999).
ledger_bak_ok() {
  case "$1" in
    misterplex/bin.bak.*|linux/misterplex.conf.bak.*|linux/misterplex-cacert.pem.bak.*|\
    linux/user-startup.sh.bak.*|linux/_user-startup.sh.bak.*) ;;
    *) return 1 ;;
  esac
  case "$1" in */*/*|*/../*|../*|/*) return 1 ;; esac
  suf=${1##*.bak.}
  case "$suf" in
    [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9]) return 0 ;;
    [0-9]|[0-9][0-9]|[0-9][0-9][0-9]) return 0 ;;
    *) return 1 ;;
  esac
}
L="$MPX/.misterplex-created"
if [ -f "$L" ] && [ ! -L "$L" ] && fatreal=$(readlink -f -- "$FAT"); then
  [ "$DRY" = 1 ] || rm -f -- "$L.keep"
  while IFS= read -r rel; do
    # #349/#353: only installer .bak copies (backup() targets + hook's user-startup), bounded suffix.
    if ! ledger_bak_ok "$rel"; then
      say "ledger entry outside the package prefixes, skipped: $rel"
      continue
    fi
    case "$rel" in
      linux/user-startup.sh.bak.*|linux/_user-startup.sh.bak.*)
        if [ "$PURGE" = 0 ]; then   # kept; stays in the ledger so a later --purge can remove it
          [ -e "$FAT/$rel" ] && printf '%s\n' "$rel" >> "$L.keep"
          continue
        fi ;;
    esac
    safe_entry "$FAT" "$fatreal" "$rel" || continue
    gone "$FAT/$rel"
  done < "$L"
else
  warn "no install ledger ($L); no .bak copies removed"
fi

# 5. The program folder. Plain uninstall removes only the folders install copies in;
#    everything else under misterplex/ (conf, identity, backup/, keys, ca/, reports/,
#    yt_screen_id, ...) is user or runtime data and stays. --purge removes the folder.
if [ "$PURGE" = 1 ]; then
  gone "$MPX"
elif [ -d "$MPX" ]; then
  for d in bin scripts docs licenses plexui conf; do gone "$MPX/$d"; done
  if [ -s "$L.keep" ] && [ "$DRY" = 0 ]; then mv -f -- "$L.keep" "$L"   # only the kept user-startup .bak entries
  else gone "$L"; rm -f -- "$L.keep"; fi
  for x in "$MPX"/* "$MPX"/.[!.]*; do
    [ -e "$x" ] || [ -L "$x" ] || continue
    say "kept $x (--purge removes it)"
  done
  [ "$DRY" = 1 ] || rmdir -- "$MPX" 2>/dev/null || true
fi

# 6. Settings: kept unless --purge; the installer's .bak copies of them go.
if [ "$PURGE" = 1 ]; then
  gone "$LNX/misterplex.conf"
elif [ -f "$LNX/misterplex.conf" ]; then
  say "kept your settings: $LNX/misterplex.conf (--purge removes it)"
fi
say "done. Reboot the MiSTer to finish. To reinstall, run install-misterplex.sh again."
