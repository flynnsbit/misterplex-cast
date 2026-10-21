#!/usr/bin/env bash
# Host or on-device CLI: list Plex library sections / items via PMS API and
# optionally control local misterplexd (playMedia / timeline / stop).
# Usage:
#   scripts/plex_browse.sh                          # list library sections
#   scripts/plex_browse.sh sections
#   scripts/plex_browse.sh section <key|id>         # list items in a section
#   scripts/plex_browse.sh item <ratingKey|/library/metadata/N>
#   scripts/plex_browse.sh servers                  # show conf multi-server list
#   scripts/plex_browse.sh play <ratingKey|key>     # playMedia → misterplexd
#   scripts/plex_browse.sh status                   # timeline poll
#   scripts/plex_browse.sh stop | pause | resume
#   scripts/plex_browse.sh seek <ms>
#   scripts/plex_browse.sh step [ms] | stepBack [ms]  # relative ±10s (or custom ms)
#   scripts/plex_browse.sh next | prev                # skipNext / skipPrevious
#
# Env / conf (first wins):
#   PLEX_BASE / PLEX_SERVERS / PLEX_TOKEN
#   MISTERPLEX_PLAYER  (default 127.0.0.1:3005)
#   --conf PATH (default: ./assets/misterplex.conf.example or /media/fat/misterplex/misterplex.conf)
#   --base URL  --token TOK  --server N   (1-based index into PLEX_SERVERS)
#   --player HOST[:PORT]  --offset MS
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONF=""
BASE=""
TOKEN=""
SERVER_IDX=1
PLAYER="${MISTERPLEX_PLAYER:-127.0.0.1:3005}"
OFFSET_MS=0

usage() {
  sed -n '2,18p' "$0" | sed 's/^# \?//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    --conf) CONF="$2"; shift 2 ;;
    --base) BASE="$2"; shift 2 ;;
    --token) TOKEN="$2"; shift 2 ;;
    --server) SERVER_IDX="$2"; shift 2 ;;
    --player) PLAYER="$2"; shift 2 ;;
    --offset) OFFSET_MS="$2"; shift 2 ;;
    --) shift; break ;;
    -*)
      echo "unknown option: $1" >&2
      usage 1
      ;;
    *) break ;;
  esac
done

CMD="${1:-sections}"
shift || true

pick_conf() {
  if [[ -n "$CONF" && -f "$CONF" ]]; then
    echo "$CONF"
    return
  fi
  for c in \
    "${MISTERPLEX_CONF:-}" \
    /media/fat/misterplex/misterplex.conf \
    "$ROOT/assets/misterplex.conf.example" \
    "$HOME/.config/misterplex/misterplex.conf"; do
    [[ -n "$c" && -f "$c" ]] && { echo "$c"; return; }
  done
  echo ""
}

conf_val() {
  local key="$1" file="$2"
  [[ -f "$file" ]] || return 0
  # last non-comment match
  grep -E "^${key}=" "$file" 2>/dev/null | grep -v '^#' | tail -n1 | cut -d= -f2- || true
}

conf_all() {
  local key="$1" file="$2"
  [[ -f "$file" ]] || return 0
  grep -E "^${key}=" "$file" 2>/dev/null | grep -v '^#' | cut -d= -f2- || true
}

normalize_base() {
  local s="$1"
  s="${s%%/}"
  s="${s//$'\r'/}"
  if [[ -z "$s" ]]; then
    echo ""
    return
  fi
  if [[ "$s" != *"://"* ]]; then
    s="http://$s"
  fi
  echo "$s"
}

