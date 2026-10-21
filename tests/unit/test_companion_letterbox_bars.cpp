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

misterplex::PlayRequest episodeRequest() {
    misterplex::PlayRequest req;
    req.key = "/library/metadata/3";
    req.ratingKey = "3";
    req.playQueueItemId = "3";
    req.address = "192.0.2.41";
    req.protocol = "http";
    req.port = "32400";
    req.serverMachineId = "plex-server";
    req.offsetMs = 0;
    req.offsetPresent = true;
    return req;
}

} // namespace

int main() {
    misterplex::Companion comp;
    misterplex::PlayRequest req = episodeRequest();
    comp.stagePlay(req);
    require(comp.bindMedia(req, 1286942), "bindMedia rejected staged play");

    // A: default-constructed bars stay omitted (full-frame).
    const std::string z = comp.timelineXml("z");
    require(!has(z, "bar_top"), "A: default timelineXml contains bar_top: " + z);
    require(!has(z, "bar_bot"), "A: default timelineXml contains bar_bot: " + z);

    // B: letterbox + pillar sizes emit on the timeline OSD path.
    comp.setLetterboxBars(65, 65, 8, 8);
    const std::string lb = comp.timelineXml("lb");
    require(has(lb, "bar_top=\"65\""), "B: missing bar_top=\"65\": " + lb);
    require(has(lb, "bar_bot=\"65\""), "B: missing bar_bot=\"65\": " + lb);
    require(has(lb, "bar_left=\"8\""), "B: missing bar_left=\"8\": " + lb);
    require(has(lb, "bar_right=\"8\""), "B: missing bar_right=\"8\": " + lb);

    // C: empty crop (0,0,0,0) omits bar attrs so full-frame stays full-frame.
    comp.setLetterboxBars(0, 0, 0, 0);
    const std::string f = comp.timelineXml("f");
    require(!has(f, "bar_top"), "C: empty crop still emits bar_top: " + f);

    // D: clearMedia zeros a prior title's bars.
    comp.setLetterboxBars(65, 65);
    comp.clearMedia();
    const std::string cl = comp.timelineXml("c");
    require(!has(cl, "bar_top"), "D: clearMedia left bar_top: " + cl);

    // E: negative top clamps; must never emit bar_top="-3".
    comp.setLetterboxBars(-3, 10);
    const std::string neg = comp.timelineXml("neg");
    require(!has(neg, "bar_top=\"-3\""), "E: negative bar_top leaked: " + neg);
    require(has(neg, "bar_bot=\"10\""), "E: missing bar_bot=\"10\": " + neg);

    std::cout << "test_companion_letterbox_bars: OK\n";
    return 0;
}
