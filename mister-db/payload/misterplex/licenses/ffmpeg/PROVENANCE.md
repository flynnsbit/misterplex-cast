# FFmpeg libav statically linked into bin/misterplexd

Version: 8.1.2
License: LGPL-2.1-or-later. The text is COPYING.LGPLv2.1 from the FFmpeg 8.1.2 source.
Source URL: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
Source identity: the FFmpeg 8.1.2 release tarball (there is no git commit). sha256 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
Libraries inside bin/misterplexd: libavformat, libavcodec, libavutil, libswresample.

The configure line passes --enable-openssl and does not pass --enable-gpl, so this libav stays LGPL-2.1-or-later.

Configure line embedded in bin/misterplexd:

```
--prefix=/...............src/mpx-repro-main-d111f42b/src/build/deps/ffmpeg-arm-f0985b0b57d3-time64 --arch=arm --cpu=cortex-a9 --target-os=linux --enable-cross-compile --cross-prefix=/...............src/mpx-repro-main-d111f42b/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf- --extra-cflags='-D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 -I/home/flynnsbit/src/mpx-repro-main-d111f42b/src/build/deps/airplay-arm/include -ffile-prefix-map=/...............src/mpx-repro-main-d111f42b/src=. -ffile-prefix-map=/...............src/mpx-repro-main-d111f42b/src/build/deps/ffmpeg-8.1.2=.' --extra-cxxflags='-D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 -I/home/flynnsbit/src/mpx-repro-main-d111f42b/src/build/deps/airplay-arm/include -ffile-prefix-map=/...............src/mpx-repro-main-d111f42b/src=. -ffile-prefix-map=/...............src/mpx-repro-main-d111f42b/src/build/deps/ffmpeg-8.1.2=.' --extra-ldflags=-L/home/flynnsbit/src/mpx-repro-main-d111f42b/src/build/deps/airplay-arm/lib --pkg-config-flags=--static --enable-static --disable-shared --disable-programs --disable-doc --disable-debug --disable-autodetect --disable-avdevice --disable-avfilter --disable-swscale --disable-everything --disable-iconv --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-network --enable-openssl --enable-protocol='file,http,tcp,pipe,https,tls,udp' --enable-demuxer='mov,mpegts,h264,matroska,mpegps,flv,aac,ac3,eac3,mp3' --enable-parser='h264,aac,aac_latm,ac3,mpegaudio,flac,opus,vorbis' --enable-decoder='h264,aac,aac_latm,ac3,eac3,mp3,mp3float,flac,alac,opus,vorbis,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,subrip,ass,ssa,movtext,webvtt' --enable-bsf='h264_mp4toannexb,extract_extradata'
```

Source offer: the corresponding source for the libav in bin/misterplexd is that release tarball, built with the configure line above for armhf with 64-bit off_t and time_t. OpenSSL is linked separately; see ../openssl/.
