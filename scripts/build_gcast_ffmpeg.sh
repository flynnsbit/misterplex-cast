#!/bin/bash
# Static armhf ffmpeg and ffprobe for the on-card AirPlay helper (linux/gcast/).
# FFmpeg 8.1.2 (--enable-gpl --enable-version3, so GPL-3.0-or-later) plus
# x264 commit 0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee (GPL-2.0-or-later)
# and OpenSSL 3.0.15 (Apache-2.0). OpenSSL 3 with --enable-gpl needs
# --enable-version3. https and TLS sources need that OpenSSL.
#
# The complete corresponding source (FFmpeg 8.1.2, x264
# 0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee, OpenSSL 3.0.15), this script,
# and scripts/build_arm_airplay.sh are attached as assets to
# https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5
# and are available on request from the distributor. This file is the
# release asset build_gcast_ffmpeg.sh. See packaging/licenses/gcast-ffmpeg/NOTICE.
# Defaults point at a local tree; override the variables to build somewhere else.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TC=${GCAST_TC:-/tmp/mpx-repro/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-linux-gnueabihf/bin}
FFMPEG_SRC=${FFMPEG_SRC:-$ROOT/build/deps/ffmpeg-8.1.2}
X264_SRC=${X264_SRC:-/tmp/x264-src}
X264_COMMIT=0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee
OPENSSL_PREFIX=${OPENSSL_PREFIX:-$ROOT/build/deps/airplay-arm/openssl-destdir/usr}
PREFIX=${GCAST_PREFIX:-/tmp/mpx-gcast-ffmpeg}
BUILD=${GCAST_BUILD:-/tmp/ffmpeg-gcast-tls}
export PATH="$TC:$PATH"
# Cross configure refuses pkg-config unless it is named. The directory below
# holds x264.pc and the OpenSSL .pc files written with the static prefix.
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"

