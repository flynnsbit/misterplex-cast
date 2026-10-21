#!/usr/bin/env bash
# Package misterplexd ARM binary + conf example + named RBF pairs + docs.
set -euo pipefail
# Seed 10 PlexOB (RTL 08076620, BUILD_ID 0x0B59C11A). This is the only
# bitstream md5 the package accepts. A caller pin that is not this value
# is refused before the daemon rebuild.
PLEXOB_RBF_MD5_PIN="c8b346048d680cc410a447490f57a820"
_plexob_pin=$(printf '%s' "${PLEXOB_RBF_MD5:-$PLEXOB_RBF_MD5_PIN}" | tr '[:upper:]' '[:lower:]')
if [[ "$_plexob_pin" != "$PLEXOB_RBF_MD5_PIN" ]]; then
  echo "ERROR: refusing PlexOB pin that is not the seed-10 bitstream" >&2
  exit 1
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
OUT_DIR="${OUT_DIR:-$ROOT/dist}"
STAGE="$OUT_DIR/stage-misterplex"
TAR="$OUT_DIR/misterplex-${VERSION}.tar.gz"

ARM_BIN="$ROOT/build/arm/misterplexd"
PAIRED=0
PAIR_ART="$ROOT/release_artifacts/v0.9.0-pre-paired"

# Dual-gate: v0.9.0-pre ships named gold pairs; older lines keep one Plex.rbf.
# Override either path or md5 via RBF_PATH / RBF_MD5_EXPECTED for lab freezes.
MD5_480P_RBF="07f54d9f8f0eda2fe75d9cc314f6de54"
MD5_240P15_RBF="4d6efef954acf7b33747f35ac2878c1b"
MD5_480I_RBF="61db00e7d54efad7c1a456b127b798bd"
MD5_720P24_RBF="0eea3580a5a0dacf60bf65c50499bcc9"
MD5_480P_DAEMON="5f1c861486844f4c83bf32a2112cfbb6"
MD5_720P24_DAEMON="0ffc1483677ef5733971d4f18e4624e4"

case "$VERSION" in
  v0.9.0-pre*|0.9.0-pre*)
    PAIRED=1
    RBF_MD5_EXPECTED="${RBF_MD5_EXPECTED:-$MD5_480P_RBF}"
    RBF_DEFAULT="$PAIR_ART/cores/Plex_480p.rbf"
    ;;
  v0.4.0*|0.4.0*)
    # softc24 HOLD=2 + CONF_STR v9 Content O[5:4] + Display O[15:14] Follow
    RBF_MD5_EXPECTED="${RBF_MD5_EXPECTED:-1c6ed06fe832fb54259d4f4ce504ccae}"
    RBF_DEFAULT="$ROOT/release_artifacts/v0.4.0/Plex.rbf"
    ;;
  *)
    RBF_MD5_EXPECTED="${RBF_MD5_EXPECTED:-41adb98c7a630b541091c22ce291be68}"
    RBF_DEFAULT="$ROOT/release_artifacts/v0.3.0/Plex.rbf"
    ;;
esac
RBF_SRC="${RBF_PATH:-$RBF_DEFAULT}"
CONF_EX="$ROOT/assets/misterplex.conf.example"

copy_gated_md5() {
  local src=$1 dest=$2 expect=$3
  [[ -f "$src" ]] || { echo "ERROR: missing $src" >&2; exit 1; }
  local actual
  actual="$(md5sum "$src" | awk '{print $1}')"
  if [[ "$actual" != "$expect" ]]; then
    echo "ERROR: refusing unverified $(basename "$src"): $src" >&2
    echo "       expected md5: $expect" >&2
    echo "       actual md5:   $actual" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$dest")"
  cp -a "$src" "$dest"
  echo "Included $(basename "$dest") from $src ($(wc -c <"$dest") bytes, md5=$actual)"
}

