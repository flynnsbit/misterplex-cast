#include "companion.hpp"

#include <cstdio>
#include <string>

int main() {
    using namespace misterplex;

    const PlayRequest pr = parsePlayRequest(
        "GET /player/playback/playMedia?key=/library/metadata/1&machineIdentifier=abc HTTP/1.1\r\n\r\n");
    if (pr.directPolicy != TitleDirectPolicy::Proxy) {
        std::fprintf(stderr, "FAIL: machineIdentifier marked Direct\n");
        return 1;
    }

    TitleStreamFacts baseline;
    baseline.codec = "h264";
    baseline.profile = "baseline";
    baseline.level = 30;
    baseline.refFrames = 1;
    baseline.width = 640;
    baseline.height = 480;
    if (decideTitleDirectPolicy(baseline) != TitleDirectPolicy::Direct) {
        std::fprintf(stderr, "FAIL: Baseline title not Direct\n");
        return 1;
    }

    TitleStreamFacts constrained = baseline;
    constrained.profile = "constrained baseline";
    if (decideTitleDirectPolicy(constrained) != TitleDirectPolicy::Direct) {
        std::fprintf(stderr, "FAIL: constrained baseline not Direct\n");
        return 1;
    }

    TitleStreamFacts high;
    high.codec = "h264";
    high.profile = "high";
    high.level = 40;
    high.refFrames = 4;
    high.width = 1904;
    high.height = 800;
    if (decideTitleDirectPolicy(high) != TitleDirectPolicy::Proxy) {
        std::fprintf(stderr, "FAIL: High title not Proxy\n");
        return 1;
    }

    TitleStreamFacts hevc = baseline;
    hevc.codec = "hevc";
    if (decideTitleDirectPolicy(hevc) != TitleDirectPolicy::Proxy) {
        std::fprintf(stderr, "FAIL: hevc marked Direct\n");
        return 1;
    }

    TitleStreamFacts interlaced = baseline;
    interlaced.interlaced = true;
    if (decideTitleDirectPolicy(interlaced) != TitleDirectPolicy::Proxy) {
        std::fprintf(stderr, "FAIL: interlaced marked Direct\n");
        return 1;
    }

    TitleStreamFacts l41 = baseline;
    l41.level = 41;
    if (decideTitleDirectPolicy(l41) != TitleDirectPolicy::Proxy) {
        std::fprintf(stderr, "FAIL: L41 marked Direct\n");
        return 1;
    }

    if (!directStreamPolicy(66, true, 1, false, 30)) {
        std::fprintf(stderr, "FAIL: Baseline L3.0 CAVLC ref1 no-B not direct\n");
        return 1;
    }
    if (directStreamPolicy(100, true, 1, false, 30)) {
        std::fprintf(stderr, "FAIL: High profile_idc 100 marked direct\n");
        return 1;
    }
    if (directStreamPolicy(66, false, 1, false, 30)) {
        std::fprintf(stderr, "FAIL: CABAC marked direct\n");
        return 1;
    }
    if (directStreamPolicy(66, true, 3, false, 30)) {
        std::fprintf(stderr, "FAIL: ref3 marked direct\n");
        return 1;
    }
    if (directStreamPolicy(66, true, 1, true, 30)) {
        std::fprintf(stderr, "FAIL: B-frame marked direct\n");
        return 1;
    }
    if (directStreamPolicy(66, true, 1, false, 21) == false) {
        std::fprintf(stderr, "FAIL: L2.1 should still be direct (cap is 30)\n");
        return 1;
    }
    if (directStreamPolicy(66, true, 1, false, 41)) {
        std::fprintf(stderr, "FAIL: L4.1 marked direct\n");
        return 1;
    }

    return 0;
}
