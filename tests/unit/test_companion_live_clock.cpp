#include <cstdlib>
#include <iostream>
#include <string>

#define private public
#include "companion.hpp"
#undef private

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

misterplex::PlayRequest episodeRequest(int64_t offsetMs) {
    misterplex::PlayRequest req;
    req.key = "/library/metadata/3";
    req.ratingKey = "3";
    req.playQueueItemId = "3";
    req.address = "192.0.2.41";
    req.protocol = "http";
    req.port = "32400";
    req.serverMachineId = "plex-server";
    req.offsetMs = offsetMs;
    req.offsetPresent = true;
    return req;
}

} // namespace

int main() {
    // Test A: start-at-0 plant; first playing sample is 3000 ms past kScrubCatchupMs.
    // Live clock must be published (frozen 0:00 is the glass bug).
    {
        misterplex::Companion comp;
        comp.setMachineId("misterplex-dev");
        misterplex::PlayRequest req = episodeRequest(0);
        comp.stagePlay(req);
        require(comp.bindMedia(req, 1286942), "A: bindMedia rejected offset-0 play");
        comp.setState("playing", 3000, 1286942);
        const std::string xml = comp.timelineXml("live");
        require(has(xml, "time=\"3000\""), "A: live playing@3000 did not report time=3000: " + xml);
        require(!has(xml, "time=\"0\""), "A: live playing@3000 still published time=0: " + xml);
    }

    // Test B: planted seek at 42000; demux restart reports playing@0. Keep plant.
    {
        misterplex::Companion comp;
        comp.setMachineId("misterplex-dev");
        misterplex::PlayRequest req = episodeRequest(42000);
        comp.stagePlay(req);
        require(comp.bindMedia(req, 1286942), "B: bindMedia rejected planted seek");
        comp.setState("playing", 0, 1286942);
        const std::string xml = comp.timelineXml("plant");
        require(has(xml, "time=\"42000\""),
                "B: rewind-to-zero after seek plant did not hold 42000: " + xml);
    }

    // Test C: start-at-0; playing@500 is inside 2000 ms catchup and past 400 ms advance.
    {
        misterplex::Companion comp;
        comp.setMachineId("misterplex-dev");
        misterplex::PlayRequest req = episodeRequest(0);
        comp.stagePlay(req);
        require(comp.bindMedia(req, 1286942), "C: bindMedia rejected offset-0 play");
        comp.setState("playing", 500, 1286942);
        const std::string xml = comp.timelineXml("near");
        require(has(xml, "time=\"500\""), "C: playing@500 did not report time=500: " + xml);
    }

    std::cout << "test_companion_live_clock: OK\n";
    return 0;
}
