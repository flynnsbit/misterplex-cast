#!/usr/bin/env bash
# MPX-FORK: Quartus in Docker with -w vendor (build_release's USE_DOCKER=1 uses git root and breaks H264.qsf).
set -euo pipefail
P="$(cd "$(dirname "$0")" && pwd)"
V="$P/vendor"
ROOT="$(git -C "$P" rev-parse --show-toplevel 2>/dev/null || true)"
export GIT_BRANCH="${GIT_BRANCH:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)}"
export GIT_SHA="${GIT_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)}"
export GIT_DIRTY="${GIT_DIRTY:-$([ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ] && echo true || echo false)}"
IMAGE="${QUARTUS_DOCKER_IMAGE:-raetro/quartus:mister}"
# Silence Discord/ntfy unless the caller explicitly opts back in with NOTIFY_SILENT=0.
export NOTIFY_SILENT="${NOTIFY_SILENT:-1}"
exec docker run --rm --name "quartus_plexob_$$" \
  -u "$(id -u):$(id -g)" -e IN_QUARTUS_DOCKER=1 -e HOME=/tmp \
  -e GIT_BRANCH -e GIT_SHA -e GIT_DIRTY -e QUARTUS_DOCKER_IMAGE="$IMAGE" \
  -e NOTIFY_SILENT \
  -v "$P:$P" -v /build -w "$V" "$IMAGE" ./build_release.sh "$@"
