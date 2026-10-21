#!/usr/bin/env bash
# One-window lab discriminator for source-rate limiting.
#
# Runs the same misterplexd present path twice on the MiSTer:
#   1. local synthetic FFmpeg lavfi testsrc2 (no PMS/network)
#   2. PMS library item through the normal universal-transcode playMedia path
#
# Required:
#   MISTER_HOST, MISTER_PASS, SOURCE_RATE_PMS_KEY=/library/metadata/N
#
# Optional:
#   SOURCE_RATE_FPS=24000/1001  SOURCE_RATE_DECODE=320x240
#   SOURCE_RATE_SECONDS=20      SOURCE_RATE_OFFSET_MS=0
#   SOURCE_RATE_CONF=/media/fat/misterplex/misterplex.conf
#
# Output:
#   build/source-rate-rca/<timestamp>/report.txt
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${MISTER_HOST:?set MISTER_HOST to the MiSTer hostname/IP for the measurement window}"
: "${MISTER_PASS:?set MISTER_PASS for ssh to the MiSTer}"
: "${SOURCE_RATE_PMS_KEY:?set SOURCE_RATE_PMS_KEY=/library/metadata/N for the PMS case}"

USER="${MISTER_USER:-root}"
HOST="$MISTER_HOST"
PASS="$MISTER_PASS"
DECODE="${SOURCE_RATE_DECODE:-320x240}"
FPS="${SOURCE_RATE_FPS:-24000/1001}"
DURATION="${SOURCE_RATE_SECONDS:-20}"
OFFSET_MS="${SOURCE_RATE_OFFSET_MS:-0}"
CONF="${SOURCE_RATE_CONF:-/media/fat/misterplex/misterplex.conf}"
REMOTE_BASE="${SOURCE_RATE_REMOTE_BASE:-/media/fat/misterplex/source-rate-rca}"
PLAYER_PORT="${SOURCE_RATE_PLAYER_PORT:-3005}"
PLAYER="http://${HOST}:${PLAYER_PORT}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$ROOT/build/source-rate-rca/$STAMP"
REPORT="$OUT/report.txt"

mkdir -p "$OUT"

SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$USER@$HOST")

urlenc() {
  python3 - "$1" <<'PY'
import sys, urllib.parse
print(urllib.parse.quote(sys.argv[1], safe=""))
PY
}

redact() {
  sed -E \
    -e 's#https?://[^ "]+#<URL>#g' \
    -e 's#X-Plex-Token=[^& "]+#X-Plex-Token=<redacted>#g' \
    -e 's#X-Plex-Token: [^[:space:]]+#X-Plex-Token: <redacted>#g'
}

player_get() {
  curl -fsS --connect-timeout 4 --max-time 12 "$PLAYER$1" >/dev/null
}

remote_setup() {
  "${SSH[@]}" \
    "REMOTE_BASE='$REMOTE_BASE' CONF='$CONF' DECODE='$DECODE' FPS='$FPS' PORT='$PLAYER_PORT' bash -s" <<'REMOTE'
set -euo pipefail
mkdir -p "$REMOTE_BASE"
RUN_CONF="$REMOTE_BASE/misterplexd.conf"
RUN_LOG="$REMOTE_BASE/misterplexd.log"
RUN_PID="$REMOTE_BASE/misterplexd.pid"
ORIG_CONF="$CONF"

{
  echo "PRESENT=fpga"
  echo "PRESENT_PROFILE=1"
  echo "STREAM=0"
  echo "DECODE=$DECODE"
  echo "AV_CONTENT_FPS=$FPS"
  echo "SOURCE_FPS=auto"
  echo "IDLE_SCREEN=last"
  [ -f "$ORIG_CONF" ] && cat "$ORIG_CONF"
} >"$RUN_CONF"

for p in $(pidof misterplexd 2>/dev/null) $(pidof ffmpeg 2>/dev/null); do
  kill "$p" 2>/dev/null || true
done
sleep 1
: >"$RUN_LOG"
nohup /media/fat/misterplex/bin/misterplexd \
  --name MiSTerPlexSourceRate --id source-rate-rca --port "$PORT" \
  --conf "$RUN_CONF" --decode "$DECODE" >>"$RUN_LOG" 2>&1 &
echo "$!" >"$RUN_PID"
REMOTE

  for _ in $(seq 1 60); do
    if curl -fsS --connect-timeout 1 --max-time 2 "$PLAYER/resources" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
  done
  echo "misterplexd did not come up at $PLAYER" >&2
  return 1
}

remote_restore() {
  "${SSH[@]}" "REMOTE_BASE='$REMOTE_BASE' CONF='$CONF' PORT='$PLAYER_PORT' bash -s" <<'REMOTE' || true
set +e
RUN_PID="$REMOTE_BASE/misterplexd.pid"
if [ -r "$RUN_PID" ]; then
  p=$(cat "$RUN_PID")
  [ -n "$p" ] && kill "$p" 2>/dev/null
fi
for p in $(pidof ffmpeg 2>/dev/null); do
  kill "$p" 2>/dev/null || true
done
sleep 0.5
if [ -x /media/fat/misterplex/bin/misterplexd ]; then
  nohup /media/fat/misterplex/bin/misterplexd \
    --name MiSTerPlex --id misterplex-dev --port "$PORT" \
    --conf "$CONF" >>/media/fat/misterplex/misterplexd.log 2>&1 &
fi
REMOTE
}
trap remote_restore EXIT

remote_marker() {
  "${SSH[@]}" "printf '\n=== SOURCE_RATE_CASE %s %s ===\n' '$1' \"\$(date -Iseconds)\" >>'$REMOTE_BASE/misterplexd.log'"
}

