#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "ERROR: usage: $0 --plex|--spelunkr" >&2
  exit 1
fi
case "$1" in
  --plex|--spelunkr) ;;
  *) echo "ERROR: unexpected argument: $1" >&2; exit 1 ;;
esac

export MPX_CORE_MANUAL_ONLY=1
exec sh "$(dirname "$0")/misterplex_mode.sh" "$1"
