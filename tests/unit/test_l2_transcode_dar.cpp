// review 19:24 L1/L2: the first-AU PLXA, firstAuSourceAspect (source_aspect.hpp).
//  1. Direct plays: r3's pick bit for bit over the 896-combination sweep. r3 is the
//     media_player.cpp first AU at 78da5c85: provided, else demux, else the coded W:H as is.
//  2. Transcodes: the stream DAR when there is one. Otherwise r3: the visible fallback is not
//     here, it stays behind MPX_TRUE_DAR && fbTrueDar.
//  3. DAR-TRUNC: every value sent (after sourceAspectForSend) has both terms <= 4095.
//  4. Ladder rows (H264 ladder-plxa-table §2/§6): a 624 rung sends 13:10 and a 640 or 320 rung
//     sends 4:3, whatever the metadata. pmsA (no VUI) as a transcode with 16:9 metadata sends
//     16:9, r3's provided value here.
// Built with -DMUT_L2_METADATA_WINS (the metadata wins on a transcode) this test must FAIL.
#include "libmisterplex/source_aspect.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace misterplex;

namespace {

int fails = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

SourceAspect A(int x, int y) { return {static_cast<uint16_t>(x), static_cast<uint16_t>(y), true}; }

std::string str(const SourceAspect& a) {
    return a.valid ? std::to_string(a.x) + ":" + std::to_string(a.y) : std::string("-");
}

bool same(const SourceAspect& a, const SourceAspect& b) {
    return a.valid == b.valid && a.x == b.x && a.y == b.y;
}

// The media_player first-AU flow after the pick: the coded W:H fallback as is, then the wire
// guard (DAR-TRUNC).
SourceAspect sentAfter(SourceAspect aspect, int codedW, int codedH) {
    if (!aspect.valid && codedW > 0 && codedH > 0) {
        aspect.x = codedW;
        aspect.y = codedH;
        aspect.valid = true;
    }
    return aspect;
}

}  // namespace

