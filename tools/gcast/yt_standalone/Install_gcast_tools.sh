#!/bin/bash
# Install yt-dlp (armv7l) + layout under /media/fat/misterplex/ for standalone GYT.
# Licenses: yt-dlp Unlicense; quickjs-ng MIT; Node MIT; cacert MPL-2.0.
set -euo pipefail
ROOT="${GCAST_MISTERPLEX_ROOT:-/media/fat/misterplex}"
YTDLP_TAG="${YTDLP_TAG:-2026.08.19}"
DEST="$ROOT"; BIN="$DEST/bin"; CA="$DEST/ca"
# Same onedir the daemon and update_ytdlp.sh use. Do not install a second copy.
YT_DEST="${YTDLP_DEST:-/media/fat/linux/misterplex/yt-dlp}"
mkdir -p "$BIN" "$CA" "$YT_DEST"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/ca/misterplex-cacert.pem" ]; then
  cp -f "$SCRIPT_DIR/ca/misterplex-cacert.pem" "$CA/"
elif [ -f "$SCRIPT_DIR/../ca/misterplex-cacert.pem" ]; then
  cp -f "$SCRIPT_DIR/../ca/misterplex-cacert.pem" "$CA/"
fi
XC="$SCRIPT_DIR/gcast_xcode.py"; [ -f "$XC" ] || XC="$SCRIPT_DIR/../gcast_xcode.py"
[ "$XC" -ef "$DEST/gcast_xcode.py" ] || cp -f "$XC" "$DEST/gcast_xcode.py"
chmod +x "$DEST/gcast_xcode.py"
TMP=$(mktemp -d); trap "rm -rf $TMP" EXIT
echo "Fetching yt-dlp $YTDLP_TAG armv7l..."
CA_FILE="${MPX_CA_BUNDLE:-}"
if [ -z "$CA_FILE" ]; then
  for c in "$CA/misterplex-cacert.pem" /media/fat/linux/misterplex-cacert.pem; do
    if [ -s "$c" ]; then CA_FILE=$c; break; fi
  done
fi
if [ -n "${YTDLP_ZIP:-}" ]; then
  cp -f "$YTDLP_ZIP" "$TMP/yt.zip"
else
  if [ ! -s "$CA_FILE" ]; then
    echo "Install_gcast_tools: CA bundle missing; refusing https without --cacert" >&2
    exit 1
  fi
  curl -fsSL --cacert "$CA_FILE" -o "$TMP/yt.zip" \
    "https://github.com/yt-dlp/yt-dlp/releases/download/${YTDLP_TAG}/yt-dlp_linux_armv7l.zip"
fi
[ -n "${YTDLP_SHA256:-}" ] && { echo "$YTDLP_SHA256  $TMP/yt.zip" | sha256sum -c - || exit 1; }
unzip -qo "$TMP/yt.zip" -d "$TMP/yt"

# Official armv7l zip is a PyInstaller onedir (yt-dlp_linux_armv7l + _internal/). /media/fat is exFAT
# (no symlinks), so bin/yt-dlp is a tiny wrapper that execs the bundle.
[ -x "$TMP/yt/yt-dlp_linux_armv7l" ] || [ -f "$TMP/yt/yt-dlp_linux_armv7l" ] || { echo "zip layout unexpected"; exit 1; }
rm -rf "$YT_DEST.new"; mkdir -p "$YT_DEST.new"; cp -a "$TMP/yt/." "$YT_DEST.new/"
rm -rf "$YT_DEST.old"; [ -d "$YT_DEST" ] && mv "$YT_DEST" "$YT_DEST.old"
mv "$YT_DEST.new" "$YT_DEST"; echo "$YTDLP_TAG" > "$YT_DEST/TAG"
printf '#!/bin/sh\nexec "%s/yt-dlp_linux_armv7l" "$@"\n' "$YT_DEST" > "$BIN/yt-dlp"
chmod +x "$BIN/yt-dlp" 2>/dev/null || true
echo "Note: place QuickJS as $BIN/qjs if missing (MIT; preferred over Node for size)."
cat > "$DEST/run-gcast-xcode.sh" << "EOF"
#!/bin/bash
ROOT="${GCAST_MISTERPLEX_ROOT:-/media/fat/misterplex}"
export GCAST_MISTERPLEX_ROOT="$ROOT"
export GCAST_XCODE_BIND="${GCAST_XCODE_BIND:-127.0.0.1}"
export GCAST_XCODE_PORT="${GCAST_XCODE_PORT:-18781}"
export GCAST_YTDLP="${GCAST_YTDLP:-/media/fat/linux/misterplex/yt-dlp/yt-dlp_linux_armv7l}"
export MPX_CA_BUNDLE="${MPX_CA_BUNDLE:-$ROOT/ca/misterplex-cacert.pem}"
[ -x "$ROOT/bin/qjs" ] && export GCAST_YT_JS="${GCAST_YT_JS:-$ROOT/bin/qjs}"
[ -x "$ROOT/bin/node" ] && export GCAST_YT_JS="${GCAST_YT_JS:-$ROOT/bin/node}"
exec python3 "$ROOT/gcast_xcode.py" "$@"
EOF
chmod +x "$DEST/run-gcast-xcode.sh"
echo "Installed under $DEST"
