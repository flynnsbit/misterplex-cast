// Arm GNU 13.2 with -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64. A sparse file
// above 2 GiB must stat as a 64-bit off_t. Ubuntu's armhf g++ already
// defaults to 64, so this binary is built with the ship toolchain.
#include <sys/stat.h>
#include <stdio.h>
#include <time.h>

static_assert(sizeof(off_t) == 8, "off_t");
static_assert(sizeof(time_t) == 8, "time_t");

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_time64_stat FILE\n");
        return 2;
    }
    struct stat st;
    if (stat(argv[1], &st) != 0) {
        perror("stat");
        return 3;
    }
    const long long min = 3LL * 1024 * 1024 * 1024;
    if ((long long)st.st_size < min) {
        fprintf(stderr, "size %lld < %lld\n", (long long)st.st_size, min);
        return 4;
    }
    printf("test_time64_stat: ok size=%lld\n", (long long)st.st_size);
    return 0;
}
