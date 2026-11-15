# License

bin/misterplexd is a combined work distributed under the GNU General Public License version 3 (licenses/uxplay/LICENSE). It statically links UxPlay lib/crypto.c (from RPiPlay, GPL-3.0-or-later) and lib/playfair (GPL-3.0). The other linked libraries keep their own licences, all compatible with GPLv3: FFmpeg 8.1.2 libav, libplist, glibc and mdnsd (LGPL-2.1-or-later), OpenSSL 3.0.15 (Apache-2.0), csrp, llhttp and owenb-h264ring (MIT), and the GCC runtime (GPL-3.0 with the runtime exception).

FFmpeg is statically linked libav (LGPL-2.1-or-later): libavformat, libavcodec, libavutil, and libswresample from FFmpeg 8.1.2. licenses/ffmpeg/ has the LGPL-2.1 text, the source URL, and the configure line (--enable-openssl, no --enable-gpl).

MiSTerPlex.rbf is built from a fork of MiSTer_h264_decoder (eac1064) under GPL-3.0-or-later (upstream's licence). It is the only FPGA core in this package. See rbf/.

stb (public domain) and qoi (MIT) notices are under stb/ and qoi/.

The corresponding source for the PlexOB bitstream is the PlexOB source tarball shipped inside this release package (source/); NOTICE gives its file name and sha256.

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
| rbf/ | MiSTerPlex.rbf | PlexOB RTL 2b6570a89d1d6f28560287fb978e18fe5c09b4ef | GPL-3.0-or-later (upstream h264/ per its NOTICE; sys/ files GPL-2.0-or-later or GPL-3.0-or-later) |
| stb/ | stb_image, stb_image_resize2, stb_truetype | 2.30 / 2.18 / 1.26 | public domain |
| qoi/ | qoi.h | 2021 | MIT |
| spleen/ | Spleen 8x16 | 2.2.0 | BSD-2-Clause |
| mozilla-ca/ | Mozilla CA bundle (misterplex-cacert.pem) | 2026-09-25 extract plus ISRG Root YE and Root YR | MPL-2.0 (certdata.txt) |
| ytdlp/ | yt-dlp onedir, embedded CPython, PyInstaller | 2026.08.19 | Unlicense; PSF-2.0; GPL-2.0-or-later bootloader |
| gcast-ffmpeg/ | on-card helper ffmpeg and ffprobe (libx264, OpenSSL) | 8.1.2 | GPL-2.0-or-later (x264); GPL-3.0-or-later (FFmpeg --enable-gpl --enable-version3); Apache-2.0 (OpenSSL 3.0.15) |
