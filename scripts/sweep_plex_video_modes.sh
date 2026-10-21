#!/usr/bin/env bash
# Sweep MiSTer [Plex] video_mode values by editing MiSTer.ini and rebooting.
#
# This intentionally touches only /media/fat/MiSTer.ini's [Plex] section.  It
# does not use vmode (not present on this lab MiSTer), does not build/deploy an
# RBF, and does not run any SPI/status tools.  Evidence is written under build/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PORT="${MISTERPLEX_PORT:-3005}"
STATUS_PORT="${MISTERPLEX_STATUS_PORT:-8090}"
EVIDENCE="${MISTERPLEX_SWEEP_EVIDENCE:-$ROOT/build/misterplex-agent-W-C1B.txt}"
DEVICE_INI="/media/fat/MiSTer.ini"
DEVICE_LOG="/media/fat/misterplex/misterplexd.log"
DEVICE_BACKUP="/media/fat/MiSTer.ini.W-C1B.pre-video-sweep.bak"
LOCAL_INI_BACKUP="$ROOT/build/MiSTer.ini.W-C1B.device-pre-sweep"
PLAY_KEY="${MISTERPLEX_PLAY_KEY:-testsrc}"
SETTLE_SECONDS="${MISTERPLEX_SETTLE_SECONDS:-8}"
MEASURE_SECONDS="${MISTERPLEX_MEASURE_SECONDS:-15}"
MAX_SECONDS="${MISTERPLEX_MAX_SECONDS:-1500}"
RESTORE_ON_EXIT="${MISTERPLEX_RESTORE_ON_EXIT:-1}"
MODE_SPEC="${MISTERPLEX_SWEEP_MODES:-5 1280x720@60 1920x1080@60}"
COMMAND_ID_BASE="${MISTERPLEX_COMMAND_ID_BASE:-87000}"

mkdir -p "$ROOT/build"
: >"$EVIDENCE"

log() {
  printf '%s\n' "$*" | tee -a "$EVIDENCE"
}

die() {
  log "ERROR: $*"
  exit 1
}

usage() {
  cat <<EOF
Usage: MISTER_PASS=REDACTED $0

Environment:
  MISTER_HOST                  MiSTer host (set MISTER_HOST)
  MISTER_USER                  SSH user (default: root)
  MISTER_PASS or SSHPASS       SSH password for sshpass -e (required)
  PMS_URL                      Plex Media Server base URL used only when the
                               script has to start misterplexd itself, e.g.
                               http://YOUR-PLEX-SERVER:32400 (default: unset,
                               daemon falls back to its own conf)
  MISTERPLEX_SWEEP_MODES       Mode specs (default: "5 1280x720@60 1920x1080@60")
  MISTERPLEX_PLAY_KEY          Plex companion play key (default: testsrc)
  MISTERPLEX_SETTLE_SECONDS    Settle time after playMedia (default: 8)
  MISTERPLEX_MEASURE_SECONDS   Fixed measurement window (default: 15)
  MISTERPLEX_MAX_SECONDS       Overall device window guard (default: 1500)
  MISTERPLEX_RESTORE_ON_EXIT   Restore [Plex] modes to 5 and reboot (default: 1)

Optional sync observations may be supplied as env vars such as:
  MISTERPLEX_HDMI_SYNC_5=PASS MISTERPLEX_VGA_SYNC_5=PASS
  MISTERPLEX_HDMI_SYNC_1280x720_60=PASS MISTERPLEX_VGA_SYNC_1280x720_60=FAIL

Evidence: $EVIDENCE
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ -n "${MISTER_PASS:-}" ]]; then
  export SSHPASS="$MISTER_PASS"
fi
[[ -n "${SSHPASS:-}" ]] || die "Set MISTER_PASS or SSHPASS before touching the device"
command -v sshpass >/dev/null 2>&1 || die "sshpass is required"
command -v curl >/dev/null 2>&1 || die "curl is required"

SSH_OPTS=(
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o ConnectTimeout=5
  -o ServerAliveInterval=5
  -o ServerAliveCountMax=2
)

ssh_m() {
  sshpass -e ssh "${SSH_OPTS[@]}" "$USER@$HOST" "$@"
}

http_get() {
  curl -fsS --connect-timeout 3 --max-time "${2:-6}" "$1"
}

START_EPOCH="$(date +%s)"
TOUCHED_DEVICE=0
RESTORING=0
MODE_COMMENTS=""
BASELINE_DROP_DELTA=""
BASELINE_ABS_DRIFT=""
PREV_FINAL_TRIPLE=""
RESULT_ROWS=()
UNREACHED_MODES=()

