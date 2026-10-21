#include "libmisterplex/mraudio_status.hpp"
#include <cstdio>
using namespace misterplex;
static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++fails; } } while (0)
int main() {
    CHECK(audioPrefillBudgetBytes(0) == 0);
    CHECK(audioPrefillBudgetBytes(-1) == 0);
    CHECK(audioPrefillBudgetBytes(100) == kFeedTargetBytes);
    CHECK(audioPrefillBudgetBytes(kAudioPrefillDefaultMs) == kMrAudioBytesPerSec / 10);
    CHECK(audioPrefillBudgetBytes(200) == kMrAudioBytesPerSec / 5);
    CHECK(audioPrefillBudgetBytes(10000) == kMrAudioBytesPerSec / 5);
    CHECK(kAudioPrefillMaxMs == 200);
    CHECK(kAudioPrefillDefaultMs == 100);
    CHECK(kMrAudioBytesPerSec == 48000LL * 4LL);
    CHECK(kFeedTargetBytes == 19200);
    if (fails) return 1;
    std::printf("PASS audio-buf-prefill\n");
    return 0;
}
