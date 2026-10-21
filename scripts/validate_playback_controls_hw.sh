#!/usr/bin/env bash
# End-to-end lab validation for MiSTerPlex playback controls.
# This script is meant for the single deploy-token window: it can safely deploy
# one candidate RBF (Menu bounce exactly once), stage the daemon/helper, then run
# the scriptable checks while the operator performs the keyboard/controller
# presses and eyes-on observations.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
PORT="${MISTERPLEX_PORT:-3005}"
# Optional: point the daemon at a specific Plex Media Server, e.g.
#   PMS_URL=http://YOUR-PLEX-SERVER:32400 ./scripts/validate_playback_controls_hw.sh
# When unset the daemon uses whatever is configured in misterplex.conf.
PMS_URL="${PMS_URL:-}"
PMS_ARG=""
[[ -n "$PMS_URL" ]] && PMS_ARG="--pms $PMS_URL"
STATE_DIR="${PLAYBACK_VALIDATE_DIR:-$ROOT/build/playback-controls-hw}"
ROLLBACK_DIR="$STATE_DIR/rollback"
REMOTE_DIR="/media/fat/misterplex/validation"
REMOTE_PROBE="$REMOTE_DIR/input_mailbox_probe"
REMOTE_PUSH="$REMOTE_DIR/push_frame"
REMOTE_KEYS="$REMOTE_DIR/osd_keys.py"
REMOTE_EDGE="$REMOTE_DIR/edge_markers.yuv420p"
LOCAL_EDGE="$STATE_DIR/edge_markers.yuv420p"
MODE="run"
RBF=""
DAEMON=""
ASSUME_YES=0
FAILS=0
MANUALS=0

SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=12 -o ServerAliveInterval=3 "$USER@$HOST")
SCP=(sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -o ConnectTimeout=12)

usage() {
  cat <<USAGE
usage:
  $0 run --build 0xID [--rbf path/to/Plex.rbf] [--daemon path/to/misterplexd] [--yes]
  $0 rollback [--yes]

Environment:
  MISTER_HOST=$HOST, MISTER_PASS, HDMI_DEV=/dev/video4
  PLX_BUILD=0xID is the same as --build: the FPGA_VIDEO_BUILD_ID of the loaded RBF
    (daemon log "media: FPGA build 0x..."). input_mailbox_probe takes the PLX
    page from its fpga_build_ids.hpp row; an unknown build is refused.
  PMS_BASE, PMS_TOKEN, PMS_RATING_KEY enable scripted PMS resume verification.
  MAX_ABS_AV_DRIFT_MS=250 MAX_DROPS_DELTA=10 MAX_CPU_PCT=95 tune no-regression gates.

Notes:
  --rbf deploys with exactly: DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh <rbf>
  --daemon stages/restarts the ARM daemon after backing up the current one.
  No device writes happen until this script is run by the deploy-token holder.
USAGE
}

PLX_BUILD="${PLX_BUILD:-}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    run|rollback) MODE="$1"; shift ;;
    --build) PLX_BUILD="$2"; shift 2 ;;
    --rbf) RBF="$2"; shift 2 ;;
    --daemon) DAEMON="$2"; shift 2 ;;
    --yes|-y) ASSUME_YES=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

mkdir -p "$STATE_DIR" "$ROLLBACK_DIR"

