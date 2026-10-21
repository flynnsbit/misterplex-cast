#!/usr/bin/env bash
# Apply committed sys/ patches (vendor/sys is upstream + T1b from sync; no hand edits).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SYS="$HERE/vendor/sys"
PATCH="$HERE/sys-patches/ascal-hpoly-pipe.patch"
STAMP="$HERE/sys-patches/ascal-hpoly-pipe.patch.md5"
if [[ ! -f "$PATCH" ]]; then
  echo "apply_sys_patches: missing $PATCH" >&2
  exit 2
fi
want="$(md5sum "$PATCH" | awk '{print $1}')"
if [[ -f "$STAMP" ]] && grep -q "$want" "$STAMP" 2>/dev/null; then
  if patch -p1 -R --dry-run -d "$SYS" < "$PATCH" >/dev/null 2>&1; then
    echo "apply_sys_patches: ascal-hpoly-pipe already applied (md5 $want)"
    exit 0
  fi
fi
cd "$SYS"
patch -p1 -N < "$PATCH"
echo "$want  ascal-hpoly-pipe.patch" > "$STAMP"
MANIFEST="$HERE/sys-patches/ascal-hpoly-manifest.env"
if [[ -f "$MANIFEST" ]]; then
  # shellcheck source=/dev/null
  source "$MANIFEST"
  got_ascal="$(md5sum "$SYS/ascal.vhd" | awk '{print $1}')"
  echo "ASCAL_VHD_MD5=$got_ascal" > "$HERE/sys-patches/ascal-hpoly-result.env"
  echo "ASCAL_HPOLY_PATCH_MD5=$want" >> "$HERE/sys-patches/ascal-hpoly-result.env"
fi
echo "apply_sys_patches: applied ascal-hpoly-pipe.patch md5=$want"
