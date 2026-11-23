/* GCAST: X (Twitter) iOS sends AirPlay /play Content-Location as
 *   twitterav://video-s.twimg.com/amplify_video/<id>/pl/<key>.m3u8?...
 * Stock UxPlay 4e95a9e only routes http:// and https:// to the player, so
 * every such /play was "Content-Location has unsupported form" + conn_reset
 * (lab 2026-11-10 19:14 CT). The same path is served over https by the CDN.
 *
 * gcast_twitterav_rewrite(loc) rewrites a leading "twitterav://" to "https://"
 * in place ONLY when the authority is exactly "video-s.twimg.com" and is
 * followed by '/'. A lookalike host (video-s.twimg.com.evil.com), userinfo
 * (x@video-s.twimg.com), a port (video-s.twimg.com:443), an uppercase host
 * or an uppercase scheme (TWITTERAV://, case-sensitive compare), or any
 * other host is left untouched, so the stock "unsupported form" path still
 * rejects it.
 *
 * Returns 1 rewritten, 0 not a twitterav:// location, -1 twitterav:// refused.
 * In place: "https://" (8) is shorter than "twitterav://" (12), so no
 * allocation and the caller's plist_mem_free() stays correct.
 *
 * Copied verbatim into lib/http_handlers.h by apply_gcast_uxplay_twitterav.py
 * and compiled directly by tests/unit/test_uxplay_twitterav.c.
 * Mutants (test only): MUT_TWITTERAV_ANYHOST, MUT_TWITTERAV_PREFIXHOST, MUT_TWITTERAV_SCHEME_CI.
 */
#ifndef GCAST_TWITTERAV_H
#define GCAST_TWITTERAV_H
#include <string.h>
#include <strings.h>

static int gcast_twitterav_rewrite(char *loc) {
    static const char scheme[] = "twitterav://";
    static const char host[] = "video-s.twimg.com";
    static const char https[] = "https://";
    const size_t sl = sizeof(scheme) - 1, hl = sizeof(host) - 1, nl = sizeof(https) - 1;
#if defined(MUT_TWITTERAV_SCHEME_CI)
    if (!loc || strncasecmp(loc, scheme, sl) != 0)
        return 0;
#else
    if (!loc || strncmp(loc, scheme, sl) != 0)
        return 0;
#endif
    const char *auth = loc + sl;
    const size_t alen = strcspn(auth, "/?#");
#if defined(MUT_TWITTERAV_ANYHOST)
    (void)hl;
    if (auth[alen] != '/')
        return -1;
#elif defined(MUT_TWITTERAV_PREFIXHOST)
    if (strncmp(auth, host, hl) != 0)
        return -1;
#else
    if (alen != hl || strncmp(auth, host, hl) != 0 || auth[alen] != '/')
        return -1;
#endif
    memcpy(loc, https, nl);
    memmove(loc + nl, auth, strlen(auth) + 1);
    return 1;
}
#endif
