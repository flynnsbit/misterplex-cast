#!/usr/bin/env bash
# On-device / SSH interactive menu: browse PMS libraries and play via misterplexd.
# For MiSTer Scripts (SSH or /media/fat/Scripts) — pure bash + curl, no dialog(1).
#
# Usage:
#   scripts/plex_menu.sh
#   scripts/plex_menu.sh --player 127.0.0.1:3005 --conf /media/fat/misterplex/misterplex.conf
#
# Env: PLEX_BASE / PLEX_TOKEN / MISTERPLEX_PLAYER / MISTERPLEX_CONF (same as plex_browse.sh)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BROWSE="$ROOT/scripts/plex_browse.sh"
[[ -x "$BROWSE" ]] || BROWSE="$(cd "$(dirname "$0")" && pwd)/plex_browse.sh"

PLAYER="${MISTERPLEX_PLAYER:-127.0.0.1:3005}"
BROWSE_ARGS=()

usage() {
  cat <<'EOF'
MiSTerPlex on-device library menu
  plex_menu.sh [--player HOST:PORT] [--conf PATH] [--base URL] [--token TOK] [--server N]

Lists PMS sections/items (via plex_browse.sh) and controls misterplexd
(playMedia, pause/resume/stop, seek, ±10s step, skip next/prev).
EOF
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    --player) PLAYER="$2"; BROWSE_ARGS+=(--player "$2"); shift 2 ;;
    --conf) BROWSE_ARGS+=(--conf "$2"); shift 2 ;;
    --base) BROWSE_ARGS+=(--base "$2"); shift 2 ;;
    --token) BROWSE_ARGS+=(--token "$2"); shift 2 ;;
    --server) BROWSE_ARGS+=(--server "$2"); shift 2 ;;
    *) echo "unknown option: $1" >&2; usage 1 ;;
  esac
done

browse() { "$BROWSE" "${BROWSE_ARGS[@]+"${BROWSE_ARGS[@]}"}" "$@"; }

if [[ "$PLAYER" != *:* ]]; then
  PLAYER="${PLAYER}:3005"
fi
echo "MiSTerPlex menu  player=http://${PLAYER}"
echo

# Extract ratingKey (preferred) or key path from a browse row.
row_pick_id() {
  local line="$1"
  local rk key
  rk=$(echo "$line" | sed -n 's/.*ratingKey=\([^ ]*\).*/\1/p' | head -1 | tr -d '[:space:]')
  key=$(echo "$line" | sed -n 's/.*key=\([^ ]*\).*/\1/p' | head -1 | tr -d '[:space:]')
  if [[ -n "$rk" && "$rk" != "-" && "$rk" != "?" ]]; then
    printf '%s' "$rk"
  elif [[ -n "$key" && "$key" != "?" ]]; then
    printf '%s' "$key"
  else
    return 1
  fi
}

row_label() {
  local line="$1"
  local title typ rk
  title=$(echo "$line" | sed -n 's/.*title=\(.*\)$/\1/p')
  typ=$(echo "$line" | sed -n 's/.*type=\([^ ]*\).*/\1/p' | head -1)
  rk=$(echo "$line" | sed -n 's/.*ratingKey=\([^ ]*\).*/\1/p' | head -1)
  printf '%s  [%s rk=%s]' "${title:--}" "${typ:--}" "${rk:--}"
}

# Numbered picker. Prints selected id on stdout; status: 0=ok 1=back 2=quit
pick_rows() {
  local prompt="$1"
  shift
  local -a lines=("$@")
  local n=${#lines[@]}
  if [[ $n -eq 0 ]]; then
    echo "(empty)" >&2
    return 1
  fi
  local i
  for i in "${!lines[@]}"; do
    printf '  %2d) %s\n' "$((i + 1))" "$(row_label "${lines[$i]}")" >&2
  done
  printf '%s [1-%d, b=back, q=quit]: ' "$prompt" "$n" >&2
  local ans
  read -r ans || return 2
  case "$ans" in
    q|Q|quit) return 2 ;;
    b|B|back|"") return 1 ;;
  esac
  if [[ "$ans" =~ ^[0-9]+$ ]] && [[ "$ans" -ge 1 && "$ans" -le $n ]]; then
    row_pick_id "${lines[$((ans - 1))]}"
    return 0
  fi
  echo "invalid choice" >&2
  return 1
}