# FFmpeg in this daemon is libav linked into bin/misterplexd (LGPL-2.1-or-later).
# Stage that licence, plus every other static third-party library the link pulls in.
stage_static_licenses() {
  local lic_src="$ROOT/packaging/licenses"
  [[ -d "$lic_src/ffmpeg" && -d "$lic_src/openssl" ]] || {
    echo "ERROR: missing $lic_src" >&2
    exit 1
  }
  local ff_copy="$ROOT/build/deps/ffmpeg-8.1.2/COPYING.LGPLv2.1"
  if [[ -f "$ff_copy" ]] && ! cmp -s "$ff_copy" "$lic_src/ffmpeg/COPYING.LGPLv2.1"; then
    echo "ERROR: vendored FFmpeg LGPL text differs from the 8.1.2 source" >&2
    exit 1
  fi
  local ssl_copy=""
  local cand
  for cand in \
    /tmp/mpx-repro/dep/openssl-3.0.15/LICENSE.txt \
    "$ROOT/build/deps/airplay-arm/src/openssl-3.0.15/LICENSE.txt"
  do
    if [[ -f "$cand" ]]; then
      ssl_copy=$cand
      break
    fi
  done
  if [[ -n "$ssl_copy" ]] && ! cmp -s "$ssl_copy" "$lic_src/openssl/LICENSE.txt"; then
    echo "ERROR: vendored OpenSSL license differs from the 3.0.15 source" >&2
    exit 1
  fi
  if ! cmp -s "$ROOT/third_party/owenb-h264ring/LICENSE" "$lic_src/owenb-h264ring/LICENSE"; then
    echo "ERROR: vendored owenb-h264ring LICENSE differs from third_party" >&2
    exit 1
  fi
  mkdir -p "$STAGE/licenses"
  cp -a "$lic_src/." "$STAGE/licenses/"
  python3 - "$ARM_BIN" "$STAGE/licenses" "$ROOT/build/deps/ffmpeg-8.1.2.tar.xz" <<'PY'
import pathlib, re, sys

binary, dest, tarball = sys.argv[1:]
dest = pathlib.Path(dest)
data = pathlib.Path(binary).read_bytes()
idx = data.find(b"--enable-openssl")
if idx < 0:
    sys.exit("misterplexd has no embedded libav configure line")
start = idx
while start > 0 and 32 <= data[start - 1] < 127:
    start -= 1
end = idx
while end < len(data) and 32 <= data[end] < 127:
    end += 1
cfg = data[start:end].decode("ascii")
if not cfg.startswith("--prefix="):
    sys.exit("embedded --enable-openssl is not the libav configure line")
if re.search(r"(^|\s)--enable-gpl(\s|$)", cfg):
    sys.exit("libav configure passes --enable-gpl")
need = "--enable-protocol='file,http,tcp,pipe,https,tls,udp'"
if need not in cfg or "--enable-openssl" not in cfg.split():
    sys.exit("libav configure is missing openssl or the https/tls/udp protocols")
recorded = "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
shown = re.sub(r"(?:/workspace|/tmp/mpx-repro)/[^\s']+", "<build>", cfg)
if "/workspace" in shown or "/tmp/mpx-repro" in shown:
    sys.exit("ffmpeg provenance still contains a build-host path")
tarball_path = pathlib.Path(tarball)
if tarball_path.is_file():
    import hashlib
    got = hashlib.sha256(tarball_path.read_bytes()).hexdigest()
    if got != recorded:
        sys.exit("ffmpeg-8.1.2.tar.xz sha256 is %s" % got)

(dest / "ffmpeg" / "PROVENANCE.md").write_text(
    """# FFmpeg libav statically linked into bin/misterplexd

Version: 8.1.2
License: LGPL-2.1-or-later. The text is COPYING.LGPLv2.1 from the FFmpeg 8.1.2 source.
Source URL: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
Source identity: the FFmpeg 8.1.2 release tarball (there is no git commit). sha256 %s
Libraries inside bin/misterplexd: libavformat, libavcodec, libavutil, libswresample.

The configure line passes --enable-openssl and does not pass --enable-gpl, so this libav stays LGPL-2.1-or-later.

Configure line embedded in bin/misterplexd:

```
%s
```

Source offer: the corresponding source for the libav in bin/misterplexd is that release tarball, built with the configure line above for armhf with 64-bit off_t and time_t. OpenSSL is linked separately; see ../openssl/.
"""
    % (recorded, shown)
)

(dest / "openssl" / "PROVENANCE.md").write_text(
    """# OpenSSL statically linked into bin/misterplexd

Version: 3.0.15
License: Apache-2.0. The text is LICENSE.txt from the OpenSSL 3.0.15 source.
Source URL: https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz
Libraries: libssl.a and libcrypto.a, used by libav TLS and by the AirPlay stack.
"""
)

(dest / "libplist" / "PROVENANCE.md").write_text(
    """# libplist statically linked into bin/misterplexd

Version: 2.6.0
License: LGPL-2.1-or-later. The text is COPYING.LESSER from the libplist 2.6.0 source.
Source URL: https://github.com/libimobiledevice/libplist/releases/download/2.6.0/libplist-2.6.0.tar.bz2
Library: libplist-2.0.a, linked with the AirPlay stack.
"""
)

(dest / "uxplay" / "PROVENANCE.md").write_text(
    """# UxPlay (libairplay) statically linked into bin/misterplexd

Version: commit 4e95a9ec013c52d8555d88dbc55abe346e94d56e (FDH2/UxPlay)
Source URL: https://github.com/FDH2/UxPlay/commit/4e95a9ec013c52d8555d88dbc55abe346e94d56e
Build: scripts/build_arm_airplay.sh compiles lib/*.c after
tools/gcast/uxplay-patches/apply_gcast_uxplay_hardening.py,
apply_gcast_uxplay_ptth.py, and apply_gcast_uxplay_zerodur.py.

License of the objects in libairplay.a:
- Most lib/*.c and lib/*.h that carry a header are LGPL-2.1-or-later (COPYING.LGPLv2.1).
- lib/crypto.c and lib/crypto.h are GPL-3.0-or-later. LICENSE is the GNU GPL version 3 text shipped at the root of that tree.
- lib/srp.c is MIT; see ../csrp/.
- lib/playfair, lib/llhttp, and lib/mdnsd are separate notices; see ../playfair, ../llhttp, and ../mdnsd.
"""
)

(dest / "playfair" / "PROVENANCE.md").write_text(
    """# playfair statically linked into bin/misterplexd

Version: the lib/playfair sources in UxPlay commit 4e95a9ec013c52d8555d88dbc55abe346e94d56e
License: GPL-3.0. LICENSE.md is the licence text shipped in that directory.
Those objects are part of libairplay.a.
"""
)

(dest / "llhttp" / "PROVENANCE.md").write_text(
    """# llhttp statically linked into bin/misterplexd

Version: 9.4.2 (LLHTTP_VERSION_MAJOR/MINOR/PATCH in UxPlay lib/llhttp/llhttp.h)
License: MIT. LICENSE-MIT is the text shipped in that directory.
Copyright Fedor Indutny, 2018.
Source: vendored at UxPlay lib/llhttp, commit 4e95a9ec013c52d8555d88dbc55abe346e94d56e.
"""
)

(dest / "mdnsd" / "PROVENANCE.md").write_text(
    """# mdnsd statically linked into bin/misterplexd

Version: the lib/mdnsd sources in UxPlay commit 4e95a9ec013c52d8555d88dbc55abe346e94d56e
License: LGPL-2.1-or-later. mdnsd.c states Copyright (C) 2026 kgbook and LGPL-2.1-or-later.
COPYING.LGPLv2.1 is that licence text.
"""
)

(dest / "csrp" / "PROVENANCE.md").write_text(
    """# csrp statically linked into bin/misterplexd

Version: lib/srp.c in UxPlay commit 4e95a9ec013c52d8555d88dbc55abe346e94d56e
License: MIT. LICENSE is the MIT text from the header of that file.
Copyright (c) 2010 Tom Cocagne. Copyright (c) 2013 Tom Cocagne.
Source URL: https://github.com/cocagne/csrp
"""
)

(dest / "owenb-h264ring" / "PROVENANCE.md").write_text(
    """# owenb-h264ring statically linked into bin/misterplexd

Version: upstream hps/ at commit eac1064 (2026-10-15)
License: MIT. LICENSE is third_party/owenb-h264ring/LICENSE.
Source URL: https://github.com/owenb321/MiSTer_h264_decoder (directory hps/ only).
The rest of that upstream repository is GPL-3.0-or-later and is not linked.
"""
)

(dest / "gcc" / "PROVENANCE.md").write_text(
    """# GCC runtime statically linked into bin/misterplexd

Version: Ubuntu arm-linux-gnueabihf g++ 13.3.0 (gcc-13-arm-linux-gnueabihf-base 13.3.0-6ubuntu2~24.04.1cross1).
The ship binary's .comment records GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0.
Libraries: libgcc, libstdc++, and libatomic, pulled in by -static.
License: GPL-3.0-or-later with the GCC Runtime Library Exception.
copyright is the Ubuntu copyright file for that compiler, which includes those terms.
"""
)

(dest / "glibc" / "PROVENANCE.md").write_text(
    """# glibc statically linked into bin/misterplexd

Version: GNU C Library 2.39, Ubuntu package libc6-dev-armhf-cross 2.39-0ubuntu8cross1.
License: LGPL-2.1-or-later. COPYING.LGPLv2.1 is that licence text.
-static links libc, libm, libdl, and libpthread from this library.
"""
)

source_line = "source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later).\n"
(dest / "rbf" / "PROVENANCE.md").write_text(
    """# Bitstreams shipped in this package

PlexOB.rbf (also at cores/PlexOB.rbf and _Utility/PlexOB.rbf) is GPL-3.0-or-later.
LICENSE is the GNU General Public License version 3.
It is built from the MiSTer_h264_decoder fork at eac1064 (hps/ of that tree is MIT and is not the bitstream).
The PlexOB RTL in this repository is commit 080766203d2ee996bd83a0a2e07735e4292f10ea
(fpga/owenb-combo-s9/PlexOB_480p, BUILD_ID 0x0B59C11A, SEED 10).

Plex.rbf is built on the GPL MiSTer framework. sys_top.v is GPL version 2 or, at your option, any later version.
GPL-2 is fpga/Plex_MiSTer/LICENSE (the same text as fpga/owenb-combo-s9/Plex_MiSTer/LICENSE).
"""
)
(dest / "stb" / "PROVENANCE.md").write_text(
    """# stb headers linked into bin/misterplexd

stb_image.h v2.30, stb_image_resize2.h v2.18, and stb_truetype.h v1.26.
Vendored at host/libmisterplex/plexui/third_party/.
Each header states that it is public domain. NOTICE quotes those lines.
"""
)
(dest / "qoi" / "PROVENANCE.md").write_text(
    """# qoi.h linked into bin/misterplexd

Vendored at host/libmisterplex/plexui/third_party/qoi.h.
Copyright (c) 2021 Dominic Szablewski. SPDX-License-Identifier: MIT.
NOTICE quotes the header.
"""
)
(dest / "README.md").write_text(
    """# License

bin/misterplexd is a combined work distributed under the GNU General Public License version 3 (licenses/uxplay/LICENSE). It statically links UxPlay lib/crypto.c (from RPiPlay, GPL-3.0-or-later) and lib/playfair (GPL-3.0). The other linked libraries keep their own licences, all compatible with GPLv3: FFmpeg 8.1.2 libav, libplist, glibc and mdnsd (LGPL-2.1-or-later), OpenSSL 3.0.15 (Apache-2.0), csrp, llhttp and owenb-h264ring (MIT), and the GCC runtime (GPL-3.0 with the runtime exception).

FFmpeg is statically linked libav (LGPL-2.1-or-later): libavformat, libavcodec, libavutil, and libswresample from FFmpeg 8.1.2. licenses/ffmpeg/ has the LGPL-2.1 text, the source URL, and the configure line (--enable-openssl, no --enable-gpl).

PlexOB.rbf is built from a GPL-3.0-or-later fork of MiSTer_h264_decoder (eac1064). Plex.rbf is built on the GPL MiSTer framework. See rbf/.

stb (public domain) and qoi (MIT) notices are under stb/ and qoi/.

%s
| Directory | Component | Version | Licence |
|-----------|-----------|---------|---------|
| ffmpeg/ | libavformat, libavcodec, libavutil, libswresample | 8.1.2 | LGPL-2.1-or-later |
| openssl/ | libssl, libcrypto | 3.0.15 | Apache-2.0 |
| libplist/ | libplist | 2.6.0 | LGPL-2.1-or-later |
| uxplay/ | UxPlay libairplay | 4e95a9ec013c52d8555d88dbc55abe346e94d56e | LGPL-2.1-or-later, plus GPL-3.0-or-later for crypto.c |
| playfair/ | playfair | same UxPlay commit | GPL-3.0 |
| llhttp/ | llhttp | 9.4.2 | MIT |
| mdnsd/ | mdnsd | same UxPlay commit | LGPL-2.1-or-later |
| csrp/ | csrp (lib/srp.c) | same UxPlay commit | MIT |
| owenb-h264ring/ | h264ring client | eac1064 | MIT |
| gcc/ | libgcc, libstdc++, libatomic | 13.3.0 | GPL-3.0-or-later with the GCC Runtime Library Exception |
| glibc/ | glibc | 2.39 | LGPL-2.1-or-later |
| rbf/ | PlexOB.rbf and Plex.rbf | PlexOB RTL 080766203d2ee996bd83a0a2e07735e4292f10ea | GPL-3.0-or-later; MiSTer framework GPL-2.0-or-later |
| stb/ | stb_image, stb_image_resize2, stb_truetype | 2.30 / 2.18 / 1.26 | public domain |
| qoi/ | qoi.h | 2021 | MIT |
| spleen/ | Spleen 8x16 | 2.2.0 | BSD-2-Clause |
| mozilla-ca/ | Mozilla CA bundle (misterplex-cacert.pem) | 2026-09-25 extract | MPL-2.0 (certdata.txt) |
| ytdlp/ | yt-dlp onedir, embedded CPython, PyInstaller | 2026.08.19 | Unlicense; PSF-2.0; GPL-2.0-or-later bootloader |
| gcast-ffmpeg/ | on-card helper ffmpeg and ffprobe (libx264, OpenSSL) | 8.1.2 | GPL-2.0-or-later (x264); GPL-3.0-or-later (FFmpeg --enable-gpl --enable-version3); Apache-2.0 (OpenSSL 3.0.15) |
"""
    % source_line
)
print("staged licenses for", binary)
PY
}

