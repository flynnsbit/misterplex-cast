#!/usr/bin/env bash
# Guard: release packages must carry the hardware-validated cores for this line.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

EXPECTED_V03="41adb98c7a630b541091c22ce291be68"
SOURCE_RBF="${RELEASE_RBF_PATH:-release_artifacts/v0.3.0/Plex.rbf}"
PAIR_ART="${PAIR_ART:-release_artifacts/v0.9.0-pre-paired}"
MD5_480P_RBF="07f54d9f8f0eda2fe75d9cc314f6de54"
MD5_240P15_RBF="4d6efef954acf7b33747f35ac2878c1b"
MD5_480I_RBF="61db00e7d54efad7c1a456b127b798bd"
MD5_720P24_RBF="0eea3580a5a0dacf60bf65c50499bcc9"
MD5_480P_DAEMON="5f1c861486844f4c83bf32a2112cfbb6"
MD5_720P24_DAEMON="0ffc1483677ef5733971d4f18e4624e4"
status=0

version="${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
scan_all="${SCAN_ARTIFACTS:-0}"
scanned_artifact=0

artifact_is_current() {
  [[ "$scan_all" == "1" ]] && return 0
  [[ "$1" == "misterplex-$version" ]]
}

version_is_paired() {
  case "$1" in
    *v0.9.0-pre*|*0.9.0-pre*) return 0 ;;
    *) return 1 ;;
  esac
}

report() {
  status=1
  echo "test_release_rbf_hash: FAIL - $1" >&2
}

check_hash() {
  local label="$1" actual="$2" expect="$3"
  if [[ "$actual" != "$expect" ]]; then
    report "$label md5 mismatch; expected $expect actual $actual"
  else
    echo "test_release_rbf_hash: OK $label md5=$actual"
  fi
}

if [[ ! -f "$SOURCE_RBF" ]]; then
  report "release source core missing: $SOURCE_RBF"
else
  check_hash "$SOURCE_RBF" "$(md5sum "$SOURCE_RBF" | awk '{print $1}')" "$EXPECTED_V03"
fi

if [[ -d "$PAIR_ART" ]]; then
  for spec in \
    "cores/Plex_480p.rbf:$MD5_480P_RBF" \
    "cores/Plex_240p15.rbf:$MD5_240P15_RBF" \
    "cores/Plex_480i.rbf:$MD5_480I_RBF" \
    "cores/Plex_720p24.rbf:$MD5_720P24_RBF" \
    "bin/misterplexd.480p:$MD5_480P_DAEMON" \
    "bin/misterplexd.720p24:$MD5_720P24_DAEMON"
  do
    rel="${spec%%:*}"
    expect="${spec##*:}"
    f="$PAIR_ART/$rel"
    if [[ ! -f "$f" ]]; then
      report "paired freeze missing $f"
    else
      check_hash "$f" "$(md5sum "$f" | awk '{print $1}')" "$expect"
    fi
  done
fi

check_paired_listing() {
  local tarball=$1 listing=$2
  local name
  for name in Plex_480p.rbf Plex_240p15.rbf Plex_480i.rbf Plex_720p24.rbf; do
    if ! grep -Eq "/cores/${name}\$" <<<"$listing"; then
      report "$tarball does not contain cores/$name"
    fi
  done
  if grep -Eq '/cores/Plex\.rbf$' <<<"$listing"; then
    report "$tarball must not contain generic cores/Plex.rbf"
  fi
  check_hash "$tarball cores/Plex_480p.rbf" \
    "$(tar --wildcards -xOzf "$tarball" '*/cores/Plex_480p.rbf' | md5sum | awk '{print $1}')" \
    "$MD5_480P_RBF"
  check_hash "$tarball cores/Plex_240p15.rbf" \
    "$(tar --wildcards -xOzf "$tarball" '*/cores/Plex_240p15.rbf' | md5sum | awk '{print $1}')" \
    "$MD5_240P15_RBF"
  check_hash "$tarball cores/Plex_480i.rbf" \
    "$(tar --wildcards -xOzf "$tarball" '*/cores/Plex_480i.rbf' | md5sum | awk '{print $1}')" \
    "$MD5_480I_RBF"
  check_hash "$tarball cores/Plex_720p24.rbf" \
    "$(tar --wildcards -xOzf "$tarball" '*/cores/Plex_720p24.rbf' | md5sum | awk '{print $1}')" \
    "$MD5_720P24_RBF"
  check_hash "$tarball bin/misterplexd.480p" \
    "$(tar --wildcards -xOzf "$tarball" '*/bin/misterplexd.480p' | md5sum | awk '{print $1}')" \
    "$MD5_480P_DAEMON"
  check_hash "$tarball bin/misterplexd.720p24" \
    "$(tar --wildcards -xOzf "$tarball" '*/bin/misterplexd.720p24' | md5sum | awk '{print $1}')" \
    "$MD5_720P24_DAEMON"
}

if compgen -G "dist/misterplex-*.tar.gz" >/dev/null; then
  for tarball in dist/misterplex-*.tar.gz; do
    [[ -f "$tarball" ]] || continue
    if ! artifact_is_current "$(basename "$tarball" .tar.gz)"; then
      echo "test_release_rbf_hash: skipping stale artifact $tarball (built from another revision, not $version); SCAN_ARTIFACTS=1 to include it"
      continue
    fi
    listing="$(tar -tzf "$tarball")"
    scanned_artifact=1
    if version_is_paired "$(basename "$tarball")"; then
      check_paired_listing "$tarball" "$listing"
    else
      if ! grep -Eq '/cores/Plex\.rbf$' <<<"$listing"; then
        report "$tarball does not contain cores/Plex.rbf"
        continue
      fi
      actual="$(tar --wildcards -xOzf "$tarball" '*/cores/Plex.rbf' | md5sum | awk '{print $1}')"
      check_hash "$tarball cores/Plex.rbf" "$actual" "$EXPECTED_V03"
    fi
  done
fi

if [[ "${REQUIRE_ARTIFACT:-0}" == "1" && "$scanned_artifact" != "1" ]]; then
  report "REQUIRE_ARTIFACT=1 but no package for $version was found to scan"
fi

exit "$status"
