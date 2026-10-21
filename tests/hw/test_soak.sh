#!/usr/bin/env bash
# Multi-title soak: play/stop several keys against live misterplexd.
# Does not replace short HW suite; use for longer cast-client stability.
#
# Env:
#   MISTER_HOST     set MISTER_HOST
#   MISTER_USER     default root
#   MISTER_PASS     required
#   MISTER_CONF     local conf path (PLEX_TOKEN / PLEX_BASE). If unset, tries:
#                     1) /tmp/misterplex-lab.conf
#                     2) scp from MiSTer /media/fat/misterplex/misterplex.conf
#   SOAK_KEYS       space-separated playMedia key= values (override discovery)
#   SOAK_HOLD_S     seconds to leave each title playing (default 8)
#   SOAK_ROUNDS     full passes over the key list (default 1)
#   SOAK_PROGRESS   if 1, require timeline time to advance during hold (default 0)
#   SOAK_FETCH_CONF if 0, never scp conf from MiSTer (default 1)
#   SOAK_NET_LABEL  optional label for Wi-Fi/Ethernet matrix rows (e.g. wifi|eth)
#   SOAK_LOG_NET    if 1, ssh and log active iface / wireless quality (default 1)
set -euo pipefail

HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
USER="${MISTER_USER:-root}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
BASE="http://${HOST}:3005"
HOLD_S="${SOAK_HOLD_S:-8}"
ROUNDS="${SOAK_ROUNDS:-1}"
CONF_LOCAL="${MISTER_CONF:-}"
FETCH_CONF="${SOAK_FETCH_CONF:-1}"
WANT_PROGRESS="${SOAK_PROGRESS:-0}"
NET_LABEL="${SOAK_NET_LABEL:-}"
LOG_NET="${SOAK_LOG_NET:-1}"

CURL=(curl -fsS --connect-timeout 5 --max-time 60)
CURL_STOP=(curl -fsS --connect-timeout 5 --max-time 20)

log() { printf '[soak] %s\n' "$*"; }
fail() { log "FAIL: $*"; exit 1; }

