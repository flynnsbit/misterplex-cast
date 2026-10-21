#include "libmisterplex/play_queue_version.hpp"

#include <iostream>
#include <string>

static int fails = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::cerr << "FAIL " << __LINE__ << " " << #c << "\n";                                 \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

int main() {
    using misterplex::playQueueVersionAfterReplace;
    CHECK(playQueueVersionAfterReplace("", "", "10", "3") == "3");
    CHECK(playQueueVersionAfterReplace("", "", "10", "") == "1");
    CHECK(playQueueVersionAfterReplace("10", "5", "10", "5") == "5");
    CHECK(playQueueVersionAfterReplace("10", "5", "10", "") == "5");
    CHECK(playQueueVersionAfterReplace("10", "5", "11", "") == "6");
    CHECK(playQueueVersionAfterReplace("10", "5", "11", "5") == "6");
    CHECK(playQueueVersionAfterReplace("10", "5", "11", "9") == "9");
    CHECK(playQueueVersionAfterReplace("10", "nope", "11", "") == "1");
    if (fails) {
        std::cerr << "test_companion_queue_version: " << fails << " failed\n";
        return 1;
    }
    std::cout << "test_companion_queue_version: OK\n";
    return 0;
}
