# Third-party notices

This file covers the files vendored in this source tree that are not listed elsewhere. The licence texts for everything that ships in the release package (UxPlay, RPiPlay, playfair, csrp, llhttp, mdnsd, libplist, FFmpeg, x264, OpenSSL, yt-dlp, stb, QOI, the Mozilla CA bundle, glibc and GCC, and the FPGA cores) are in [packaging/licenses/](packaging/licenses/).

## Roboto

The Plex menus use the Roboto font (`host/libmisterplex/plexui/third_party/Roboto-Regular.ttf` and `Roboto-Bold.ttf`), Copyright Google LLC, under the Apache License 2.0. The licence text is `host/libmisterplex/plexui/third_party/LICENSE-Roboto.txt`.

## Big Buck Bunny test frame

`tests/fixtures/glass_baseline/b6_play_sample_mjpeg720.jpg` is a frame from *Big Buck Bunny*, (c) copyright 2008, Blender Foundation, www.bigbuckbunny.org, used under the Creative Commons Attribution 3.0 license (https://creativecommons.org/licenses/by/3.0/).

## Spleen 8x16

MiSTerPlex vendors the Spleen 8x16 bitmap font for the 480p R1 host idle framebuffer browse renderer. This notice must ship in the deploy bundle with the daemon binary.

- Upstream: <https://github.com/fcambus/spleen>
- Release tag: `2.2.0`
- Commit: `0493c34e22791824767c618fed42c434b477662c`
- File: `third_party/spleen-2.2.0/spleen-8x16.bdf`
- BDF sha256: `4a3d97ee61a8c86a7525d8c723cb8a14081f395cd2feb4227ba5e3baf0629bae`
- License sha256: `f33fe8679d5b2abecc4f1313ce6c6bfa58262964de5f7bca146596a7318047af`

```text
Copyright (c) 2018-2024, Frederic Cambus
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
