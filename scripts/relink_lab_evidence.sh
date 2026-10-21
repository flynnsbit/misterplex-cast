#!/usr/bin/env bash
# Recreate the /tmp -> permanent-store symlinks for lab evidence.
#
# Orchestration logs and per-agent evidence are written to /tmp/misterplex-*
# paths by long-standing convention. /tmp is tmpfs and is cleared on reboot,
# which has already destroyed a session's history once. The real files
# therefore live under $HOME/misterplex-lab-evidence, and the /tmp paths
# are symlinks into it, so existing agents and scripts need no changes.
#
# Run this after a reboot, or any time /tmp has been cleared.
#
# Memory/ is git-ignored because it holds the lab PMS address, which
# tests/unit/test_no_private_data.sh forbids in tracked files. It sits in the
# primary clone rather than a build worktree so that `git clean -xfd` during a
# build cannot reach it; do not run `git clean -xfd` in the primary clone.

set -uo pipefail

LAB_EVIDENCE_DIR="${LAB_EVIDENCE_DIR:-$HOME/misterplex-lab-evidence}"

if [[ ! -d "$LAB_EVIDENCE_DIR" ]]; then
  echo "relink_lab_evidence: store not found: $LAB_EVIDENCE_DIR" >&2
  echo "  set LAB_EVIDENCE_DIR to override" >&2
  exit 1
fi

mkdir -p "$LAB_EVIDENCE_DIR"/{parent,agents,status,quartus}

linked=0
skipped=0

link_one() {
  local target="$1" link="$2"
  # Never clobber a real file sitting at the /tmp path: it may be evidence
  # written after the store went missing, and silently replacing it with a
  # symlink would destroy it.
  if [[ -e "$link" && ! -L "$link" ]]; then
    echo "relink_lab_evidence: REFUSING to replace real file $link" >&2
    echo "  move it into $LAB_EVIDENCE_DIR by hand, then re-run" >&2
    skipped=$((skipped + 1))
    return
  fi
  ln -sfn "$target" "$link"
  linked=$((linked + 1))
}

for f in "$LAB_EVIDENCE_DIR"/parent/*.txt; do
  [[ -e "$f" ]] || continue
  link_one "$f" "/tmp/$(basename "$f")"
done

for f in "$LAB_EVIDENCE_DIR"/agents/*.txt; do
  [[ -e "$f" ]] || continue
  link_one "$f" "/tmp/$(basename "$f")"
done

for f in "$LAB_EVIDENCE_DIR"/status/*; do
  [[ -e "$f" ]] || continue
  link_one "$f" "/tmp/$(basename "$f")"
done

# Pre-create links for lanes whose evidence file does not exist yet, so the
# first write lands in permanent storage rather than in /tmp. Writing through a
# symlink whose target is absent creates the target.
for id in rd-duck w-plxd w-scaler w-nostub w-osd w-path w-clock w-mem \
          w-fitgate w-area w-bw w-thruput; do
  link="/tmp/misterplex-agent-${id}.txt"
  [[ -e "$link" || -L "$link" ]] && continue
  link_one "$LAB_EVIDENCE_DIR/agents/misterplex-agent-${id}.txt" "$link"
done

echo "relink_lab_evidence: ${linked} link(s) -> ${LAB_EVIDENCE_DIR}"
[[ "$skipped" -gt 0 ]] && echo "relink_lab_evidence: ${skipped} skipped (real files present)" >&2
exit 0
