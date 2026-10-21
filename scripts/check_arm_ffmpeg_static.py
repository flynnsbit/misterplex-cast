#!/usr/bin/env python3
"""Reject libav archives that load host glibc modules or ship extra decoders."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys


ARCHIVES = ("libavformat.a", "libavcodec.a", "libavutil.a", "libswresample.a")
REQUIRED = {
    "ff_file_protocol", "ff_http_protocol", "ff_tcp_protocol",
    "ff_https_protocol", "ff_tls_protocol", "ff_udp_protocol",
    "ff_mpegts_demuxer", "ff_aac_decoder", "ff_h264_parser",
    "ff_aac_demuxer", "ff_ac3_demuxer", "ff_mp3_demuxer",   # GATE7-1018: settle the TS audio probe
    "ff_h264_mp4toannexb_bsf", "swr_init",
}
MODULE_LOADERS = {
    "iconv", "iconv_open", "iconv_close",
    "libiconv", "libiconv_open", "libiconv_close",
    "__gconv", "__gconv_open", "__gconv_find_shlib",
    "dlopen", "dlmopen", "__libc_dlopen_mode",
}
# FFmpeg 8.1.2 libavcodec/allcodecs.c subtitle decoders, including the
# external-library caption decoders. Any of these symbols means the archive
# was not built from the hashed configure line.
SUBTITLE_DECODERS = {
    "ff_ssa_decoder", "ff_ass_decoder", "ff_ccaption_decoder",
    "ff_dvbsub_decoder", "ff_dvdsub_decoder", "ff_jacosub_decoder",
    "ff_microdvd_decoder", "ff_movtext_decoder", "ff_mpl2_decoder",
    "ff_pgssub_decoder", "ff_pjs_decoder", "ff_realtext_decoder",
    "ff_sami_decoder", "ff_srt_decoder", "ff_stl_decoder",
    "ff_subrip_decoder", "ff_subviewer_decoder", "ff_subviewer1_decoder",
    "ff_text_decoder", "ff_vplayer_decoder", "ff_webvtt_decoder",
    "ff_xsub_decoder", "ff_libaribcaption_decoder", "ff_libaribb24_decoder",
    "ff_libzvbi_teletext_decoder",
}
EXPECTED_DECODERS = (
    "h264,aac,aac_latm,ac3,eac3,mp3,mp3float,flac,alac,opus,vorbis,"
    "pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le"
)
# Prefix, arch, cpu, and cross-prefix stay out of the hash: they differ per
# machine and the prefix path is named after this hash.
CONFIGURE_FLAGS = (
    "--enable-static --disable-shared --disable-programs --disable-doc --disable-debug "
    "--disable-autodetect --disable-avdevice --disable-avfilter --disable-swscale "
    "--disable-everything --disable-iconv "
    "--enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-network "
    "--enable-openssl "
    "--enable-protocol=file,http,tcp,pipe,https,tls,udp "
    "--enable-demuxer=mov,mpegts,h264,matroska,mpegps,flv,aac,ac3,eac3,mp3 "
    "--enable-parser=h264,aac,aac_latm,ac3,mpegaudio,flac,opus,vorbis "
    "--enable-decoder=" + EXPECTED_DECODERS + " "
    "--enable-bsf=h264_mp4toannexb,extract_extradata"
)
FFMPEG_VERSION = "8.1.2"
_DECODER_RE = re.compile(r"--enable-decoder=(?:'([^']*)'|\"([^\"]*)\"|(\S+))")
_PROTOCOL_RE = re.compile(r"--enable-protocol=(?:'([^']*)'|\"([^\"]*)\"|(\S+))")
_REQUIRED_PROTOCOLS = ("file", "http", "tcp", "pipe", "https", "tls", "udp")


class Refused(RuntimeError):
    pass


def config_hash():
    return hashlib.sha256(CONFIGURE_FLAGS.encode()).hexdigest()[:12]


def prefix_dirname():
    return "ffmpeg-arm-" + config_hash() + "-time64"


def embedded_decoder_list(blob):
    text = blob.decode("latin1", errors="ignore")
    match = _DECODER_RE.search(text)
    if not match:
        raise Refused("libavutil.a has no embedded --enable-decoder list")
    return next(group for group in match.groups() if group is not None), text


def check_protocol_list(text):
    """DASH googlevideo URLs are https. A protocol list of file,http,tcp,pipe
    makes avformat_open_input fail with 'Protocol not found'."""
    match = _PROTOCOL_RE.search(text)
    if not match:
        raise Refused("libavutil.a has no embedded --enable-protocol list")
    protocols = next(group for group in match.groups() if group is not None).split(",")
    missing = [name for name in _REQUIRED_PROTOCOLS if name not in protocols]
    if missing:
        raise Refused("ARM libav protocol list lacks " + ",".join(missing))
    if not re.search(r"(?:^|\s)--enable-openssl(?:\s|$)", text):
        raise Refused("ARM libav was not built with OpenSSL")


def check_archives(prefix, nm):
    archives = [Path(prefix) / "lib" / name for name in ARCHIVES]
    if not all(path.is_file() for path in archives):
        raise Refused("all four ARM libav archives are required")
    util = Path(prefix) / "lib" / "libavutil.a"
    decoders, text = embedded_decoder_list(util.read_bytes())
    if decoders != EXPECTED_DECODERS:
        raise Refused(
            "embedded --enable-decoder list does not match the hashed configure line")
    if "FFmpeg version " + FFMPEG_VERSION not in text and "ffmpeg version " + FFMPEG_VERSION not in text:
        raise Refused("libavutil.a is not FFmpeg " + FFMPEG_VERSION)
    check_protocol_list(text)
    result = subprocess.run([nm, "-g", *map(str, archives)], capture_output=True, text=True)
    if result.returncode:
        raise Refused("cannot inspect ARM libav archive symbols")
    defined = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 2 or len(fields[-2]) != 1:
            continue
        kind, symbol = fields[-2], fields[-1].split("@", 1)[0]
        if symbol in MODULE_LOADERS:
            raise Refused("libav references a runtime module loader; rebuild with --disable-iconv")
        if symbol in SUBTITLE_DECODERS:
            raise Refused("libav contains a subtitle decoder symbol")
        if kind not in ("U", "w", "v"):
            defined.add(symbol)
    if not REQUIRED <= defined:
        raise Refused("ARM libav lacks required HTTP/HTTPS/MPEGTS/H.264/AAC/PCM support")


def check_time64(prefix):
    """The daemon is 64-bit time_t. A 32-bit libav (av_timegm returns time_t) is a different ABI."""
    stamp = Path(prefix) / "TIME_T_BITS"
    if not stamp.is_file():
        raise Refused("FFmpeg prefix has no TIME_T_BITS stamp; rebuild with the 64-bit time_t toolchain")
    if stamp.read_text().strip() != "64":
        raise Refused("FFmpeg prefix TIME_T_BITS is not 64")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--print-config-hash", action="store_true")
    parser.add_argument("--print-configure-flags", action="store_true")
    parser.add_argument("--print-prefix-dirname", action="store_true")
    args = parser.parse_args()
    if args.print_config_hash:
        print(config_hash())
        return 0
    if args.print_configure_flags:
        print(CONFIGURE_FLAGS)
        return 0
    if args.print_prefix_dirname:
        print(prefix_dirname())
        return 0
    if args.prefix is None:
        parser.error("--prefix is required")
    try:
        check_archives(args.prefix, args.nm)
        check_time64(args.prefix)
    except (OSError, Refused) as error:
        print("ARM_FFMPEG_REFUSED " + str(error), file=sys.stderr)
        return 1
    print(
        "ARM_FFMPEG_STATIC_SAFE HTTP/HTTPS/MPEGTS/H264/AAC/PCM; "
        "FFmpeg " + FFMPEG_VERSION + "; decoders match " + config_hash() +
        "; time_t=64; no iconv/module-loader/subtitle-decoder symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