deadline_remaining() {
  local now
  now="$(date +%s)"
  echo $((MAX_SECONDS - (now - START_EPOCH)))
}

abs_int() {
  local v="$1"
  if [[ "$v" =~ ^- ]]; then
    echo "${v#-}"
  else
    echo "$v"
  fi
}

sanitize_label() {
  printf '%s' "$1" | tr -c '[:alnum:]' '_'
}

sync_observation() {
  local kind="$1"
  local mode="$2"
  local label="$3"
  local safe_label
  safe_label="$(sanitize_label "$label")"
  local var_mode="MISTERPLEX_${kind}_SYNC_${mode}"
  local var_label="MISTERPLEX_${kind}_SYNC_${safe_label}"
  local val="${!var_mode:-}"
  if [[ -z "$val" ]]; then
    val="${!var_label:-UNOBSERVED}"
  fi
  printf '%s' "$val"
}

fetch_mode_comments() {
  MODE_COMMENTS="$(ssh_m sed -n '1,170p' "$DEVICE_INI")"
  log "=== Device video_mode comments (first 170 lines) ==="
  printf '%s\n' "$MODE_COMMENTS" | tee -a "$EVIDENCE" >/dev/null
}

resolve_mode_token() {
  local token="$1"
  if [[ "$token" =~ ^[0-9]+$ ]]; then
    printf '%s' "$token"
    return 0
  fi

  local line idx=""
  while IFS= read -r line; do
    if [[ "$line" == *"$token"* && "$line" =~ ([0-9]+)[[:space:]]*[-=:][[:space:]]* ]]; then
      idx="${BASH_REMATCH[1]}"
      break
    fi
  done <<<"$MODE_COMMENTS"

  [[ -n "$idx" ]] || die "Could not resolve video_mode index for '$token' from device comments"
  printf '%s' "$idx"
}

label_for_token() {
  local token="$1"
  local mode="$2"
  if [[ "$token" =~ ^[0-9]+$ ]]; then
    case "$token" in
      5) printf '800x600@60-baseline' ;;
      *) printf 'mode-%s' "$token" ;;
    esac
  else
    printf '%s' "$token"
  fi
}

backup_ini() {
  log "=== Backing up $DEVICE_INI ==="
  ssh_m sh -s -- "$DEVICE_INI" "$DEVICE_BACKUP" <<'REMOTE'
set -eu
ini="$1"
backup="$2"
test -f "$ini"
if [ ! -f "$backup" ]; then
  cp -p "$ini" "$backup"
fi
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$ini" "$backup"
else
  cksum "$ini" "$backup"
fi
REMOTE
  ssh_m cat "$DEVICE_INI" >"$LOCAL_INI_BACKUP"
  log "Local ini backup: $LOCAL_INI_BACKUP"
}

read_plex_modes() {
  ssh_m awk '
    /^\[[^]]+\][[:space:]]*$/ { in_plex = ($0 ~ /^\[Plex\][[:space:]]*$/) }
    in_plex && /^[[:space:]]*(video_mode|video_mode_ntsc|video_mode_pal)[[:space:]]*=/ { print }
  ' "$DEVICE_INI"
}

apply_plex_mode() {
  local mode="$1"
  TOUCHED_DEVICE=1
  ssh_m sh -s -- "$mode" "$DEVICE_INI" <<'REMOTE'
set -eu
mode="$1"
ini="$2"
edit="/media/fat/MiSTer.ini.W-C1B.edit"
awk -v mode="$mode" '
  BEGIN {
    in_plex = 0; found = 0
    saw_video = 0; saw_ntsc = 0; saw_pal = 0
  }
  function emit_missing() {
    if (!saw_video) print "video_mode=" mode
    if (!saw_ntsc) print "video_mode_ntsc=" mode
    if (!saw_pal) print "video_mode_pal=" mode
  }
  /^\[[^]]+\][[:space:]]*$/ {
    if (in_plex) {
      emit_missing()
      in_plex = 0
    }
    if ($0 ~ /^\[Plex\][[:space:]]*$/) {
      in_plex = 1; found = 1
      saw_video = 0; saw_ntsc = 0; saw_pal = 0
    }
    print
    next
  }
  in_plex && /^[[:space:]]*video_mode[[:space:]]*=/ {
    print "video_mode=" mode
    saw_video = 1
    next
  }
  in_plex && /^[[:space:]]*video_mode_ntsc[[:space:]]*=/ {
    print "video_mode_ntsc=" mode
    saw_ntsc = 1
    next
  }
  in_plex && /^[[:space:]]*video_mode_pal[[:space:]]*=/ {
    print "video_mode_pal=" mode
    saw_pal = 1
    next
  }
  { print }
  END {
    if (in_plex)
      emit_missing()
    if (!found) {
      print "ERROR: [Plex] section not found in " ARGV[1] > "/dev/stderr"
      exit 2
    }
  }
