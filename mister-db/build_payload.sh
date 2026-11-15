#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build_payload.sh - lay out the MiSTer Downloader / update_all payload tree for MisterPlex.
#
# Input is an unpacked release stage from scripts/package_release.sh (the same tree
# scripts/install-misterplex.sh installs from). Output is a folder that mirrors the
# SD card root (/media/fat), using the paths install-misterplex.sh writes:
#
#   _Utility/MiSTerPlex.rbf                  the core (exactly one copy)
#   misterplex/{bin,scripts,docs,licenses,plexui,conf}/   program, scripts, docs, fonts
#   misterplex/yt_warm_helper.py             casting warm helper
#   misterplex/cores/MiSTerPlex.rbf.md5      md5 the boot hook checks before it retires PlexOB.rbf
#   misterplex/linux/...                     files that belong in /media/fat/linux. Downloader
#                                            refuses any path under linux/, so they are staged
#                                            here and Scripts/misterplex_setup.sh copies them.
#   Scripts/misterplex_setup.sh              one-time / re-runnable post-install step
#
# bin/yt-dlp_linux_armv7l.zip is NOT copied: it is listed in external_files.csv and
# fetched from the pinned upstream yt-dlp release.
#
# Usage: build_payload.sh --stage DIR --rbf FILE --rbf-md5 MD5 --rbf-build-id HEX \
#          --rbf-rtl SHA40 --rbf-seed N --daemon-rev SHA40 [--out DIR]
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STAGE="" RBF="" RBF_MD5="" RBF_BUILD_ID="" RBF_RTL="" RBF_SEED="" DAEMON_REV="" OUT="$HERE/payload"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage) STAGE=$2; shift 2 ;;
    --rbf) RBF=$2; shift 2 ;;
    --rbf-md5) RBF_MD5=$2; shift 2 ;;
    --rbf-build-id) RBF_BUILD_ID=$2; shift 2 ;;
    --rbf-rtl) RBF_RTL=$2; shift 2 ;;
    --rbf-seed) RBF_SEED=$2; shift 2 ;;
    --daemon-rev) DAEMON_REV=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    *) echo "build_payload: unknown option $1" >&2; exit 2 ;;
  esac
done
die() { echo "build_payload: ERROR: $*" >&2; exit 1; }
[[ -d "$STAGE/bin" && -f "$STAGE/bin/misterplexd" ]] || die "--stage must be a package_release.sh stage (bin/misterplexd missing)"
[[ -f "$RBF" ]] || die "--rbf file missing"
[[ "$RBF_MD5" =~ ^[0-9a-f]{32}$ ]] || die "--rbf-md5 must be 32 lowercase hex"
[[ "$RBF_BUILD_ID" =~ ^[0-9A-F]{8}$ ]] || die "--rbf-build-id must be 8 uppercase hex (e.g. 0B111401)"
[[ "$RBF_RTL" =~ ^[0-9a-f]{40}$ ]] || die "--rbf-rtl must be the 40-hex commit the RBF was built from"
[[ "$RBF_SEED" =~ ^[0-9]+$ ]] || die "--rbf-seed must be an integer"
[[ "$DAEMON_REV" =~ ^[0-9a-f]{40}$ ]] || die "--daemon-rev must be a 40-hex commit"
got=$(md5sum "$RBF" | cut -d' ' -f1)
[[ "$got" == "$RBF_MD5" ]] || die "RBF md5 $got != --rbf-md5 $RBF_MD5"
grep -a -F -q "$DAEMON_REV" "$STAGE/bin/misterplexd" || die "bin/misterplexd does not carry rev $DAEMON_REV"
grep -a -F -q "$DAEMON_REV" "$STAGE/bin/misterplexd-spelunkr" || die "bin/misterplexd-spelunkr does not carry rev $DAEMON_REV"
cmp -s "$STAGE/bin/misterplexd" "$STAGE/bin/misterplexd-spelunkr" && die "misterplexd-spelunkr is a copy of misterplexd"
case "$OUT" in ''|/|"$HOME") die "refusing --out $OUT" ;; esac

rm -rf -- "$OUT"
mkdir -p "$OUT/_Utility" "$OUT/misterplex/cores" "$OUT/misterplex/linux" "$OUT/Scripts"
# exFAT has no modes/owners: plain copies (no -a/-p), symlinks resolved (-L).
cp "$RBF" "$OUT/_Utility/MiSTerPlex.rbf"
printf '%s\n' "$RBF_MD5" > "$OUT/misterplex/cores/MiSTerPlex.rbf.md5"
# install-misterplex.sh step 2: these package folders go to /media/fat/misterplex/<d>/
for d in bin scripts docs licenses plexui conf; do
  [[ -d "$STAGE/$d" ]] && { mkdir -p "$OUT/misterplex/$d"; cp -RL "$STAGE/$d/." "$OUT/misterplex/$d/"; }
