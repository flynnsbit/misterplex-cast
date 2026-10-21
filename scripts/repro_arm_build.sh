#!/usr/bin/env bash
# Byte-for-byte reproducible armhf misterplexd from a clean clone (rc2; also used for gcast-rc1).
# Usage: scripts/repro_arm_build.sh [REV]   (REV defaults to HEAD of this clone; run from any clone)
# Every input is pinned (sha256 / git rev) and everything is built under one fixed path ($WORK, default
# /tmp/mpx-repro) because the binary embeds absolute paths (FFmpeg configure line, UxPlay __FILE__).
# Two hosts running this with the same REV and WORK get the same md5 (verified box vs lab-pc, see README).
# Env: MPX_REPRO_REPO (clone to build from; default: the clone holding this script), MPX_REPRO_WORK (default /tmp/mpx-repro, wiped), MPX_REPRO_DL (download cache, default ~/.cache/mpx-repro),
#      MPX_REPRO_JOBS (default nproc). Network needed only for missing downloads.
set -euo pipefail
export LC_ALL=C TZ=UTC; umask 022
REPO=${MPX_REPRO_REPO:-$(cd "$(dirname "$0")/.." && pwd)}   # any clone that has REV
REV=$(git -C "$REPO" rev-parse "${1:-HEAD}")
WORK=${MPX_REPRO_WORK:-/tmp/mpx-repro}
DL=${MPX_REPRO_DL:-$HOME/.cache/mpx-repro}
J=${MPX_REPRO_JOBS:-$(nproc)}
export SOURCE_DATE_EPOCH=$(git -C "$REPO" log -1 --format=%ct "$REV")
TC_NAME=arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-linux-gnueabihf
TC_TAR=arm-gnu-toolchain-13.2.rel1-x86_64-arm-none-linux-gnueabihf.tar.xz
TC_URL=https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/$TC_TAR
TC_SHA=df0f4927a67d1fd366ff81e40bd8c385a9324fbdde60437a512d106215f257b3
FF_TAR=ffmpeg-8.1.2.tar.xz; FF_URL=https://ffmpeg.org/releases/$FF_TAR
FF_SHA=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
OS_TAR=openssl-3.0.15.tar.gz; OS_URL=https://github.com/openssl/openssl/releases/download/openssl-3.0.15/$OS_TAR
OS_SHA=23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533
PL_TAR=libplist-2.6.0.tar.bz2; PL_URL=https://github.com/libimobiledevice/libplist/releases/download/2.6.0/$PL_TAR
PL_SHA=67be9ee3169366589c92dc7c22809b90f51911dd9de22520c39c9a64fb047c9c
UX_URL=https://github.com/FDH2/UxPlay.git; UX_REV=4e95a9ec013c52d8555d88dbc55abe346e94d56e

fetch() { # file url sha
  mkdir -p "$DL"
  [ -f "$DL/$1" ] || curl -fsSL -o "$DL/$1.part" "$2" && { [ -f "$DL/$1" ] || mv "$DL/$1.part" "$DL/$1"; }
  echo "$3  $DL/$1" | sha256sum -c --quiet - || { echo "sha256 mismatch: $1" >&2; exit 1; }
}
fetch "$TC_TAR" "$TC_URL" "$TC_SHA"; fetch "$FF_TAR" "$FF_URL" "$FF_SHA"
fetch "$OS_TAR" "$OS_URL" "$OS_SHA"; fetch "$PL_TAR" "$PL_URL" "$PL_SHA"

exec 9>"$WORK.lock"; flock -n 9 || { echo "another repro build holds $WORK.lock" >&2; exit 3; }
rm -rf "$WORK"; mkdir -p "$WORK"/{src,dep,out}
tar -C "$WORK" -xf "$DL/$TC_TAR"; TC=$WORK/$TC_NAME/bin; export PATH="$TC:$PATH"
git -C "$REPO" archive "$REV" | tar -C "$WORK/src" -xf -
D=$WORK/src/build/deps; mkdir -p "$D"
tar -C "$D" -xf "$DL/$FF_TAR"
tar -C "$WORK/dep" -xf "$DL/$OS_TAR"; tar -C "$WORK/dep" -xf "$DL/$PL_TAR"
git clone -q "$UX_URL" "$WORK/dep/UxPlay"; git -C "$WORK/dep/UxPlay" checkout -q "$UX_REV"

X=arm-none-linux-gnueabihf
cd "$WORK/src"
echo "== ffmpeg 8.1.2 (static-safe)"; FFMPEG_SOURCE=build/deps/ffmpeg-8.1.2 ARM_FFMPEG_CROSS="$TC/$X-" FFMPEG_JOBS=$J \
  bash scripts/build_arm_ffmpeg.sh >"$WORK/out/ffmpeg.log" 2>&1 || { tail -30 "$WORK/out/ffmpeg.log"; exit 1; }
echo "== airplay (UxPlay ${UX_REV:0:7}, OpenSSL 3.0.15, libplist 2.6.0)"
AIRPLAY_PREFIX=$D/airplay-arm ARM_CC=$X-gcc ARM_HOST=$X UXPLAY_SRC=$WORK/dep/UxPlay \
  OPENSSL_SRC=$WORK/dep/openssl-3.0.15 PLIST_SRC=$WORK/dep/libplist-2.6.0 \
  bash scripts/build_arm_airplay.sh >"$WORK/out/airplay.log" 2>&1 || { tail -30 "$WORK/out/airplay.log"; exit 1; }
echo "== misterplexd $REV"
FF_PREFIX="$D/$(python3 scripts/check_arm_ffmpeg_static.py --print-prefix-dirname)"
make -j"$J" arm-plexd-daemon ARM_CXX="$TC/$X-g++" ARM_NM="$TC/$X-nm" ARM_PLEXD_AIRPLAY=1 \
  ARM_FFMPEG_PREFIX="$FF_PREFIX" ARM_AIRPLAY_PREFIX="$D/airplay-arm" \
  ARM_PLEXD_OUTPUT="$WORK/out/misterplexd" >"$WORK/out/daemon.log" 2>&1 || { tail -30 "$WORK/out/daemon.log"; exit 1; }
"$TC/$X-strip" -o "$WORK/out/misterplexd.stripped" "$WORK/out/misterplexd"
{
  echo "rev=$REV SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH work=$WORK"
  echo "toolchain=$("$TC/$X-g++" --version | head -1)"
  (cd "$D" && md5sum "$(basename "$FF_PREFIX")"/lib/*.a airplay-arm/lib/*.a)
  (cd "$WORK/out" && md5sum misterplexd misterplexd.stripped)
} | tee "$WORK/out/REPRO.txt"