' "$ini" >"$edit"
mv "$edit" "$ini"
sync
REMOTE
}

assert_plex_mode() {
  local mode="$1"
  local section
  section="$(read_plex_modes || true)"
  log "[Plex] after edit/reboot:"
  printf '%s\n' "$section" | tee -a "$EVIDENCE" >/dev/null
  grep -qx "video_mode=$mode" <<<"$section" &&
    grep -qx "video_mode_ntsc=$mode" <<<"$section" &&
    grep -qx "video_mode_pal=$mode" <<<"$section"
}

soft_reboot() {
  log "Soft rebooting $HOST ..."
  ssh_m sh -c 'sync; reboot' >/dev/null 2>&1 || true
  sleep 4

  local saw_down=0
  for _ in $(seq 1 20); do
    if ! ssh_m true >/dev/null 2>&1; then
      saw_down=1
      break
    fi
    sleep 1
  done
  if [[ "$saw_down" != 1 ]]; then
    log "WARN: SSH did not drop during reboot wait; continuing to wait for healthy state"
  fi

  for _ in $(seq 1 120); do
    if ssh_m true >/dev/null 2>&1; then
      log "SSH is back"
      return 0
    fi
    sleep 2
  done
  return 1
}

start_daemon_if_needed() {
  ssh_m sh -s -- "${PMS_URL:-}" <<'REMOTE'
set -eu
pms_url="$1"
if ! ps w | grep -q '[m]isterplexd'; then
  mkdir -p /media/fat/misterplex
  if [ ! -x /media/fat/misterplex/bin/misterplexd ]; then
    echo "misterplexd binary missing" >&2
    exit 3
  fi
  if [ -n "$pms_url" ]; then
    set -- --pms "$pms_url"
  else
    set --
  fi
  nohup /media/fat/misterplex/bin/misterplexd \
    --name MiSTerPlex \
    --id misterplex-183 \
    --port 3005 \
    --conf /media/fat/misterplex/misterplex.conf \
    "$@" \
    >>/media/fat/misterplex/misterplexd.log 2>&1 &
fi
REMOTE
}

wait_for_healthy() {
  log "Waiting for misterplexd health ..."
  for i in $(seq 1 90); do
    if ssh_m sh -c "ps w | grep -q '[m]isterplexd' && wget -qO- http://127.0.0.1:${PORT}/resources >/dev/null 2>&1" >/dev/null 2>&1; then
      log "misterplexd healthy"
      return 0
    fi
    if (( i == 10 || i == 30 )); then
      start_daemon_if_needed >/dev/null 2>&1 || true
    fi
    sleep 2
  done
  return 1
}

remote_log_next_line() {
  ssh_m sh -s -- "$DEVICE_LOG" <<'REMOTE'
set -eu
log="$1"
if [ -f "$log" ]; then
  lines="$(wc -l <"$log" 2>/dev/null || echo 0)"
else
  lines=0
fi
echo $((lines + 1))
REMOTE
}

append_log_marker() {
  local marker="$1"
  ssh_m sh -s -- "$DEVICE_LOG" "$marker" <<'REMOTE'
set -eu
log="$1"
marker="$2"
mkdir -p "$(dirname "$log")"
printf '\n=== W-C1B %s ===\n' "$marker" >>"$log"
REMOTE
}

stats_after_line() {
  local start_line="$1"
  ssh_m sh -s -- "$DEVICE_LOG" "$start_line" <<'REMOTE' || true
log="$1"
start="$2"
if [ -f "$log" ]; then
  tail -n +"$start" "$log" 2>/dev/null | grep 'media: frames=' | tail -1
fi
REMOTE
}

parse_stats() {
  local line="$1"
  local __frames="$2"
  local __drops="$3"
  local __drift="$4"
  local frames="NA" drops="NA" drift="NA"
  if [[ "$line" =~ frames=([0-9]+).*av_drift_ms=(-?[0-9]+).*drops=([0-9]+) ]]; then
    frames="${BASH_REMATCH[1]}"
    drift="${BASH_REMATCH[2]}"
    drops="${BASH_REMATCH[3]}"
  fi
  printf -v "$__frames" '%s' "$frames"
  printf -v "$__drops" '%s' "$drops"
  printf -v "$__drift" '%s' "$drift"
}

