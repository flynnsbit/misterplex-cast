#include "libmisterplex/mraudio_status.hpp"

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
    using misterplex::audioQueueBound240p;
    using misterplex::kAudioQueueBound240pBytes;
    CHECK(kAudioQueueBound240pBytes == 23040);
    CHECK(!audioQueueBound240p(320, 240, 23040));
    CHECK(audioQueueBound240p(320, 240, 23041));
    CHECK(!audioQueueBound240p(640, 480, 40000));
    CHECK(!audioQueueBound240p(720, 480, 40000));
    CHECK(!audioQueueBound240p(160, 120, 40000));
    CHECK(!audioQueueBound240p(320, 239, 40000));
    if (fails) {
        std::fprintf(stderr, "%d FAIL\n", fails);
        return 1;
    }
    std::printf("test_audio_queue_bound_240p: OK\n");
    return 0;
}