# Snapshot MiSTer net path for Wi-Fi vs Ethernet matrix rows (docs/crt-lcd-matrix.md).
log_net_snapshot() {
  [[ "$LOG_NET" == "1" ]] || return 0
  command -v sshpass >/dev/null 2>&1 || { log "net: sshpass missing — skip"; return 0; }
  local snap
  snap=$(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
    "${USER}@${HOST}" 'default_if=$(ip route 2>/dev/null | awk "/^default/ {print \$5; exit}");
      ip -4 -o addr show dev "$default_if" 2>/dev/null | awk "{print \$4}" | head -1;
      echo IFACE=$default_if;
      if [[ -n "$default_if" && -f /proc/net/wireless ]]; then
        awk -v i="$default_if" "\$1 ~ i {printf \"WL_QUAL=%s WL_LEVEL=%s\\n\", \$3, \$4}" /proc/net/wireless;
      fi
      if [[ "$default_if" == eth* ]]; then
        ethtool "$default_if" 2>/dev/null | awk "/Speed:|Duplex:/ {print}" | tr "\\n" " ";
        echo;
      fi' 2>/dev/null || true)
  if [[ -n "$snap" ]]; then
    log "net snapshot:${NET_LABEL:+ label=$NET_LABEL} $(echo "$snap" | tr '\n' ' ')"
  else
    log "net snapshot: unavailable (ssh failed)"
  fi
}

# --- conf load (local path or MiSTer lab conf) ---------------------------------
TOKEN=""
PLEX_BASE=""
CONF_USED=""

load_conf_file() {
  local f="$1"
  [[ -f "$f" ]] || return 1
  CONF_USED="$f"
  TOKEN=$(grep -E '^PLEX_TOKEN=' "$f" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '\r' || true)
  PLEX_BASE=$(grep -E '^PLEX_BASE=' "$f" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '\r' || true)
  local ph
  ph=$(grep -E '^PLEX_HOST=' "$f" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '\r' || true)
  if [[ -z "$PLEX_BASE" && -n "$ph" ]]; then
    PLEX_BASE="http://${ph}:32400"
  fi
  return 0
}

if [[ -n "$CONF_LOCAL" ]]; then
  load_conf_file "$CONF_LOCAL" || fail "MISTER_CONF not found: $CONF_LOCAL"
elif load_conf_file /tmp/misterplex-lab.conf; then
  :
elif [[ "$FETCH_CONF" == "1" ]] && command -v sshpass >/dev/null 2>&1; then
  TMP_CONF="$(mktemp /tmp/misterplex-soak-conf.XXXXXX)"
  if sshpass -p "$PASS" scp -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
      "${USER}@${HOST}:/media/fat/misterplex/misterplex.conf" "$TMP_CONF" 2>/dev/null; then
    load_conf_file "$TMP_CONF" || true
  else
    rm -f "$TMP_CONF"
  fi
fi

if [[ -n "$CONF_USED" ]]; then
  if [[ -n "$TOKEN" ]]; then
    log "conf $CONF_USED (token=set pms=${PLEX_BASE:-unset})"
  else
    log "conf $CONF_USED (token=unset pms=${PLEX_BASE:-unset})"
  fi
else
  log "no PMS conf — local-path / default keys only"
fi

# --- key discovery -------------------------------------------------------------
# Prefer real playable items when PMS conf is available; always include local mp4
# when SOAK_KEYS is unset.
DEFAULT_KEYS=(
  "/media/fat/mistercast/test.mp4"
  "/library/metadata/1"
  "/library/metadata/3"
)

discover_pms_keys() {
  local base="$1" token="$2"
  local hdr=(-H "X-Plex-Token: ${token}")
  local xml keys=()
  # Enumerate library sections dynamically (do not hardcode movie=1 / show=2).
  local sections
  sections=$("${CURL[@]}" "${hdr[@]}" "${base}/library/sections" 2>/dev/null || true)
  local sec_ids=()
  while read -r sk; do
    [[ -n "$sk" ]] && sec_ids+=("$sk")
  done < <(printf '%s' "$sections" | grep -oE 'key="[0-9]+"' | sed 's/key="//;s/"//')
  local sid
  for sid in "${sec_ids[@]}"; do
    xml=$("${CURL[@]}" "${hdr[@]}" \
      "${base}/library/sections/${sid}/all?X-Plex-Container-Size=20" 2>/dev/null || true)
    # Movies / other leaf Video
    while read -r rk; do
      [[ -n "$rk" ]] && keys+=("/library/metadata/${rk}")
    done < <(printf '%s' "$xml" | grep -oE '<Video[^>]*ratingKey="[0-9]+"' \
      | grep -oE 'ratingKey="[0-9]+"' | sed 's/ratingKey="//;s/"//' | head -5)
    # TV: Directory shows → first show seasons → episodes
    local show_rk
    show_rk=$(printf '%s' "$xml" | grep -oE '<Directory[^>]*ratingKey="[0-9]+"' \
      | grep -oE 'ratingKey="[0-9]+"' | head -1 | sed 's/ratingKey="//;s/"//' || true)
    if [[ -n "$show_rk" ]]; then
      local seas
      seas=$("${CURL[@]}" "${hdr[@]}" "${base}/library/metadata/${show_rk}/children" 2>/dev/null || true)
      local seas_rk
      seas_rk=$(printf '%s' "$seas" | grep -oE 'ratingKey="[0-9]+"' | head -1 \
        | sed 's/ratingKey="//;s/"//' || true)
      if [[ -n "$seas_rk" ]]; then
        local eps
        eps=$("${CURL[@]}" "${hdr[@]}" \
          "${base}/library/metadata/${seas_rk}/children" 2>/dev/null || true)
        while read -r rk; do
          [[ -n "$rk" ]] && keys+=("/library/metadata/${rk}")
        done < <(printf '%s' "$eps" | grep -oE 'ratingKey="[0-9]+"' \
          | sed 's/ratingKey="//;s/"//' | head -5)
      fi
      local extras
      extras=$("${CURL[@]}" "${hdr[@]}" \
        "${base}/library/metadata/${show_rk}/extras?X-Plex-Container-Size=5" 2>/dev/null || true)
      while read -r rk; do
        [[ -n "$rk" ]] && keys+=("/library/metadata/${rk}")
      done < <(printf '%s' "$extras" | grep -oE 'ratingKey="[0-9]+"' \
        | sed 's/ratingKey="//;s/"//' | head -3)
    fi
  done
  # onDeck / recentlyAdded catch thin libraries (empty Movies section, one TV ep)
  for path in library/onDeck "library/recentlyAdded?X-Plex-Container-Size=20"; do
    xml=$("${CURL[@]}" "${hdr[@]}" "${base}/${path}" 2>/dev/null || true)
    while read -r rk; do
      [[ -n "$rk" ]] && keys+=("/library/metadata/${rk}")
    done < <(printf '%s' "$xml" | grep -oE '<Video[^>]*ratingKey="[0-9]+"' \
      | grep -oE 'ratingKey="[0-9]+"' | sed 's/ratingKey="//;s/"//' | head -5)
  done
  # Dedup preserve order
  local out=() seen="|"
  for k in "${keys[@]}"; do
    case "$seen" in
      *"|$k|"*) ;;
      *) out+=("$k"); seen="${seen}${k}|" ;;
    esac
  done
  if [[ ${#out[@]} -gt 0 ]]; then
    printf '%s\n' "${out[@]}"
  fi
}

if [[ -n "${SOAK_KEYS:-}" ]]; then
  # shellcheck disable=SC2206
  KEYS=($SOAK_KEYS)
  log "SOAK_KEYS override (${#KEYS[@]})"
else
  KEYS=("/media/fat/mistercast/test.mp4")
  if [[ -n "$TOKEN" && -n "$PLEX_BASE" ]]; then
    log "discovering PMS titles at $PLEX_BASE…"
    mapfile -t DISC < <(discover_pms_keys "$PLEX_BASE" "$TOKEN" || true)
    if [[ ${#DISC[@]} -gt 0 ]]; then
      KEYS+=("${DISC[@]}")
      log "PMS discovered: ${DISC[*]}"
    else
      log "WARN: PMS discovery empty — falling back to default library stubs"
      KEYS+=("/library/metadata/3" "/library/metadata/4")
    fi
  else
    KEYS=("${DEFAULT_KEYS[@]}")
  fi
fi

# Cap extreme lists unless user overrode
if [[ -z "${SOAK_KEYS:-}" && ${#KEYS[@]} -gt 6 ]]; then
  KEYS=("${KEYS[@]:0:6}")
fi

log "resources on $HOST…"
"${CURL[@]}" "$BASE/resources" | grep -q MiSTerPlex || fail "no MiSTerPlex on $BASE/resources"
log_net_snapshot

cmd=0
ok_count=0
fail_count=0
declare -a FAIL_KEYS=()

timeline_time_ms() {
  # Extract first time="N" from Timeline XML (ms)
  printf '%s' "$1" | grep -oE 'time="[0-9]+"' | head -1 | sed 's/time="//;s/"//' || echo 0
}

play_one() {
  local key="$1"
  cmd=$((cmd + 1))
  local extra=()
  if [[ -n "$TOKEN" ]]; then
    extra+=(--data-urlencode "token=${TOKEN}")
  fi
  # Full scrubber-style bind when key looks like library metadata
  local pq_args=()
  if [[ "$key" == /library/metadata/* ]]; then
    local rk="${key##*/}"
    pq_args+=(
      --data-urlencode "containerKey=/playQueues/soak${rk}?own=1"
      --data-urlencode "playQueueItemID=${rk}"
      --data-urlencode "playQueueVersion=1"
      --data-urlencode "ratingKey=${rk}"
    )
    if [[ -n "$PLEX_BASE" ]]; then
      local hostport="${PLEX_BASE#*://}"
      hostport="${hostport%%/*}"
      local ph="${hostport%%:*}"
      local pp="${hostport##*:}"
      [[ "$pp" == "$ph" ]] && pp=32400
      pq_args+=(
        --data-urlencode "address=${ph}"
        --data-urlencode "port=${pp}"
        --data-urlencode "protocol=http"
      )
    fi
  fi
  log "play key=$key (cmd=$cmd)"
  local body
  body=$("${CURL[@]}" --get "$BASE/player/playback/playMedia" \
    --data-urlencode "key=${key}" \
    --data-urlencode "offset=0" \
    --data-urlencode "commandID=${cmd}" \
    "${pq_args[@]+"${pq_args[@]}"}" \
    "${extra[@]+"${extra[@]}"}") || { log "playMedia HTTP failed for $key"; return 1; }
  echo "$body" | grep -q Timeline || { log "playMedia no Timeline for $key"; return 1; }

  sleep 1
  cmd=$((cmd + 1))
  local poll
  poll=$("${CURL[@]}" "$BASE/player/timeline/poll?commandID=${cmd}") || { log "poll failed"; return 1; }
  echo "$poll" | grep -Eq 'state="(playing|buffering)"' || { log "not playing after play: $poll"; return 1; }
  if [[ "$key" == /library/metadata/* ]]; then
    echo "$poll" | grep -q "key=\"${key}\"" || log "WARN: poll missing key (ok for local-path resolve paths)"
  fi

  local t0
  t0=$(timeline_time_ms "$poll")

  log "hold ${HOLD_S}s…"
  # Mid-hold health: companion still up
  sleep "$HOLD_S"
  "${CURL[@]}" "$BASE/resources" | grep -q MiSTerPlex || { log "resources lost mid-hold"; return 1; }

  cmd=$((cmd + 1))
  poll=$("${CURL[@]}" "$BASE/player/timeline/poll?commandID=${cmd}") || { log "poll mid failed"; return 1; }
  echo "$poll" | grep -Eq 'state="(playing|buffering)"' || { log "dropped during hold: $poll"; return 1; }
  if [[ "$WANT_PROGRESS" == "1" ]]; then
    local t1
    t1=$(timeline_time_ms "$poll")
    if [[ "${t1:-0}" -le "${t0:-0}" ]]; then
      log "WARN: timeline did not advance (${t0}→${t1}) — continuing"
    else
      log "progress ${t0}→${t1} ms"
    fi
  fi

  cmd=$((cmd + 1))
  log "stop"
  if ! "${CURL_STOP[@]}" "$BASE/player/playback/stop?commandID=${cmd}" >/dev/null; then
    log "WARN: stop HTTP slow/failed — retry once"
    sleep 1
    cmd=$((cmd + 1))
    "${CURL_STOP[@]}" "$BASE/player/playback/stop?commandID=${cmd}" >/dev/null || {
      log "stop failed after retry"
      return 1
    }
  fi
  # Async stop: poll until navigation / idle (prePlayHold buffering@navigation)
  local ok_nav=0
  for _try in 1 2 3 4 5 6; do
    sleep 0.4
    cmd=$((cmd + 1))
    poll=$("${CURL[@]}" "$BASE/player/timeline/poll?commandID=${cmd}") || continue
    if echo "$poll" | grep -q 'location="navigation"' &&
       echo "$poll" | grep -Eq 'state="(buffering|stopped)"'; then
      ok_nav=1
      break
    fi
  done
  if [[ "$ok_nav" -ne 1 ]]; then
    log "after stop not navigation: $poll"
    return 1
  fi
  return 0
}

log "keys (${#KEYS[@]}): ${KEYS[*]}  rounds=$ROUNDS hold=${HOLD_S}s"
START_TS=$(date +%s)
for ((r = 1; r <= ROUNDS; r++)); do
  log "=== round $r/$ROUNDS ==="
  for k in "${KEYS[@]}"; do
    if play_one "$k"; then
      ok_count=$((ok_count + 1))
      log "OK title=$k"
    else
      fail_count=$((fail_count + 1))
      FAIL_KEYS+=("$k")
      log "FAIL title=$k — continuing soak"
      # Best-effort stop so next title is clean
      cmd=$((cmd + 1))
      "${CURL[@]}" "$BASE/player/playback/stop?commandID=${cmd}" >/dev/null 2>&1 || true
      sleep 0.5
    fi
  done
done
ELAPSED=$(( $(date +%s) - START_TS ))

log_net_snapshot
log "summary: ok=$ok_count fail=$fail_count elapsed=${ELAPSED}s host=$HOST${NET_LABEL:+ net=$NET_LABEL}"
if [[ "$fail_count" -gt 0 ]]; then
  log "failed keys: ${FAIL_KEYS[*]}"
  fail "soak had $fail_count failure(s)"
fi

log "OK — ${#KEYS[@]} titles × ${ROUNDS} rounds on $HOST${NET_LABEL:+ ($NET_LABEL)}"
echo "test_soak: OK on $HOST (${ok_count} plays, ${ELAPSED}s${NET_LABEL:+, $NET_LABEL})"