echo "=== package_release $VERSION paired=$PAIRED ==="

# The public tree builds misterplexd from this source. Reusing a binary from
# another commit would ship strings that this tree has already removed.
if [[ "${PACKAGE_REUSE_ARM_BIN:-0}" == "1" ]]; then
  echo "ERROR: PACKAGE_REUSE_ARM_BIN=1 cannot ship; build misterplexd from this source" >&2
  exit 1
fi

# Resolve the seed-10 bitstream and refuse anything else before the rebuild.
# PLEXOB_RBF, when set, wins. Otherwise the tracked re-fit, then the
# verified lab path.
if [[ -z "${PLEXOB_RBF:-}" ]]; then
  if [[ -f "$ROOT/release_artifacts/plexob-yc-0b59c11a/PlexOB.rbf" ]]; then
    PLEXOB_RBF="$ROOT/release_artifacts/plexob-yc-0b59c11a/PlexOB.rbf"
  else
    echo "ERROR: set PLEXOB_RBF to the seed-10 PlexOB.rbf (md5 $PLEXOB_RBF_MD5_PIN)" >&2
    exit 2
  fi
fi
repo_slot="$(tr -d '[:space:]' < "$ROOT/cores/PlexOB.rbf.md5" | tr '[:upper:]' '[:lower:]')"
if [[ "$repo_slot" != "$PLEXOB_RBF_MD5_PIN" ]]; then
  echo "ERROR: cores/PlexOB.rbf.md5 is not the seed-10 pin" >&2
  exit 1
fi
if [[ ! -f "$PLEXOB_RBF" ]]; then
  echo "ERROR: PlexOB.rbf missing: $PLEXOB_RBF" >&2
  exit 1
fi
_plexob_got="$(md5sum "$PLEXOB_RBF" | awk '{print $1}' | tr '[:upper:]' '[:lower:]')"
if [[ "$_plexob_got" != "$PLEXOB_RBF_MD5_PIN" ]]; then
  echo "ERROR: refusing PlexOB.rbf whose md5 is not the seed-10 pin" >&2
  exit 1
fi
PLEXOB_RBF_MD5="$PLEXOB_RBF_MD5_PIN"

