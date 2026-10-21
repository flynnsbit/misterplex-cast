// GDM reply filter — RED if the Sweep 114 self-loop predicate returns.
//
// Old companion code:
//   if (strstr(buf, "M-SEARCH") || strstr(buf, "plex")) reply;
// Our advertise body contains many "plex" tokens, so the old predicate
// replies to our own packets → unicast self-loop → ~100% of one core.
//
// This test fails under the legacy predicate and passes under gdmShouldReply.

#include "libmisterplex/gdm_filter.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

// Exact pre-fix companion predicate (must stay byte-faithful for the RED twin).
static bool legacyGdmReplyPredicate(const char* buf) {
    if (!buf)
        return false;
    return std::strstr(buf, "M-SEARCH") != nullptr || std::strstr(buf, "plex") != nullptr;
}

int main() {
    using namespace misterplex;

    const char* msearch = "M-SEARCH * HTTP/1.1\r\nHost: 239.255.255.250\r\n\r\n";
    const char* msearchLower = "m-search * http/1.1\r\n\r\n";
    const char* noise = "hello world no discovery here\r\n";
    const char* plexOnly = "Content-Type: plex/media-player\r\nProtocol: plex\r\n";

    // --- Positive: real probes must match ------------------------------------
    CHECK(gdmShouldReplyCStr(msearch));
    CHECK(gdmShouldReplyCStr(msearchLower));
    CHECK(gdmShouldReply(msearch, std::strlen(msearch)));

    // --- Negatives a naive "contains plex" implementation fails to reject ----
    CHECK(!gdmShouldReplyCStr(kGdmAdvertiseShape));
    CHECK(!gdmShouldReplyCStr(plexOnly));
    CHECK(!gdmShouldReplyCStr(noise));
    CHECK(!gdmShouldReplyCStr(""));
    CHECK(!gdmShouldReply(nullptr, 0));
    CHECK(!gdmShouldReplyCStr(nullptr));

    // Substring "plex" alone is NOT a probe (the defect class).
    CHECK(!gdmShouldReplyCStr("plex"));
    CHECK(!gdmShouldReplyCStr("I love plex media server"));

    // --- RED twin: legacy predicate MUST fire on advertise (documents bug) ---
    // If this ever goes false, the advertise shape lost its "plex" tokens and
    // the self-loop regression shield is weakened — fail loud.
    CHECK(legacyGdmReplyPredicate(kGdmAdvertiseShape));
    CHECK(legacyGdmReplyPredicate(plexOnly));
    CHECK(legacyGdmReplyPredicate(msearch));
    // And the new filter must disagree with legacy on advertise / plex-only.
    CHECK(legacyGdmReplyPredicate(kGdmAdvertiseShape) != gdmShouldReplyCStr(kGdmAdvertiseShape));
    CHECK(legacyGdmReplyPredicate(plexOnly) != gdmShouldReplyCStr(plexOnly));

    // Advertise shape really contains "plex" (guard against a hollow fixture).
    CHECK(std::strstr(kGdmAdvertiseShape, "plex") != nullptr);

    // --- HTTP-ready gate (Cast picker vanish class) --------------------------
    // Chevron / process-up is not enough. Mute GDM until HTTP is listening.
    CHECK(!gdmMayAdvertise(false));
    CHECK(gdmMayAdvertise(true));
    CHECK(!gdmMayReply(false, msearch, std::strlen(msearch)));
    CHECK(gdmMayReply(true, msearch, std::strlen(msearch)));
    CHECK(!gdmMayReply(true, kGdmAdvertiseShape, std::strlen(kGdmAdvertiseShape)));
    CHECK(!gdmMayReply(false, kGdmAdvertiseShape, std::strlen(kGdmAdvertiseShape)));
    CHECK(!gdmMayReply(true, plexOnly, std::strlen(plexOnly)));

    // RED twin: legacy companion advertised even with HTTP down.
    CHECK(legacyGdmAdvertiseRegardlessOfHttp(false));
    CHECK(legacyGdmAdvertiseRegardlessOfHttp(false) != gdmMayAdvertise(false));
    CHECK(legacyGdmAdvertiseRegardlessOfHttp(true) == gdmMayAdvertise(true));

    if (fails) {
        std::fprintf(stderr, "test_gdm_filter: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_gdm_filter: OK\n");
    return 0;
}
