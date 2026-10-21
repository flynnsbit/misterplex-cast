#include "libmisterplex/pts_wrap.hpp"
#include <cstdio>

int main() {
    misterplex::PtsWrapGuard g;
    const int64_t wrap = 8589934592LL; // 2^33
    if (g.apply(wrap - 2) != wrap - 2) {
        std::fprintf(stderr, "FAIL init\n");
        return 1;
    }
    const int64_t after = g.apply(10);
    if (after != 10 + wrap) {
        std::fprintf(stderr, "FAIL wrap sample got %lld\n", static_cast<long long>(after));
        return 1;
    }
    const int64_t next = g.apply(20);
    if (next != 20 + wrap) {
        std::fprintf(stderr, "FAIL post-wrap must keep correction got %lld\n",
                     static_cast<long long>(next));
        return 1;
    }
    g.reset();
    if (g.apply(5) != 5) {
        std::fprintf(stderr, "FAIL reset\n");
        return 1;
    }
    std::printf("test_pts_wrap: OK\n");
    return 0;
}
