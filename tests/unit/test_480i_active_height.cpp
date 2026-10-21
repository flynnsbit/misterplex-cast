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
std::string boundXml(misterplex::Companion& comp, const std::string& mode) {
    misterplex::PlayRequest req;
    req.key = "/library/metadata/3";
    req.ratingKey = "3";
    req.address = "192.0.2.41";
    req.protocol = "http";
    req.port = "32400";
    req.serverMachineId = "plex-server";
    comp.stagePlay(req);
    require(comp.bindMedia(req, 1286942), "bindMedia rejected " + mode);
    comp.setRasterMode(mode);
    return comp.timelineXml("h");
}
} // namespace

int main() {
    require(misterplex::Companion::k480iActiveHeight == 480, "k480iActiveHeight");
    require(misterplex::Companion::k480iPicHeight == 240, "k480iPicHeight");

    misterplex::Companion i;
    const std::string ixml = boundXml(i, "480i");
    require(has(ixml, "active_height=\"480\""), "480i missing frame height: " + ixml);
    require(has(ixml, "pic_height=\"240\""), "480i missing field height: " + ixml);

    misterplex::Companion p;
    const std::string pxml = boundXml(p, "480p");
    require(has(pxml, "active_height=\"480\""), "480p missing active_height: " + pxml);
    require(!has(pxml, "pic_height=\"240\""), "480p must omit 480i field crop: " + pxml);

    misterplex::Companion n;
    const std::string nxml = boundXml(n, "240p");
    require(has(nxml, "active_height=\"240\""), "240p missing active_height: " + nxml);
    require(!has(nxml, "pic_height=\"240\""), "240p must omit 480i field crop: " + nxml);

    i.clearMedia();
    const std::string after = i.timelineXml("h");
    require(!has(after, "active_height=\""), "clearMedia must drop 480i heights: " + after);
    require(!has(after, "pic_height=\""), "clearMedia must drop 480i field crop: " + after);

    std::cout << "test_480i_active_height: OK\n";
    return 0;
}
