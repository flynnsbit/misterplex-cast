#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
source="${FFMPEG_SOURCE:-build/deps/ffmpeg-8.1.2}"
config_hash="$(python3 "$root/scripts/check_arm_ffmpeg_static.py" --print-config-hash)"
configure_flags="$(python3 "$root/scripts/check_arm_ffmpeg_static.py" --print-configure-flags)"
build="$(realpath -m --relative-to="$root" "${FFMPEG_BUILD_DIR:-build/deps/ffmpeg-8.1.2-${config_hash}-time64-build}")"
prefix="$(realpath -m --relative-to="$root" "${ARM_FFMPEG_PREFIX:-build/deps/ffmpeg-arm-${config_hash}-time64}")"
for output in "$build" "$prefix"; do
    case "$output" in
        .|..|../*|/*) echo "FFmpeg outputs must stay inside this project" >&2; exit 1;;
    esac
    [[ ! -e "$output" && ! -L "$output" ]] || {
        echo "Refusing to overwrite an existing FFmpeg build/install: $output" >&2; exit 1;
    }
done
[[ -f "$source/configure" && -f "$source/RELEASE" &&
   "$(cat "$source/RELEASE")" == 8.1.2 ]] || {
    echo "FFMPEG_SOURCE must name an existing FFmpeg 8.1.2 source tree" >&2; exit 1;
}
source="$(realpath "$source")"
for output in "$build" "$prefix"; do
    case "$root/$output" in
        "$source"|"$source"/*)
            echo "FFmpeg outputs must not modify the source tree" >&2; exit 1;;
    esac
done
# Same toolchain as the daemon: arm-linux-gnueabihf, not the 32-bit-time_t
# arm-none default. ARM_CXX (g++) wins when make exports it.
cross="${ARM_FFMPEG_CROSS:-}"
if [[ -z "$cross" && -n "${ARM_CXX:-}" ]]; then
    cxx_path="$(command -v "$ARM_CXX" || true)"
    if [[ -n "$cxx_path" ]]; then
        cross="${cxx_path%g++}"
    fi
fi
if [[ -z "$cross" ]]; then
    for compiler in arm-linux-gnueabihf-gcc arm-none-linux-gnueabihf-gcc \
        "${ARM_TOOLCHAIN_BIN:-/opt/arm-gnu-toolchain/bin}/arm-none-linux-gnueabihf-gcc"; do
        if compiler_path="$(command -v "$compiler")"; then
            cross="${compiler_path%gcc}"
            break
        fi
    done
fi
[[ -n "$cross" ]] && command -v "${cross}gcc" >/dev/null &&
    command -v "${cross}nm" >/dev/null || {
    echo "An existing ARM hard-float cross toolchain is required" >&2; exit 1;
}
# glibc 64-bit time_t. The flags are not part of the configure-line hash;
# TIME_T_BITS in the prefix is what the daemon link checks.
time64_flags="-D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64"
jobs="${FFMPEG_JOBS:-2}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "FFMPEG_JOBS must be positive" >&2; exit 1; }
mkdir -p "$(dirname "$build")" "$(dirname "$prefix")"
mkdir "$build" "$build/obj" "$build/runtime"
export TMPDIR="$root/$build/runtime"
cp -a "$source" "$build/source"
if [[ -f "$build/source/ffbuild/config.mak" ]]; then
    make -s -C "$build/source" distclean
fi
cat > "$build/time_t_width.c" << 'EOF'
#include <time.h>
char mpx_time_t_is_64[sizeof(time_t) == 8 ? 1 : -1];
EOF
"${cross}gcc" -c $time64_flags -o "$build/time_t_width.o" "$build/time_t_width.c" || {
    echo "FFmpeg toolchain ${cross}gcc cannot build 64-bit time_t" >&2; exit 1;
}
# HTTPS uses the same static OpenSSL the daemon already links (AirPlay).
# tls_openssl.c also calls the UDP helpers (DTLS), so udp is in the protocol
# list. Playback still opens https. The prefix path stays out of the
# configure hash; --enable-openssl and the protocol list are what
# check_arm_ffmpeg_static.py hashes.
openssl_prefix="${ARM_OPENSSL_PREFIX:-${ARM_AIRPLAY_PREFIX:-}}"
if [[ -z "$openssl_prefix" ]]; then
    for cand in \
        /tmp/mpx-repro/src/build/deps/airplay-arm \
        "$root/build/deps/airplay-arm"; do
        if [[ -f "$cand/lib/libssl.a" && -f "$cand/include/openssl/ssl.h" ]]; then
            openssl_prefix="$cand"
            break
        fi
    done
fi
[[ -n "$openssl_prefix" && -f "$openssl_prefix/lib/libssl.a" &&
   -f "$openssl_prefix/include/openssl/ssl.h" ]] || {
    echo "HTTPS needs the static OpenSSL the daemon already links (libssl.a + include/openssl/ssl.h)" >&2
    exit 1
}
export PKG_CONFIG_LIBDIR="$openssl_prefix/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
ossl_cflags="-I$openssl_prefix/include"
ossl_ldflags="-L$openssl_prefix/lib"
cd "$build/obj"
# GATE7-1018: aac,ac3,eac3,mp3 are the raw-audio demuxers MPEG-TS uses to settle its codec probe of an audio PES
# (request_probe). Without them the probe never scores and buffers up to 2500 packets, each video packet
# pinning a 200 KiB PES buffer: ~350 MB per GCAST yt.ts open, the rc3 G7 OOM. check_arm_ffmpeg_static.py
# requires ff_aac_demuxer.
# Feature flags come from check_arm_ffmpeg_static.py so the prefix hash and the
# embedded --enable-decoder list cannot drift from the checker.
# shellcheck disable=SC2206
extra=( $configure_flags )
../source/configure \
    --prefix="$root/$prefix" --arch=arm --cpu=cortex-a9 --target-os=linux \
    --enable-cross-compile --cross-prefix="$cross" \
    --extra-cflags="$time64_flags $ossl_cflags" --extra-cxxflags="$time64_flags $ossl_cflags" \
    --extra-ldflags="$ossl_ldflags" --pkg-config-flags=--static \
    "${extra[@]}"
make -s -j"$jobs"
make -s install
printf '64\n' > "$root/$prefix/TIME_T_BITS"
python3 "$root/scripts/check_arm_ffmpeg_static.py" --prefix "$root/$prefix" --nm "${cross}nm"
