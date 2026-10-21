#!/usr/bin/env bash
# Guard: no personal Plex server addresses or live Plex tokens may be committed.
#
# MiSTerPlex is a public repo. Lab worktrees routinely hardcode the maintainer's
# Plex Media Server address and tokens while iterating; this test makes such a
# value fail the build instead of shipping in a release tarball.
#
# There is no built-in MiSTer address. Set MISTER_HOST. Examples use the
# documentation ranges 192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24 only;
# every RFC1918 address literal is refused.
#
# Extra site-specific tokens (your own server names, hostnames, WAN prefixes)
# go one per line in a git-ignored .private-denylist at the repo root, or in
# a file named by PRIVATE_DENYLIST. They are never committed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

status=0
scan_files=()

report() {
  status=1
  echo "test_no_private_data: FAIL - $1" >&2
}

add_tree() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0
  local found=()
  mapfile -d '' -t found < <(find "$dir" -type f -not -path '*/.git/*' -print0)
  scan_files+=("${found[@]}")
}

if [[ -n "${SCAN_TREE:-}" ]]; then
  [[ -d "$SCAN_TREE" ]] || report "SCAN_TREE is not a directory: $SCAN_TREE"
  add_tree "$SCAN_TREE"
elif command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
  mapfile -t tracked_files < <(git ls-files)
  scan_files+=("${tracked_files[@]}")
else
  add_tree "."
fi

# Generated packages are not git-tracked, but they are exactly what users
# download, so they are scanned too.
#
# Scope matters here: dist/ accumulates tarballs from every past build, and
# those old artifacts can contain values that were legitimately removed from
# the source since. Scanning them unconditionally made this test fail forever
# on any machine holding a stale tarball, which is a property of local
# filesystem history rather than of the code under test. Only artifacts built
# from the current tree are scanned by default; SCAN_ARTIFACTS=1 forces every
# artifact to be scanned and is what `make package` uses, so whatever actually
# ships is always covered.
version="${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
scan_all="${SCAN_ARTIFACTS:-0}"

artifact_is_current() {
  [[ "$scan_all" == "1" ]] && return 0
  # Exact match only: a dirty tree is a different build from the tagged one,
  # so it must not be validated against the tagged tree's package.
  [[ "$1" == "misterplex-$version" ]]
}

scan_root="$ROOT/build/private-data-scan"
if compgen -G "dist/misterplex-*.tar.gz" >/dev/null; then
  rm -rf "$scan_root"
  mkdir -p "$scan_root"
  for tarball in dist/misterplex-*.tar.gz; do
    [[ -f "$tarball" ]] || continue
    safe_name="$(basename "$tarball" .tar.gz)"
    if ! artifact_is_current "$safe_name"; then
      echo "test_no_private_data: skipping stale artifact $tarball (built from another revision, not $version); SCAN_ARTIFACTS=1 to include it"
      continue
    fi
    mkdir -p "$scan_root/$safe_name"
    tar -xzf "$tarball" -C "$scan_root/$safe_name"
    add_tree "$scan_root/$safe_name"
    scanned_artifact=1
  done
fi

# package_release.sh rebuilds the staging tree immediately before tarring it,
# so it is current exactly when a current-version tarball is present.
if [[ "$scan_all" == "1" || "${scanned_artifact:-0}" == "1" ]]; then
  add_tree "dist/stage-misterplex"
elif [[ -d "dist/stage-misterplex" ]]; then
  echo "test_no_private_data: skipping stale dist/stage-misterplex (no $version tarball to date it against); SCAN_ARTIFACTS=1 to include it"
fi

# Release gate: refuse to report success without having actually inspected a
# package, so a version/naming mismatch cannot turn this into a no-op.
if [[ "${REQUIRE_ARTIFACT:-0}" == "1" && "${scanned_artifact:-0}" != "1" ]]; then
  report "REQUIRE_ARTIFACT=1 but no package for $version was found to scan"
fi

files=()
for f in "${scan_files[@]}"; do
  [[ -f "$f" ]] && files+=("$f")
done

