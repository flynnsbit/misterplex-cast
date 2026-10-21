#include "libmisterplex/audio_lead_clamp.hpp"

#include <cstdint>
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
    using misterplex::audioLeadClamp240p;
    using misterplex::audioLeadClamp480p;
    using misterplex::kAudioLeadClamp240pDmaBytes;
    using misterplex::kAudioLeadClamp480pMs;
    using misterplex::kAudioLeadClampDefaultDmaBytes;

    CHECK(kAudioLeadClamp480pMs == 80);
    CHECK(audioLeadClamp480p(0, 0, 80) == 0);
    CHECK(audioLeadClamp480p(80000, 0, 80) == 0);
    CHECK(audioLeadClamp480p(81000, 0, 80) == 1);
    CHECK(audioLeadClamp480p(300000, 0, 80) == 220);
    CHECK(audioLeadClamp480p(300000, 0, 0) == 0);
    CHECK(audioLeadClamp480p(300000, 0, -1) == 0);

    CHECK(kAudioLeadClamp240pDmaBytes == 5760);
    CHECK(audioLeadClamp240p(0, 320, 240) == 5760);
    CHECK(audioLeadClamp240p(192000, 320, 240) == 5760);
    CHECK(audioLeadClamp240p(19200, 640, 480) == kAudioLeadClampDefaultDmaBytes);
    CHECK(audioLeadClamp240p(19200, 720, 480) == kAudioLeadClampDefaultDmaBytes);
    CHECK(audioLeadClamp240p(19200, 1280, 720) == kAudioLeadClampDefaultDmaBytes);
    CHECK(19200 > audioLeadClamp240p(19200, 320, 240));
    CHECK(!(19200 > audioLeadClamp240p(19200, 640, 480)));

    if (fails) {
        std::fprintf(stderr, "%d FAIL\n", fails);
        return 1;
    }
    std::printf("test_audio_lead_clamp: OK\n");
    return 0;
}
