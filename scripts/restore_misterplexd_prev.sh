#!/usr/bin/env bash
# Restore the pre-C2 daemon saved by deploy_misterplexd.sh, then restart it.
set -euo pipefail

HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"

sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no "$USER@$HOST" bash -s -- "${PMS_URL:-}" <<'REMOTE'
set -euo pipefail
PMS_URL="${1:-}"
BIN=/media/fat/misterplex/bin/misterplexd
PREV=/media/fat/misterplex/bin/misterplexd.prev-c2
LOG=/media/fat/misterplex/misterplexd.log

if [[ ! -f "$PREV" ]]; then
  echo "No backup at $PREV" >&2
  exit 1
fi

for p in $(pidof misterplexd 2>/dev/null) $(pidof ffmpeg 2>/dev/null); do
  kill -9 "$p" 2>/dev/null || true
done
sleep 0.4
cp -f "$PREV" "$BIN"
chmod +x "$BIN"
: >"$LOG"
pms_args=()
if [[ -n "$PMS_URL" ]]; then
  pms_args=(--pms "$PMS_URL")
fi
nohup "$BIN" --name MiSTerPlex --id misterplex-183 --port 3005 \
  --conf /media/fat/misterplex/misterplex.conf \
  "${pms_args[@]}" \
  >>"$LOG" 2>&1 &
sleep 0.8
ps w | grep '[m]isterplexd' || true
wget -qO- http://127.0.0.1:3005/resources | head -c 300
echo
REMOTE

echo "Restored previous misterplexd on $HOST"