# Text plus `strings` of any shipped misterplexd. RBF and other binaries stay out.
string_dir="$(mktemp -d)"
si=0
for f in "${files[@]}"; do
  base="$(basename "$f")"
  if [[ "$base" == "misterplexd" ]] && file -b "$f" 2>/dev/null | grep -q ELF; then
    strings -a -n 4 "$f" >"$string_dir/daemon-$si.txt"
    files+=("$string_dir/daemon-$si.txt")
    si=$((si + 1))
  fi
done

git_hit=""
for f in "${files[@]}"; do
  case "$f" in
    .git|.git/*|*/.git|*/.git/*|*/.git-writable-test|*/.git-writable-test/*|*/.git.shared-worktree-ptr)
      git_hit+="$f"$'\n'
      ;;
  esac
done
if [[ -n "$git_hit" ]]; then
  report "git metadata leftover in the tree"
  printf '%s' "$git_hit" >&2
fi

denylist="${PRIVATE_DENYLIST:-$ROOT/.private-denylist}"
export MPX_SCAN_DENYLIST=""
[[ -f "$denylist" ]] && export MPX_SCAN_DENYLIST="$denylist"
export MPX_SCAN_HOME="$(printf '/ho%s' me)"
list="$(mktemp)"
printf '%s\n' "${files[@]}" >"$list"
set +e
python3 - "$list" <<'PY'
import os, re, sys
paths = [p for p in open(sys.argv[1], encoding="utf-8").read().splitlines() if p]
extra = []
if os.environ.get("MPX_SCAN_DENYLIST"):
    for line in open(os.environ["MPX_SCAN_DENYLIST"], encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            extra.append(line)
deny_path = os.path.realpath(os.environ["MPX_SCAN_DENYLIST"]) if os.environ.get("MPX_SCAN_DENYLIST") else ""
home = os.environ["MPX_SCAN_HOME"] + "/"
quad = re.compile(r"(?<![0-9])(\d{1,3}(?:\.\d{1,3}){3})(?![0-9])")
dash = re.compile(r"(?<![0-9])(\d{1,3}(?:-\d{1,3}){3})(?![0-9])")
home_re = re.compile(re.escape(home) + r"[A-Za-z0-9_.-]+")

def private(parts):
    a, b, c, d = parts
    if a == 10:
        return True
    if a == 192 and b == 168:
        return True
    if a == 172 and 16 <= b <= 31:
        return True
    return False

hits = []
for path in paths:
    if deny_path and os.path.realpath(path) == deny_path:
        continue
    try:
        raw = open(path, "rb").read()
    except OSError:
        continue
    if b"\0" in raw:
        continue
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        continue
    for m in quad.finditer(text):
        parts = [int(x) for x in m.group(1).split(".")]
        if len(parts) == 4 and all(0 <= n <= 255 for n in parts) and private(parts):
            hits.append(f"{path}: {m.group(1)}")
    for m in dash.finditer(text):
        parts = [int(x) for x in m.group(1).split("-")]
        if len(parts) == 4 and all(0 <= n <= 255 for n in parts) and private(parts):
            hits.append(f"{path}: {m.group(1)}")
    low = text.lower()
    for needle in extra:
        if needle.lower() in low:
            hits.append(f"{path}: denylist token")
    if home_re.search(text):
        hits.append(f"{path}: home path")
if hits:
    print("\n".join(hits[:80]), file=sys.stderr)
    if len(hits) > 80:
        print(f"... {len(hits) - 80} more", file=sys.stderr)
    sys.exit(1)
PY
py_rc=$?
rm -f "$list"
if [[ "$py_rc" -ne 0 ]]; then
  report "private address, denylist token, or home path"
fi

# Literal Plex tokens. Placeholders and redactions are fine.
tok="$(grep -I -nE 'X-Plex-Token[=:] *[A-Za-z0-9_-]{16,}' "${files[@]}" 2>/dev/null \
  | grep -viE 'REDACTED|YOUR[_-]?PLEX|PLACEHOLDER|<[^>]*>|\$\{|\$[A-Za-z_]|xxxx|\.\.\.' || true)"
if [[ -n "$tok" ]]; then
  report "literal X-Plex-Token found"
  printf '%s\n' "$tok" >&2
fi

rm -rf "$string_dir"
if [[ $status -eq 0 ]]; then
  echo "test_no_private_data: OK"
fi
exit $status
