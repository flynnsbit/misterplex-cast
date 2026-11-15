#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# build_db.sh - generate the MiSTer Downloader custom database (db.json.zip) for MisterPlex
# from the committed payload/ tree, the way theypsilon/DB-Template_MiSTer does it
# (MiSTer-devel/Distribution_MiSTer db_operator.py "build"), without its force-push of
# an orphan "db" branch: the result is written next to this script and committed normally.
#
# The payload must already be committed: file URLs are pinned to that commit
# (BASE_FILES_URL uses git HEAD), so a later push never changes what an old db points at.
#
# Env (all optional; defaults are the public location):
#   MPX_DB_ID      db_id and downloader.ini section. NEVER change it once published.
#   MPX_DB_REPO    owner/repo that hosts payload/ and db.json.zip (must be public:
#                  Downloader cannot authenticate)
#   MPX_DB_BRANCH  branch that carries db.json.zip (the db_url)
#   MPX_DB_DIR     folder of this script inside the repo
#   MPX_DB_OPERATOR  local db_operator.py (default: download the pinned copy below)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DB_ID=${MPX_DB_ID:-flynnsbit/misterplex}
REPO=${MPX_DB_REPO:-flynnsbit/misterplex-cast}
BRANCH=${MPX_DB_BRANCH:-castor/update-all-db}
DIR=${MPX_DB_DIR:-mister-db}
RAW=https://raw.githubusercontent.com/$REPO
DB_URL="$RAW/refs/heads/$BRANCH/$DIR/db.json.zip"
BASE_FILES_URL="$RAW/%s/$DIR/payload/"
# Pinned generator (Distribution_MiSTer main as of 2026-11-15). Bump both together.
OP_URL=https://raw.githubusercontent.com/MiSTer-devel/Distribution_MiSTer/main/.github/db_operator.py
OP_SHA=9b9ab55b5b9156451748972e520a5b9e5eeecf66705f5ac65561c6ba8fe910e4
die() { echo "build_db: ERROR: $*" >&2; exit 1; }
[[ -d "$HERE/payload/misterplex" ]] || die "$HERE/payload is missing; run build_payload.sh and commit it first"
git -C "$HERE" diff --quiet HEAD -- payload || die "payload/ has uncommitted changes; commit it first (URLs pin the commit)"
[[ -z "$(git -C "$HERE" ls-files --others --exclude-standard -- payload)" ]] || die "payload/ has untracked files; commit them first"
SHA=$(git -C "$HERE" rev-parse --verify HEAD)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
OP=${MPX_DB_OPERATOR:-$TMP/db_operator.py}
if [[ -z "${MPX_DB_OPERATOR:-}" ]]; then curl -fsSL -o "$OP" "$OP_URL"; fi
echo "$OP_SHA  $OP" | sha256sum -c --quiet - || die "db_operator.py sha256 does not match the pin"
# External files (yt-dlp) are listed with absolute paths so the generator can read them.
cp "$HERE/external_files.csv" "$TMP/external_files.csv"
(
  cd "$HERE/payload"
  env -i PATH="$PATH" HOME="$TMP" DB_ID="$DB_ID" DB_URL="$DB_URL" TEST_DB_URL="$TMP/none.json" \
    DB_JSON_NAME="$TMP/db.json" BASE_FILES_URL="$BASE_FILES_URL" \
    EXTERNAL_FILES="$TMP/external_files.csv" FINDER_IGNORE="" BROKEN_MRAS_IGNORE=true \
    OMIT_DUAL_SDRAM_CORES=false python3 "$OP" build .
) > "$TMP/build.log" 2>&1 || { cat "$TMP/build.log" >&2; die "db_operator build failed"; }
[[ -s "$TMP/db.json" ]] || { cat "$TMP/build.log" >&2; die "db_operator wrote no db.json"; }
python3 "$HERE/validate_db.py" "$TMP/db.json" "$HERE/payload" --db-id "$DB_ID" --sha "$SHA"
rm -f "$HERE/db.json.zip"
(cd "$TMP" && TZ=UTC touch -t 202601010000 db.json && zip -q -X -9 "$HERE/db.json.zip" db.json)
# Drop-in ini (DB-Template_MiSTer create_drop_in_database_files): users copy it to the SD root.
safe=$(printf '%s' "$DB_ID" | sed 's/[^A-Za-z0-9._-]\{1,\}/_/g; s/^[._-]*//; s/[._-]*$//')
printf '[%s]\ndb_url = %s\n' "$DB_ID" "$DB_URL" > "$HERE/downloader_$safe.ini"
echo "build_db: db_id=$DB_ID payload_commit=$SHA"
echo "build_db: db_url=$DB_URL"
echo "build_db: wrote $HERE/db.json.zip ($(md5sum "$HERE/db.json.zip" | cut -d' ' -f1)) and downloader_$safe.ini"