echo "Building arm misterplexd…"
export PATH="${PATH}:${ARM_TOOLCHAIN_BIN:-/opt/arm-gnu-toolchain/bin}"
make -C "$ROOT" arm-plexd ARM_PLEXD_AIRPLAY=1
[[ -f "$ARM_BIN" ]] || { echo "missing $ARM_BIN"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/conf" "$STAGE/docs" "$STAGE/cores" "$STAGE/linux"
# Stock MiSTer Linux has no CA store. HTTPS Date and lounge TLS read
# /media/fat/linux/misterplex-cacert.pem (see mpx::caBundlePath).
CA_BUNDLE="$ROOT/third_party/mozilla-ca/cacert.pem"
if [[ ! -s "$CA_BUNDLE" ]]; then
  echo "ERROR: missing Mozilla CA bundle $CA_BUNDLE" >&2
  exit 1
fi
cp -a "$CA_BUNDLE" "$STAGE/linux/misterplex-cacert.pem"

# yt-dlp onedir zip (2026.08.19, ~37 MB). Upstream dropped the bare armv7l binary.
# Bundled so a fresh card can unpack offline. Omitted from git; fetched here.
YTDLP_ZIP_URL="${YTDLP_ZIP_URL:-https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp_linux_armv7l.zip}"
YTDLP_SUM_URL="${YTDLP_SUM_URL:-https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/SHA2-256SUMS}"
YTDLP_ZIP_MAX=$((48 * 1024 * 1024))
ytdlp_stage=""
if curl -fsSL --cacert "$CA_BUNDLE" --max-time 180 -o "$STAGE/linux/SHA2-256SUMS" "$YTDLP_SUM_URL" \
   && curl -fsSL --cacert "$CA_BUNDLE" --max-time 180 -o "$STAGE/linux/yt-dlp_linux_armv7l.zip" "$YTDLP_ZIP_URL"; then
  ybytes=$(wc -c < "$STAGE/linux/yt-dlp_linux_armv7l.zip")
  yhex=$(awk '$2=="yt-dlp_linux_armv7l.zip"{print $1; exit}' "$STAGE/linux/SHA2-256SUMS")
  ygot=$(sha256sum "$STAGE/linux/yt-dlp_linux_armv7l.zip" | awk '{print $1}')
  if [[ -n "$yhex" && "$yhex" == "$ygot" && "$ybytes" -le "$YTDLP_ZIP_MAX" ]]; then
    printf '%s\n' "$yhex" > "$STAGE/linux/yt-dlp_linux_armv7l.zip.sha256"
    cp -a "$STAGE/linux/yt-dlp_linux_armv7l.zip" "$STAGE/bin/yt-dlp_linux_armv7l.zip"
    echo "Included yt-dlp_linux_armv7l.zip ($ybytes bytes, sha256=$ygot)"
  else
    echo "NOTE: yt-dlp zip failed checksum or size ($ybytes bytes); not bundled"
    rm -f "$STAGE/linux/yt-dlp_linux_armv7l.zip" "$STAGE/linux/SHA2-256SUMS"
  fi
else
  echo "NOTE: yt-dlp zip download failed; tarball will not carry an offline copy"
  rm -f "$STAGE/linux/yt-dlp_linux_armv7l.zip" "$STAGE/linux/SHA2-256SUMS"
fi
# On-card AirPlay helper. The boot hook copies this to the misterplex root
# and starts it on 127.0.0.1:18781. Not bin/ffmpeg: that path stays forbidden.
mkdir -p "$STAGE/linux/gcast"
cp -a "$ROOT/tools/gcast/gcast_xcode.py" "$STAGE/linux/gcast/gcast_xcode.py"
cp -a "$ROOT/tools/gcast/yt_standalone/run-gcast-xcode.sh" "$STAGE/linux/gcast/run-gcast-xcode.sh"
chmod +x "$STAGE/linux/gcast/gcast_xcode.py" "$STAGE/linux/gcast/run-gcast-xcode.sh"
GCAST_FFMPEG_BIN="${GCAST_FFMPEG_BIN:-/tmp/mpx-gcast-ffmpeg/ffmpeg}"
GCAST_FFPROBE_BIN="${GCAST_FFPROBE_BIN:-/tmp/mpx-gcast-ffmpeg/ffprobe}"
if [[ ! -x "$GCAST_FFMPEG_BIN" || ! -x "$GCAST_FFPROBE_BIN" ]]; then
  echo "ERROR: on-card AirPlay helper needs static ffmpeg and ffprobe at $GCAST_FFMPEG_BIN and $GCAST_FFPROBE_BIN" >&2
  exit 1
fi
cp -a "$GCAST_FFMPEG_BIN" "$STAGE/linux/gcast/ffmpeg"
cp -a "$GCAST_FFPROBE_BIN" "$STAGE/linux/gcast/ffprobe"
chmod +x "$STAGE/linux/gcast/ffmpeg" "$STAGE/linux/gcast/ffprobe"
if [[ -e "$STAGE/bin/ffmpeg" ]]; then
  echo "ERROR: refusing to package a standalone ffmpeg program at bin/ffmpeg" >&2
  exit 1
fi

if [[ -f "$STAGE/linux/yt-dlp_linux_armv7l.zip" ]]; then
  YTDLP_NOTE="linux/yt-dlp_linux_armv7l.zip (verified, about 37 MB) is unpacked to /media/fat/linux/misterplex/yt-dlp/ by misterplexd-boot-rc3.sh so a fresh card works offline."
else
  YTDLP_NOTE="yt-dlp_linux_armv7l.zip was NOT bundled in this tarball (download failed or exceeded 48 MB). Run update_ytdlp.sh once the clock and network are up."
fi

if [[ "$PAIRED" == "1" ]]; then
  copy_gated_md5 "$PAIR_ART/bin/misterplexd.480p" "$STAGE/bin/misterplexd.480p" "$MD5_480P_DAEMON"
  copy_gated_md5 "$PAIR_ART/bin/misterplexd.720p24" "$STAGE/bin/misterplexd.720p24" "$MD5_720P24_DAEMON"
  cp -a "$STAGE/bin/misterplexd.480p" "$STAGE/bin/misterplexd.240p15"
  cp -a "$STAGE/bin/misterplexd.480p" "$STAGE/bin/misterplexd.480i"
  # Direct-run default is the 480p gold daemon. Current-tree rebuild is
  # recorded next to it; do not overwrite the frozen pair.
  cp -a "$STAGE/bin/misterplexd.480p" "$STAGE/bin/misterplexd"
  SRC_MD5="$(md5sum "$ARM_BIN" | awk '{print $1}')"
  echo "Source-tree arm misterplexd md5=$SRC_MD5 (not shipped as a pair; freeze 480p=$MD5_480P_DAEMON 720p24=$MD5_720P24_DAEMON)"
  cp -a "$ARM_BIN" "$STAGE/bin/misterplexd.src-built"
  chmod +x "$STAGE/bin/misterplexd.src-built"
else
  cp -a "$ARM_BIN" "$STAGE/bin/misterplexd"
  chmod +x "$STAGE/bin/misterplexd"
fi
chmod +x "$STAGE/bin/misterplexd"
if [[ -f "$ROOT/build/arm/push_frame" ]]; then
  cp -a "$ROOT/build/arm/push_frame" "$STAGE/bin/push_frame"
  chmod +x "$STAGE/bin/push_frame"
fi
if [[ -f "$ROOT/build/arm/set_status" ]]; then
  cp -a "$ROOT/build/arm/set_status" "$STAGE/bin/set_status"
  chmod +x "$STAGE/bin/set_status"
fi

if [[ -f "$CONF_EX" ]]; then
  cp -a "$CONF_EX" "$STAGE/conf/misterplex.conf.example"
else
  cat >"$STAGE/conf/misterplex.conf.example" <<'EOF'
# Set this to your Plex Media Server, for example:
# PLEX_BASE=http://YOUR-PLEX-SERVER:32400
DECODE=320x240
PRESENT=fb0
STREAM=0
MATCH_SOURCE_HZ=off
EOF
fi

# RBF: releases must package the exact hardware-validated bitstream. Do not
# silently search output_files/, releases/, or MISTER_DEV: stale local cores are
# indistinguishable by path and have already nearly shipped once. Operators may
# pass RBF_PATH=/path/to/Plex.rbf, but every candidate is gated by this MD5.
if [[ -n "${PACKAGE_ALLOW_NO_RBF:-}" || -n "${PACKAGE_DAEMON_ONLY:-}" ]]; then
  echo "ERROR: daemon-only packages are disabled for release builds; $VERSION must ship a verified Plex.rbf." >&2
  exit 1
fi
if [[ "$RBF_MD5_EXPECTED" == "REPLACE_AT_FREEZE" ]]; then
  echo "ERROR: RBF_MD5_EXPECTED still REPLACE_AT_FREEZE for $VERSION." >&2
  echo "       Freeze a LOCK_OK core into release_artifacts/ and set RBF_MD5_EXPECTED=<md5>." >&2
  exit 1
fi

RBF_MD5_ACTUAL=""
if [[ "$PAIRED" == "1" ]]; then
  copy_gated_md5 "$PAIR_ART/cores/Plex_480p.rbf" "$STAGE/cores/Plex_480p.rbf" "$MD5_480P_RBF"
  copy_gated_md5 "$PAIR_ART/cores/Plex_240p15.rbf" "$STAGE/cores/Plex_240p15.rbf" "$MD5_240P15_RBF"
  copy_gated_md5 "$PAIR_ART/cores/Plex_480i.rbf" "$STAGE/cores/Plex_480i.rbf" "$MD5_480I_RBF"
  copy_gated_md5 "$PAIR_ART/cores/Plex_720p24.rbf" "$STAGE/cores/Plex_720p24.rbf" "$MD5_720P24_RBF"
  RBF_MD5_ACTUAL="$MD5_480P_RBF"
  cp -a "$PAIR_ART/rbf_daemon_pairs.txt" "$STAGE/rbf_daemon_pairs.txt"
  cp -a "$PAIR_ART/Plex_README.txt" "$STAGE/Plex_README.txt"
  cp -a "$PAIR_ART/README.md" "$STAGE/README.md"
  # No generic Plex.rbf — every sibling reports CORENAME=Plex.
else
  if [[ ! -f "$RBF_SRC" ]]; then
    echo "ERROR: verified release core missing: $RBF_SRC" >&2
    echo "       Use release_artifacts for $VERSION, or set RBF_PATH to a core with MD5 $RBF_MD5_EXPECTED." >&2
    exit 1
  fi
  RBF_MD5_ACTUAL="$(md5sum "$RBF_SRC" | awk '{print $1}')"
  if [[ "$RBF_MD5_ACTUAL" != "$RBF_MD5_EXPECTED" ]]; then
    echo "ERROR: refusing to package unverified Plex.rbf: $RBF_SRC" >&2
    echo "       expected md5: $RBF_MD5_EXPECTED" >&2
    echo "       actual md5:   $RBF_MD5_ACTUAL" >&2
    echo "       $VERSION ships only a hardware-validated core (see docs/release-notes-${VERSION#v}.md)." >&2
    exit 1
  fi
  cp -a "$RBF_SRC" "$STAGE/cores/Plex.rbf"
  echo "Included verified cores/Plex.rbf from $RBF_SRC ($(wc -c <"$STAGE/cores/Plex.rbf") bytes, md5=$RBF_MD5_ACTUAL)"
fi

# Y/C PlexOB seed 10, BUILD_ID 0x0B59C11A. The bitstream checked above is
# installed at both prod paths. cores/PlexOB.rbf.md5 is that same pin.
sh "$ROOT/scripts/stage_plexob_yc_rbf.sh" "$PLEXOB_RBF" "$STAGE" "$PLEXOB_RBF_MD5"
cp -a "$STAGE/PlexOB.rbf" "$STAGE/cores/PlexOB.rbf"
printf '%s\n' "$PLEXOB_RBF_MD5" > "$STAGE/cores/PlexOB.rbf.md5"

# Operator docs
for doc in release.md release-notes-v0.3.0.md release-notes-v0.4.0.md release-notes-v0.9.0-pre.md match-source-hz.md architecture.md subtitles-burnin.md architecture.png architecture.svg VERIFY.md; do
  if [[ -f "$ROOT/docs/$doc" ]]; then
    cp -a "$ROOT/docs/$doc" "$STAGE/docs/"
  fi
done
mkdir -p "$STAGE/scripts"
# install_paired_all.sh is the old named-pair installer (stale pair md5s and a
# hard-coded lab host). The rc3 tarball does not ship it. The paired package
# mode is the only install path that still runs it.
RC3_SCRIPTS="plex_browse.sh plex_menu.sh misterplexd_supervise.sh misterplex_core_watch.sh misterplex_cast_ready.sh misterplex_pair_conf.sh misterplex_named_rbf.sh pms_720p_proxy.py misterplexd-boot-rc3.sh update_ytdlp.sh stage_plexob_yc_rbf.sh"
if [[ "$PAIRED" == "1" ]]; then
  RC3_SCRIPTS="$RC3_SCRIPTS install_paired_all.sh"
fi
for scr in $RC3_SCRIPTS; do
  if [[ -f "$ROOT/scripts/$scr" ]]; then
    cp -a "$ROOT/scripts/$scr" "$STAGE/scripts/"
    chmod +x "$STAGE/scripts/$scr"
  fi
done
# Boot helpers also under bin/ so _user-startup paths stay short.
for scr in misterplexd_supervise.sh misterplex_core_watch.sh misterplex_cast_ready.sh misterplex_pair_conf.sh misterplex_named_rbf.sh misterplexd-boot-rc3.sh update_ytdlp.sh; do
  if [[ -f "$STAGE/scripts/$scr" ]]; then
    cp -a "$STAGE/scripts/$scr" "$STAGE/bin/$scr"
  fi
done

if [[ -d "$ROOT/kmod/mplex_ddr" ]]; then
  mkdir -p "$STAGE/kmod/mplex_ddr"
  cp -a "$ROOT/kmod/mplex_ddr/mplex_ddr.c" "$ROOT/kmod/mplex_ddr/Makefile" "$STAGE/kmod/mplex_ddr/"
fi

stage_static_licenses
if [[ -e "$STAGE/bin/ffmpeg" ]]; then
  echo "ERROR: refusing to package a standalone ffmpeg program" >&2
  exit 1
fi

# Operator README and release notes at the tarball root (rc3 binary package).
# The GPLv3 License section (published source at the rc5 release) is appended
# when the README does not already carry it. The paired package keeps its own
# README.md from the pair artifact.
if [[ "$PAIRED" != "1" ]]; then
  cp -a "$ROOT/packaging/rc3/README.md" "$STAGE/README.md"
  cp -a "$ROOT/packaging/rc3/RELEASE-NOTES.md" "$STAGE/RELEASE-NOTES.md"
  if ! grep -q 'source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later).' "$STAGE/README.md" \
     || ! grep -q 'GNU General Public License version 3' "$STAGE/README.md"; then
    cat >>"$STAGE/README.md" <<'EOF'

## License

bin/misterplexd is a combined work distributed under the GNU General Public License version 3 (licenses/uxplay/LICENSE). It statically links UxPlay lib/crypto.c (from RPiPlay, GPL-3.0-or-later) and lib/playfair (GPL-3.0). The other linked libraries keep their own licences, all compatible with GPLv3: FFmpeg 8.1.2 libav, libplist, glibc and mdnsd (LGPL-2.1-or-later), OpenSSL 3.0.15 (Apache-2.0), csrp, llhttp and owenb-h264ring (MIT), and the GCC runtime (GPL-3.0 with the runtime exception). See licenses/README.md.

source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later).
EOF
  fi
