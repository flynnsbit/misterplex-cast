#!/bin/sh
# DRAFT, NOT YET RUN ON A MISTER. Proposed installer for the minimal package from
# v1.0.2 on. The v1.0.1 package shipped an earlier script under another name; it
# is in git history at f1da3c24 (md5 5831183b).
# See docs/INSTALLER-PLAN.md for the plan this follows and its open questions.
#
# MiSTerPlex release package installer. Run it ON the MiSTer, as root, from the
# unpacked package folder (for example /tmp/misterplex-<build>/):
#   sh install-misterplex.sh [--dry-run] [--no-boot-line]
#
#   1. Checks every package file against MD5SUMS. Any mismatch: nothing is installed.
#   2. Before each change it saves what it will replace to
#      /media/fat/misterplex/backup-<time>/ and adds the undo step to restore.sh
#      there, so restore.sh is complete at every point, even if a later step fails.
#      The settings file's prior state is saved too. Old PlexOB.rbf cores the boot
#      hook may retire are saved first, on EVERY run: a re-run with nothing else
#      to change still makes a backup folder holding those copies (about 4 MB
#      each) and a restore.sh for them. With no PlexOB.rbf on the card, a run that
#      changes nothing makes no backup folder. Old backup-<time> folders can be
#      deleted once you no longer need their restore.sh.
#   3. Copies the payload: the core to /media/fat/_Utility/MiSTerPlex.rbf, the
#      program and scripts to /media/fat/misterplex/bin/. An old root copy
#      /media/fat/MiSTerPlex.rbf (the boot hook would load it first) is moved
#      into the backup folder when it differs from the new core.
#   4. Creates /media/fat/linux/misterplex.conf from misterplex.conf.example ONLY
#      when the package has that file and no settings file exists. An existing
#      settings file is never opened for writing.
#   5. Adds the one boot line to /media/fat/linux/user-startup.sh through the boot
#      hook's own "install" step (never twice). --no-boot-line skips this.
# If a step fails it stops, names the step, and prints how to run restore.sh.
# It never copies licence texts, READMEs, MD5SUMS or this script to the card.
# exFAT: no modes, owners or symlinks are needed; scripts are run with sh.
set -eu
PROG=install-misterplex.sh
ROOT="${ROOT:-}"                     # test prefix only; empty on a MiSTer
FAT="$ROOT/media/fat"
MPX="$FAT/misterplex"
BIN="$MPX/bin"
CORE="$FAT/_Utility/MiSTerPlex.rbf"
ROOTCORE="$FAT/MiSTerPlex.rbf"
CONF="$FAT/linux/misterplex.conf"
HOOK="$BIN/misterplexd-boot-rc3.sh"
DRY=0
BOOTLINE=1
STEP="start"
FINISHED=0
RS=""
STEPS=""
MPX_NEW=0

say()  { printf '%s\n' "$PROG: $*"; }
die()  { printf '%s\n' "$PROG: error: $*" >&2; exit 1; }
run()  { if [ "$DRY" = 1 ]; then printf '  [dry-run] %s\n' "$*"; else "$@"; fi; }
did()  { if [ "$DRY" = 1 ]; then say "would $1"; else say "$2"; fi; }   # did "<would ...>" "<done text>"

# Host tests only: MPX_TEST_FAIL_AT=<point> forces a failure there. Honored only
# under a fake ROOT (never ROOT=/ or the live /media/fat).
fail_point() {
  [ -n "${MPX_TEST_FAIL_AT:-}" ] && [ "$MPX_TEST_FAIL_AT" = "$1" ] || return 0
  [ -n "$ROOT" ] || return 0
  case "$ROOT" in /) return 0 ;; esac
  fat=$(CDPATH='' cd -- "$FAT" 2>/dev/null && pwd -P || printf '%s' "$FAT")
  [ "$fat" != "/media/fat" ] || return 0
  if [ -e /media/fat ] && [ "$FAT" -ef /media/fat ]; then return 0; fi
  printf '%s\n' "$PROG: test: forced failure at $1" >&2
  exit 1
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1 ;;
    --no-boot-line) BOOTLINE=0 ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

