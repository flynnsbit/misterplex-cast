#include "libmisterplex/idle_pairing_error.hpp"
#include "libmisterplex/gate_error_render.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using misterplex::daemonIdForPairedRaster;
using misterplex::formatDaemonDownError;
using misterplex::formatPairingError;
using misterplex::IdleErrorPaintGuard;
using misterplex::idleErrorPaintScope;
using misterplex::kIdleDaemonDownText;
using misterplex::kSelectedCoreId240p;
using misterplex::kSelectedCoreId480i;
using misterplex::kSelectedCoreId480p;
using misterplex::gate::renderGateErrorRgb24;
using misterplex::gate::wrapErrorText;

static int fails = 0;
#define CHECK(c)                                                                                 \
    do {                                                                                         \
        if (!(c)) {                                                                              \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                     \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    CHECK(std::strcmp(formatDaemonDownError().c_str(), kIdleDaemonDownText) == 0);
    CHECK(formatPairingError(0x487A1BD1u, 0x4801F40Au) ==
          "incorrect pairing daemon=0x487A1BD1 core=0x4801F40A");
    CHECK(formatPairingError(0x48005D45u, 0x2400F40Bu) ==
          "incorrect pairing daemon=0x48005D45 core=0x2400F40B");
    CHECK(formatPairingError(kSelectedCoreId480p, 0) ==
          "incorrect pairing daemon=0x487A1BD1 core=0x00000000");
    CHECK(daemonIdForPairedRaster("480p") == kSelectedCoreId480p);
    CHECK(daemonIdForPairedRaster("") == kSelectedCoreId480p);
    CHECK(daemonIdForPairedRaster("480i") == kSelectedCoreId480i);
    CHECK(daemonIdForPairedRaster("240p") == kSelectedCoreId240p);

    CHECK(!idleErrorPaintScope());
    {
        IdleErrorPaintGuard g(true);
        CHECK(idleErrorPaintScope());
        IdleErrorPaintGuard nested(true);
        CHECK(idleErrorPaintScope());
    }
    CHECK(!idleErrorPaintScope());
    {
        IdleErrorPaintGuard off(false);
        CHECK(!idleErrorPaintScope());
    }

    const std::string text = formatPairingError(0x487A1BD1u, 0x24003997u);
    const auto lines = wrapErrorText(text, 640, 480);
    CHECK(!lines.empty());
    std::string joined;
    for (const auto& l : lines)
        joined += (joined.empty() ? "" : " ") + l;
    CHECK(joined.find("incorrect pairing") != std::string::npos);
    CHECK(joined.find("0x487A1BD1") != std::string::npos);
    CHECK(joined.find("0x24003997") != std::string::npos);

    std::vector<uint8_t> rgb(static_cast<size_t>(640) * 480 * 3, 0x7F);
    CHECK(renderGateErrorRgb24(rgb.data(), 640, 480, 0, 640, text));
    bool anyWhite = false;
    for (size_t i = 0; i < rgb.size(); i += 3) {
        if (rgb[i] == 255 && rgb[i + 1] == 255 && rgb[i + 2] == 255)
            anyWhite = true;
    }
    CHECK(anyWhite);

    if (fails) {
        std::fprintf(stderr, "test_idle_pairing_error: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_idle_pairing_error: OK (both-id string, raster ids, scope, 640 wrap)\n");
    return 0;
}
