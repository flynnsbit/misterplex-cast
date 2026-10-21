#!/usr/bin/env bash
# ns_tmp_pid.sh <cmd> [args...]: run <cmd> alone, in a private tmpfs /tmp and a private PID ns, as the caller's
# uid/gid (T-UNIT-DANGLING-3 rev 2 §3.4, d2: the SUPP-1 form). Its first output line is the probe
#   NS uid=<u> gid=<g> pid=1 fstype=tmpfs empty=yes dev=<d> unshare=<util-linux version>
# and then <cmd> runs as PID 1 of the new PID ns. The caller checks the probe: pid=1, tmpfs, empty, and a /tmp dev
# that is not the host's. --map-user/--map-group need util-linux >= 2.38.
set -euo pipefail
case "${1:-}" in
--stage-mount)   # in a root-mapped user + mount ns: a private tmpfs on /tmp, then the PID ns as the caller's ids
    shift
    u=$1 g=$2 v=$3
    shift 3
    mount -t tmpfs ns_tmp_pid /tmp
    exec unshare --user --map-user="$u" --map-group="$g" --pid --fork --kill-child --mount-proc \
        "$0" --stage-pid "$v" "$@" ;;
--stage-pid)     # PID 1 of the new PID ns: print the probe, then become <cmd>
    shift
    v=$1
    shift
    printf 'NS uid=%s gid=%s pid=%s fstype=%s empty=%s dev=%s unshare=%s\n' "$(id -u)" "$(id -g)" "$$" \
        "$(stat -f -c %T /tmp)" "$([ -z "$(ls -A /tmp)" ] && echo yes || echo no)" "$(stat -c %d /tmp)" "$v"
    exec "$@" ;;
esac
[ "$#" -ge 1 ] || { echo "usage: ns_tmp_pid.sh <cmd> [args...]" >&2; exit 2; }
v=$(unshare --version | sed -n 's/^unshare from util-linux \(.*\)$/\1/p')
exec unshare --user --map-root-user --mount "$0" --stage-mount "$(id -u)" "$(id -g)" "${v:-unknown}" "$@"
