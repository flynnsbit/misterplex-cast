// K2: the first ring open marks reinit once and does not open its own session.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const std::string play = slurp("host/libmisterplex/owenb_play.cpp");
    const std::string ring = slurp("host/libmisterplex/owenb_ring.cpp");
    const auto open = play.find("OwenbPlayer::openRingLocked");
    const auto call = play.find("reinitOutput", open == std::string::npos ? 0 : open);
    const auto fn = ring.find("OwenbRing::reinitOutput");
    const auto next = ring.find("OwenbRing::begin", fn == std::string::npos ? 0 : fn);
    if (open == std::string::npos || call == std::string::npos || fn == std::string::npos ||
        next == std::string::npos) {
        std::cerr << "FAIL openRingLocked must call reinitOutput\n";
        return 1;
    }
    const auto body = ring.substr(fn, next - fn);
    if (body.find("outputReinited_") == std::string::npos || body.find("h264ring_flush") != std::string::npos ||
        body.find("begin(") != std::string::npos) {
        std::cerr << "FAIL reinitOutput must set the once-flag and must not open a session\n";
        return 1;
    }
    std::cout << "test_ob_output_reinit: ok\n";
    return 0;
}
