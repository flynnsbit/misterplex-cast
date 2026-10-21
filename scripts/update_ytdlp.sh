#!/bin/sh
# Install the standalone yt-dlp PyInstaller onedir (yt-dlp_linux_armv7l.zip).
# Upstream no longer ships a bare yt-dlp_linux_armv7l binary. The zipapp needs
# Python >= 3.10; stock MiSTer python3 is 3.9. The onedir bundles its own
# interpreter and must sit next to _internal/. FAT/exFAT has no symlinks, so
# the exec bit is a shell launcher written here (and chmod after unzip).
#
#   YTDLP_RELEASE_BASE  default: GitHub latest download directory
#   YTDLP_ASSET         default: yt-dlp_linux_armv7l.zip
#   YTDLP_DEST          default: /media/fat/linux/misterplex/yt-dlp  (a directory)
#   MPX_CA_BUNDLE       default: /media/fat/linux/misterplex-cacert.pem
#   YTDLP_LOCAL_ZIP     install this file instead of downloading
#   YTDLP_LOCAL_SUMS    SHA2-256SUMS (or a one-line hex) for YTDLP_LOCAL_ZIP
#
# busybox unzip: `unzip -o -q ZIP -d DIR` (the 2026.08.19 zip is method=store).
set -eu
BASE="${YTDLP_RELEASE_BASE:-https://github.com/yt-dlp/yt-dlp/releases/latest/download}"
ASSET="${YTDLP_ASSET:-yt-dlp_linux_armv7l.zip}"
DEST="${YTDLP_DEST:-/media/fat/linux/misterplex/yt-dlp}"
CA="${MPX_CA_BUNDLE:-/media/fat/linux/misterplex-cacert.pem}"
TAG_URL="${YTDLP_TAG_URL:-https://github.com/yt-dlp/yt-dlp/releases/latest}"
case "$BASE" in
  */) BASE="${BASE%/}" ;;
esac

work=
stage=
flat=
cleanup() {
  # `set -e` turns a failing `[ -n ]` in an EXIT trap into the script's status,
  # so a deliberate `exit 0` (version already installed) would come out as 1.
  if [ -n "$work" ]; then rm -rf "$work"; fi
  if [ -n "$stage" ]; then rm -rf "$stage"; fi
  if [ -n "$flat" ]; then rm -rf "$flat"; fi
  return 0
}
trap cleanup EXIT

# A curl that dies on --max-time used to skip the rm under set -eu.
parent=$(dirname "$DEST")
mkdir -p "$parent" 2>/dev/null || true
command -v flock >/dev/null 2>&1 || {
  echo "update_ytdlp: need flock" >&2
  exit 1
}
# The sweep lock covers "delete stale dirs, then create and lock ours".
# A second updater can run once that lock is dropped, and it skips any
# in-progress dir whose .lock is still held. fd 9 stays open until exit.
hold_sweep() {
  exec 8>"$parent/.yt-dlp-sweep.lock"
  flock -x 8
}
drop_sweep() {
  flock -u 8
}
sweep_stale() {
  for stale in "$parent"/.yt-dlp-dl.*; do
    [ -e "$stale" ] || continue
    if [ -e "$stale/.lock" ] && ! flock -n -x "$stale/.lock" true; then
      echo "update_ytdlp: leaving live $stale"
      continue
    fi
    rm -rf "$stale"
  done
}
claim_work() {
  hold_sweep
  sweep_stale
  work="$parent/.yt-dlp-dl.$$"
  mkdir -p "$work"
  exec 9>"$work/.lock"
  flock -x 9
  drop_sweep
}
hold_sweep
sweep_stale
drop_sweep

fetch() {
  url=$1
  out=$2
  # The zip is ~39 MB. A hard 120s cap aborts a slow but live transfer.
  # --speed-limit/--speed-time aborts a stall; 600s is only the backstop.
  if command -v curl >/dev/null 2>&1; then
    case "$url" in
      https://*)
        if [ ! -s "$CA" ]; then
          echo "update_ytdlp: CA bundle missing ($CA); refusing plain https" >&2
          exit 1
        fi
        curl -fsSL --cacert "$CA" --speed-limit 1024 --speed-time 30 --max-time 600 -o "$out" "$url"
        ;;
      *)
        curl -fsSL --speed-limit 1024 --speed-time 30 --max-time 600 -o "$out" "$url"
        ;;
    esac
  elif command -v wget >/dev/null 2>&1; then
    case "$url" in
      https://*)
        if [ ! -s "$CA" ]; then
          echo "update_ytdlp: CA bundle missing ($CA)" >&2
          exit 1
        fi
        wget -q -O "$out" --ca-certificate="$CA" --timeout=30 "$url"
        ;;
      *)
        wget -q -O "$out" --timeout=30 "$url"
        ;;
    esac
  else
    echo "update_ytdlp: need curl or wget" >&2
    exit 1
  fi
}

