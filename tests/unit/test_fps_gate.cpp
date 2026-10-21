#include "libmisterplex/fps_gate.hpp"

#include <cstdio>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    using namespace misterplex;

    CHECK(sizeof(kFpsGateModes) / sizeof(kFpsGateModes[0]) == 3);
    CHECK(kFpsGateModes[0].mode == FpsGateMode::Mode240p);
    CHECK(kFpsGateModes[1].mode == FpsGateMode::Mode480p);
    CHECK(kFpsGateModes[2].mode == FpsGateMode::Mode480i);

    CHECK(is240p(320, 240));
    CHECK(!is240p(640, 480));
    CHECK(is480p(640, 480, nullptr, nullptr));
    CHECK(!is480i(640, 480, nullptr, nullptr));
    CHECK(is480i(640, 480, "480i", nullptr));
    CHECK(is480i(320, 240, nullptr, "1"));
    CHECK(!is480p(640, 480, "480i", nullptr));
    CHECK(fpsGateMode(1280, 720, nullptr, nullptr) == FpsGateMode::None);

    int n = 24000, d = 1001;
    CHECK(!snapFpsToStandard(n, d, true));
    CHECK(n == 24000 && d == 1001);

    n = 24; d = 1;
    CHECK(!snapFpsToStandard(n, d, true));
    CHECK(n == 24 && d == 1);

    n = 30000; d = 1001;
    CHECK(!snapFpsToStandard(n, d, true));
    CHECK(n == 30000 && d == 1001);

    n = 60000; d = 1001;
    CHECK(snapFpsToStandard(n, d, true));
    CHECK(n == 30000 && d == 1001);

    n = 60; d = 1;
    CHECK(snapFpsToStandard(n, d, true));
    CHECK(n == 30 && d == 1);

    n = 23976; d = 1000;
    CHECK(snapFpsToStandard(n, d, true));
    CHECK(n == 24000 && d == 1001);

    n = 25; d = 1;
    CHECK(snapFpsToStandard(n, d, true));
    CHECK(n == 24 && d == 1);

    n = 15; d = 1;
    CHECK(snapFpsToStandard(n, d, true));
    CHECK(n == 24 && d == 1);

    n = 24000; d = 1001;
    CHECK(!snapFpsToStandard(n, d, false));
    CHECK(n == 24000 && d == 1001);

    if (fails) {
        std::fprintf(stderr, "%d FAIL\n", fails);
        return 1;
    }
    std::printf("test_fps_gate: OK\n");
    return 0;
}