on_exit() {
  rc=$?
  if [ -n "$STEPS" ]; then rm -f "$STEPS" "$STEPS.new" 2>/dev/null || true; fi
  if [ "$FINISHED" = 1 ] && [ "$rc" = 0 ]; then return 0; fi
  [ "$rc" = 0 ] && rc=1
  {
    echo ""
    echo "$PROG: FAILED at step: $STEP (exit $rc). MiSTerPlex is not fully installed."
    if [ "$DRY" = 1 ]; then
      echo "$PROG: this was a dry run; nothing on the card was changed."
    elif [ -n "$RS" ] && [ -f "$RS" ] && [ -s "$RS" ] && grep -q 'Restored' "$RS"; then
      echo "$PROG: to undo everything this run changed, run:"
      echo "    sh $RS"
      echo "$PROG: then reboot the MiSTer."
    elif [ -n "$RS" ] && [ -d "$BK" ]; then
      echo "$PROG: it stopped while creating $BK; nothing else was changed."
      echo "$PROG: you can delete that folder (and $MPX if it is empty)."
    else
      if [ "$MPX_NEW" = 1 ]; then rmdir "$MPX" 2>/dev/null || true; fi
      echo "$PROG: nothing on the card was changed."
    fi
  } >&2
  exit "$rc"
}
trap on_exit EXIT
trap 'exit 130' INT TERM HUP

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"
[ -d "$FAT" ] || die "$FAT not found; run this on the MiSTer"
[ -f MD5SUMS ] || die "MD5SUMS is missing; download and unpack the package again"

# 1. Verify. Every payload file must be listed in MD5SUMS and match.
STEP="check the package"
PAYLOAD="MiSTerPlex.rbf misterplexd owenb_idle_card misterplexd-boot-rc3.sh misterplex_mode.sh"
for f in $PAYLOAD; do
  [ -f "$f" ] || die "package file $f is missing; nothing installed"
  grep -Eq "^[0-9a-fA-F]{32} [ *]$f\$" MD5SUMS || die "MD5SUMS does not list $f; nothing installed"
done
if [ -f misterplex.conf.example ]; then
  grep -Eq '^[0-9a-fA-F]{32} [ *]misterplex.conf.example$' MD5SUMS || die "MD5SUMS does not list misterplex.conf.example; nothing installed"
fi
md5sum -c MD5SUMS >/dev/null 2>&1 || { md5sum -c MD5SUMS || true; die "checksum mismatch; nothing installed"; }
say "package checksums OK"

# Target table: <package file> <path on the card>. Nothing else is copied.
TABLE="MiSTerPlex.rbf $CORE
misterplexd $BIN/misterplexd
misterplexd $BIN/misterplexd-spelunkr
owenb_idle_card $BIN/owenb_idle_card
misterplexd-boot-rc3.sh $BIN/misterplexd-boot-rc3.sh
misterplex_mode.sh $BIN/misterplex_mode.sh"

TS=$(date +%Y%m%d-%H%M%S)
BK="$MPX/backup-$TS"
[ -d "$BK" ] && BK="$BK-$$"
RS="$BK/restore.sh"
STEPS="$BK/.restore-steps"        # undo steps, newest first

# restore.sh is rewritten after every recorded step, so it is always complete.
# Undo runs newest first; each step is best effort and failures are counted.
rs_write() {
  {
    echo '#!/bin/sh'
    echo "# Undo the MiSTerPlex install of $TS. Run: sh $RS   then reboot."
    echo 'set -u'
    echo 'ERR=0'
    echo "export ROOT=\"$ROOT\""
    echo "[ -f \"$HOOK\" ] && sh \"$HOOK\" stop >/dev/null 2>&1 || true"
    if [ -f "$STEPS" ]; then cat "$STEPS"; fi
    echo 'sync'
    echo 'if [ "$ERR" = 0 ]; then echo "Restored. Reboot the MiSTer."; else echo "Restore finished with $ERR error(s); see the messages above." >&2; exit 1; fi'
  } > "$RS.new"
  mv -f "$RS.new" "$RS"
  sync 2>/dev/null || true
}
bk_open() {                      # create the backup folder on first use only
  [ "$DRY" = 1 ] && return 0
  [ -d "$BK" ] && return 0
  if [ ! -d "$MPX" ]; then MPX_NEW=1; fi
  mkdir -p "$BK"
  : > "$STEPS"
  rs_write
}
rs() {                           # rs '<undo command>': prepend, then rewrite restore.sh
  [ "$DRY" = 1 ] && return 0
  bk_open
  { printf '%s || ERR=$((ERR+1))\n' "$*"; cat "$STEPS"; } > "$STEPS.new"
  mv -f "$STEPS.new" "$STEPS"
  rs_write
}
mkdirs() {                       # mkdirs DIR...: create, and record rmdir for dirs this run made
  for md_t in "$@"; do
    # missing dirs, top first, so the deepest one is undone first
    md_list=""; md_d=$md_t
    while [ ! -d "$md_d" ]; do md_list="$md_d
$md_list"; md_d=$(dirname -- "$md_d"); done
    while read -r md_d; do
      if [ -z "$md_d" ]; then continue; fi
      if [ "$DRY" = 1 ]; then printf '  [dry-run] mkdir %s\n' "$md_d"; continue; fi
      if [ "$md_d" = "$MPX" ]; then MPX_NEW=1; fi
      if [ "$md_d" != "$MPX" ]; then          # $MPX holds the backup folder; kept
        rs "{ rmdir \"$md_d\" 2>/dev/null || true; }"
      fi
      mkdir "$md_d"
    done <<MDEOF
$md_list
MDEOF
  done
}


