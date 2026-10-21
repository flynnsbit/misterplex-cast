#!/usr/bin/env bash
# MPX-FORK: refresh read-only vendor tree for PlexOB Quartus (never commit full upstream).
# Every git-tracked file under vendor/ is excluded, so a sync cannot overwrite a fork
# (H264.qsf, emu.sv, ring_stat.sv, and the rest). sys/ is not a fork: it comes from the pin.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
VENDOR="${OWENB_VENDOR_ROOT:?set OWENB_VENDOR_ROOT to an owenb321 MiSTer_h264_decoder checkout at eac1064}"
DEST="$ROOT/vendor"
test -f "$VENDOR/H264.qsf" || { echo "missing vendor at $VENDOR (pin eac1064)"; exit 1; }
mkdir -p "$DEST"
# Committed vendor files. rsync --delete does not remove excluded destination files.
EXCLUDES=(
  H264.qsf
  h264/cdc.sv
  h264/dec_stub.sv
  h264/disp_reader.sv
  h264/emu.sv
  h264/h264.sdc
  h264/ring_in.sv
  h264/ring_stat.sv
)
args=()
for e in "${EXCLUDES[@]}"; do
  args+=(--exclude "$e")
done
rsync -a --delete \
  "${args[@]}" \
  --exclude '.git' --exclude 'releases' --exclude 'site' --exclude 'bench' \
  --exclude 'tools/.notify_url' \
  "$VENDOR/" "$DEST/"
# An excluded file is absent from the stream, so a fresh tree still needs the committed bytes.
for e in "${EXCLUDES[@]}"; do
  if [ ! -f "$DEST/$e" ]; then
    echo "sync_vendor: missing committed fork $e (it is excluded and was not on disk)" >&2
    exit 1
  fi
done
echo "synced $VENDOR -> $DEST ($(git -C "$VENDOR" rev-parse --short HEAD 2>/dev/null || echo unknown))"
