// BUG1 (lab 7AE78D80): the per-PMS memory of what a server's transcoder emitted for the PlexOB core.
// The remote PMS: CAVLC at 5228 kbps, rewrite load 94 % -> next first request 5228 * 60 / 94 = 3337 kbps.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "libmisterplex/owenb_pms_traits.hpp"

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...)                                                   \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(c)) {                                                     \
            ++g_fails;                                                  \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
            std::printf(__VA_ARGS__);                                   \
            std::printf("\n");                                          \
        }                                                               \
    } while (0)

int main() {
    using misterplex::ObPmsTraits;
    const int64_t t0 = 1760000000;
    const std::string rpms = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    ObPmsTraits m;
    CHECK(m.firstRequestKbps(rpms, 6000, t0) == 6000, "M1 no entry: the ladder");
    CHECK(m.record(rpms, true, 5228, 94, t0), "M2 a new CAVLC entry is worth saving");
    CHECK(m.firstRequestKbps(rpms, 6000, t0 + 60) == 3337, "M2 rpms: 5228 * 60 / 94 = 3337 (got %d)",
          m.firstRequestKbps(rpms, 6000, t0 + 60));
    CHECK(m.firstRequestKbps(rpms, 3000, t0) == 3000, "M3 min with the ladder");
    CHECK(m.firstRequestKbps(rpms, 0, t0) == 3337, "M3 no ladder known (main): the memory's rate");
    m.record(rpms, true, 5411, 98, t0 + 5);
    CHECK(m.firstRequestKbps(rpms, 6000, t0 + 5) == 5411 * 60 / 98,
          "M4 newest wins (5411 at 98 %%)");
    CHECK(!m.record(rpms, true, 5420, 99, t0 + 6), "M4b a small change is not re-saved");
    m.record(rpms, true, 9000, 400, t0);
    CHECK(m.firstRequestKbps(rpms, 6000, t0) == 1500, "M5 floor 1500 kbps (got %d)", m.firstRequestKbps(rpms, 6000, t0));
    m.record(rpms, true, 3000, 55, t0);
    CHECK(m.firstRequestKbps(rpms, 6000, t0) == 6000, "M6 a CAVLC stream under 60 %% load: the ladder");
    m.record(rpms, true, 5228, 94, t0);
    CHECK(m.firstRequestKbps(rpms, 6000, t0 + ObPmsTraits::kTtlSec + 1) == 6000, "M7 older than 30 days: forgotten");
    CHECK(m.record(rpms, false, 0, 0, t0 + 10) && m.find(rpms) == nullptr && m.firstRequestKbps(rpms, 6000, t0 + 10) == 6000,
          "M8 CABAC output clears the entry (the full ladder again)");
    CHECK(!m.record(rpms, false, 0, 0, t0 + 11), "M8b CABAC with no entry: nothing to save");
    CHECK(!m.record("", true, 5000, 90, t0) && m.firstRequestKbps("", 6000, t0) == 6000, "M9 no machineIdentifier: no memory");
    m.record(rpms, true, 5228, 94, t0);
    m.record("other", true, 4000, 80, t0 - ObPmsTraits::kTtlSec - 5);
    char tmpl[] = "/tmp/ob_pms_traits_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0)
        ::close(fd);
    CHECK(m.save(tmpl, t0), "M10 save");
    ObPmsTraits n;
    CHECK(n.load(tmpl) && n.firstRequestKbps(rpms, 6000, t0) == 3337 && n.find("other") == nullptr,
          "M10b round trip; the expired entry is not written");
    ::unlink(tmpl);
    ObPmsTraits bad;
    bad.parse("garbage\nx 1 0 0 5\n");
    CHECK(bad.find("x") == nullptr && bad.find("garbage") == nullptr, "M11 malformed lines are ignored");
    std::printf("test_owenb_pms_traits: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
