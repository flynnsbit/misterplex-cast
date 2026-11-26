#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build_payload.sh - lay out the MiSTer Downloader / update_all payload for MisterPlex
# from an unpacked PUBLISHED release package (misterplex-<tag>.tar.gz from the
# flynnsbit/misterplex-cast GitHub release). Nothing is rebuilt: every byte comes
# from the release, checked against the package's own SHA256SUMS.
#
# Output mirrors the SD card root (/media/fat) at the paths install-misterplex.sh writes:
#   _Utility/MiSTerPlex.rbf                  the core (exactly one copy)
#   misterplex/{bin,plexui,conf}/           program, boot scripts, fonts, default conf (no scripts/)
#   misterplex/yt_warm_helper.py             casting warm helper
#   misterplex/cores/MiSTerPlex.rbf.md5      the release md5 slot (the boot hook checks it
#                                            before it retires PlexOB.rbf)
# Runtime files only: docs/, licenses/, LICENSE, NOTICE, README and release notes are not
# installed on the MiSTer (they stay in the release package and the repo).
#   misterplex/linux/...                     files that belong in /media/fat/linux. Downloader
#                                            refuses any path under linux/, so they are staged
#                                            here and Scripts/configure-misterplex.sh copies them
#                                            (first run, or Repair / Reinstall).
#   Scripts/configure-misterplex.sh          the one Scripts menu entry: Settings, Restart,
#                                            Repair / Reinstall (= setup), Uninstall
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
# The db installs exactly the MiSTer runtime set (28 required files: N14 ad-break
# card + owenb_idle_card, plus optional yt-dlp zip).
# Same set as packaging/runtime-files.txt (tests/unit/test_runtime_set_db_mirror.sh checks it).
# @menu: the package's Scripts/configure-misterplex.sh; a package older than the one-menu change
# (v1.0.0) has none, so the repo copy next to this script is used.
# "<db path> <package path>[|<fallback package path>]"; "?" before the db path = optional.
RUNTIME_SET='
Scripts/configure-misterplex.sh               @menu
_Utility/MiSTerPlex.rbf                       cores/MiSTerPlex.rbf
misterplex/cores/MiSTerPlex.rbf.md5           cores/MiSTerPlex.rbf.md5
misterplex/bin/misterplexd                    bin/misterplexd
misterplex/bin/misterplexd-spelunkr           bin/misterplexd-spelunkr
misterplex/bin/misterplexd-boot-rc3.sh        bin/misterplexd-boot-rc3.sh
misterplex/bin/misterplexd_supervise.sh       bin/misterplexd_supervise.sh
misterplex/bin/misterplexd_update_watch.sh    bin/misterplexd_update_watch.sh
misterplex/bin/misterplex_cast_ready.sh       bin/misterplex_cast_ready.sh
misterplex/bin/misterplex_mode.sh             bin/misterplex_mode.sh
misterplex/bin/update_ytdlp.sh                bin/update_ytdlp.sh
?misterplex/bin/yt-dlp_linux_armv7l.zip       bin/yt-dlp_linux_armv7l.zip
misterplex/bin/owenb_idle_card                bin/owenb_idle_card
misterplex/conf/misterplex.conf.example       conf/misterplex.conf.example
misterplex/plexui/Roboto-Regular.ttf          plexui/Roboto-Regular.ttf
misterplex/plexui/Roboto-Bold.ttf             plexui/Roboto-Bold.ttf
misterplex/plexui/misterplex-ad-break.jpg     plexui/misterplex-ad-break.jpg
misterplex/yt_warm_helper.py                  linux/misterplex/yt_warm_helper.py
misterplex/linux/misterplex-cacert.pem        linux/misterplex-cacert.pem
misterplex/linux/airplay/airplay_xcode.py     linux/airplay/airplay_xcode.py
misterplex/linux/airplay/run-airplay-xcode.sh linux/airplay/run-airplay-xcode.sh
misterplex/linux/airplay/ffmpeg               linux/airplay/ffmpeg
misterplex/linux/airplay/ffprobe              linux/airplay/ffprobe
misterplex/linux/airplay/MD5SUMS              linux/airplay/MD5SUMS
misterplex/linux/gcast/gcast_xcode.py         linux/gcast/gcast_xcode.py
misterplex/linux/gcast/run-gcast-xcode.sh     linux/gcast/run-gcast-xcode.sh
misterplex/linux/gcast/ffmpeg                 linux/gcast/ffmpeg
misterplex/linux/gcast/ffprobe                linux/gcast/ffprobe
misterplex/linux/gcast/MD5SUMS                linux/gcast/MD5SUMS
'
mkdir -p "$OUT"
n_req=0
while read -r dbp srcs; do
  [[ -n "$dbp" ]] || continue
  opt=0; [[ "$dbp" == \?* ]] && { opt=1; dbp=${dbp#\?}; }
  [[ "$opt" = 1 ]] || n_req=$((n_req + 1))
  src=""
  if [[ "$srcs" == @menu ]]; then
    src="$STAGE/Scripts/configure-misterplex.sh"
    [[ -f "$src" ]] || { src="$HERE/../packaging/Scripts/configure-misterplex.sh"; echo "build_payload: package has no Scripts/configure-misterplex.sh; using the repo copy"; }
  else
    IFS='|' read -r -a cands <<<"$srcs"
    for c in "${cands[@]}"; do [[ -f "$STAGE/$c" ]] && { src="$STAGE/$c"; break; }; done
  fi
  if [[ -z "$src" ]]; then
    [[ "$opt" = 1 ]] && continue
    die "runtime file $dbp: none of $srcs is in the package"
  fi
  mkdir -p "$OUT/$(dirname "$dbp")"
  # exFAT has no modes/owners: plain copies (no -a/-p), symlinks resolved.
  cp -L "$src" "$OUT/$dbp"
done <<<"$RUNTIME_SET"
[[ "$n_req" = 28 ]] || die "runtime set lists $n_req required files, expected 28"
(cd "$OUT/misterplex/linux/airplay" && md5sum -c --quiet MD5SUMS) || die "linux/airplay MD5SUMS check failed"
(cd "$OUT/misterplex/linux/gcast" && md5sum -c --quiet MD5SUMS) || die "linux/gcast MD5SUMS check failed"
# Runtime only: no licence / notice / readme / release-notes files anywhere on the card.
find "$OUT" -type f \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'NOTICE*' -o -iname 'README*' -o -iname 'RELEASE-NOTES*' -o -iname 'PROVENANCE*' \) -delete
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