# Build ordered unique server list from conf + env
build_servers() {
  local file="$1"
  local list=()
  local raw=""
  if [[ -n "${PLEX_SERVERS:-}" ]]; then
    raw="$PLEX_SERVERS"
  else
    raw="$(conf_val PLEX_SERVERS "$file")"
  fi
  if [[ -n "$raw" ]]; then
    IFS=',;' read -r -a parts <<<"$raw"
    for p in "${parts[@]}"; do
      p="$(normalize_base "$(echo "$p" | xargs)")"
      [[ -n "$p" ]] && list+=("$p")
    done
  fi
  # repeated PLEX_BASE= lines
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    # allow comma-separated on one line too
    IFS=',;' read -r -a parts <<<"$line"
    for p in "${parts[@]}"; do
      p="$(normalize_base "$(echo "$p" | xargs)")"
      [[ -z "$p" ]] && continue
      local seen=0
      for e in "${list[@]+"${list[@]}"}"; do
        [[ "$e" == "$p" ]] && { seen=1; break; }
      done
      [[ $seen -eq 0 ]] && list+=("$p")
    done
  done < <(conf_all PLEX_BASE "$file")

  if [[ ${#list[@]} -eq 0 && -n "${PLEX_BASE:-}" ]]; then
    list+=("$(normalize_base "$PLEX_BASE")")
  fi
  if [[ ${#list[@]} -eq 0 ]]; then
    local host
    host="$(conf_val PLEX_HOST "$file")"
    if [[ -n "$host" ]]; then
      list+=("$(normalize_base "$host")")
    fi
  fi
  printf '%s\n' "${list[@]+"${list[@]}"}"
}

CONF_FILE="$(pick_conf)"
if [[ -z "$TOKEN" ]]; then
  TOKEN="${PLEX_TOKEN:-}"
  [[ -z "$TOKEN" && -n "$CONF_FILE" ]] && TOKEN="$(conf_val PLEX_TOKEN "$CONF_FILE")"
fi

mapfile -t SERVERS < <(build_servers "$CONF_FILE")
if [[ -n "$BASE" ]]; then
  BASE="$(normalize_base "$BASE")"
elif [[ -n "${PLEX_BASE:-}" && ${#SERVERS[@]} -eq 0 ]]; then
  BASE="$(normalize_base "$PLEX_BASE")"
elif [[ ${#SERVERS[@]} -gt 0 ]]; then
  if [[ "$SERVER_IDX" -lt 1 || "$SERVER_IDX" -gt ${#SERVERS[@]} ]]; then
    echo "server index $SERVER_IDX out of range (1..${#SERVERS[@]})" >&2
    exit 1
  fi
  BASE="${SERVERS[$((SERVER_IDX - 1))]}"
else
  # Player-only commands do not need PMS base
  case "$CMD" in
    status|stop|pause|resume|seek|step|stepForward|stepBack|ff|rw|next|prev|skipNext|skipPrevious)
      BASE=""
      ;;
    play)
      # play can still work without PMS list — misterplexd resolves
      BASE=""
      ;;
    *)
      echo "No PLEX_BASE/PLEX_SERVERS. Set env or conf." >&2
      exit 1
      ;;
  esac
fi

if [[ "$PLAYER" != *:* ]]; then
  PLAYER="${PLAYER}:3005"
fi
PLAYER_URL="http://${PLAYER}"

xml_get() {
  local path="$1"
  local url="${BASE}${path}"
  if [[ "$url" == *"?"* ]]; then
    url="${url}&X-Plex-Token=${TOKEN}"
  else
    url="${url}?X-Plex-Token=${TOKEN}"
  fi
  curl -sS -g -k -L --http1.1 --connect-timeout 6 --max-time 20 \
    -H 'Accept: application/xml' \
    -H 'X-Plex-Client-Identifier: misterplex-browse' \
    -H 'X-Plex-Product: MiSTerPlex' \
    -H 'X-Plex-Provides: player' \
    "$url"
}

player_get() {
  local path="$1"
  curl -sS -g --connect-timeout 3 --max-time 15 \
    -H 'Accept: application/xml' \
    "${PLAYER_URL}${path}"
}

# Percent-encode a path for query (minimal: / → %2F, keep alnum)
urlenc_path() {
  # Prefer python if present; else sed / only (enough for /library/metadata/N)
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"
  else
    printf '%s' "$1" | sed 's|/|%2F|g; s| |%20|g'
  fi
}

normalize_meta_key() {
  local key="$1"
  if [[ "$key" =~ ^[0-9]+$ ]]; then
    key="/library/metadata/$key"
  fi
  [[ "$key" == /* ]] || key="/library/metadata/$key"
  printf '%s' "$key"
}

# Attr extractors must not fail under set -o pipefail when the attr is absent.
xml_attr() {
  # $1=tag text  $2=attr name → value or empty
  echo "$1" | grep -oE "$2=\"[^\"]*\"" 2>/dev/null | head -1 | sed "s/^$2=\"//;s/\"$//" || true
}

# Print tag attrs: title, key, ratingKey, type (best-effort sed/grep; no xmllint required)
print_directory_rows() {
  # Each <Directory ...> on one logical line (PMS usually one line per tag open)
  grep -oE '<Directory[^>]+>' | while read -r tag; do
    title=$(xml_attr "$tag" title)
    key=$(xml_attr "$tag" key)
    rk=$(xml_attr "$tag" ratingKey)
    typ=$(xml_attr "$tag" type)
    printf 'key=%-40s ratingKey=%-10s type=%-10s title=%s\n' \
      "${key:-?}" "${rk:--}" "${typ:--}" "${title:--}"
  done
}

print_video_rows() {
  grep -oE '<Video[^>]+>' | while read -r tag; do
    title=$(xml_attr "$tag" title)
    key=$(xml_attr "$tag" key)
    rk=$(xml_attr "$tag" ratingKey)
    dur=$(xml_attr "$tag" duration)
    printf 'key=%-40s ratingKey=%-10s durationMs=%-10s title=%s\n' \
      "${key:-?}" "${rk:--}" "${dur:--}" "${title:--}"
  done
}

case "$CMD" in
  servers)
    echo "conf=${CONF_FILE:-none}"
    echo "selected_base=${BASE:-none} (server index $SERVER_IDX)"
    echo "player=$PLAYER_URL"
    if [[ ${#SERVERS[@]} -eq 0 ]]; then
      echo "(no multi-server list; using selected_base only)"
    else
      i=1
      for s in "${SERVERS[@]}"; do
        mark=""
        [[ $i -eq $SERVER_IDX ]] && mark=" *"
        echo "  [$i] $s$mark"
        i=$((i + 1))
      done
    fi
    ;;
  sections|libraries|list)
    echo "# PMS $BASE — library sections"
    xml="$(xml_get '/library/sections')"
    if ! echo "$xml" | grep -q 'MediaContainer'; then
      echo "fetch failed (check token / base). bytes=${#xml}" >&2
      exit 2
    fi
    echo "$xml" | print_directory_rows
    echo "# Tip: scripts/plex_browse.sh section <key|id>"
    echo "# Play:  scripts/plex_browse.sh play <ratingKey>   # → misterplexd $PLAYER_URL"
    echo "# Menu:  scripts/plex_menu.sh"
    ;;
  section|sec)
    SID="${1:-}"
    [[ -n "$SID" ]] || { echo "usage: $0 section <key|id>" >&2; exit 1; }
    # Accept raw id, key path, or /library/sections/N
    if [[ "$SID" =~ ^[0-9]+$ ]]; then
      PATHQ="/library/sections/${SID}/all"
    elif [[ "$SID" == /library/sections/* ]]; then
      PATHQ="$SID"
      [[ "$PATHQ" == */all ]] || PATHQ="${PATHQ%/}/all"
    else
      PATHQ="/library/sections/${SID}/all"
    fi
    echo "# PMS $BASE — section $PATHQ"
    xml="$(xml_get "$PATHQ")"
    if ! echo "$xml" | grep -q 'MediaContainer'; then
      echo "fetch failed. bytes=${#xml}" >&2
      exit 2
    fi
    # Movies/shows: mix of Video and Directory (show)
    echo "$xml" | print_directory_rows || true
    echo "$xml" | print_video_rows || true
    echo "# Tip: playMedia key=/library/metadata/<ratingKey>"
    echo "#      scripts/plex_browse.sh play <ratingKey>"
    ;;
  item|meta)
    KEY="${1:-}"
    [[ -n "$KEY" ]] || { echo "usage: $0 item <ratingKey|/library/metadata/N>" >&2; exit 1; }
    KEY="$(normalize_meta_key "$KEY")"
    echo "# PMS $BASE — $KEY"
    xml="$(xml_get "$KEY")"
    if ! echo "$xml" | grep -q 'MediaContainer'; then
      echo "fetch failed. bytes=${#xml}" >&2
      exit 2
    fi
    # Compact summary
    echo "$xml" | print_video_rows || true
    echo "$xml" | print_directory_rows || true
    # Part key for direct stream debugging
    echo "$xml" | grep -oE '<Part[^>]+>' | head -5 | while read -r tag; do
      pk=$(echo "$tag" | grep -oE 'key="[^"]*"' | head -1 | sed 's/^key="//;s/"$//')
      file=$(echo "$tag" | grep -oE 'file="[^"]*"' | head -1 | sed 's/^file="//;s/"$//')
      printf 'part_key=%s file=%s\n' "${pk:--}" "${file:--}"
    done
    # Frame-rate hints for match-source-Hz / Content FPS
    vfr=$(echo "$xml" | grep -oE 'videoFrameRate="[^"]*"' | head -1 | sed 's/^videoFrameRate="//;s/"$//')
    fr=$(echo "$xml" | grep -oE 'frameRate="[^"]*"' | head -1 | sed 's/^frameRate="//;s/"$//')
    [[ -n "$vfr" || -n "$fr" ]] && printf 'videoFrameRate=%s frameRate=%s\n' "${vfr:--}" "${fr:--}"
    ;;
  play)
    KEY="${1:-}"
    [[ -n "$KEY" ]] || { echo "usage: $0 play <ratingKey|/library/metadata/N> [--offset MS]" >&2; exit 1; }
    KEY="$(normalize_meta_key "$KEY")"
    ENC="$(urlenc_path "$KEY")"
    # Pass PMS address when known so scrubber bind + multi-server resolve pin correctly
    EXTRA=""
    if [[ -n "$BASE" ]]; then
      hostport="${BASE#*://}"
      hostport="${hostport%%/*}"
      phost="${hostport%%:*}"
      pport="${hostport##*:}"
      [[ "$pport" == "$phost" ]] && pport="32400"
      proto="http"
      [[ "$BASE" == https://* ]] && proto="https"
      EXTRA="&address=$(urlenc_path "$phost")&port=${pport}&protocol=${proto}"
    fi
    if [[ -n "$TOKEN" ]]; then
      EXTRA="${EXTRA}&token=$(urlenc_path "$TOKEN")"
    fi
    echo "# playMedia → $PLAYER_URL key=$KEY offset=${OFFSET_MS}"
    resp=$(player_get "/player/playback/playMedia?key=${ENC}&offset=${OFFSET_MS}&commandID=browse-play${EXTRA}") || {
      echo "playMedia request failed (misterplexd up on $PLAYER?)" >&2
      exit 2
    }
    if ! echo "$resp" | grep -q 'Timeline\|MediaContainer'; then
      echo "unexpected response: ${resp:0:240}" >&2
      exit 2
    fi
    echo "$resp" | tr '\n' ' ' | grep -oE 'state="[^"]*"|time="[^"]*"|duration="[^"]*"|key="[^"]*"|location="[^"]*"' | tr '\n' ' '
    echo
    ;;
  status|poll)
    resp=$(player_get "/player/timeline/poll?commandID=browse-status") || {
      echo "player unreachable at $PLAYER_URL" >&2
      exit 2
    }
    echo "# player $PLAYER_URL"
    echo "$resp" | tr '\n' ' ' | grep -oE 'state="[^"]*"|time="[^"]*"|duration="[^"]*"|seekRange="[^"]*"|key="[^"]*"|location="[^"]*"|playQueueID="[^"]*"|playQueueItemID="[^"]*"|containerKey="[^"]*"|ratingKey="[^"]*"|address="[^"]*"' | tr '\n' ' '
    echo
    # Full XML available with STATUS_XML=1
    if [[ "${STATUS_XML:-0}" == "1" ]]; then
      echo "$resp"
    fi
    ;;
  stop)
    player_get "/player/playback/stop?commandID=browse-stop" | grep -q Timeline
    echo "stop OK"
    ;;
  pause)
    player_get "/player/playback/pause?commandID=browse-pause" | grep -q Timeline
    echo "pause OK"
    ;;
  resume|play-ctrl)
    player_get "/player/playback/play?commandID=browse-resume" | grep -q Timeline
    echo "resume OK"
    ;;
  seek)
    MS="${1:-}"
    [[ -n "$MS" ]] || { echo "usage: $0 seek <ms>" >&2; exit 1; }
    # Non-negative integer ms only (companion also clamps; reject junk early).
    if [[ ! "$MS" =~ ^[0-9]+$ ]]; then
      echo "seek: need non-negative integer ms, got: $MS" >&2
      exit 1
    fi
    player_get "/player/playback/seekTo?offset=${MS}&commandID=browse-seek" | grep -q Timeline
    echo "seek OK offset=${MS}"
    ;;
  step|stepForward|ff)
    # Relative +N ms (default 10000). Companion stepForward accepts optional offset= (cap 120s).
    MS="${1:-10000}"
    if [[ ! "$MS" =~ ^[0-9]+$ ]] || [[ "$MS" -eq 0 ]]; then
      echo "step: need positive integer ms, got: $MS" >&2
      exit 1
    fi
    # Match companion clamp: relative step size max 120000 ms.
    if [[ "$MS" -gt 120000 ]]; then
      MS=120000
    fi
    EXTRA=""
    [[ "$MS" != "10000" ]] && EXTRA="&offset=${MS}"
    player_get "/player/playback/stepForward?commandID=browse-step${EXTRA}" | grep -q Timeline
    echo "stepForward OK deltaMs=${MS}"
    ;;
  stepBack|rw)
    MS="${1:-10000}"
    if [[ ! "$MS" =~ ^[0-9]+$ ]] || [[ "$MS" -eq 0 ]]; then
      echo "stepBack: need positive integer ms, got: $MS" >&2
      exit 1
    fi
    if [[ "$MS" -gt 120000 ]]; then
      MS=120000
    fi
    EXTRA=""
    [[ "$MS" != "10000" ]] && EXTRA="&offset=${MS}"
    player_get "/player/playback/stepBack?commandID=browse-stepBack${EXTRA}" | grep -q Timeline
    echo "stepBack OK deltaMs=-${MS}"
    ;;
  next|skipNext)
    player_get "/player/playback/skipNext?commandID=browse-skipNext" | grep -q Timeline
    echo "skipNext OK"
    ;;
  prev|skipPrevious)
    # Plex-style: restart @0 when scrub >3s; near start tries previous playQueue item.
    player_get "/player/playback/skipPrevious?commandID=browse-skipPrevious" | grep -q Timeline
    echo "skipPrevious OK (restart@0 or queue previous when near start)"
    ;;
  *)
    echo "unknown command: $CMD" >&2
    usage 1
    ;;
esac