collect_rows() {
  # stdin → stdout lines starting with key=
  grep -E '^key=' || true
}

while true; do
  echo "======== MiSTerPlex ========"
  echo "  1) Browse library → play"
  echo "  2) Play by ratingKey / key"
  echo "  3) Status"
  echo "  4) Pause"
  echo "  5) Resume"
  echo "  6) Stop"
  echo "  7) Seek to ms"
  echo "  8) Step +10s   9) Step -10s"
  echo "  n) Next (skipNext)   p) Prev (restart@0 / queue prev near start)"
  echo "  s) Servers (conf)"
  echo "  q) Quit"
  printf 'Choice: '
  read -r main || exit 0
  case "$main" in
    q|Q|quit) exit 0 ;;
    3) browse status || true; echo ;;
    4) browse pause || true; echo ;;
    5) browse resume || true; echo ;;
    6) browse stop || true; echo ;;
    7)
      printf 'Seek offset (ms): '
      read -r ms || continue
      [[ -z "$ms" ]] && continue
      if [[ ! "$ms" =~ ^[0-9]+$ ]]; then
        echo "need non-negative integer ms" >&2
        continue
      fi
      browse seek "$ms" || true
      echo
      ;;
    8) browse step || true; echo ;;
    9) browse stepBack || true; echo ;;
    n|N|next) browse next || true; echo ;;
    p|P|prev) browse prev || true; echo ;;
    s|S|servers) browse servers; echo ;;
    2)
      printf 'ratingKey or /library/metadata/N: '
      read -r k || continue
      [[ -z "$k" ]] && continue
      browse play "$k" || true
      echo
      ;;
    1)
      mapfile -t SEC_ROWS < <(browse sections 2>/dev/null | collect_rows)
      if [[ ${#SEC_ROWS[@]} -eq 0 ]]; then
        echo "No sections (set PLEX_TOKEN + PLEX_BASE in conf)." >&2
        continue
      fi
      echo "--- Library sections ---"
      sid=$(pick_rows "Section" "${SEC_ROWS[@]}") || {
        rc=$?
        [[ $rc -eq 2 ]] && exit 0
        continue
      }
      # section id from path or raw ratingKey
      if [[ "$sid" == */* ]]; then
        sid="${sid%/all}"
        sid="${sid##*/}"
      fi
      echo
      echo "--- Section $sid ---"
      mapfile -t ITEM_ROWS < <(browse section "$sid" 2>/dev/null | collect_rows)
      if [[ ${#ITEM_ROWS[@]} -eq 0 ]]; then
        echo "No items." >&2
        continue
      fi
      item=$(pick_rows "Item" "${ITEM_ROWS[@]}") || {
        rc=$?
        [[ $rc -eq 2 ]] && exit 0
        continue
      }
      # If show/season directory, offer children from item metadata Video rows
      mapfile -t CHILD_ROWS < <(browse item "$item" 2>/dev/null | collect_rows)
      # Prefer Video rows (durationMs=) when mixed
      mapfile -t VIDEO_ROWS < <(printf '%s\n' "${CHILD_ROWS[@]+"${CHILD_ROWS[@]}"}" | grep -E 'durationMs=' || true)
      if [[ ${#VIDEO_ROWS[@]} -gt 1 ]]; then
        echo "--- Children / episodes ---"
        item=$(pick_rows "Play" "${VIDEO_ROWS[@]}") || {
          rc=$?
          [[ $rc -eq 2 ]] && exit 0
          continue
        }
      elif [[ ${#VIDEO_ROWS[@]} -eq 1 ]]; then
        item=$(row_pick_id "${VIDEO_ROWS[0]}")
      fi
      echo
      browse play "$item" || true
      echo
      ;;
    *)
      echo "unknown choice"
      ;;
  esac
done