done
# ... and these root-level / scripts/ helpers are also copied into bin/
for f in install-misterplex.sh configure-misterplex.sh misterplex_mode.sh misterplex_restart_manual.sh misterplex_plex.sh misterplex_spelunkr.sh; do
  for src in "$STAGE/$f" "$STAGE/scripts/$f"; do
    [[ -f "$src" ]] && { cp "$src" "$OUT/misterplex/bin/$f"; break; }
  done
done
# yt-dlp zip is fetched from upstream (external_files.csv), not hosted here.
rm -f "$OUT/misterplex/bin/yt-dlp_linux_armv7l.zip"
# step 5: warm helper into the misterplex root
cp "$STAGE/linux/misterplex/yt_warm_helper.py" "$OUT/misterplex/yt_warm_helper.py"
# step 5: /media/fat/linux files, staged for misterplex_setup.sh (downloader cannot write linux/)
cp "$STAGE/linux/misterplex-cacert.pem" "$OUT/misterplex/linux/misterplex-cacert.pem"
for h in airplay gcast; do
  mkdir -p "$OUT/misterplex/linux/$h"; cp -RL "$STAGE/linux/$h/." "$OUT/misterplex/linux/$h/"
  (cd "$OUT/misterplex/linux/$h" && md5sum -c --quiet MD5SUMS) || die "linux/$h MD5SUMS check failed"
done
# GPL notice files travel with the program.
for f in LICENSE NOTICE; do [[ -f "$STAGE/$f" ]] && cp "$STAGE/$f" "$OUT/misterplex/$f"; done
# The package text names the seed-10 release pin. Rewrite the identity lines so the docs
# describe the core this payload actually ships (licenses/rbf, licenses/README, docs/INSTALL.txt).
old_md5=$(cut -d' ' -f1 "$STAGE/cores/MiSTerPlex.rbf.md5")
old_rtl=$(sed -n 's/^The PlexOB RTL in this repository is commit \([0-9a-f]\{40\}\).*/\1/p' "$STAGE/licenses/rbf/PROVENANCE.md")
old_id=$(sed -n 's/.*BUILD_ID 0x\([0-9A-F]\{8\}\), SEED \([0-9]*\)).*/\1/p' "$STAGE/licenses/rbf/PROVENANCE.md")
old_seed=$(sed -n 's/.*BUILD_ID 0x[0-9A-F]\{8\}, SEED \([0-9]*\)).*/\1/p' "$STAGE/licenses/rbf/PROVENANCE.md")
[[ -n "$old_rtl" && -n "$old_id" && -n "$old_seed" ]] || die "cannot read the package RBF identity from licenses/rbf/PROVENANCE.md"
for f in licenses/rbf/PROVENANCE.md licenses/README.md docs/INSTALL.txt; do
  [[ -f "$OUT/misterplex/$f" ]] || continue
  sed -i -e "s/$old_rtl/$RBF_RTL/g" -e "s/${old_rtl:0:8}/${RBF_RTL:0:8}/g" -e "s/$old_md5/$RBF_MD5/g" \
    -e "s/$old_id/$RBF_BUILD_ID/g" -e "s/SEED $old_seed\b/SEED $RBF_SEED/g" "$OUT/misterplex/$f"
done
cp "$HERE/misterplex_setup.sh" "$OUT/Scripts/misterplex_setup.sh"
cat > "$OUT/misterplex/DB-INFO.txt" <<INFO
MisterPlex update_all / Downloader payload
core       _Utility/MiSTerPlex.rbf md5 $RBF_MD5 BUILD_ID 0x$RBF_BUILD_ID seed $RBF_SEED RTL $RBF_RTL
daemon     misterplex/bin/misterplexd md5 $(md5sum "$OUT/misterplex/bin/misterplexd" | cut -d' ' -f1) rev $DAEMON_REV
spelunkr   misterplex/bin/misterplexd-spelunkr md5 $(md5sum "$OUT/misterplex/bin/misterplexd-spelunkr" | cut -d' ' -f1) rev $DAEMON_REV
After update_all, run Scripts > misterplex_setup once (and again after an update).
INFO
find "$OUT" -type f | wc -l | xargs echo "build_payload: files:"