fi

# README.txt, docs/INSTALL.txt, and SHA256SUMS are written below. The Contents
# list is every file that will be in the tarball, including those three.
CONTENTS_LIST="$(
  {
    (cd "$STAGE" && find . -type f -printf '%P\n')
    printf '%s\n' README.txt docs/INSTALL.txt SHA256SUMS
  } | LC_ALL=C sort -u
)"
if printf '%s\n' "$CONTENTS_LIST" | grep -qx 'bin/ffmpeg'; then
  echo "ERROR: Contents would list a standalone ffmpeg program" >&2
  exit 1
fi
CONTENTS_BLOCK="$(printf '%s\n' "$CONTENTS_LIST" | sed 's/^/  /')"

# Path notes for operators (also docs/INSTALL.txt)
if [[ "$PAIRED" == "1" ]]; then
  cat >"$STAGE/README.txt" <<EOF
MiSTerPlex pre-release package
version: ${VERSION}

Named cores (pick ONE from _Utility — no generic Plex.rbf)
----------------------------------------------------------
  cores/Plex_480p.rbf     HDMI 240p/480p  md5 ${MD5_480P_RBF}
  cores/Plex_240p15.rbf   15 kHz 240p CRT md5 ${MD5_240P15_RBF}
  cores/Plex_480i.rbf     15 kHz 480i CRT md5 ${MD5_480I_RBF}
  cores/Plex_720p24.rbf   HDMI 720p24 lab md5 ${MD5_720P24_RBF}

  bin/misterplexd.480p    gold daemon (also .240p15 / .480i)
  bin/misterplexd.720p24  lab 720p remux-PCM daemon
  bin/misterplexd         copy of .480p (direct-run default)
  rbf_daemon_pairs.txt    watcher map

