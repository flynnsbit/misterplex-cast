#!/bin/sh
# GCAST P2: static armhf deps for airplay_fe (no cmake, no GStreamer, no avahi).
#   UxPlay lib/ + playfair + llhttp + mdnsd (pinned FDH2/UxPlay 4e95a9e) -> libairplay.a
#   openssl 3.0.15 libssl.a + libcrypto.a (YouTube Lounge HTTPS), libplist 2.6.0 libplist-2.0.a
# Usage: AIRPLAY_PREFIX=build/deps/airplay-arm sh scripts/build_arm_airplay.sh
# Reuses existing source trees via UXPLAY_SRC / OPENSSL_SRC / PLIST_SRC when given.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=${AIRPLAY_PREFIX:-$ROOT/build/deps/airplay-arm}
CC=${ARM_CC:-arm-linux-gnueabihf-gcc}
HOST=${ARM_HOST:-arm-linux-gnueabihf}
W=$P/src; mkdir -p "$W" "$P/lib" "$P/include/uxplay"
UXPLAY_REV=4e95a9e
if [ -z "$UXPLAY_SRC" ]; then
  [ -d "$W/UxPlay" ] || git clone -q https://github.com/FDH2/UxPlay.git "$W/UxPlay"
  git -C "$W/UxPlay" checkout -q $UXPLAY_REV; UXPLAY_SRC=$W/UxPlay
fi
# GCAST: harden UxPlay for in-process embedding (see tools/gcast/uxplay-patches/).
python3 "$ROOT/tools/gcast/uxplay-patches/apply_gcast_uxplay_hardening.py" "$UXPLAY_SRC"
python3 "$ROOT/tools/gcast/uxplay-patches/apply_gcast_uxplay_ptth.py" "$UXPLAY_SRC"   # FIELD-1018 F2
python3 "$ROOT/tools/gcast/uxplay-patches/apply_gcast_uxplay_zerodur.py" "$UXPLAY_SRC"   # FIELD-1018 F2b
NO_EXIT="-include $ROOT/tools/gcast/uxplay-patches/uxplay_no_exit.h"
# Rebuild when the archives are missing or still bake a workspace OPENSSLDIR.
# --prefix=/usr --openssldir=/etc/ssl so the static libs do not embed
# /workspace/build/deps. install_dev goes to a DESTDIR, never the real /usr.
need_openssl=0
if [ ! -f "$P/lib/libcrypto.a" ] || [ ! -f "$P/lib/libssl.a" ]; then
  need_openssl=1
elif strings "$P/lib/libcrypto.a" | grep -q '/workspace/build/deps/'; then
  need_openssl=1
fi
if [ "$need_openssl" = 1 ]; then
  if [ -z "$OPENSSL_SRC" ]; then
    (cd "$W" && curl -sSLO https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz && tar xf openssl-3.0.15.tar.gz)
    OPENSSL_SRC=$W/openssl-3.0.15
  fi
  rm -rf "$P/openssl-destdir"
  (cd "$OPENSSL_SRC" && make distclean >/dev/null 2>&1 || true
     ./Configure linux-armv4 --cross-compile-prefix=${HOST}- \
     --prefix=/usr --openssldir=/etc/ssl --libdir=lib \
     no-shared no-tests no-engine no-dso -march=armv7-a -mfpu=neon >/dev/null && \
     make -j"$(nproc)" build_libs >/dev/null && \
     make install_dev DESTDIR="$P/openssl-destdir" >/dev/null)
  cp -f "$P/openssl-destdir/usr/lib/libcrypto.a" "$P/openssl-destdir/usr/lib/libssl.a" "$P/lib/"
  rm -rf "$P/include/openssl"
  mkdir -p "$P/include"
  cp -a "$P/openssl-destdir/usr/include/openssl" "$P/include/openssl"
fi
if [ ! -f "$P/lib/libplist-2.0.a" ]; then
  if [ -z "$PLIST_SRC" ]; then
    (cd "$W" && curl -sSLO https://github.com/libimobiledevice/libplist/releases/download/2.6.0/libplist-2.6.0.tar.bz2 && tar xf libplist-2.6.0.tar.bz2)
    PLIST_SRC=$W/libplist-2.6.0
  fi
  (cd "$PLIST_SRC" && ./configure --host=$HOST --prefix="$P" --enable-static --disable-shared --without-cython --without-tests >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi
OBJ=$W/airplay-obj; rm -rf "$OBJ"; mkdir -p "$OBJ"
DEF="-DSTANDALONE -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX -D_LINUX -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DNOHOLD -DPLIST_210 -DPLIST_230 -DOPENSSL_API_COMPAT=0x10101000L"
INC="-I$UXPLAY_SRC/lib -I$UXPLAY_SRC/lib/playfair -I$UXPLAY_SRC/lib/llhttp -I$UXPLAY_SRC/lib/mdnsd -I$P/include"
for f in "$UXPLAY_SRC"/lib/*.c "$UXPLAY_SRC"/lib/playfair/*.c "$UXPLAY_SRC"/lib/llhttp/*.c "$UXPLAY_SRC"/lib/mdnsd/*.c; do
  o=$OBJ/$(echo "$f" | sed "s#$UXPLAY_SRC/##; s#/#_#g").o
  $CC $DEF $INC ${NO_EXIT:-} -O2 -w -c "$f" -o "$o"
done
rm -f "$P/lib/libairplay.a"; ${HOST}-ar rcs "$P/lib/libairplay.a" "$OBJ"/*.o
cp "$UXPLAY_SRC"/lib/*.h "$P/include/uxplay/"
echo "uxplay=$UXPLAY_REV $(git -C "$UXPLAY_SRC" rev-parse --short HEAD 2>/dev/null)" > "$P/VERSION"
ls -la "$P/lib"/*.a