capture_cpu() {
  ssh_m sh -c "top -b -n2 -d2 | awk '/^CPU:/ { line = \$0 } END { print line }'" || true
}

cpu_idle_from_line() {
  local line="$1"
  if [[ "$line" =~ ([0-9]+)%[[:space:]]*idle ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  else
    printf 'NA'
  fi
}

status_8090() {
  http_get "http://${HOST}:${STATUS_PORT}/status" 4 2>/dev/null | tr '\n' ' ' || true
}

stop_playback() {
  http_get "http://${HOST}:${PORT}/player/playback/stop?commandID=$((COMMAND_ID_BASE + 999))" 5 >/dev/null 2>&1 || true
}

start_playback() {
  local command_id="$1"
  stop_playback
  sleep 1
  curl -fsS --connect-timeout 3 --max-time 8 \
    --get \
    --data-urlencode "key=${PLAY_KEY}" \
    --data-urlencode "offset=0" \
    --data-urlencode "commandID=${command_id}" \
    "http://${HOST}:${PORT}/player/playback/playMedia" >/dev/null
}

wait_for_fresh_stats() {
  local start_line="$1"
  local last_line=""
  local f d drift
  for _ in $(seq 1 20); do
    last_line="$(stats_after_line "$start_line")"
    parse_stats "$last_line" f d drift
    if [[ "$f" =~ ^[0-9]+$ && "$f" -gt 0 ]]; then
      printf '%s' "$last_line"
      return 0
    fi
    sleep 1
  done
  printf '%s' "$last_line"
  return 1
}

restore_known_good() {
  [[ "$RESTORING" == 0 ]] || return 0
  RESTORING=1
  log "=== Restore: forcing [Plex] video_mode/video_mode_ntsc/video_mode_pal to 5 ==="
  set +e
  stop_playback
  apply_plex_mode 5
  soft_reboot
  wait_for_healthy
  if assert_plex_mode 5; then
    log "RESTORE_OK: [Plex] modes are 5 and misterplexd is healthy"
  else
    log "RESTORE_FAIL: could not verify [Plex] mode 5"
  fi
  stop_playback
  set -e
}

cleanup() {
  local rc=$?
  if [[ "$RESTORE_ON_EXIT" == 1 && "$TOUCHED_DEVICE" == 1 ]]; then
    restore_known_good
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

result_status() {
  local applied="$1"
  local frames_delta="$2"
  local drops_delta="$3"
  local drift="$4"
  if [[ "$applied" != "YES" || ! "$frames_delta" =~ ^[0-9]+$ || "$frames_delta" -le 0 ]]; then
    printf 'FAIL'
    return
  fi
  if [[ -z "$BASELINE_DROP_DELTA" ]]; then
    printf 'BASELINE'
    return
  fi
  local abs_drift
  abs_drift="$(abs_int "$drift")"
  if [[ "$drops_delta" =~ ^[0-9]+$ && "$abs_drift" =~ ^[0-9]+$ &&
        "$drops_delta" -le "$BASELINE_DROP_DELTA" && "$abs_drift" -le "$BASELINE_ABS_DRIFT" ]]; then
    printf 'PASS'
  else
    printf 'WARN'
  fi
}

run_mode() {
  local token="$1"
  local ordinal="$2"
  local mode label applied hdmi_sync vga_sync
  mode="$(resolve_mode_token "$token")"
  label="$(label_for_token "$token" "$mode")"
  hdmi_sync="$(sync_observation HDMI "$mode" "$label")"
  vga_sync="$(sync_observation VGA "$mode" "$label")"

  if [[ "$(deadline_remaining)" -lt 180 ]]; then
    log "Skipping $token ($label): device window nearly exhausted"
    UNREACHED_MODES+=("$token")
    return 0
  fi

  log ""
  log "=== MODE $mode ($label) ==="
  log "Editing [Plex] only: video_mode/video_mode_ntsc/video_mode_pal=$mode"
  apply_plex_mode "$mode"
  soft_reboot || die "reboot failed for mode $mode"
  wait_for_healthy || die "misterplexd did not become healthy for mode $mode"

  if assert_plex_mode "$mode"; then
    applied="YES"
  else
    applied="NO"
  fi

  local marker start_line command_id start_stats end_stats cpu_line cpu_idle status_json
  local sf sd sdrift ef ed edrift frames_delta drops_delta status stale
  command_id=$((COMMAND_ID_BASE + ordinal))
  marker="mode=${mode} label=${label} commandID=${command_id} start=$(date -Iseconds)"
  append_log_marker "$marker"
  start_line="$(remote_log_next_line)"

  log "Starting playback key='$PLAY_KEY', settle=${SETTLE_SECONDS}s, measure=${MEASURE_SECONDS}s"
  start_playback "$command_id" || die "playMedia failed for mode $mode"
  sleep "$SETTLE_SECONDS"
  start_stats="$(wait_for_fresh_stats "$start_line" || true)"
  parse_stats "$start_stats" sf sd sdrift
  log "start_stats: ${start_stats:-NONE}"

  local measure_start elapsed remain
  measure_start="$(date +%s)"
  cpu_line="$(capture_cpu)"
  cpu_idle="$(cpu_idle_from_line "$cpu_line")"
  elapsed=$(($(date +%s) - measure_start))
  remain=$((MEASURE_SECONDS - elapsed))
  if (( remain > 0 )); then
    sleep "$remain"
  fi
  end_stats="$(stats_after_line "$start_line")"
  parse_stats "$end_stats" ef ed edrift
  status_json="$(status_8090)"
  log "end_stats: ${end_stats:-NONE}"
  log "cpu_second_sample: ${cpu_line:-NONE}"
  [[ -n "$status_json" ]] && log "status_${STATUS_PORT}: $status_json" || log "status_${STATUS_PORT}: unavailable"

  frames_delta="NA"
  drops_delta="NA"
  if [[ "$sf" =~ ^[0-9]+$ && "$ef" =~ ^[0-9]+$ ]]; then
    frames_delta=$((ef - sf))
  fi
  if [[ "$sd" =~ ^[0-9]+$ && "$ed" =~ ^[0-9]+$ ]]; then
    drops_delta=$((ed - sd))
  fi

  stale="NO"
  local final_triple="${ef}/${ed}/${edrift}"
  if [[ -n "$PREV_FINAL_TRIPLE" && "$final_triple" == "$PREV_FINAL_TRIPLE" ]]; then
    stale="YES"
    log "WARN: final frames/drops/drift identical to previous mode; treating as stale-capture suspect"
  fi
  PREV_FINAL_TRIPLE="$final_triple"

  status="$(result_status "$applied" "$frames_delta" "$drops_delta" "$edrift")"
  if [[ "$stale" == "YES" && "$status" == "PASS" ]]; then
    status="STALE_SUSPECT"
  fi
  if [[ -z "$BASELINE_DROP_DELTA" && "$status" == "BASELINE" ]]; then
    BASELINE_DROP_DELTA="$drops_delta"
    BASELINE_ABS_DRIFT="$(abs_int "$edrift")"
    log "Baseline recorded: drops_delta=$BASELINE_DROP_DELTA abs_drift=$BASELINE_ABS_DRIFT"
  fi

  RESULT_ROWS+=("$mode|$label|$applied|$hdmi_sync|$vga_sync|$sf->$ef|$sd->$ed|$sdrift->$edrift|$frames_delta|$drops_delta|$cpu_idle|$stale|$status")
  stop_playback

  if [[ "$applied" != "YES" || ! "$frames_delta" =~ ^[0-9]+$ || "$frames_delta" -le 0 ]]; then
    die "mode $mode did not produce valid fresh playback stats; stopping early"
  fi
}

main() {
  log "W-C1B Plex output-mode sweep"
  log "host=$HOST user=$USER modes='$MODE_SPEC' play_key='$PLAY_KEY'"
  log "No RBF build/deploy; edit $DEVICE_INI [Plex] only; restore-to-5 trap enabled=$RESTORE_ON_EXIT"

  backup_ini
  fetch_mode_comments

  read -r -a modes <<<"$MODE_SPEC"
  local i=0
  for token in "${modes[@]}"; do
    i=$((i + 1))
    run_mode "$token" "$i"
  done

  log ""
  log "=== SUMMARY ==="
  log "mode|label|applied|HDMI sync|VGA sync|frames|drops|av_drift_ms|frames_delta|drops_delta|CPU idle %|stale?|status"
  local row
  for row in "${RESULT_ROWS[@]}"; do
    log "$row"
  done
  if ((${#UNREACHED_MODES[@]})); then
    log "unreached_modes=${UNREACHED_MODES[*]}"
  else
    log "unreached_modes=none"
  fi
}

main "$@"
