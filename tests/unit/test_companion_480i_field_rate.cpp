#include <cstdio>
#include <cstdlib>
#include <string>

#include "libmisterplex/core_gate.hpp"

// timelineXml is private (companion.hpp:209). Same host-test access as
// test_companion_plant_seek.cpp. companion.hpp is not changed.
#define private public
#include "companion.hpp"
#undef private

namespace {

struct EnvRestore {
    const char* name;
    bool had;
    std::string value;

    explicit EnvRestore(const char* n) : name(n) {
        const char* v = std::getenv(n);
        had = v != nullptr;
        if (had)
            value = v;
    }

    ~EnvRestore() {
        if (had)
            setenv(name, value.c_str(), 1);
        else
            unsetenv(name);
    }

    EnvRestore(const EnvRestore&) = delete;
    EnvRestore& operator=(const EnvRestore&) = delete;
};

} // namespace

int main() {
    const EnvRestore savedMode("MPX_FPGA_MODE");
    const EnvRestore savedPresent("MPX_PRESENT_480I");

    misterplex::Companion c;

    unsetenv("MPX_FPGA_MODE");
    unsetenv("MPX_PRESENT_480I");
    {
        const std::string xml = c.timelineXml("idle");
        if (xml.find("fieldRate=") != std::string::npos) {
            std::fprintf(stderr, "FAIL: idle tagged fieldRate\n");
            return 1;
        }
    }

    setenv("MPX_FPGA_MODE", "480i", 1);
    unsetenv("MPX_PRESENT_480I");
    {
        const std::string xml = c.timelineXml("i");
        if (xml.find("fieldRate=\"59.94\"") == std::string::npos) {
            std::fprintf(stderr, "FAIL: 480i missing fieldRate=59.94\n");
            return 1;
        }
        if (xml.find("29.97i") != std::string::npos) {
            std::fprintf(stderr, "FAIL: 480i still 29.97i\n");
            return 1;
        }
    }

    setenv("MPX_FPGA_MODE", "480p", 1);
    unsetenv("MPX_PRESENT_480I");
    {
        const std::string xml = c.timelineXml("p");
        if (xml.find("fieldRate=") != std::string::npos) {
            std::fprintf(stderr, "FAIL: 480p tagged fieldRate\n");
            return 1;
        }
    }

    setenv("MPX_FPGA_MODE", "240p", 1);
    unsetenv("MPX_PRESENT_480I");
    {
        const std::string xml = c.timelineXml("t");
        if (xml.find("fieldRate=") != std::string::npos) {
            std::fprintf(stderr, "FAIL: 240p tagged fieldRate\n");
            return 1;
        }
    }

    unsetenv("MPX_FPGA_MODE");
    setenv("MPX_PRESENT_480I", "1", 1);
    {
        const std::string xml = c.timelineXml("pr");
        if (xml.find("fieldRate=\"59.94\"") == std::string::npos) {
            std::fprintf(stderr, "FAIL: PRESENT_480I=1 missing fieldRate\n");
            return 1;
        }
    }

    unsetenv("MPX_FPGA_MODE");
    setenv("MPX_PRESENT_480I", "0", 1);
    {
        const std::string xml = c.timelineXml("z");
        if (xml.find("fieldRate=") != std::string::npos) {
            std::fprintf(stderr, "FAIL: PRESENT_480I=0 tagged fieldRate\n");
            return 1;
        }
    }

    unsetenv("MPX_FPGA_MODE");
    unsetenv("MPX_PRESENT_480I");
    misterplex::CoreGate::process().resetForTest(false);
    misterplex::CoreGate::process().setPairedRaster("480i", "own-core CAPS 0x4801F40A");
    {
        const std::string xml = c.timelineXml("own");
        if (xml.find("fieldRate=\"59.94\"") == std::string::npos) {
            std::fprintf(stderr, "FAIL: paired 480i missing fieldRate=59.94\n");
            misterplex::CoreGate::process().resetForTest(false);
            return 1;
        }
    }
    misterplex::CoreGate::process().resetForTest(false);

    return 0;
}
