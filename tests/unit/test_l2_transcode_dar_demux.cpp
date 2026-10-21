// review 19:24 L2 at the FPGA demux: av_inproc_decode.cpp built for the host with the system
// FFmpeg, on H264's ladder clips (review/crop/ladder/today) and pmsA
// (tests/unit/test_l2_transcode_dar_demux.sh).
//
//   test_l2_transcode_dar_demux CLIP TRANSCODED [PROVIDED_X PROVIDED_Y]
//
// Opens CLIP as fpgaThreadMain does on a 480p core (compressed video, 640x480 ceiling, inter and
// deblocking allowed), reads the first access unit, and prints:
//   L2 demux=X:Y|- coded=WxH visible=WxH sent=X:Y|-/<source> r3=X:Y|-/<source>
// sent is firstAuSourceAspect, then the coded W:H fallback, then the wire guard. r3 is r3's first
// AU (provided, else demux, else coded). PROVIDED is the Plex metadata DAR (absent: none).
#include "libmisterplex/av_inproc_decode.hpp"
#include "libmisterplex/source_aspect.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace misterplex;

static std::string str(const SourceAspect& a) {
    return a.valid ? std::to_string(a.x) + ":" + std::to_string(a.y) : std::string("-");
}

static std::string shown(SourceAspect a, const char* source, int codedW, int codedH) {
    if (!a.valid && codedW > 0 && codedH > 0) {
        a = {static_cast<uint16_t>(codedW), static_cast<uint16_t>(codedH), true};
        source = "coded";
    }
    return str(sourceAspectForSend(a)) + "/" + source;
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 5) {
        std::fprintf(stderr,
                     "usage: test_l2_transcode_dar_demux CLIP TRANSCODED [PROVIDED_X PROVIDED_Y]\n");
        return 2;
    }
    const bool transcoded = std::atoi(argv[2]) != 0;
    SourceAspect provided{};
    if (argc == 5)
        provided = {static_cast<uint16_t>(std::atoi(argv[3])),
                    static_cast<uint16_t>(std::atoi(argv[4])), true};

    AvInprocOpenOpts opts;
    opts.compressedVideo = true;
    opts.decodeAudio = false;
    opts.expectW = 640;
    opts.expectH = 480;
    opts.allowInter = true;
    opts.allowDeblock = true;
    opts.maxAccessUnitBytes = 131072;
    opts.maxVclRbspBytes = 131072;

    AvInprocDecoder demux;
    std::string err;
    AvCompressedAccessUnit au;
    if (!demux.open(argv[1], opts, err) || demux.readAccessUnit(au, err) != 1) {
        std::printf("L2 result=error error=%s\n", err.c_str());
        return 1;
    }
    const auto& g = au.geometry;
    const SourceAspect d = demux.sourceAspect();
    const auto first = firstAuSourceAspect(transcoded, provided, d);
    std::printf("L2 demux=%s coded=%dx%d visible=%dx%d sent=%s r3=%s\n", str(d).c_str(),
                g.codedWidth, g.codedHeight, g.visibleWidth, g.visibleHeight,
                shown(first.aspect, first.source, g.codedWidth, g.codedHeight).c_str(),
                shown(provided.valid ? provided : d, provided.valid ? "provided" : "demux",
                      g.codedWidth, g.codedHeight)
                    .c_str());
    return 0;
}