# Old PlexOB.rbf cores: the boot hook retires them (retire_old_plexob) whenever a
# MiSTerPlex.rbf.md5 on the card matches the installed core, during this install
# or on a later boot or mode switch. Save every one first, before any other card
# change (so a failed save leaves the retire unarmed), whatever the boot-line
# branch, and record the undo (put back any that are gone). This runs on every
# run, also a re-run with nothing else to change: each backup folder stays
# self-contained rather than pointing at an earlier one.
STEP="save old PlexOB.rbf cores"
for f in "$FAT/PlexOB.rbf" "$FAT/_Utility/PlexOB.rbf" "$FAT/_Console/PlexOB.rbf" "$FAT/cores/PlexOB.rbf"; do
  if [ -f "$f" ]; then
    k="PlexOB.rbf.$(basename "$(dirname "$f")")"
    if [ "$f" = "$FAT/PlexOB.rbf" ]; then k=PlexOB.rbf.root; fi
    bk_open
    run cp "$f" "$BK/$k"
    if [ "$DRY" != 1 ]; then
      [ "$(md5sum < "$f" | cut -d' ' -f1)" = "$(md5sum < "$BK/$k" | cut -d' ' -f1)" ] \
        || { printf '%s\n' "$PROG: backup copy of $f does not match" >&2; exit 1; }
    fi
    rs "{ [ -f \"$f\" ] || { cp \"$BK/$k\" \"$f.rbk\" && mv -f \"$f.rbk\" \"$f\"; }; }"
    did "save $f" "saved $f"
  fi
done
fail_point after-plexob-save

# 2 + 3. Back up what changes, then copy (write .new, then rename). The undo step
# is recorded before each change.
STEP="create folders"
mkdirs "$BIN" "$FAT/_Utility"
while read -r src dest; do
  STEP="install $dest"
  if [ -f "$dest" ] && cmp -s "$src" "$dest"; then
    say "unchanged: $dest"
    continue
  fi
  key=$(basename "$dest")
  if [ -f "$dest" ]; then
    bk_open
    run cp "$dest" "$BK/$key"
    [ "$DRY" = 1 ] || cmp -s "$dest" "$BK/$key"
    rs "{ rm -f \"$dest.new\"; cp \"$BK/$key\" \"$dest.rbk\" && mv -f \"$dest.rbk\" \"$dest\"; }"
    did "save $dest" "saved $dest"
  else
    rs "rm -f \"$dest\" \"$dest.new\""
  fi
  run cp "$src" "$dest.new"
  [ "$DRY" = 1 ] || cmp -s "$src" "$dest.new"
  run chmod 755 "$dest.new" 2>/dev/null || true
  run mv -f "$dest.new" "$dest"
  did "install $dest" "installed $dest"
  if [ "$dest" = "$CORE" ]; then fail_point after-core; fi
done <<EOF
$TABLE
EOF

# An old root core is loaded before _Utility by the boot hook; move it aside
# (copy into the backup, check it, then remove the root copy).
if [ -f "$ROOTCORE" ] && ! cmp -s MiSTerPlex.rbf "$ROOTCORE"; then
  STEP="move the old root core $ROOTCORE"
  bk_open
  run cp "$ROOTCORE" "$BK/MiSTerPlex.rbf.root"
  [ "$DRY" = 1 ] || cmp -s "$ROOTCORE" "$BK/MiSTerPlex.rbf.root"
  rs "{ [ -f \"$ROOTCORE\" ] || { cp \"$BK/MiSTerPlex.rbf.root\" \"$ROOTCORE.rbk\" && mv -f \"$ROOTCORE.rbk\" \"$ROOTCORE\"; }; }"
  fail_point during-root-core
  run rm -f "$ROOTCORE"
  did "move old root core $ROOTCORE into the backup folder" "moved old root core $ROOTCORE into the backup folder"
fi


# 4. Settings: create only when missing and the package carries an example.
STEP="settings file"
if [ -f "$CONF" ]; then
  say "keeping your settings: $CONF"