log() { printf '\n== %s ==\n' "$*"; }
pass() { printf 'PASS: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; FAILS=$((FAILS + 1)); }
manual() { printf 'MANUAL: %s\n' "$*"; MANUALS=$((MANUALS + 1)); }

confirm() {
  local prompt="$1"
  if [[ "$ASSUME_YES" == 1 ]]; then
    printf 'CHECK: %s [assumed yes]\n' "$prompt"
    return 0
  fi
  local ans
  printf '\n%s [y/N] ' "$prompt"
  read -r ans
  [[ "$ans" == y || "$ans" == Y || "$ans" == yes || "$ans" == YES ]]
}

remote() { "${SSH[@]}" "$@"; }
remote_sh() { "${SSH[@]}" 'bash -s'; }

curl_timeline() {
  curl -fsS "http://$HOST:$PORT/player/timeline/poll?commandID=hw$(date +%s%N)&wait=${1:-0}"
}

xml_attr() {
  local xml="$1" name="$2"
  sed -n "s/.*${name}=\"\([^\"]*\)\".*/\1/p" <<<"$xml" | head -1
}

timeline_state() { xml_attr "$1" state; }
timeline_time() { xml_attr "$1" time; }
timeline_location() { xml_attr "$1" location; }
timeline_duration() { xml_attr "$1" duration; }

wait_timeline_pred() {
  local label="$1" pred="$2" deadline=$((SECONDS + ${3:-5})) xml
  while (( SECONDS < deadline )); do
    xml="$(curl_timeline 0 || true)"
    if [[ -n "$xml" ]] && eval "$pred"; then
      pass "$label: state=$(timeline_state "$xml") time=$(timeline_time "$xml") location=$(timeline_location "$xml")"
      return 0
    fi
    sleep 0.2
  done
  xml="$(curl_timeline 0 || true)"
  fail "$label: condition not met; last timeline: $xml"
  return 1
}

build_helpers() {
  log "Building ARM validation helpers"
  export PATH="${PATH}:${ARM_TOOLCHAIN_BIN:-/opt/arm-gnu-toolchain/bin}"
  make -C "$ROOT" "$ROOT/build/arm/input_mailbox_probe" >/dev/null
  make -C "$ROOT" arm-plexd >/dev/null
  pass "ARM helper build"
}

stage_helpers() {
  log "Staging DDR-only mailbox probe and DDR push helper"
  remote "mkdir -p '$REMOTE_DIR'"
  "${SCP[@]}" "$ROOT/build/arm/input_mailbox_probe" "$ROOT/build/arm/push_frame" \
    "$ROOT/tests/hw/osd_keys.py" "$USER@$HOST:$REMOTE_DIR/" >/dev/null
  remote "chmod +x '$REMOTE_PROBE' '$REMOTE_PUSH' '$REMOTE_KEYS'"
  pass "helpers staged under $REMOTE_DIR"
}

backup_current() {
  log "Backing up current lab state for rollback"
  mkdir -p "$ROLLBACK_DIR"
  "${SCP[@]}" "$USER@$HOST:/media/fat/_Utility/Plex.rbf" "$ROLLBACK_DIR/Plex.rbf" >/dev/null || fail "backup current Plex.rbf"
  "${SCP[@]}" "$USER@$HOST:/media/fat/misterplex/bin/misterplexd" "$ROLLBACK_DIR/misterplexd" >/dev/null || fail "backup current misterplexd"
  [[ -f "$ROLLBACK_DIR/Plex.rbf" && -f "$ROLLBACK_DIR/misterplexd" ]] && pass "rollback payload saved in $ROLLBACK_DIR"
}

restart_daemon_payload() {
  local bin="$1"
  "${SCP[@]}" "$bin" "$USER@$HOST:/media/fat/misterplex/bin/misterplexd" >/dev/null
  remote_sh <<REMOTE
set -e
chmod +x /media/fat/misterplex/bin/misterplexd
killall misterplexd 2>/dev/null || true
for i in 1 2 3 4 5 6 7 8; do
  ps | grep -v grep | grep -q '[m]isterplexd' || break
  sleep 0.25
done
if ps | grep -v grep | grep -q '[m]isterplexd'; then killall -9 misterplexd 2>/dev/null || true; fi
: >/media/fat/misterplex/misterplexd.log
nohup /media/fat/misterplex/bin/misterplexd --name MiSTerPlex --id misterplex-${PORT} --port ${PORT} \\
  --conf /media/fat/misterplex/misterplex.conf ${PMS_ARG} \\
  >>/media/fat/misterplex/misterplexd.log 2>&1 &
sleep 0.8
ps w | grep '[m]isterplexd'
REMOTE
}

deploy_candidate() {
  if [[ -n "$RBF" ]]; then
    log "Deploying candidate RBF with one safe Menu bounce"
    DEPLOY_LOAD=menu "$ROOT/scripts/deploy_plex_core.sh" "$RBF"
    pass "candidate RBF deploy returned success (exit 3/4 would be trusted over md5)"
  else
    manual "No --rbf supplied; assuming parent already deployed the candidate core with one DEPLOY_LOAD=menu bounce"
  fi
  if [[ -n "$DAEMON" ]]; then
    log "Deploying candidate daemon"
    restart_daemon_payload "$DAEMON"
    pass "candidate daemon restarted"
  else
    manual "No --daemon supplied; assuming candidate misterplexd is already running"
  fi
}

rollback() {
  log "ROLLBACK: restoring last known-good RBF + daemon"
  [[ -f "$ROLLBACK_DIR/Plex.rbf" ]] || { echo "missing $ROLLBACK_DIR/Plex.rbf" >&2; exit 1; }
  [[ -f "$ROLLBACK_DIR/misterplexd" ]] || { echo "missing $ROLLBACK_DIR/misterplexd" >&2; exit 1; }
  DEPLOY_LOAD=menu "$ROOT/scripts/deploy_plex_core.sh" "$ROLLBACK_DIR/Plex.rbf"
  restart_daemon_payload "$ROLLBACK_DIR/misterplexd"
  pass "rollback restored $ROLLBACK_DIR payload"
}

probe_once() {
  remote "$REMOTE_PROBE --build '$PLX_BUILD' --once" 2>&1 || true
}

inject_key() {
  local key="$1"
  remote "python3 '$REMOTE_KEYS' '$key'"
}

press_and_probe() {
  local label="$1" expect="$2" prompt="$3" key="${4:-}"
  log "$label"
  printf '%s\n' "$prompt"
  if [[ -n "$key" ]]; then
    if remote "('$REMOTE_PROBE' --build '$PLX_BUILD' --expect '$expect' --timeout-ms 12000 --settle-ms 800 & p=\$!; sleep 1; python3 '$REMOTE_KEYS' '$key' >'$REMOTE_DIR/osd_keys_${label// /_}.log' 2>&1; wait \$p; rc=\$?; cat '$REMOTE_DIR/osd_keys_${label// /_}.log'; exit \$rc)" | tee "$STATE_DIR/${label// /_}.log"; then
      pass "$label: injected $key; mailbox cmd_seq advanced exactly once as $expect"
    else
      fail "$label: injected $key but mailbox did not report exactly one $expect event"
    fi
    return
  fi
  if remote "$REMOTE_PROBE --build '$PLX_BUILD' --expect '$expect' --timeout-ms 12000 --settle-ms 800" | tee "$STATE_DIR/${label// /_}.log"; then
    pass "$label: mailbox cmd_seq advanced exactly once as $expect"
  else
    fail "$label: mailbox did not report exactly one $expect event"
  fi
}

check_osd_load_lines() {
  log "1. F12 OSD Load-line regression check"
  cat <<'TEXT'
Open F12 OSD. The three file slots must be clean, readable Load entries, not split/garbled:
  - Load RGB565 frame (*.raw / RGB565 frame wording fits on one line)
  - Load PCM audio (*.raw / s16le stereo wording fits on one line)
  - Load H.264 stream (*.h264 / Annex-B wording fits on one line)
FAIL if you see the old broken fragments: "*.raw,RGB,565, fr,ame", "*.raw, s1,6le , st,ere,o", or "*.H.2,64 ,ann,ex-,B e,...".
TEXT
  local cap="$STATE_DIR/osd_f12.png" ocr="$STATE_DIR/osd_f12_ocr.txt" keylog="$STATE_DIR/osd_f12_keys.log"
  log "1a. Automated F12 injection + capture"
  remote "python3 '$REMOTE_KEYS' --hold 14 f12" >"$keylog" 2>&1 &
  local keypid=$!
  sleep 8
  if ffmpeg -hide_banner -loglevel error -f v4l2 -input_format yuyv422 \
      -video_size 1920x1080 -i "${HDMI_DEV:-/dev/video4}" \
      -vf 'select=gte(n\,60)' -frames:v 1 -y "$cap"; then
    pass "captured F12 OSD evidence at $cap"
    if command -v tesseract >/dev/null 2>&1; then
      tesseract "$cap" stdout --psm 6 >"$ocr" 2>&1 || true
      if grep -Eiq 'RGB.*frame' "$ocr" &&
         grep -Eiq '(s.?16le|stereo).*(48k|PCM)|48k.*stereo' "$ocr" &&
         grep -Eiq 'H.?264.*annex.?B|annex.?B.*elementary' "$ocr"; then
        pass "OCR recognized the three corrected Load lines ($ocr)"
        wait "$keypid" || true
        return
      fi
      manual "F12 OSD captured at $cap, but OCR was inconclusive; inspect $ocr."
    else
      manual "F12 OSD captured at $cap; install tesseract locally for scripted OCR."
    fi
  else
    fail "could not capture F12 OSD via ${HDMI_DEV:-/dev/video4}"
  fi
  wait "$keypid" || true
  if confirm "Do all three F12 Load lines render cleanly as described?"; then pass "F12 Load lines clean"; else fail "F12 Load lines still garbled"; fi
}

check_mailbox_live() {
  log "2. Input mailbox live proof"
  echo "Initial mailbox read:"
  probe_once | tee "$STATE_DIR/mailbox_initial.log"
  press_and_probe "mailbox live first Space" playpause "Inject Space once now (no active media required)." space
  press_and_probe "mailbox live second Space" playpause "Inject Space once more; this proves seq/cmd_seq advance on the live bitstream." space
}

require_real_playback() {
  log "Prepare real PMS playback"
  cat <<'TEXT'
Cast a REAL library item (not testsrc) from the Plex phone/web app to MiSTerPlex.
Seek to at least 60 seconds in so Skip Back can be distinguished from clamp-to-zero.
Keep the casting app visible for the Companion sync check.
TEXT
  if ! confirm "Real library item is playing at >=60s and the controller app is visible?"; then
    fail "real PMS playback not ready"
    return 1
  fi
  wait_timeline_pred "real playback timeline" '[[ "$(timeline_state "$xml")" == "playing" || "$(timeline_state "$xml")" == "paused" ]] && [[ "$(timeline_duration "$xml")" =~ ^[0-9]+$ ]] && (( $(timeline_duration "$xml") > 0 ))' 8 || true
}

check_companion_wait_poll() {
  local label="$1" expect="$2" xml state time loc start end delta
  start=$(date +%s%3N)
  xml="$(curl_timeline 1 || true)"
  end=$(date +%s%3N)
  delta=$((end - start))
  state=$(timeline_state "$xml")
  time=$(timeline_time "$xml")
  loc=$(timeline_location "$xml")
  if eval "$expect"; then
    pass "$label Companion timeline wait=1 reflected local press in ${delta}ms (state=$state time=$time location=$loc)"
  else
    fail "$label Companion timeline mismatch after wait=1 (${delta}ms): $xml"
  fi
}

keyboard_controls() {
  log "3. Keyboard controls end-to-end"
  press_and_probe "keyboard PlayPause pause" playpause "Inject keyboard Space once; video should pause." space
  wait_timeline_pred "keyboard pause transport" '[[ "$(timeline_state "$xml")" == "paused" ]]' 5 || true
  check_companion_wait_poll "keyboard pause" '[[ "$state" == "paused" ]]'

  press_and_probe "keyboard PlayPause resume" playpause "Inject keyboard Space once; video should resume." space
  wait_timeline_pred "keyboard resume transport" '[[ "$(timeline_state "$xml")" == "playing" || "$(timeline_state "$xml")" == "buffering" ]]' 5 || true

  local before after dur
  before=$(timeline_time "$(curl_timeline 0 || true)"); before=${before:-0}
  press_and_probe "keyboard Skip Forward" skipforward "Inject keyboard Right Arrow once." right
  wait_timeline_pred "keyboard skip forward transport" 'after=$(timeline_time "$xml"); [[ "$after" =~ ^[0-9]+$ ]] && (( after >= before + 10000 || after == $(timeline_duration "$xml") ))' 8 || true

  before=$(timeline_time "$(curl_timeline 0 || true)"); before=${before:-0}
  press_and_probe "keyboard Skip Back" skipback "Inject keyboard Left Arrow once." left
  wait_timeline_pred "keyboard skip back transport" 'after=$(timeline_time "$xml"); [[ "$after" =~ ^[0-9]+$ ]] && (( after <= before - 5000 || after == 0 ))' 8 || true

  press_and_probe "keyboard Stop" stop "Inject keyboard Esc once." esc
  wait_timeline_pred "keyboard stop transport" '[[ "$(timeline_location "$xml")" == "navigation" || "$(timeline_state "$xml")" == "stopped" || "$(timeline_state "$xml")" == "buffering" ]]' 5 || true
}

controller_controls() {
  log "3b. Controller controls end-to-end"
  cat <<'TEXT'
Open F12 -> Define buttons and map controller buttons for:
  Play/Pause, Stop, Skip Fwd, Skip Back.
Then cast/resume the same real library item again at >=60s.
TEXT
  if ! confirm "Controller buttons are mapped and real playback is running at >=60s?"; then fail "controller setup not ready"; return; fi

  press_and_probe "controller PlayPause pause" playpause "Press mapped controller Play/Pause once."
  wait_timeline_pred "controller pause transport" '[[ "$(timeline_state "$xml")" == "paused" ]]' 5 || true
  press_and_probe "controller PlayPause resume" playpause "Press mapped controller Play/Pause once."
  wait_timeline_pred "controller resume transport" '[[ "$(timeline_state "$xml")" == "playing" || "$(timeline_state "$xml")" == "buffering" ]]' 5 || true

  local before after
  before=$(timeline_time "$(curl_timeline 0 || true)"); before=${before:-0}
  press_and_probe "controller Skip Forward" skipforward "Press mapped controller Skip Fwd once."
  wait_timeline_pred "controller skip forward transport" 'after=$(timeline_time "$xml"); [[ "$after" =~ ^[0-9]+$ ]] && (( after >= before + 10000 || after == $(timeline_duration "$xml") ))' 8 || true

  before=$(timeline_time "$(curl_timeline 0 || true)"); before=${before:-0}
  press_and_probe "controller Skip Back" skipback "Press mapped controller Skip Back once."
  wait_timeline_pred "controller skip back transport" 'after=$(timeline_time "$xml"); [[ "$after" =~ ^[0-9]+$ ]] && (( after <= before - 5000 || after == 0 ))' 8 || true

  press_and_probe "controller Stop" stop "Press mapped controller Stop once."
  wait_timeline_pred "controller stop transport" '[[ "$(timeline_location "$xml")" == "navigation" || "$(timeline_state "$xml")" == "stopped" || "$(timeline_state "$xml")" == "buffering" ]]' 5 || true
}

check_overlay() {
  log "4. Overlay eyes-on"
  cat <<'TEXT'
During the Play/Pause and Skip checks, the overlay must:
  - show the correct state icon (pause while paused, play while playing),
  - show progress/time near the current position,
  - flash the correct skip direction/delta for Right/Left,
  - auto-hide without leaving dirty pixels.
TEXT
  if confirm "Overlay behavior matched the checklist and auto-hid cleanly?"; then pass "overlay eyes-on"; else fail "overlay eyes-on failed"; fi
}

check_pms_progress() {
  log "6. PMS progress persistence"
  if [[ -n "${PMS_BASE:-}" && -n "${PMS_TOKEN:-}" && -n "${PMS_RATING_KEY:-}" ]]; then
    sleep 3
    local xml view
    xml=$(curl -fsS "${PMS_BASE%/}/library/metadata/$PMS_RATING_KEY?X-Plex-Token=$PMS_TOKEN" || true)
    view=$(sed -n 's/.*viewOffset="\([0-9][0-9]*\)".*/\1/p' <<<"$xml" | head -1)
    if [[ "$view" =~ ^[0-9]+$ && "$view" -gt 0 ]]; then pass "PMS viewOffset persisted ($view ms)"; else fail "PMS viewOffset missing/zero for ratingKey $PMS_RATING_KEY"; fi
  else
    manual "Set PMS_BASE/PMS_TOKEN/PMS_RATING_KEY for scripted PMS check, or confirm in Plex UI that the stopped real item shows Resume/On Deck."
    if confirm "Plex UI shows Resume/On Deck for the stopped real library item?"; then pass "PMS progress eyes-on"; else fail "PMS progress did not persist"; fi
  fi
}

check_no_regression() {
  log "7. No-regression checks: drift/drops/CPU"
  local logf="$STATE_DIR/misterplexd_tail.log"
  remote "tail -n 300 /media/fat/misterplex/misterplexd.log" > "$logf" || true
  local max_abs=0 max_drops=0 v d
  while read -r v; do
    [[ -z "$v" ]] && continue
    [[ "$v" == -* ]] && v=$(( -v ))
    (( v > max_abs )) && max_abs=$v
  done < <(grep -oE 'av_drift_ms=-?[0-9]+' "$logf" | sed 's/.*=//')
  while read -r d; do
    [[ -z "$d" ]] && continue
    (( d > max_drops )) && max_drops=$d
  done < <(grep -oE 'drops=[0-9]+' "$logf" | sed 's/.*=//')
  if (( max_abs <= ${MAX_ABS_AV_DRIFT_MS:-250} )); then pass "av_drift_ms max_abs=$max_abs"; else fail "av_drift_ms max_abs=$max_abs exceeds ${MAX_ABS_AV_DRIFT_MS:-250}"; fi
  if (( max_drops <= ${MAX_DROPS_DELTA:-10} )); then pass "drops max=$max_drops"; else fail "drops max=$max_drops exceeds ${MAX_DROPS_DELTA:-10}"; fi

  local cpu
  cpu=$(remote_sh <<'REMOTE' || true
pid=$(pidof misterplexd | awk '{print $1}')
[ -n "$pid" ] || exit 1
hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
read ut1 st1 < <(awk '{print $14, $15}' /proc/$pid/stat)
read c1 < /proc/stat
sleep 2
read ut2 st2 < <(awk '{print $14, $15}' /proc/$pid/stat)
read c2 < /proc/stat
# shellcheck disable=SC2086
set -- $c1; shift; total1=0; for x in "$@"; do total1=$((total1+x)); done
# shellcheck disable=SC2086
set -- $c2; shift; total2=0; for x in "$@"; do total2=$((total2+x)); done
proc=$(( (ut2+st2) - (ut1+st1) ))
total=$(( total2 - total1 ))
[ "$total" -gt 0 ] || total=1
awk -v p="$proc" -v t="$total" 'BEGIN { printf "%.1f", (100.0*p)/t }'
REMOTE
)
  if [[ -n "$cpu" ]]; then
    awk -v c="$cpu" -v m="${MAX_CPU_PCT:-95}" 'BEGIN { exit !(c <= m) }' && pass "misterplexd CPU ${cpu}%" || fail "misterplexd CPU ${cpu}% exceeds ${MAX_CPU_PCT:-95}%"
  else
    fail "could not sample misterplexd CPU"
  fi
}

check_edges() {
  log "7b. G-VID1 edge alignment"
  EDGE_CAP="$STATE_DIR/edge_prev.png" HDMI_DEV="${HDMI_DEV:-/dev/video4}" \
    python3 "$ROOT/scripts/check_edges.py" --capture-only --out "$STATE_DIR/edge_prev.png"
  python3 "$ROOT/scripts/gen_edge_markers.py" --format yuv420p "$LOCAL_EDGE"
  "${SCP[@]}" "$LOCAL_EDGE" "$USER@$HOST:$REMOTE_EDGE" >/dev/null
  remote "'$REMOTE_PUSH' --ddr --yuv420p 320x240 '$REMOTE_EDGE'"
  EDGE_CAP="$STATE_DIR/edge_cap.png" HDMI_DEV="${HDMI_DEV:-/dev/video4}" \
    python3 "$ROOT/scripts/check_edges.py" --previous "$STATE_DIR/edge_prev.png" \
    --out "$STATE_DIR/edge_cap.png" | tee "$STATE_DIR/check_edges.log"
  if grep -q 'PASS: all four edges correct' "$STATE_DIR/check_edges.log"; then pass "G-VID1 all four edges correct"; else fail "G-VID1 edge check failed"; fi
}

run_all() {
  if [[ -z "$PLX_BUILD" ]]; then
    echo "run needs --build 0xID (or PLX_BUILD): the PLX mailbox page comes from the" \
         "build's fpga_build_ids.hpp row, never a fixed address" >&2
    exit 2
  fi
  build_helpers
  backup_current
  deploy_candidate
  stage_helpers
  check_osd_load_lines
  check_mailbox_live
  require_real_playback
  keyboard_controls
  check_overlay
  check_pms_progress
  controller_controls
  check_no_regression
  check_edges
  log "Summary"
  if (( FAILS == 0 )); then
    pass "playback controls hardware validation complete (manual checks=$MANUALS)"
    exit 0
  fi
  fail "$FAILS validation item(s) failed. Run: $0 rollback"
  exit 1
}

case "$MODE" in
  rollback) rollback ;;
  run) run_all ;;
  *) usage; exit 2 ;;
esac
