#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# regen_from_release.sh - rebuild the MisterPlex Downloader db from a PUBLISHED GitHub release
# of MPX_DB_REPO and commit it on the current branch (normal commits; it never pushes).
#
#   regen_from_release.sh [TAG]   TAG defaults to the newest published (non-draft) release,
#                                 pre-releases included (GitHub's "latest" skips them).
# Steps: download misterplex-<tag>.tar.gz + SHA256SUMS release assets, check the sha256,
# unpack, build_payload.sh, commit payload/ (if changed), build_db.sh (URLs pin that commit),
# commit db.json.zip + drop-in ini.
# Env: MPX_DB_REPO (default flynnsbit/misterplex-cast); GITHUB_TOKEN (optional, only for API rate limits).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=${MPX_DB_REPO:-flynnsbit/misterplex-cast}
TAG=${1:-}
die() { echo "regen_from_release: ERROR: $*" >&2; exit 1; }
api() { curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$REPO/$1"; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [[ -z "$TAG" ]]; then
  api 'releases?per_page=30' > "$T/rels.json"
  TAG=$(python3 -c 'import json,sys; r=[x for x in json.load(open(sys.argv[1])) if not x["draft"] and x.get("published_at")]; r.sort(key=lambda x:x["published_at"]); print(r[-1]["tag_name"] if r else "")' "$T/rels.json")
  [[ -n "$TAG" ]] || die "no published release in $REPO"
fi
[[ "$TAG" =~ ^[A-Za-z0-9._-]+$ ]] || die "bad tag: $TAG"
api "releases/tags/$TAG" > "$T/rel.json"
python3 - "$T/rel.json" > "$T/assets.txt" <<'PY'
import json, re, sys
rel = json.load(open(sys.argv[1]))
if rel["draft"]: sys.exit("release is a draft")
a = {x["name"]: x["browser_download_url"] for x in rel["assets"]}
pk = [n for n in a if re.fullmatch(r"misterplex-[^/]+\.tar\.gz", n)]
if len(pk) != 1 or "SHA256SUMS" not in a: sys.exit("release needs exactly one misterplex-*.tar.gz and a SHA256SUMS asset: %r" % sorted(a))
print(pk[0], a[pk[0]]); print("SHA256SUMS", a["SHA256SUMS"])
PY
read -r PKG PKG_URL < <(sed -n 1p "$T/assets.txt"); SUM_URL=$(sed -n 2p "$T/assets.txt" | cut -d' ' -f2)
echo "regen_from_release: $REPO $TAG $PKG"
curl -fsSL -o "$T/$PKG" "$PKG_URL"; curl -fsSL -o "$T/SHA256SUMS" "$SUM_URL"
(cd "$T" && grep -E "^[0-9a-f]{64}  \*?$PKG\$" SHA256SUMS | sha256sum -c --quiet -) || die "$PKG does not match the release SHA256SUMS"
mkdir "$T/x" && tar -xzf "$T/$PKG" -C "$T/x"
STAGE=$(find "$T/x" -mindepth 1 -maxdepth 1 -type d | head -1)
"$HERE/build_payload.sh" --stage "$STAGE"
printf '%s %s %s\n' "$REPO" "$TAG" "$(sha256sum "$T/$PKG" | cut -d' ' -f1)" > "$HERE/RELEASE"
git -C "$HERE" add -A payload external_files.csv RELEASE
if git -C "$HERE" diff --cached --quiet; then
  echo "regen_from_release: payload unchanged"
else
  git -C "$HERE" commit -q -m "mister-db: payload from release $TAG ($PKG)"
fi
"$HERE/build_db.sh"
git -C "$HERE" add db.json.zip downloader_*.ini
if git -C "$HERE" diff --cached --quiet; then
  echo "regen_from_release: db unchanged"
else
  git -C "$HERE" commit -q -m "mister-db: db.json.zip for release $TAG (payload $(git -C "$HERE" log -1 --format=%h -- payload))"
fi
