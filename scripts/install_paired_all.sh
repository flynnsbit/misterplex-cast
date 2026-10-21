#!/usr/bin/env bash
# Install every verified RBF+daemon pair onto the MiSTer.
# Does not load_core / menu-bounce. Does not start a second watcher.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${REL:-}" ]; then
  if [ -f "$ROOT/cores/Plex_480p.rbf" ]; then
    REL=$ROOT
  else
    REL=$ROOT/release_artifacts/v0.9.0-pre-paired
  fi
fi
HOST="${MISTER_HOST:?Set MISTER_HOST to the MiSTer address}"
PASS="${MISTER_PASS:?set MISTER_PASS}"
SSH=(sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=12 root@"$HOST")
SCP=(sshpass -p "$PASS" scp -o StrictHostKeyChecking=no)

need() { [[ -f "$1" ]] || { echo "missing $1" >&2; exit 2; }; }
need "$REL/cores/Plex_480p.rbf"
need "$REL/cores/Plex_240p15.rbf"
need "$REL/cores/Plex_480i.rbf"
need "$REL/bin/misterplexd.480p"
need "$REL/rbf_daemon_pairs.txt"
need "$REL/Plex_README.txt"

# 720p24 lab pair is required for this pre-release pack.
need "$REL/cores/Plex_720p24.rbf"
need "$REL/bin/misterplexd.720p24"

bin_of() {
  local name=$1
  if [ -f "$REL/bin/$name" ]; then
    echo "$REL/bin/$name"
    return
  fi
  if [ -f "$ROOT/scripts/$name" ]; then
    echo "$ROOT/scripts/$name"
    return
  fi
  echo "missing $name in $REL/bin or $ROOT/scripts" >&2
  exit 2
}

WATCH=$(bin_of misterplex_core_watch.sh)
SUP=$(bin_of misterplexd_supervise.sh)
CAST=$(bin_of misterplex_cast_ready.sh)
PAIRCONF=$(bin_of misterplex_pair_conf.sh)
NAMED=$(bin_of misterplex_named_rbf.sh)

# 240p15 / 480i daemons are named copies of the 480p gold binary.
DAE_480P="$REL/bin/misterplexd.480p"
DAE_240="${REL}/bin/misterplexd.240p15"
DAE_480I="${REL}/bin/misterplexd.480i"
[[ -f "$DAE_240" ]] || DAE_240=$DAE_480P
[[ -f "$DAE_480I" ]] || DAE_480I=$DAE_480P

echo "=== install_paired_all -> $HOST from $REL ==="
if [[ "${INSTALL_DRY:-0}" == "1" ]]; then
  echo "INSTALL_DRY=1 — file check only; no ssh"
  exit 0
fi

"${SSH[@]}" 'mkdir -p /media/fat/_Utility /media/fat/misterplex/bin /media/fat/misterplex/scripts'
"${SCP[@]}" \
  "$REL/cores/Plex_480p.rbf" "$REL/cores/Plex_720p24.rbf" \
  "$REL/cores/Plex_240p15.rbf" "$REL/cores/Plex_480i.rbf" \
  "$REL/Plex_README.txt" \
  root@"$HOST":/media/fat/_Utility/
"${SCP[@]}" \
  "$DAE_480P" "$REL/bin/misterplexd.720p24" \
  "$WATCH" "$SUP" "$CAST" "$PAIRCONF" "$NAMED" \
  root@"$HOST":/media/fat/misterplex/bin/
"${SCP[@]}" "$DAE_240" root@"$HOST":/media/fat/misterplex/bin/misterplexd.240p15
"${SCP[@]}" "$DAE_480I" root@"$HOST":/media/fat/misterplex/bin/misterplexd.480i
"${SCP[@]}" "$REL/rbf_daemon_pairs.txt" root@"$HOST":/media/fat/misterplex/
if [[ -f "$REL/bin/ffmpeg" ]]; then
  "${SCP[@]}" "$REL/bin/ffmpeg" root@"$HOST":/media/fat/misterplex/bin/
elif [[ -f "$ROOT/build/arm/ffmpeg" ]]; then
  true
fi
if [[ -f "$REL/conf/misterplex.conf.example" ]]; then
  "${SSH[@]}" 'test -f /media/fat/misterplex/misterplex.conf' || \
    "${SCP[@]}" "$REL/conf/misterplex.conf.example" \
      root@"$HOST":/media/fat/misterplex/misterplex.conf
fi

"${SSH[@]}" 'chmod +x /media/fat/misterplex/bin/misterplexd.480p \
  /media/fat/misterplex/bin/misterplexd.720p24 \
  /media/fat/misterplex/bin/misterplexd.240p15 \
  /media/fat/misterplex/bin/misterplexd.480i \
  /media/fat/misterplex/bin/misterplex_core_watch.sh \
  /media/fat/misterplex/bin/misterplexd_supervise.sh \
  /media/fat/misterplex/bin/misterplex_cast_ready.sh \
  /media/fat/misterplex/bin/misterplex_pair_conf.sh \
  /media/fat/misterplex/bin/misterplex_named_rbf.sh
if [ -x /media/fat/misterplex/bin/ffmpeg ]; then chmod +x /media/fat/misterplex/bin/ffmpeg; fi
cp -f /media/fat/misterplex/bin/misterplexd.480p /media/fat/misterplex/bin/misterplexd
rm -f /media/fat/Plex.rbf /media/fat/_Utility/Plex.rbf
echo REMOVED_GENERIC_Plex.rbf
echo === rbf ===
md5sum /media/fat/_Utility/Plex_480p.rbf /media/fat/_Utility/Plex_720p24.rbf \
  /media/fat/_Utility/Plex_240p15.rbf /media/fat/_Utility/Plex_480i.rbf
echo === daemons ===
md5sum /media/fat/misterplex/bin/misterplexd.480p \
  /media/fat/misterplex/bin/misterplexd.720p24 \
  /media/fat/misterplex/bin/misterplexd.240p15 \
  /media/fat/misterplex/bin/misterplexd.480i
if ! grep -q misterplex_core_watch.sh /media/fat/linux/_user-startup.sh 2>/dev/null; then
  printf "\n/media/fat/misterplex/bin/misterplex_core_watch.sh &\n" >> /media/fat/linux/_user-startup.sh
  echo ADDED_WATCH_STARTUP
else
  echo WATCH_STARTUP_OK
fi
'
echo "=== install done (no load_core) ==="