# Redirect target of releases/latest, for example .../tag/2026.08.19.
latest_tag() {
  loc=""
  if command -v curl >/dev/null 2>&1; then
    case "$TAG_URL" in
      https://*)
        [ -s "$CA" ] || return 0
        # No -L: %{redirect_url} is empty once curl follows the 302.
        loc=$(curl -fsS -o /dev/null -w '%{redirect_url}' --cacert "$CA" --max-time 20 "$TAG_URL" || true)
        ;;
      *)
        loc=$(curl -fsS -o /dev/null -w '%{redirect_url}' --max-time 20 "$TAG_URL" || true)
        ;;
    esac
  fi
  tag=${loc##*/}
  case "$tag" in
    ''|latest|download) return 0 ;;
  esac
  printf '%s\n' "$tag"
}

sum_for() {
  sums=$1
  asset=$2
  hex=$(awk -v a="$asset" '$2 == a { print $1; exit }' "$sums")
  if [ -n "$hex" ]; then
    printf '%s\n' "$hex"
    return 0
  fi
  awk 'NF==1 && length($1)==64 { print $1; exit }' "$sums"
}

install_tree() {
  zip=$1
  parent=$(dirname "$DEST")
  mkdir -p "$parent"
  stage="$parent/.yt-dlp-new.$$"
  flat="$parent/.yt-dlp-flat.$$"
  # cleanup() removes these if a later step exits under set -eu.
  rm -rf "$stage" "$flat"
  mkdir -p "$stage"
  unzip -o -q "$zip" -d "$stage"
  bin=$(find "$stage" -type f -name 'yt-dlp_linux_armv7l' | head -n 1)
  if [ -z "$bin" ]; then
    echo "update_ytdlp: zip has no yt-dlp_linux_armv7l" >&2
    rm -rf "$stage" "$flat"
    exit 1
  fi
  srcdir=$(dirname "$bin")
  mkdir -p "$flat"
  # Copy rather than mv so a single top-level tree and a nested one both work,
  # and so a busybox mv across the stage directory cannot drop _internal/.
  cp -a "$srcdir"/. "$flat"/
  cat > "$flat/yt-dlp" << 'EOF'
#!/bin/sh
# Launcher (not a symlink: FAT/exFAT). _internal/ must sit beside the binary.
DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
exec "$DIR/yt-dlp_linux_armv7l" "$@"
EOF
  chmod 755 "$flat/yt-dlp" "$flat/yt-dlp_linux_armv7l" 2>/dev/null || true
  if [ -L "$flat/yt-dlp" ] || [ -L "$flat/yt-dlp_linux_armv7l" ]; then
    echo "update_ytdlp: refusing a symlink install" >&2
    rm -rf "$stage" "$flat"
    exit 1
  fi
  rm -rf "$DEST.prev"
  if [ -e "$DEST" ]; then
    mv "$DEST" "$DEST.prev"
  fi
  if ! mv "$flat" "$DEST"; then
    if [ -e "$DEST.prev" ]; then
      mv "$DEST.prev" "$DEST" || true
    fi
    rm -rf "$stage" "$flat"
    echo "update_ytdlp: atomic replace failed" >&2
    exit 1
  fi
  rm -rf "$stage"
  echo "update_ytdlp: installed $DEST"
}

if [ -z "${YTDLP_LOCAL_ZIP:-}" ] && [ -x "$DEST/yt-dlp" ]; then
  tag=$(latest_tag || true)
  have=$("$DEST/yt-dlp" --version 2>/dev/null | head -n 1 || true)
  if [ -n "$tag" ] && [ "$have" = "$tag" ]; then
    echo "update_ytdlp: $tag already installed"
    exit 0
  fi
fi

if [ -n "${YTDLP_LOCAL_ZIP:-}" ]; then
  if [ ! -f "$YTDLP_LOCAL_ZIP" ]; then
    echo "update_ytdlp: missing $YTDLP_LOCAL_ZIP" >&2
    exit 1
  fi
  if [ -n "${YTDLP_LOCAL_SUMS:-}" ]; then
    hex=$(sum_for "$YTDLP_LOCAL_SUMS" "$ASSET")
    got=$(sha256sum "$YTDLP_LOCAL_ZIP" | awk '{ print $1 }')
    if [ -z "$hex" ] || [ "$hex" != "$got" ]; then
      echo "update_ytdlp: checksum mismatch" >&2
      exit 1
    fi
  fi
  install_tree "$YTDLP_LOCAL_ZIP"
  exit 0
fi

claim_work
SUMS="$work/SHA2-256SUMS"
TMP="$work/$ASSET"
fetch "$BASE/SHA2-256SUMS" "$SUMS"
fetch "$BASE/$ASSET" "$TMP"
hex=$(sum_for "$SUMS" "$ASSET")
got=$(sha256sum "$TMP" | awk '{ print $1 }')
if [ -z "$hex" ] || [ "$hex" != "$got" ]; then
  echo "update_ytdlp: checksum mismatch" >&2
  rm -rf "$work"
  exit 1
fi
install_tree "$TMP"
rm -rf "$work"
