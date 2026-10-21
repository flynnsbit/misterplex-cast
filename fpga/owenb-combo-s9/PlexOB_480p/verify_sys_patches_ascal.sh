#!/usr/bin/env bash
# Gate: vendor/sys/ascal.vhd must match hpoly-patched md5 after sync + apply_sys_patches.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ASCAL="$HERE/vendor/sys/ascal.vhd"
MANIFEST="$HERE/sys-patches/ascal-hpoly-manifest.env"
PATCH="$HERE/sys-patches/ascal-hpoly-pipe.patch"

if [[ ! -f "$MANIFEST" ]]; then
  echo "verify_sys_patches_ascal: missing $MANIFEST" >&2
  exit 2
fi
# shellcheck source=/dev/null
source "$MANIFEST"
: "${ASCAL_VHD_MD5:?}"
: "${ASCAL_HPOLY_PATCH_MD5:?}"

if [[ ! -f "$ASCAL" ]]; then
  echo "verify_sys_patches_ascal: missing $ASCAL (run sync_vendor + apply_sys_patches)" >&2
  exit 2
fi
got_patch="$(md5sum "$PATCH" | awk '{print $1}')"
if [[ "$got_patch" != "$ASCAL_HPOLY_PATCH_MD5" ]]; then
  echo "verify_sys_patches_ascal: patch md5 mismatch got=$got_patch want=$ASCAL_HPOLY_PATCH_MD5" >&2
  exit 1
fi
if ! patch -p1 -R --dry-run -d "$HERE/vendor/sys" < "$PATCH" >/dev/null 2>&1; then
  echo "verify_sys_patches_ascal: hpoly patch not applied to $ASCAL" >&2
  exit 1
fi
got_ascal="$(md5sum "$ASCAL" | awk '{print $1}')"
if [[ "$got_ascal" != "$ASCAL_VHD_MD5" ]]; then
  echo "verify_sys_patches_ascal: ascal.vhd md5 mismatch got=$got_ascal want=$ASCAL_VHD_MD5" >&2
  exit 1
fi
echo "verify_sys_patches_ascal: OK ascal=$got_ascal patch=$got_patch"