Install
-------
  ./scripts/install_paired_all.sh

Or from the repo freeze:
  REL=release_artifacts/v0.9.0-pre-paired ./scripts/install_paired_all.sh

Set PLEX_BASE in /media/fat/misterplex/misterplex.conf.
See docs/release-notes-v0.9.0-pre.md for 720p findings.

PlexOB Y/C (RTL 08076620, SEED 10, BUILD_ID 0x0B59C11A). Install both:
  /media/fat/PlexOB.rbf
  /media/fat/_Utility/PlexOB.rbf
PlexOB.rbf md5: ${PLEXOB_RBF_MD5}
Word 88 expects 4D505842/0B59C11A. Gate-open must not show "OB input region DEAD".
Gate 5 pad/key is proven. OSD Send Home via Main/uinput is unproven (pad BACK goes home).
CEA 1080p was not measured on this rig.
Seed 6 (ae7e35a6) is the documented fallback only if seed 10 is unavailable. This package does not bundle seed 6.

CA bundle (HTTPS Date and YouTube lounge)
-----------------------------------------
  linux/misterplex-cacert.pem   Mozilla roots (curl.se/ca/cacert.pem)
  Install as /media/fat/linux/misterplex-cacert.pem. Override with MPX_CA_BUNDLE.
EOF
else
  cat >"$STAGE/README.txt" <<EOF
MiSTerPlex release package
version: rc5

License
-------
  bin/misterplexd is a combined work distributed under the GNU General Public License version 3
  (licenses/uxplay/LICENSE). It statically links
  UxPlay lib/crypto.c (from RPiPlay, GPL-3.0-or-later) and lib/playfair
  (GPL-3.0). The other linked libraries keep their own licences, all
  compatible with GPLv3: FFmpeg 8.1.2 libav, libplist, glibc and mdnsd
  (LGPL-2.1-or-later), OpenSSL 3.0.15 (Apache-2.0), csrp, llhttp and
  owenb-h264ring (MIT), and the GCC runtime (GPL-3.0 with the runtime
  exception). See licenses/README.md.
  PlexOB.rbf is built from a GPL-3.0-or-later fork of MiSTer_h264_decoder
  (eac1064). Plex.rbf is built on the GPL MiSTer framework.
  FFmpeg is statically linked libav (LGPL-2.1-or-later):
  libavformat, libavcodec, libavutil, and libswresample from FFmpeg 8.1.2.
  licenses/ffmpeg/ has the LGPL-2.1 text, the source URL, and the configure
  line (--enable-openssl, no --enable-gpl).
  source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later).

Contents
--------
${CONTENTS_BLOCK}

Quick install from this extracted directory
-------------------------------------------
  ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop
  ssh root@<mister-ip> "mkdir -p /media/fat/misterplex /media/fat/_Utility /media/fat/linux"
  scp -r bin scripts docs licenses root@<mister-ip>:/media/fat/misterplex/
  ssh root@<mister-ip> 'test -f /media/fat/linux/misterplex.conf' || scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf
  # The hook starts misterplexd with --conf /media/fat/linux/misterplex.conf
  # when that file exists. /media/fat/misterplex/misterplex.conf is only the
  # fallback when the linux file is missing. A bare daemon with no --conf
  # still defaults to the misterplex/ path; the boot path does not.
  scp cores/Plex.rbf root@<mister-ip>:/media/fat/_Utility/Plex.rbf
  # PlexOB Y/C (RTL 08076620, SEED 10, BUILD_ID 0x0B59C11A). Word 88
  # expects 4D505842/0B59C11A. Gate-open must not show "OB input region DEAD".
  # Gate 5 pad/key is proven. OSD Send Home returns to the Plex home
  # screen. CEA 1080p was not measured on this rig.
  # Seed 6 (ae7e35a6) is the documented fallback only if seed 10 is
  # unavailable. This package does not bundle seed 6. Back up whatever is
  # already on the card, then install only when the bitstream md5 matches
  # the pinned slot cores/PlexOB.rbf.md5. Prod load_core tries
  # /media/fat/PlexOB.rbf, then /media/fat/_Utility/PlexOB.rbf, then
  # /media/fat/_Utility/Plex.rbf. The daemon still prefers PlexOB and
  # recovers a stranded menu core.
  # PlexOB.rbf md5: ${PLEXOB_RBF_MD5}
  ssh root@<mister-ip> 'set -eu
    stamp=\$(date +%Y%m%d%H%M%S)
    for f in /media/fat/PlexOB.rbf /media/fat/_Utility/PlexOB.rbf /media/fat/_Utility/Plex.rbf; do
      if [ -f "\$f" ]; then cp -a "\$f" "\$f.bak.\$stamp"; fi
    done
    mkdir -p /media/fat/_Utility /tmp'
  scp cores/PlexOB.rbf root@<mister-ip>:/tmp/PlexOB.rbf.mpx-tmp
  ssh root@<mister-ip> 'set -eu
    want=${PLEXOB_RBF_MD5}
    got=\$(md5sum /tmp/PlexOB.rbf.mpx-tmp | cut -d" " -f1)
    if [ "\$got" != "\$want" ]; then
      echo "refusing PlexOB.rbf md5 \$got (want \$want)" >&2
      rm -f /tmp/PlexOB.rbf.mpx-tmp
      exit 1
    fi
    for dest in /media/fat/PlexOB.rbf /media/fat/_Utility/PlexOB.rbf; do
      cp /tmp/PlexOB.rbf.mpx-tmp "\$dest.mpx-tmp"
      sync
      mv -f "\$dest.mpx-tmp" "\$dest"
      got=\$(md5sum "\$dest" | cut -d" " -f1)
      test "\$got" = "\$want"
    done
    rm -f /tmp/PlexOB.rbf.mpx-tmp'
  scp linux/misterplex-cacert.pem root@<mister-ip>:/media/fat/linux/misterplex-cacert.pem
  scp -r linux/gcast root@<mister-ip>:/media/fat/linux/

