// 480p A/V present policy: shipped lead 40 ms; hold-only drop when
// every-decoded (480p/240p/L4) so Star Trek-class lag cannot 2:1-shred unique rate.
#include "libmisterplex/av_clock.hpp"
#if __has_include("libmisterplex/fabric_direct.hpp")
#include "libmisterplex/fabric_direct.hpp"
#endif

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

    // RED twin: lead default 0 is the HEAD lie and must fail this gate.
    CHECK(kDefaultPresentLeadMs != 0);
    // GREEN
    CHECK(kDefaultPresentLeadMs == 40);
    CHECK(avResyncDropMsForPresent(80, true) == 0);
    CHECK(avResyncDropMsForPresent(80, false) == 80);
    CHECK(avResyncDropMsForPresent(0, false) == 0);

    if (fails) {
        std::fprintf(stderr, "test_cast_av_480p_policy: %d failures\n", fails);
        return 1;
    }
    std::printf("test_cast_av_480p_policy: OK lead=%d\n", kDefaultPresentLeadMs);
    return 0;
}
