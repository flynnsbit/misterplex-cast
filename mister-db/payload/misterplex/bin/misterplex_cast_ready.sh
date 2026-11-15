#!/bin/sh
# Shared Cast-ready predicates (sourced by watch + supervise).
#
# Defect: Plex Web lists us from GDM UDP 32412, then talks HTTP :PORT.
# Chevron / a live misterplexd PID is not enough. If bind :3005 failed,
# the Cast picker appears and vanishes. Ready means /resources returns
# companion XML (MediaContainer + machineIdentifier + MiSTerPlex).
#
# A bare wget -O /dev/null 200 (any listener) is the RED twin — reject it.

# PORT must be set by the caller (default 3005).
PORT="${PORT:-3005}"

cast_resources_looks_ready() {
  # Body from file $1, or stdin if no arg.
  if [ -n "${1:-}" ]; then
    [ -f "$1" ] || return 1
    grep -q 'MediaContainer' "$1" || return 1
    grep -q 'machineIdentifier' "$1" || return 1
    grep -q 'MiSTerPlex' "$1" || return 1
    return 0
  fi
  body=$(cat)
  printf '%s\n' "$body" | grep -q 'MediaContainer' || return 1
  printf '%s\n' "$body" | grep -q 'machineIdentifier' || return 1
  printf '%s\n' "$body" | grep -q 'MiSTerPlex' || return 1
  return 0
}

# Legacy ready check (pid-or-any-HTTP-200). Kept for the RED twin in tests.
# Do not call this from watch/supervise.
cast_ready_legacy_any_http() {
  wget -q -T 1 -O /dev/null "http://127.0.0.1:${PORT}/resources" 2>/dev/null
}

cast_http_ok() {
  tmp="${TMPDIR:-/tmp}/mplex-cast-ready.$$"
  if ! wget -q -T 1 -O "$tmp" "http://127.0.0.1:${PORT}/resources" 2>/dev/null; then
    rm -f "$tmp"
    return 1
  fi
  cast_resources_looks_ready "$tmp"
  rc=$?
  rm -f "$tmp"
  return $rc
}

cast_port_hex() {
  printf '%04X' "$PORT"
}

# True if IPv4 TCP LISTEN on $PORT (state 0A).
cast_port_listen() {
  hex=$(cast_port_hex)
  awk -v hx="$hex" '
    NR == 1 { next }
    {
      n = split($2, a, ":")
      if (n >= 2 && toupper(a[n]) == hx && $4 == "0A") found = 1
    }
    END { exit found ? 0 : 1 }
  ' /proc/net/tcp 2>/dev/null
}

# Kill only misterplexd (not foreign listeners) that own LISTEN :PORT.
cast_kill_misterplexd_on_port() {
  hex=$(cast_port_hex)
  inodes=$(awk -v hx="$hex" '
    NR == 1 { next }
    {
      n = split($2, a, ":")
      if (n >= 2 && toupper(a[n]) == hx && $4 == "0A") print $10
    }
  ' /proc/net/tcp 2>/dev/null)
  [ -n "${inodes:-}" ] || return 0
  for inode in $inodes; do
    [ -n "$inode" ] && [ "$inode" != "0" ] || continue
    for fd in /proc/[0-9]*/fd/*; do
      link=$(readlink "$fd" 2>/dev/null || true)
      case "$link" in
        "socket:[$inode]")
          pid=$(echo "$fd" | cut -d/ -f3)
          cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)
          case "$cmd" in
            *misterplexd*)
              kill "$pid" 2>/dev/null || true
              ;;
          esac
          ;;
      esac
    done
  done
}
