#!/bin/sh
# Install the seed-10 PlexOB Y/C RBF onto the two paths prod load_core
# searches. The only accepted md5 is cores/PlexOB.rbf.md5 in this repo
# (c8b346048d680cc410a447490f57a820, BUILD_ID 0x0B59C11A). Argument 3,
# PLEXOB_RBF_MD5, or a destination slot file must equal that pin, and the
# source file must hash to it. Any other pin or file is refused before a
# copy, including a void seed-5 bitstream. A non-md5 pin also refuses.
#   DEST_ROOT/PlexOB.rbf
#   DEST_ROOT/_Utility/PlexOB.rbf
#
# Each copy is written to a temp file in the destination directory, fsynced,
# md5-checked against the pin, then renamed. A short copy (ENOSPC, killed
# mid-write) removes the temp file and leaves any existing PlexOB.rbf in place.
# The installed file is md5-checked again after the rename.
set -eu
SRC=${1:-${PLEXOB_RBF:-}}
DEST_ROOT=${2:-}
PIN_ARG=${3:-}
if [ -z "$SRC" ] || [ -z "$DEST_ROOT" ]; then
  echo "usage: stage_plexob_yc_rbf.sh PlexOB.rbf DEST_ROOT [PIN_MD5]" >&2
  exit 2
fi
if [ ! -f "$SRC" ]; then
  echo "stage_plexob_yc_rbf: missing $SRC" >&2
  exit 1
fi
if [ -e "$DEST_ROOT" ] && [ ! -d "$DEST_ROOT" ]; then
  echo "stage_plexob_yc_rbf: DEST_ROOT is not a directory: $DEST_ROOT" >&2
  exit 1
fi
command -v md5sum >/dev/null 2>&1 || {
  echo "stage_plexob_yc_rbf: md5sum is required" >&2
  exit 1
}

fsync_path() {
  target=$1
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import os,sys
p=sys.argv[1]
flags=os.O_RDONLY
if os.path.isdir(p):
    flags |= getattr(os, "O_DIRECTORY", 0)
fd=os.open(p, flags)
os.fsync(fd)
os.close(fd)' "$target"
    return $?
  fi
  if [ -d "$target" ]; then
    sync
    return 0
  fi
  dd if="$target" of="$target" conv=notrunc,fsync status=none 2>/dev/null || sync
}

# Refuse names that are not regular files before any byte is written, so a
# directory squatting on PlexOB.rbf cannot swallow the copy.
for dest in "$DEST_ROOT/PlexOB.rbf" "$DEST_ROOT/_Utility/PlexOB.rbf"; do
  if [ -e "$dest" ] && [ ! -f "$dest" ]; then
    echo "stage_plexob_yc_rbf: $dest exists and is not a regular file" >&2
    exit 1
  fi
done
if [ -e "$DEST_ROOT/_Utility" ] && [ ! -d "$DEST_ROOT/_Utility" ]; then
  echo "stage_plexob_yc_rbf: $DEST_ROOT/_Utility is not a directory" >&2
  exit 1
fi

PIN=$PIN_ARG
if [ -z "$PIN" ]; then
  PIN=${PLEXOB_RBF_MD5:-}
fi
if [ -z "$PIN" ] && [ -f "$DEST_ROOT/cores/PlexOB.rbf.md5" ]; then
  PIN=$(tr -d '[:space:]' < "$DEST_ROOT/cores/PlexOB.rbf.md5")
fi
if [ -z "$PIN" ]; then
  here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
  if [ -f "$here/../cores/PlexOB.rbf.md5" ]; then
    PIN=$(tr -d '[:space:]' < "$here/../cores/PlexOB.rbf.md5")
  fi
fi
if ! printf '%s' "$PIN" | grep -Eq '^[0-9a-fA-F]{32}$'; then
  echo "stage_plexob_yc_rbf: pinned md5 is missing or not 32 hex (argument, PLEXOB_RBF_MD5, or cores/PlexOB.rbf.md5). A non-md5 pin does not install a bitstream." >&2
  exit 1
fi
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo_pin=""
if [ -f "$here/../cores/PlexOB.rbf.md5" ]; then
  repo_pin=$(tr -d '[:space:]' < "$here/../cores/PlexOB.rbf.md5" | tr '[:upper:]' '[:lower:]')
fi
if ! printf '%s' "$repo_pin" | grep -Eq '^[0-9a-f]{32}$'; then
  echo "stage_plexob_yc_rbf: cores/PlexOB.rbf.md5 is not a 32-hex pin" >&2
  exit 1
fi
pin_lc=$(printf '%s' "$PIN" | tr '[:upper:]' '[:lower:]')
if [ "$pin_lc" != "$repo_pin" ]; then
  echo "stage_plexob_yc_rbf: pin is not the seed-10 PlexOB md5 in cores/PlexOB.rbf.md5" >&2
  exit 1
fi
EXPECT=$(md5sum "$SRC" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')
if [ -z "$EXPECT" ]; then
  echo "stage_plexob_yc_rbf: could not hash $SRC" >&2
  exit 1
fi
if [ "$EXPECT" != "$repo_pin" ]; then
  echo "stage_plexob_yc_rbf: $SRC md5 is not the seed-10 pin" >&2
  exit 1
fi

install_one() {
  dest=$1
  dir=$(dirname "$dest")
  base=$(basename "$dest")
  tmp="$dir/.$base.mpx-tmp"
  mkdir -p "$dir"
  rm -f "$tmp"
  if ! cp -- "$SRC" "$tmp"; then
    rm -f "$tmp"
    echo "stage_plexob_yc_rbf: copy to $tmp failed" >&2
    return 1
  fi
  if ! fsync_path "$tmp"; then
    rm -f "$tmp"
    echo "stage_plexob_yc_rbf: fsync $tmp failed" >&2
    return 1
  fi
  staged=$(md5sum "$tmp" | awk '{print $1}')
  if [ "$staged" != "$EXPECT" ]; then
    rm -f "$tmp"
    echo "stage_plexob_yc_rbf: temp md5 $staged is not $EXPECT" >&2
    return 1
  fi
  if ! mv -f "$tmp" "$dest"; then
    rm -f "$tmp"
    echo "stage_plexob_yc_rbf: rename to $dest failed" >&2
    return 1
  fi
  fsync_path "$dest"
  fsync_path "$dir"
  installed=$(md5sum "$dest" | awk '{print $1}')
  if [ "$installed" != "$EXPECT" ]; then
    echo "stage_plexob_yc_rbf: installed $dest md5 $installed drifted" >&2
    return 1
  fi
}

install_one "$DEST_ROOT/PlexOB.rbf"
install_one "$DEST_ROOT/_Utility/PlexOB.rbf"
echo "stage_plexob_yc_rbf: $EXPECT $DEST_ROOT/PlexOB.rbf and _Utility/PlexOB.rbf"
