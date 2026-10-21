#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$ROOT/build/confstr-guard"
mkdir -p "$WORK"

BAD="$WORK/Plex.bad-confstr.sv"
python3 - "$ROOT/fpga/Plex_MiSTer/Plex.sv" "$BAD" <<'PY'
from pathlib import Path
src = Path(__import__("sys").argv[1])
dst = Path(__import__("sys").argv[2])
text = src.read_text()
bad = text.replace(
    '"F1,raw,RGB565 frame (320x240);"',
    '"F1,raw,RBS,565, fr,ame (320x240);"',
    1,
)
if bad == text:
    raise SystemExit("failed to inject malformed CONF_STR")
dst.write_text(bad)
PY

if python3 "$ROOT/scripts/check_confstr_guard.py" "$BAD" >"$WORK/red.out" 2>"$WORK/red.err"; then
  echo "FAIL: malformed CONF_STR unexpectedly passed" >&2
  exit 1
fi
grep -q "fixed 3-character chunks" "$WORK/red.err" || {
  echo "FAIL: malformed CONF_STR error did not explain field/chunk guard" >&2
  cat "$WORK/red.err" >&2
  exit 1
}
echo "RED OK: malformed CONF_STR rejected"
cat "$WORK/red.err"

python3 "$ROOT/scripts/check_confstr_guard.py" "$ROOT/fpga/Plex_MiSTer/Plex.sv"
echo "GREEN OK: checked-in CONF_STR passed"
