#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$ROOT/build/mister-ini-plex-guard"
mkdir -p "$WORK"

GOOD="$WORK/good.ini"
BAD_MISSING="$WORK/bad-missing.ini"
BAD_COMMENTED="$WORK/bad-commented.ini"

cat >"$GOOD" <<'INI'
[Menu]
video_mode=8

[Plex]
video_mode=5
video_mode_ntsc=5
video_mode_pal=5
INI

cat >"$BAD_MISSING" <<'INI'
[Plex]
video_mode=5
video_mode_ntsc=5
INI

cat >"$BAD_COMMENTED" <<'INI'
[Plex]
; video_mode=5
video_mode_ntsc=5
video_mode_pal=5
INI

python3 "$ROOT/scripts/check_mister_ini_plex_guard.py" "$GOOD"

if python3 "$ROOT/scripts/check_mister_ini_plex_guard.py" "$BAD_MISSING" >/dev/null 2>"$WORK/bad-missing.err"; then
  echo "FAIL: bad MiSTer.ini without video_mode_pal unexpectedly passed" >&2
  exit 1
fi
grep -q "video_mode_pal" "$WORK/bad-missing.err" || {
  echo "FAIL: missing-key error did not name video_mode_pal" >&2
  cat "$WORK/bad-missing.err" >&2
  exit 1
}

if python3 "$ROOT/scripts/check_mister_ini_plex_guard.py" "$BAD_COMMENTED" >/dev/null 2>"$WORK/bad-commented.err"; then
  echo "FAIL: commented video_mode unexpectedly passed" >&2
  exit 1
fi
grep -q "anti-retune pins" "$WORK/bad-commented.err" || {
  echo "FAIL: commented-key error did not explain anti-retune guard" >&2
  cat "$WORK/bad-commented.err" >&2
  exit 1
}

# [PlexOB] is optional. Absence passes. A present invalid value fails.
NO_OB="$WORK/no-plexob.ini"
BAD_OB="$WORK/bad-plexob.ini"
GOOD_OB="$WORK/good-plexob.ini"
cp "$GOOD" "$NO_OB"
cat >"$BAD_OB" <<'INI'
[Plex]
video_mode=5
video_mode_ntsc=5
video_mode_pal=5

[PlexOB]
vga_mode=component
ntsc_mode=0
INI
cat >"$GOOD_OB" <<'INI'
[Plex]
video_mode=5
video_mode_ntsc=5
video_mode_pal=5

[plexob]
vga_mode=svideo
ntsc_mode=1
vga_scaler=0
INI

python3 "$ROOT/scripts/check_mister_ini_plex_guard.py" "$NO_OB"
python3 "$ROOT/scripts/check_mister_ini_plex_guard.py" "$GOOD_OB"
if python3 "$ROOT/scripts/check_mister_ini_plex_guard.py" "$BAD_OB" >/dev/null 2>"$WORK/bad-ob.err"; then
  echo "FAIL: invalid [PlexOB] vga_mode unexpectedly passed" >&2
  exit 1
fi
grep -q "vga_mode" "$WORK/bad-ob.err" || {
  echo "FAIL: invalid [PlexOB] error did not name vga_mode" >&2
  cat "$WORK/bad-ob.err" >&2
  exit 1
}

echo "PASS MiSTer.ini [Plex] guard rejects missing/commented pins"
