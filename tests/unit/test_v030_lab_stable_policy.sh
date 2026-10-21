#!/usr/bin/env bash
# Offline integrity for v0.3.0 lab-stable pair freeze.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
POLICY="$ROOT/tests/fixtures/v030_lab_stable/pair.json"
ART="$ROOT/release_artifacts/v0.3.0-lab-stable"
status=0
fail() { status=1; echo "test_v030_lab_stable_policy: FAIL - $*" >&2; }
ok() { echo "test_v030_lab_stable_policy: OK $*"; }

[[ -f "$POLICY" ]] || fail "missing $POLICY"
python3 - "$POLICY" <<'PY' || fail "schema"
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("github_release") is None, "github_release must be null (never published)"
assert d["pair"]["rbf_md5"] == "41adb98c7a630b541091c22ce291be68"
assert d["pair"]["daemon_md5"] == "06c5735a2f85114688f0ff2ac36e4fd4"
assert d["conf"]["PRESENT"] == "both", d["conf"]
print("schema_ok")
PY

if [[ ! -f "$ART/Plex.rbf" || ! -f "$ART/misterplexd" ]]; then
  fail "missing $ART binaries"
else
  r=$(md5sum "$ART/Plex.rbf" | awk '{print $1}')
  d=$(md5sum "$ART/misterplexd" | awk '{print $1}')
  [[ "$r" == "41adb98c7a630b541091c22ce291be68" ]] && ok "Plex.rbf" || fail "RBF md5 $r"
  [[ "$d" == "06c5735a2f85114688f0ff2ac36e4fd4" ]] && ok "misterplexd" || fail "daemon md5 $d"
fi
[[ -f "$ROOT/docs/v3-stable-320-loop.md" ]] && ok "docs" || fail "docs missing"
[[ -f "$ROOT/scripts/v3_stable_320_loop.sh" ]] && ok "loop script" || fail "loop script"
[[ -f "$ART/PAIR.md" ]] && ok "PAIR.md" || fail "PAIR.md"

[[ $status -eq 0 ]] && echo "test_v030_lab_stable_policy: PASS" && exit 0
exit 1