Boot (rc5) — chevron after reboot, no manual core load
-------------------------------------------------------
  misterplexd-boot.sh is the old cand launcher. It does not load a core, so a
  reboot sits on the menu core (colorbars). Use misterplexd-boot-rc3.sh instead.
  It load_core's /media/fat/PlexOB.rbf when that file is present, else
  /media/fat/_Utility/PlexOB.rbf, else /media/fat/_Utility/Plex.rbf (FPGA pin
  prefix 999800cf), waits until CORENAME is Plex or PlexOB, then starts
  bin/misterplexd with MPX_LEGACY_CORE_PREFIX=999800cf.

  Comment out any misterplexd-boot.sh line, then append (the "\$1" is
  required; S99user calls user-startup.sh stop):
    /media/fat/misterplex/bin/misterplexd-boot-rc3.sh "\$1" &
  to /media/fat/linux/user-startup.sh and, if that file already exists,
  /media/fat/linux/_user-startup.sh. The same edit, including a rewrite of
  a no-arg line and a user-startup.sh.bak.<stamp> backup, is:
    /media/fat/misterplex/bin/misterplexd-boot-rc3.sh install

  Boot starts misterplexd with GAIRPLAY=1 GYT=1 and
  --conf /media/fat/linux/misterplex.conf. The hook waits up to 60 seconds
  for a default route and a non-169.254 IPv4 on that route before it
  launches, even when the name is already known. A FRIENDLY_NAME and
  PLAYER_ID already in the conf are kept. Otherwise the hook uses
  FRIENDLY_NAME and MPX_ID in the environment, then --name and --id on a
  misterplexd-boot.sh or misterplexd_supervise.sh line in user-startup.sh
  (a commented-out line still counts), then a persisted identity file.
  NAME= / ID= in the legacy misterplexd-boot.sh is not a source. With none
  of those, the hook picks a non-169.254 IPv4 (the default-route source
  when the wait found one, otherwise any non-link-local) and writes
  FRIENDLY_NAME and PLAYER_ID once as the same string misterplex-<last
  three octets> (192.0.2.12 is misterplex-0.2.12). A later boot does
  not rewrite a saved name. With no usable IPv4 the hook uses MiSTerPlex /
  misterplex-1 for this boot only and does not write it. S99user runs
  user-startup.sh stop at shutdown. With "\$1" the hook SIGINT/SIGKILLs
  the daemon and exits without saving. A no-arg line still refuses to
  launch or save when rcK is running or runlevel is 0 or 6.

  YouTube without gcast-xcode needs the PyInstaller onedir
  yt-dlp_linux_armv7l.zip (stock python3 is 3.9, too old for the zipapp).
  ${YTDLP_NOTE}
  A later refresh (once a day, after the clock is valid):
    /media/fat/misterplex/bin/update_ytdlp.sh
  uses --cacert /media/fat/linux/misterplex-cacert.pem.

AirPlay works on the MiSTer by itself; a helper on a computer is optional
----------------------------------------------------------------------------
  AirPlay works on the MiSTer by itself; a helper on a computer is optional.
  The package includes linux/gcast/ (gcast_xcode.py, run-gcast-xcode.sh,
  ffmpeg, and ffprobe). Copy it with:
    scp -r linux/gcast root@<mister-ip>:/media/fat/linux/
  misterplexd-boot-rc3.sh install turns on start at boot. The hook starts
  the helper on 127.0.0.1:18781 and stops it on stop and uninstall.
  The ready screen shows AirPlay only when that helper is up and its
  /probe self-test passes. GCAST_XCODE overrides the on-card helper.
  192.0.2.10 is a documentation-range example:
    GCAST_XCODE=http://192.0.2.10:18780
  See licenses/gcast-ffmpeg/NOTICE for the helper ffmpeg, ffprobe, x264,
  and OpenSSL.
EOF
fi
cp -a "$STAGE/README.txt" "$STAGE/docs/INSTALL.txt"

# Checksums for every shipped payload file. The manifest necessarily excludes
# itself, then verifies before tar creation so coverage cannot silently regress.
(
  cd "$STAGE"
  find . -type f ! -name SHA256SUMS -printf '%P\0' | sort -z | xargs -0 sha256sum
) >"$STAGE/SHA256SUMS"
(
  cd "$STAGE"
  sha256sum -c SHA256SUMS
)

# Every path under Contents must be a file in the stage, and every staged file
# must be listed. docs/INSTALL.txt is a copy of README.txt.
if [[ "$PAIRED" != "1" ]]; then
  python3 - "$STAGE" <<'PY'
import os, pathlib, re, sys
stage = pathlib.Path(sys.argv[1])
readme = (stage / "README.txt").read_text()
install = (stage / "docs" / "INSTALL.txt").read_text()
if readme != install:
    sys.exit("docs/INSTALL.txt is not a copy of README.txt")
if "scp -r bin scripts docs licenses " not in readme:
    sys.exit("README dropped the licenses scp line")
if "LGPL-2.1-or-later" not in readme or "statically linked libav" not in readme:
    sys.exit("README does not describe statically linked LGPL libav")
if "GNU General Public License version 3" not in readme:
    sys.exit("README missing the GPLv3 combined-work statement")
if "License\n-------\n" not in readme or readme.find("License") > readme.find("\nContents\n"):
    sys.exit("License section is not before Contents")
lines = readme.splitlines()
try:
    i = lines.index("Contents")
except ValueError:
    sys.exit("README has no Contents section")
if not lines[i + 1].startswith("---"):
    sys.exit("Contents underline missing")
listed = []
for line in lines[i + 2:]:
    if line == "":
        break
    if not line.startswith("  ") or " " in line[2:] or line[2:] != line[2:].strip():
        sys.exit("Contents line is not a single path: " + line)
    listed.append(line[2:])
actual = sorted(p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file())
if listed != actual:
    missing = [p for p in actual if p not in listed]
    extra = [p for p in listed if p not in actual]
    sys.exit("Contents does not match the staged files\n  missing: %s\n  extra: %s" % (missing, extra))
if "bin/ffmpeg" in listed or (stage / "bin" / "ffmpeg").exists():
    sys.exit("standalone ffmpeg program is in the package")
required = [
    "licenses/ffmpeg/COPYING.LGPLv2.1",
    "licenses/ffmpeg/PROVENANCE.md",
    "licenses/openssl/LICENSE.txt",
    "licenses/openssl/PROVENANCE.md",
    "licenses/libplist/COPYING.LESSER",
    "licenses/libplist/PROVENANCE.md",
    "licenses/uxplay/LICENSE",
    "licenses/uxplay/COPYING.LGPLv2.1",
    "licenses/uxplay/PROVENANCE.md",
    "licenses/playfair/LICENSE.md",
    "licenses/llhttp/LICENSE-MIT",
    "licenses/mdnsd/COPYING.LGPLv2.1",
    "licenses/csrp/LICENSE",
    "licenses/owenb-h264ring/LICENSE",
    "licenses/gcc/copyright",
    "licenses/glibc/COPYING.LGPLv2.1",
    "licenses/README.md",
    "licenses/rbf/LICENSE",
    "licenses/rbf/GPL-2",
    "licenses/rbf/PROVENANCE.md",
    "licenses/stb/NOTICE",
    "licenses/stb/PROVENANCE.md",
    "licenses/qoi/NOTICE",
    "licenses/qoi/PROVENANCE.md",
]
for req in required:
    if req not in listed:
        sys.exit("Contents missing " + req)
prov = (stage / "licenses/ffmpeg/PROVENANCE.md").read_text()
match = re.search(r"```\n(.*)\n```", prov, re.S)
if not match:
    sys.exit("ffmpeg provenance has no configure line")