# OpenSSL's installed .pc says prefix=/usr. Write copies that point at the
# static 3.0.15 tree so pkg-config does not pick the host OpenSSL.
mkdir -p "$PREFIX/lib/pkgconfig"
cat >"$PREFIX/lib/pkgconfig/libcrypto.pc" <<EOF
prefix=${OPENSSL_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: OpenSSL-libcrypto
Description: OpenSSL cryptography library
Version: 3.0.15
Libs: -L\${libdir} -lcrypto
Libs.private: -ldl -pthread -latomic
Cflags: -I\${includedir}
EOF
cat >"$PREFIX/lib/pkgconfig/libssl.pc" <<EOF
prefix=${OPENSSL_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: OpenSSL-libssl
Description: Secure Sockets Layer and cryptography libraries
Version: 3.0.15
Requires.private: libcrypto
Libs: -L\${libdir} -lssl
Cflags: -I\${includedir}
EOF
cat >"$PREFIX/lib/pkgconfig/openssl.pc" <<EOF
prefix=${OPENSSL_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: OpenSSL
Description: Secure Sockets Layer and cryptography libraries
Version: 3.0.15
Requires: libssl libcrypto
EOF

# x264 configure (static, no CLI, no asm: the helper runs under qemu-arm too):
#   ./configure --host=arm-none-linux-gnueabihf --cross-prefix=arm-none-linux-gnueabihf- \
#     --enable-static --disable-cli --disable-opencl --disable-asm --prefix="$PREFIX"
if [ ! -f "$PREFIX/lib/libx264.a" ]; then
  if [ ! -d "$X264_SRC" ]; then
    git clone --branch stable https://code.videolan.org/videolan/x264.git "$X264_SRC"
  fi
  git -C "$X264_SRC" checkout "$X264_COMMIT"
  ( cd "$X264_SRC" && ./configure --host=arm-none-linux-gnueabihf \
      --cross-prefix=arm-none-linux-gnueabihf- --enable-static --disable-cli \
      --disable-opencl --disable-asm --prefix="$PREFIX" && make -j"$(nproc)" && make install )
fi

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
# FFmpeg configure. https/tls come from the static OpenSSL 3.0.15 already
# built by scripts/build_arm_airplay.sh. ffprobe is not disabled.
"$FFMPEG_SRC/configure" \
  --prefix="$PREFIX" \
  --enable-cross-compile \
  --cross-prefix=arm-none-linux-gnueabihf- \
  --arch=arm --cpu=armv7-a --target-os=linux \
  --pkg-config=pkg-config \
  --enable-gpl --enable-version3 --enable-libx264 --enable-openssl \
  --enable-static --disable-shared \
  --disable-doc --disable-debug --disable-ffplay \
  --disable-avdevice --enable-small \
  --disable-everything \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale --enable-avfilter \
  --enable-encoder=libx264,aac \
  --enable-decoder=h264,hevc,aac,aac_latm,mp3,pcm_s16le \
  --enable-muxer=mpegts \
  --enable-demuxer=mov,mpegts,h264,aac,wav,matroska,mp3,hls,pcm_s16le \
  --enable-parser=h264,hevc,aac,aac_latm,mpegaudio \
  --enable-bsf=h264_mp4toannexb \
  --enable-protocol=file,http,https,tcp,udp,tls,pipe,crypto \
  --enable-filter=scale,pad,fps,format,setsar,showspectrum,noise,asplit,aresample,aformat \
  --extra-cflags="-Os -ffunction-sections -fdata-sections -march=armv7-a -mfpu=neon -I$PREFIX/include -I$OPENSSL_PREFIX/include" \
  --extra-ldflags="-static -Wl,--gc-sections -L$PREFIX/lib -L$OPENSSL_PREFIX/lib" \
  --extra-libs="-lssl -lcrypto -lx264 -lpthread -lm -ldl -latomic" \
  --pkg-config-flags=--static
# ffmpeg only. ffprobe is a second, smaller build below (no libx264): probe
# does not encode, and the pair has to fit the release tarball.
make -j"$(nproc)" ffmpeg
arm-none-linux-gnueabihf-strip --strip-unneeded ffmpeg
cp -f ffmpeg "$PREFIX/ffmpeg"

# ffprobe configure. Same FFmpeg 8.1.2 and OpenSSL 3.0.15, same decoders,
# demuxers, and protocols (https/tls/hls/wav). No libx264, encoders, muxers,
# or filters. --disable-ffprobe is not passed.
PROBE=${GCAST_PROBE_BUILD:-/tmp/ffmpeg-gcast-probe}
rm -rf "$PROBE"
mkdir -p "$PROBE"
cd "$PROBE"
"$FFMPEG_SRC/configure" \
  --prefix="$PREFIX" \
  --enable-cross-compile \
  --cross-prefix=arm-none-linux-gnueabihf- \
  --arch=arm --cpu=armv7-a --target-os=linux \
  --pkg-config=pkg-config \
  --enable-openssl \
  --enable-static --disable-shared \
  --disable-doc --disable-debug --disable-ffplay --disable-ffmpeg \
  --disable-avdevice --enable-small \
  --disable-everything \
  --enable-avcodec --enable-avformat --enable-avutil \
  --enable-decoder=h264,hevc,aac,aac_latm,mp3,pcm_s16le \
  --enable-demuxer=mov,mpegts,h264,aac,wav,matroska,mp3,hls,pcm_s16le \
  --enable-parser=h264,hevc,aac,aac_latm,mpegaudio \
  --enable-protocol=file,http,https,tcp,udp,tls,pipe,crypto \
  --extra-cflags="-Os -ffunction-sections -fdata-sections -march=armv7-a -mfpu=neon -I$OPENSSL_PREFIX/include" \
  --extra-ldflags="-static -Wl,--gc-sections -L$OPENSSL_PREFIX/lib" \
  --extra-libs="-lssl -lcrypto -lpthread -lm -ldl -latomic" \
  --pkg-config-flags=--static
make -j"$(nproc)" ffprobe
arm-none-linux-gnueabihf-strip --strip-unneeded ffprobe
cp -f ffprobe "$PREFIX/ffprobe"
ls -l "$PREFIX/ffmpeg" "$PREFIX/ffprobe"
echo GCAST_FFMPEG_OK