elif [ -f misterplex.conf.example ]; then
  # Pre-install state is recorded in the backup (no settings file), so the undo
  # does not depend on the package folder. Written as .new, then renamed.
  mkdirs "$FAT/linux" "$MPX/conf"
  bk_open
  [ "$DRY" = 1 ] || printf 'absent %s\n' "$CONF" > "$BK/misterplex.conf.state"
  if [ -f "$CONF.new" ]; then          # a stray file from an earlier run: keep it
    run cp "$CONF.new" "$BK/misterplex.conf.new.pre"
    [ "$DRY" = 1 ] || cmp -s "$CONF.new" "$BK/misterplex.conf.new.pre"
    rs "{ cp \"$BK/misterplex.conf.new.pre\" \"$CONF.new\"; }"
  fi
  run cp misterplex.conf.example "$BK/misterplex.conf.installed"
  [ "$DRY" = 1 ] || cmp -s misterplex.conf.example "$BK/misterplex.conf.installed"
  # Undo: remove the conf this run created; if it was edited since, move it into
  # the backup folder instead of deleting it.
  rs "{ rm -f \"$CONF.new\"; if [ -f \"$CONF\" ]; then if cmp -s \"$CONF\" \"$BK/misterplex.conf.installed\"; then rm -f \"$CONF\"; else mv -f \"$CONF\" \"$BK/misterplex.conf.after-install\" && echo \"Your edited settings were kept as $BK/misterplex.conf.after-install\"; fi; fi; }"
  run cp misterplex.conf.example "$CONF.new"
  [ "$DRY" = 1 ] || cmp -s misterplex.conf.example "$CONF.new"
  fail_point during-conf
  run mv -f "$CONF.new" "$CONF"
  if [ ! -f "$MPX/conf/misterplex.conf.example" ]; then
    rs "rm -f \"$MPX/conf/misterplex.conf.example\" \"$MPX/conf/misterplex.conf.example.new\""
    run cp misterplex.conf.example "$MPX/conf/misterplex.conf.example.new"
    run mv -f "$MPX/conf/misterplex.conf.example.new" "$MPX/conf/misterplex.conf.example"
  fi
  did "create $CONF from the example" "created $CONF from the example"
else
  say "no settings file yet; MiSTerPlex starts with built-in defaults (see the README)"
fi

# 5. Boot line (the hook backs up user-startup.sh itself and never adds it twice).
# Both startup files are saved first so restore.sh puts them back byte for byte.
if [ "$BOOTLINE" = 1 ]; then
  STEP="add the boot line to $FAT/linux/user-startup.sh"
  if grep -qs '^[^#]*misterplexd-boot-rc3.sh' "$FAT/linux/user-startup.sh"; then
    say "boot line already present"
  else
    STAMP=$(echo "$TS" | tr -d -)
    mkdirs "$FAT/linux"
    for f in "$FAT/linux/user-startup.sh" "$FAT/linux/_user-startup.sh"; do
      k=$(basename "$f")
      if [ -f "$f" ]; then
        bk_open
        run cp "$f" "$BK/$k"
        [ "$DRY" = 1 ] || cmp -s "$f" "$BK/$k"
        rs "{ cp \"$BK/$k\" \"$f.rbk\" && mv -f \"$f.rbk\" \"$f\"; }"
      elif [ "$k" = user-startup.sh ]; then
        rs "rm -f \"$f\""
      fi
      if [ ! -e "$f.bak.$STAMP" ]; then rs "rm -f \"$f.bak.$STAMP\" \"$f.mpx-tmp\""; fi
    done
    STEP="add the boot line to $FAT/linux/user-startup.sh"
    run env ROOT="$ROOT" BOOT_INSTALL_STAMP="$STAMP" sh "$HOOK" install
    fail_point after-retire
    fail_point after-boot-line
    grep -qs '^[^#]*misterplexd-boot-rc3.sh' "$FAT/linux/user-startup.sh" || [ "$DRY" = 1 ] \
      || { printf '%s\n' "$PROG: the boot hook ran but $FAT/linux/user-startup.sh has no boot line" >&2; exit 1; }
    did "add the boot line to $FAT/linux/user-startup.sh" "added the boot line to $FAT/linux/user-startup.sh"
  fi
fi

STEP="finish"
[ "$DRY" = 1 ] || sync
if [ -d "$BK" ]; then
  rm -f "$STEPS"
  say "backup and undo script: $RS"
fi
FINISHED=1
if [ "$DRY" = 1 ]; then
  say "dry run finished; nothing was changed."
else
  say "done. Reboot the MiSTer. You can delete this package folder ($HERE)."
fi