int main() {
    const SourceAspect none{};

    // 1-3. The 896-combination sweep (8 metadata x 8 demux x 7 sizes x direct/transcode).
    const std::vector<SourceAspect> aspects = {none,        A(4, 3),   A(16, 9),  A(13, 10),
                                               A(239, 100), A(160, 67), A(4095, 1), A(1, 4095)};
    const int sizes[][2] = {{624, 480}, {640, 480}, {640, 368}, {1920, 1088},
                            {320, 240}, {0, 0},     {640, 272}};
    int n = 0, directSame = 0, transcodeChanged = 0;
    for (const auto& provided : aspects)
        for (const auto& demux : aspects)
            for (const auto& s : sizes)
                for (int t = 0; t < 2; ++t) {
                    const bool tr = t != 0;
                    const auto first = firstAuSourceAspect(tr, provided, demux);
                    const SourceAspect r3 = provided.valid ? provided : demux;
                    const char* r3Source = provided.valid ? "provided" : "demux";
                    const std::string at = "provided=" + str(provided) + " demux=" + str(demux) +
                                           " coded=" + std::to_string(s[0]) + "x" +
                                           std::to_string(s[1]) + " transcoded=" +
                                           std::to_string(t);
                    if (!tr || !demux.valid) {
                        expect(same(first.aspect, r3) && std::string(first.source) == r3Source &&
                                   !first.metadataOverridden,
                               "not r3 at " + at);
                        directSame += tr ? 0 : 1;
                    } else {
                        expect(same(first.aspect, demux) && std::string(first.source) == "demux",
                               "transcode with a stream DAR must send it at " + at);
                        const bool differs = provided.valid &&
                                             uint32_t(provided.x) * demux.y !=
                                                 uint32_t(demux.x) * provided.y;
                        expect(first.metadataOverridden == differs,
                               "metadataOverridden flag at " + at);
                        transcodeChanged += same(first.aspect, r3) ? 0 : 1;
                    }
                    const SourceAspect wire = sourceAspectForSend(sentAfter(first.aspect, s[0], s[1]));
                    expect(!wire.valid || (wire.x <= kSourceAspectMax && wire.y <= kSourceAspectMax),
                           "DAR-TRUNC bound at " + at + ": sent " + str(wire));
                    ++n;
                }
    // DAR-TRUNC canonical vectors through the same flow (a metadata term above 4095 is folded).
    expect(same(sourceAspectForSend(firstAuSourceAspect(false, A(65535, 47999), none).aspect),
                A(2403, 1760)),
           "direct 65535:47999 metadata must fold to 2403:1760 (DAR-TRUNC)");
    expect(same(sourceAspectForSend(firstAuSourceAspect(true, A(65535, 47999), A(13, 10)).aspect),
                A(13, 10)),
           "transcode: the stream 13:10 wins over a 65535:47999 metadata");

    // 4. Ladder rows (stream DAR from VUI idc 1 on the rung; metadata = the movie's).
    struct Row {
        const char* name;
        SourceAspect provided, demux, want;
    };
    const Row rows[] = {
        {"L1 624 rung, 16:9 title", A(16, 9), A(13, 10), A(13, 10)},
        {"L2 640 rung, 16:9 title", A(16, 9), A(4, 3), A(4, 3)},
        {"L3 624 rung, 2.35 title (47:20)", A(47, 20), A(13, 10), A(13, 10)},
        {"L4 640 rung, 2.35 title (47:20)", A(47, 20), A(4, 3), A(4, 3)},
        {"L5 624 rung, 2.39 title (239:100)", A(239, 100), A(13, 10), A(13, 10)},
        {"L6 624 rung, 4:3 title", A(4, 3), A(13, 10), A(13, 10)},
        {"L7 640 rung, 4:3 title", A(4, 3), A(4, 3), A(4, 3)},
        {"L8 624 rung, 16:9 anamorphic title", A(16, 9), A(13, 10), A(13, 10)},
        {"L8 640 rung, 16:9 anamorphic title", A(16, 9), A(4, 3), A(4, 3)},
        {"L9 624 rung, 4:3 anamorphic title", A(4, 3), A(13, 10), A(13, 10)},
        {"L9 640 rung, 4:3 anamorphic title", A(4, 3), A(4, 3), A(4, 3)},
        {"L10 320 crf240 rung, 16:9 title", A(16, 9), A(4, 3), A(4, 3)},
        {"L10 320 crf240 rung, 2.35 title", A(47, 20), A(4, 3), A(4, 3)},
        {"L11 ladder, metadata without DAR", none, A(13, 10), A(13, 10)},
        // pmsA640x360_first60 (712f28d3): PMS universal, no VUI SAR. No stream DAR, so r3's
        // order: the 16:9 metadata (equal to the visible 16:9 the flagged branch would send).
        {"pmsA transcode, no stream DAR, 16:9 metadata", A(16, 9), none, A(16, 9)},
        // cap-tng (e3de352a) VUI idc 1 on 640x480 (the 480p demux refuses its High SPS today).
        {"cap-tng PMS universal 640x480 VUI 1:1, 16:9 metadata", A(16, 9), A(4, 3), A(4, 3)},
    };
    for (const auto& r : rows) {
        const auto first = firstAuSourceAspect(true, r.provided, r.demux);
        expect(same(first.aspect, r.want),
               std::string(r.name) + ": got " + str(first.aspect) + " want " + str(r.want));
        std::printf("L2_ROW %-52s -> %-6s (r3 %s)\n", r.name, str(first.aspect).c_str(),
                    str(r.provided.valid ? r.provided : r.demux).c_str());
    }
    // review 19:37 N2, pmsA (712f28d3, no VUI) as a transcode with 16:9 metadata. Here (no
    // MPX_TRUE_DAR) it is r3's pick, dar_source=provided 16:9, with no override. The flagged
    // true-DAR branch (dev/true-dar, MPX_TRUE_DAR && fbTrueDar) sends the visible 16:9
    // instead, label "visible" (its test_true_dar N2(a) rows). Same value, different source.
    {
        const auto pmsa = firstAuSourceAspect(true, A(16, 9), none);
        expect(same(pmsa.aspect, A(16, 9)) && std::string(pmsa.source) == "provided" &&
                   !pmsa.metadataOverridden,
               "N2 pmsA flag-off must be r3's provided 16:9, got " + str(pmsa.aspect) + "/" +
                   pmsa.source);
    }

    if (fails) {
        std::printf("FAIL L2 transcode DAR (%d failures)\n", fails);
        return 1;
    }
    std::printf("PASS L2 transcode DAR (%d combinations; direct == r3 on %d; %d transcode values "
                "change; DAR-TRUNC bound held; %zu ladder rows)\n",
                n, directSame, transcodeChanged, sizeof rows / sizeof rows[0]);
    return 0;
}