cfg = match.group(1)
if "--enable-openssl" not in cfg.split():
    sys.exit("configure line has no --enable-openssl")
if re.search(r"(^|\s)--enable-gpl(\s|$)", cfg):
    sys.exit("configure line passes --enable-gpl")
if "8.1.2" not in prov or "https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz" not in prov:
    sys.exit("ffmpeg provenance is missing the version or source URL")
ssl = (stage / "licenses/openssl/PROVENANCE.md").read_text()
if "3.0.15" not in ssl or "Apache-2.0" not in ssl:
    sys.exit("openssl provenance is missing the version or Apache-2.0")
index = (stage / "licenses/README.md").read_text()
if "GNU General Public License version 3" not in index:
    sys.exit("licenses/README.md missing the GPLv3 combined-work statement")
published_gen = "source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later)."
published_op = "source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later)."
if published_gen not in readme or published_gen not in index:
    sys.exit("published-source line missing from README.txt or licenses/README.md")
if not readme.startswith("MiSTerPlex release package\nversion: rc5\n"):
    sys.exit("README.txt version line is not rc5")
if "Boot (rc5)" not in readme or "Boot (rc4)" in readme or "Boot (rc3)" in readme:
    sys.exit("README.txt boot heading is not Boot (rc5)")
if "misterplexd-boot-rc3.sh" not in readme:
    sys.exit("README.txt dropped the boot hook filename")
if "/workspace" in prov or "/tmp" in prov:
    sys.exit("ffmpeg provenance still contains a build-host path")
op = stage / "README.md"
notes = stage / "RELEASE-NOTES.md"
if not op.is_file() or not notes.is_file():
    sys.exit("tarball root is missing README.md or RELEASE-NOTES.md")
op_text = op.read_text()
if "GNU General Public License version 3" not in op_text or published_op not in op_text:
    sys.exit("README.md is missing the GPLv3 License section")
if "test -f /media/fat/linux/misterplex.conf || cp /tmp/misterplex.conf.example /media/fat/linux/misterplex.conf" not in op_text:
    sys.exit("README.md does not install the conf where the boot hook reads it, guarded by test -f")
if "scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf\n" in op_text:
    sys.exit("README.md copies the conf example over an existing settings file")
if "scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/misterplex/misterplex.conf" in op_text:
    sys.exit("README.md still copies the conf to /media/fat/misterplex/")
if "scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf" not in readme:
    sys.exit("README.txt does not install the conf where the boot hook reads it")
if "\n  scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf\n" in readme:
    sys.exit("README.txt copies misterplex.conf.example unconditionally")
if "test -f /media/fat/linux/misterplex.conf" not in readme:
    sys.exit("README.txt does not guard the conf copy")
if "/media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop" not in readme:
    sys.exit("README.txt does not stop the hook before copying")
if "scripts/install_paired_all.sh" in {p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file()}:
    sys.exit("rc3 tarball still contains install_paired_all.sh")
if "Corresponding source" in readme or "Corresponding source" in index:
    sys.exit("README still has a corresponding-source offer")
print("readme contents: %d paths, all present" % len(listed))
PY
fi

mkdir -p "$OUT_DIR"
# Extract as misterplex-<version>/ rather than leaking the staging directory name.
tar -C "$STAGE/.." --transform="s|^$(basename "$STAGE")|misterplex-${VERSION}|" \
  -czf "$TAR" "$(basename "$STAGE")"
ls -la "$TAR"
echo "Packaged → $TAR"
if [[ "$PAIRED" == "1" ]]; then
  echo "RBFs: Plex_480p/240p15/480i/720p24 (named; no generic Plex.rbf)"
else
  echo "RBF: present ($(wc -c <"$STAGE/cores/Plex.rbf") bytes, md5=$(md5sum "$STAGE/cores/Plex.rbf" | awk '{print $1}'))"
fi
file "$STAGE/bin/misterplexd" || true
# Fail soft-list of expected docs
for need in docs/release.md docs/INSTALL.txt conf/misterplex.conf.example bin/misterplexd linux/misterplex-cacert.pem; do
  [[ -e "$STAGE/$need" ]] || { echo "ERROR: missing $need in stage"; exit 1; }
done
if [[ "$PAIRED" == "1" ]]; then
  for need in cores/Plex_480p.rbf cores/Plex_240p15.rbf cores/Plex_480i.rbf cores/Plex_720p24.rbf \
              bin/misterplexd.480p bin/misterplexd.720p24 rbf_daemon_pairs.txt; do
    [[ -e "$STAGE/$need" ]] || { echo "ERROR: missing $need in stage"; exit 1; }
  done
  if [[ -e "$STAGE/cores/Plex.rbf" ]]; then
    echo "ERROR: generic cores/Plex.rbf must not ship in paired $VERSION" >&2
    exit 1
  fi
fi
for need in cores/PlexOB.rbf PlexOB.rbf _Utility/PlexOB.rbf; do
  [[ -f "$STAGE/$need" ]] || { echo "ERROR: missing $need"; exit 1; }
  got="$(md5sum "$STAGE/$need" | awk '{print $1}')"
  [[ "$got" == "$PLEXOB_RBF_MD5" ]] || {
    echo "ERROR: $need md5 is not the seed-10 pin" >&2
    exit 1
  }
done
[[ -f "$STAGE/cores/PlexOB.rbf.md5" ]] || { echo "ERROR: missing cores/PlexOB.rbf.md5"; exit 1; }
slot_md5="$(tr -d '[:space:]' < "$STAGE/cores/PlexOB.rbf.md5")"
[[ "$slot_md5" == "$PLEXOB_RBF_MD5" ]] || {
  echo "ERROR: cores/PlexOB.rbf.md5 is $slot_md5, expected $PLEXOB_RBF_MD5" >&2
  exit 1
}
grep -q "PlexOB.rbf md5: ${PLEXOB_RBF_MD5}" "$STAGE/README.txt" || {
  echo "ERROR: README missing the PlexOB md5 slot" >&2
  exit 1
}
grep -q '4D505842/0B59C11A' "$STAGE/README.txt" || {
  echo "ERROR: README missing word 88 4D505842/0B59C11A" >&2
  exit 1
}
grep -q 'OB input region DEAD' "$STAGE/README.txt" || {
  echo "ERROR: README missing the gate-open DEAD note" >&2
  exit 1
}
scan_one_tree() {
  local label="$1" root="$2" log rc
  log="$(mktemp)"
  set +e
  SCAN_TREE="$root" bash "$ROOT/tests/unit/test_no_private_data.sh" >"$log" 2>&1
  rc=$?
  set -e
  if [[ "$rc" -ne 0 ]]; then
    cat "$log" >&2
    rm -f "$log"
    echo "ERROR: $label failed the private-data scan" >&2
    exit 1
  fi
  rm -f "$log"
  echo "$label: test_no_private_data OK"
}
scan_dir="$(mktemp -d)"
tar -xzf "$TAR" -C "$scan_dir"
key_suffix=$(printf '%s KEY' PRIVATE)
if grep -R -I -l -e "BEGIN ${key_suffix}" -e "BEGIN OPENSSH ${key_suffix}" -e "BEGIN RSA ${key_suffix}" "$scan_dir" >/dev/null; then
  rm -rf "$scan_dir"
  echo "ERROR: package has a private key" >&2
  exit 1
fi
scan_one_tree "package ${TAR}" "$scan_dir"
rm -rf "$scan_dir"
echo "package_release: OK"
