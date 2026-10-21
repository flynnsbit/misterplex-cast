// GATE7-1018: an OB open of an MPEG-TS with AAC must not hold the demuxer's codec-probe backlog. With the ARM
// static-safe libav (no ADTS demuxer) avformat_find_stream_info buffered up to 2500 packets, each video packet
// pinning a 200 KiB PES buffer: ~350 MB of heap per GCAST yt.ts play (the rc3 G7 OOM). Fixed by
// scripts/build_arm_ffmpeg.sh (aac,ac3,eac3,mp3 demuxers) and AvSource's max_probe_packets bound.
// P1 heap after open (malloc in use + mmapped chunks) under 24 MB   P2 the source still yields video + audio
// P3 source rows: AvSource sets max_probe_packets; the ARM libav build enables the aac demuxer.
// Run on the host (any libav) and, as the regression proper, built with ARM_CXX + the ARM libav under qemu-arm.
#include "owenb_av_source.hpp"

#include <cstdio>
#include <fstream>
#include <malloc.h>
#include <sstream>
#include <string>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif
using namespace misterplex::owenb;
static int g_fail = 0, g_n = 0;
#define CHECK(c, m) do { ++g_n; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m); } } while (0)

static size_t heapKb() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    const struct mallinfo2 mi = mallinfo2();
    return (mi.uordblks + mi.hblkhd) / 1024;
#else
    const struct mallinfo mi = mallinfo();
    return (size_t(unsigned(mi.uordblks)) + size_t(unsigned(mi.hblkd))) / 1024;
#endif
}
static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    const std::string root = MPX_SOURCE_ROOT;
    if (argc > 1) {
        const size_t before = heapKb();
        OpenRequest q;
        q.url = argv[1];
        std::string err;
        auto s = openAvSource(q, err);
        CHECK(s != nullptr, "P2 open");
        if (s) {
            const size_t held = heapKb() - (heapKb() >= before ? before : 0);
            std::printf("heap after open: %zu KB (limit 24576)\n", held);
            CHECK(held < 24u * 1024u, "P1 the codec-probe backlog is bounded (was ~120 MB on this fixture)");
            int v = 0, a = 0;
            MediaPacket p;
            for (int i = 0; i < 400 && (v == 0 || a == 0); ++i) {
                if (s->read(p, err) != SourceRead::Packet)
                    break;
                (p.kind == MediaPacket::Kind::Video ? v : a)++;
            }
            CHECK(v > 0 && a > 0, "P2 video and audio packets still flow");
        }
    } else {
        std::printf("(no fixture argument: source rows only)\n");
    }
    const std::string src = slurp(root + "/arm/misterplexd/owenb_av_source.cpp");
    CHECK(src.find("\"max_probe_packets\"") != std::string::npos, "P3 AvSource bounds max_probe_packets");
    CHECK(src.find("\"probesize\"") != std::string::npos, "P3 AvSource bounds probesize");
    CHECK(src.find("\"analyzeduration\"") != std::string::npos, "P3 AvSource bounds analyzeduration");
    CHECK(src.find("av_dict_set(&o, \"fflags\"") == std::string::npos,
          "P3 nobuffer is not set; it drops the probed keyframe");
    CHECK(src.find("AVDISCARD_ALL") != std::string::npos, "P3 AvSource discards non-selected streams");
    CHECK(src.find("kReadyByteCap") != std::string::npos, "P3 AvSource caps the ready queue");
    // The demuxer list lives in check_arm_ffmpeg_static.py. build_arm_ffmpeg.sh
    // applies that line, so a hashed prefix cannot drift from the aac demuxer.
    const std::string ff = slurp(root + "/scripts/build_arm_ffmpeg.sh");
    const std::string flags = slurp(root + "/scripts/check_arm_ffmpeg_static.py");
    CHECK(ff.find("--print-configure-flags") != std::string::npos,
          "P3 build_arm_ffmpeg.sh applies the checked configure flags");
    const size_t d = flags.find("--enable-demuxer=");
    const std::string line = d == std::string::npos ? "" : flags.substr(d, flags.find('\n', d) - d);
    CHECK(line.find(",aac") != std::string::npos, "P3 the ARM libav build enables the aac (ADTS) demuxer");
    std::printf("test_owenb_probe_mem: %d/%d OK\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