sample_cpu() {
  local label="$1"
  "${SSH[@]}" "LABEL='$label' DURATION='$DURATION' REMOTE_BASE='$REMOTE_BASE' bash -s" <<'REMOTE'
set -euo pipefail
hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
read_cpu() { awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8,$9}' /proc/stat; }
sum_proc() {
  total=0
  for p in "$@"; do
    [ -r "/proc/$p/stat" ] || continue
    t=$(awk '{print $14 + $15}' "/proc/$p/stat")
    total=$((total + t))
  done
  echo "$total"
}
mpids=$(pidof misterplexd 2>/dev/null || true)
fpids=$(pidof ffmpeg 2>/dev/null || true)
read u0 n0 s0 i0 w0 irq0 sirq0 steal0 <<EOF
$(read_cpu)
EOF
m0=$(sum_proc $mpids)
f0=$(sum_proc $fpids)
t0=$(date +%s)
sleep "$DURATION"
read u1 n1 s1 i1 w1 irq1 sirq1 steal1 <<EOF
$(read_cpu)
EOF
m1=$(sum_proc $mpids)
f1=$(sum_proc $fpids)
t1=$(date +%s)
elapsed=$((t1 - t0))
[ "$elapsed" -gt 0 ] || elapsed=1
total0=$((u0+n0+s0+i0+w0+irq0+sirq0+steal0))
total1=$((u1+n1+s1+i1+w1+irq1+sirq1+steal1))
dt=$((total1 - total0))
[ "$dt" -gt 0 ] || dt=1
onecpu() { awk -v d="$1" -v hz="$hz" -v e="$elapsed" 'BEGIN{printf "%.1f", (100.0*d)/(hz*e)}'; }
pct() { awk -v d="$1" -v t="$dt" 'BEGIN{printf "%.1f", (100.0*d)/t}'; }
echo "CPU case=$LABEL seconds=$elapsed misterplexd_pct_onecpu=$(onecpu $((m1-m0))) ffmpeg_pct_onecpu=$(onecpu $((f1-f0))) usr_pct=$(pct $((u1-u0+n1-n0))) sys_pct=$(pct $((s1-s0))) idle_pct=$(pct $((i1-i0+w1-w0))) sirq_pct=$(pct $((sirq1-sirq0))) mpids='$mpids' ffmpeg_pids='$fpids'"
base=$(awk -F= '/^PLEX_BASE=/{print $2; exit}' "$REMOTE_BASE/misterplexd.conf" 2>/dev/null || true)
tok=$(awk -F= '/^PLEX_TOKEN=/{print $2; exit}' "$REMOTE_BASE/misterplexd.conf" 2>/dev/null || true)
if [ "$LABEL" = "pms" ] && [ -n "$base" ]; then
  url="${base%/}/transcode/sessions"
  if command -v curl >/dev/null 2>&1; then
    body=$(curl -fsS --connect-timeout 3 --max-time 6 -H "X-Plex-Token: $tok" "$url" 2>/dev/null || true)
  else
    body=$(wget -qO- --header="X-Plex-Token: $tok" "$url" 2>/dev/null || true)
  fi
  [ -n "$body" ] && printf 'PMS_TRANSCODE_SESSIONS %s\n' "$body" | tr '\n' ' '; echo
fi
REMOTE
}

extract_case_log() {
  local label="$1"
  "${SSH[@]}" "LABEL='$label' REMOTE_BASE='$REMOTE_BASE' bash -s" <<'REMOTE' | redact
set -euo pipefail
awk -v label="$LABEL" '
  $0 ~ "=== SOURCE_RATE_CASE " label " " {show=1}
  show && /media: (spawn single-process|resolved|PLAY |frames=|present_profile|short read|A\/V origin|STREAM=0)/ {print}
' "$REMOTE_BASE/misterplexd.log"
REMOTE
}

run_case() {
  local label="$1" key="$2" enc
  enc="$(urlenc "$key")"
  remote_marker "$label"
  player_get "/player/playback/stop?commandID=source-rate-pre-$label" || true
  sleep 1
  player_get "/player/playback/playMedia?key=$enc&offset=$OFFSET_MS&commandID=source-rate-$label"
  sleep 3
  sample_cpu "$label" | redact | tee "$OUT/${label}_cpu.txt"
  player_get "/player/playback/stop?commandID=source-rate-stop-$label" || true
  sleep 1
  extract_case_log "$label" >"$OUT/${label}_log.txt"
}

{
  echo "source-rate RCA run $STAMP"
  echo "decode=$DECODE fps=$FPS seconds=$DURATION pms_key=<redacted>"
  echo
  echo "Interpretation:"
  echo "- If local testsrc read_us_f and ffmpeg CPU are low while PMS read_us_f is high and/or PMS TranscodeSession shows throttled/speed near 1.x, upstream delivery is limiting."
  echo "- If local and PMS show the same read_us_f, vfps, drops, and CPU profile, the ARM present path remains the bottleneck."
  echo
} >"$REPORT"

remote_setup
run_case "local" "testsrc"
run_case "pms" "$SOURCE_RATE_PMS_KEY"

{
  echo "== CPU =="
  cat "$OUT/local_cpu.txt"
  cat "$OUT/pms_cpu.txt"
  echo
  echo "== local log =="
  cat "$OUT/local_log.txt"
  echo
  echo "== pms log =="
  cat "$OUT/pms_log.txt"
} >>"$REPORT"

echo "report: $REPORT"
