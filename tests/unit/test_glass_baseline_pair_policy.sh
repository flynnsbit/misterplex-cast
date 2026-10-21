#!/usr/bin/env bash
# Offline integrity for the HDMI glass regression floor pair.
# Does not touch hardware.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

POLICY="$ROOT/tests/fixtures/glass_baseline/pair.json"
ART="$ROOT/release_artifacts/v0.2.0-glass-baseline"
status=0

fail() {
  status=1
  echo "test_glass_baseline_pair_policy: FAIL - $*" >&2
}

ok() {
  echo "test_glass_baseline_pair_policy: OK $*"
}

if [[ ! -f "$POLICY" ]]; then
  fail "missing $POLICY"
  exit 1
fi

python3 - "$POLICY" <<'PY' || fail "pair.json schema/fields"
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
d = json.loads(p.read_text())
for k in ("id", "pair", "conf", "gates", "role"):
    if k not in d:
        raise SystemExit(f"missing key {k}")
pair = d["pair"]
for k in ("rbf_md5", "daemon_md5", "artifact_dir"):
    if k not in pair or not pair[k]:
        raise SystemExit(f"pair.{k} missing")
if len(pair["rbf_md5"]) != 32 or len(pair["daemon_md5"]) != 32:
    raise SystemExit("md5 fields must be 32 hex chars")
if d.get("role") != "REGRESSION_FLOOR":
    raise SystemExit(f"role must be REGRESSION_FLOOR, got {d.get('role')}")
gates = d["gates"]
if "idle" not in gates or "play" not in gates:
    raise SystemExit("gates.idle and gates.play required")
print("schema_ok id=", d["id"])
print("rbf_md5=", pair["rbf_md5"])
print("daemon_md5=", pair["daemon_md5"])
PY

RBF_MD5="$(python3 -c "import json; print(json.load(open('$POLICY'))['pair']['rbf_md5'])")"
DAE_MD5="$(python3 -c "import json; print(json.load(open('$POLICY'))['pair']['daemon_md5'])")"

if [[ ! -d "$ART" ]]; then
  fail "artifact dir missing: $ART"
else
  if [[ ! -f "$ART/Plex.rbf" ]]; then
    fail "missing $ART/Plex.rbf"
  else
    act="$(md5sum "$ART/Plex.rbf" | awk '{print $1}')"
    if [[ "$act" != "$RBF_MD5" ]]; then
      fail "Plex.rbf md5 $act != policy $RBF_MD5"
    else
      ok "Plex.rbf md5=$act"
    fi
  fi
  if [[ ! -f "$ART/misterplexd" ]]; then
    fail "missing $ART/misterplexd"
  else
    act="$(md5sum "$ART/misterplexd" | awk '{print $1}')"
    if [[ "$act" != "$DAE_MD5" ]]; then
      fail "misterplexd md5 $act != policy $DAE_MD5"
    else
      ok "misterplexd md5=$act"
    fi
  fi
  if [[ ! -f "$ART/PAIR.md" ]]; then
    fail "missing $ART/PAIR.md"
  else
    ok "PAIR.md present"
  fi
fi

for f in idle_chevron_mjpeg720.png b6_play_sample_mjpeg720.jpg; do
  if [[ ! -f "$ROOT/tests/fixtures/glass_baseline/$f" ]]; then
    fail "missing fixture still $f"
  else
    ok "fixture $f"
  fi
done

if [[ ! -f "$ROOT/docs/glass-baseline-pair.md" ]]; then
  fail "missing docs/glass-baseline-pair.md"
else
  ok "docs/glass-baseline-pair.md"
fi

if [[ "$status" -ne 0 ]]; then
  exit 1
fi
echo "test_glass_baseline_pair_policy: PASS"
exit 0
