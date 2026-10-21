#include "libmisterplex/bitrate_floor_480i.hpp"

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
    using misterplex::bitrate_floor_480i;
    CHECK(bitrate_floor_480i(nullptr, nullptr, nullptr, 1500) == 1500);
    CHECK(bitrate_floor_480i("480p", nullptr, nullptr, 1500) == 1500);
    CHECK(bitrate_floor_480i("240p", nullptr, nullptr, 400) == 400);
    CHECK(bitrate_floor_480i("720p", nullptr, nullptr, 1500) == 1500);
    CHECK(bitrate_floor_480i("480i", nullptr, nullptr, 1500) == 2000);
    CHECK(bitrate_floor_480i("480i", nullptr, nullptr, 350) == 2000);
    CHECK(bitrate_floor_480i("480i", nullptr, nullptr, 2500) == 2500);
    CHECK(bitrate_floor_480i("480i", nullptr, nullptr, 0) == 0);
    CHECK(bitrate_floor_480i(nullptr, "1", nullptr, 350) == 2000);
    CHECK(bitrate_floor_480i(nullptr, "0", nullptr, 350) == 350);
    CHECK(bitrate_floor_480i(nullptr, nullptr, "480i", 350) == 2000);
    if (fails) {
        std::fprintf(stderr, "%d FAIL\n", fails);
        return 1;
    }
    std::printf("test_bitrate_floor_480i: OK\n");
    return 0;
}
