#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build_payload.sh - lay out the MiSTer Downloader / update_all payload for MisterPlex
# from an unpacked PUBLISHED release package (misterplex-<tag>.tar.gz from the
# flynnsbit/misterplex-cast GitHub release). Nothing is rebuilt: every byte comes
# from the release, checked against the package's own SHA256SUMS.
#
# Output mirrors the SD card root (/media/fat) at the paths install-misterplex.sh writes:
#   _Utility/MiSTerPlex.rbf                  the core (exactly one copy)
#   misterplex/{bin,scripts,plexui,conf}/   program, scripts, fonts, default conf
#   misterplex/yt_warm_helper.py             casting warm helper
#   misterplex/cores/MiSTerPlex.rbf.md5      the release md5 slot (the boot hook checks it
#                                            before it retires PlexOB.rbf)
# Runtime files only: docs/, licenses/, LICENSE, NOTICE, README and release notes are not
# installed on the MiSTer (they stay in the release package and the repo).
#   misterplex/linux/...                     files that belong in /media/fat/linux. Downloader
#                                            refuses any path under linux/, so they are staged
#                                            here and Scripts/misterplex_setup.sh copies them.
#   Scripts/misterplex_setup.sh              the post-install step
# A file listed in external_files.csv with the same md5 is fetched from that URL instead of
# being hosted here; a listed file whose md5 differs is hosted here and its row is dropped.
#
# Usage: build_payload.sh --stage <unpacked misterplex-TAG/> [--out DIR]
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STAGE="" OUT="$HERE/payload"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage) STAGE=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    *) echo "build_payload: unknown option $1" >&2; exit 2 ;;
  esac
done
die() { echo "build_payload: ERROR: $*" >&2; exit 1; }
[[ -f "$STAGE/bin/misterplexd" && -f "$STAGE/cores/MiSTerPlex.rbf" ]] || die "--stage must be an unpacked release package"
[[ -f "$STAGE/SHA256SUMS" ]] || die "$STAGE/SHA256SUMS missing"
# Every file the package lists must be present and match (source/ may be absent).
(cd "$STAGE" && awk '{ f=substr($0, 67); if ((getline line < f) >= 0) print; close(f) }' SHA256SUMS \
  | sha256sum -c --quiet -) || die "package SHA256SUMS check failed"
want=$(cut -d' ' -f1 "$STAGE/cores/MiSTerPlex.rbf.md5" | tr 'A-F' 'a-f')
[[ "$(md5sum "$STAGE/cores/MiSTerPlex.rbf" | cut -d' ' -f1)" == "$want" ]] || die "cores/MiSTerPlex.rbf does not match its .md5"
cmp -s "$STAGE/bin/misterplexd" "$STAGE/bin/misterplexd-spelunkr" && die "misterplexd-spelunkr is a copy of misterplexd"
case "$OUT" in ''|/|"$HOME") die "refusing --out $OUT" ;; esac

rm -rf -- "$OUT"
mkdir -p "$OUT/_Utility" "$OUT/misterplex/cores" "$OUT/misterplex/linux" "$OUT/Scripts"
# exFAT has no modes/owners: plain copies (no -a/-p), symlinks resolved (-L).
cp "$STAGE/cores/MiSTerPlex.rbf" "$OUT/_Utility/MiSTerPlex.rbf"
cp "$STAGE/cores/MiSTerPlex.rbf.md5" "$OUT/misterplex/cores/MiSTerPlex.rbf.md5"
# install-misterplex.sh step 2 (runtime folders only): /media/fat/misterplex/<d>/
for d in bin scripts plexui conf; do
  [[ -d "$STAGE/$d" ]] && { mkdir -p "$OUT/misterplex/$d"; cp -RL "$STAGE/$d/." "$OUT/misterplex/$d/"; }
done
# ... and these root-level / scripts/ helpers are also copied into bin/
for f in install-misterplex.sh configure-misterplex.sh misterplex_mode.sh misterplex_restart_manual.sh misterplex_plex.sh misterplex_spelunkr.sh; do
  for src in "$STAGE/$f" "$STAGE/scripts/$f"; do
    [[ -f "$src" ]] && { cp "$src" "$OUT/misterplex/bin/$f"; break; }
  done
done
# step 5: warm helper into the misterplex root
[[ -f "$STAGE/linux/misterplex/yt_warm_helper.py" ]] && cp "$STAGE/linux/misterplex/yt_warm_helper.py" "$OUT/misterplex/yt_warm_helper.py"
# step 5: /media/fat/linux files, staged for misterplex_setup.sh (downloader cannot write linux/)
[[ -f "$STAGE/linux/misterplex-cacert.pem" ]] && cp "$STAGE/linux/misterplex-cacert.pem" "$OUT/misterplex/linux/misterplex-cacert.pem"
for h in airplay gcast; do
  [[ -d "$STAGE/linux/$h" ]] || continue
  mkdir -p "$OUT/misterplex/linux/$h"; cp -RL "$STAGE/linux/$h/." "$OUT/misterplex/linux/$h/"
  if [[ -f "$OUT/misterplex/linux/$h/MD5SUMS" ]]; then
    (cd "$OUT/misterplex/linux/$h" && md5sum -c --quiet MD5SUMS) || die "linux/$h MD5SUMS check failed"
  fi
done
cp "$HERE/misterplex_setup.sh" "$OUT/Scripts/misterplex_setup.sh"
# External files: host upstream when the bytes match, else host the release copy here.
python3 - "$HERE/external_files.csv" "$OUT" <<'PY'
import csv, hashlib, os, sys
path, out = sys.argv[1], sys.argv[2]
if not os.path.exists(path): sys.exit(0)
rows = list(csv.reader(open(path, newline="")))
keep = [rows[0]] if rows else []
for r in rows[1:]:
    if len(r) < 4: keep.append(r); continue
    fp = os.path.join(out, r[0].strip())
    if os.path.isfile(fp):
        if hashlib.md5(open(fp, "rb").read()).hexdigest() == r[3].strip().lower():
            os.remove(fp); keep.append(r)
            print("build_payload: external", r[0].strip(), "->", r[1].strip())
        else:
            print("build_payload: release copy of", r[0].strip(), "differs from external_files.csv; hosting it here")
    else:
        keep.append(r)
with open(path, "w", newline="") as f:
    csv.writer(f, lineterminator="\n").writerows(keep)
PY
n=$(find "$OUT" -type f | wc -l)
echo "build_payload: core md5 $want, daemon md5 $(md5sum "$OUT/misterplex/bin/misterplexd" | cut -d' ' -f1), files: $n"
