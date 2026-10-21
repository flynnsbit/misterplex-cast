// Start-offset URL helpers (arm/misterplexd/transcode_offset.hpp): what a seek restart and the
// doPlay continue-watching start bake into the PMS universal / lab ladder URL.
// Plex position audit 2026-10-04 (release item 7, dbcc42fa): a lab-ladder seek kept the first
// offset_ms, so a second seek (or any seek after a resume, or a seek to 0) restarted the stream at
// the old start while the timeline reported the new target; and a PMS viewOffset start left the URL
// at the cast offset, so the FPGA demuxer had to seek a live TS.
// T-BROWSE-DOUBLE-SEEK fix F-B (PREREG-FB rev1.1 s3): a ladder whose src is live (the helper's
// relay, or a PMS universal transcode) keeps its start in the src's own carrier and carries no
// offset_ms. The F-B expect() messages start with the needles pinned in the pre-reg ("FB(a) ...").
// Built with -DFB_TEETH_BASE_PROBE against the BASE transcode_offset.hpp, teeth (a)-(d) must show
// the pinned RED/GREEN split; (e) needs the new codec header and is compiled out there.
#include "transcode_offset.hpp"
#include "plex_resolve.hpp"  // the producers: urlEncodeQuery (src= of every ladder resolve builds)
#ifndef FB_TEETH_BASE_PROBE
#include "url_query_codec.hpp"
#endif

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

int fails = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++fails;
    }
}

std::string param(const std::string& url, const std::string& key) {
    const auto q = url.find('?');
    if (q == std::string::npos)
        return "<none>";
    size_t pos = q + 1;
    while (pos < url.size()) {
        auto end = url.find('&', pos);
        if (end == std::string::npos)
            end = url.size();
        const std::string kv = url.substr(pos, end - pos);
        if (kv.rfind(key + "=", 0) == 0)
            return kv.substr(key.size() + 1);
        pos = end + 1;
    }
    return "<none>";
}

int count(const std::string& url, const std::string& key) {
    int n = 0;
    for (size_t pos = url.find(key + "="); pos != std::string::npos; pos = url.find(key + "=", pos + 1)) {
        if (pos > 0 && (url[pos - 1] == '?' || url[pos - 1] == '&'))
            ++n;
    }
    return n;
}

// Python int(s) for a decimal string (surrounding whitespace, one sign, '_' only between digits),
// dflt on anything else.
int64_t pyIntOr(const std::string& s, int64_t dflt) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b]))
        ++b;
    while (e > b && ws(s[e - 1]))
        --e;
    bool neg = false;
    if (b < e && (s[b] == '+' || s[b] == '-')) {
        neg = s[b] == '-';
        ++b;
    }
    int64_t v = 0;
    bool lastDigit = false;
    for (size_t i = b; i < e; ++i) {
        if (s[i] >= '0' && s[i] <= '9') {
            if (v > (INT64_MAX - 9) / 10)
                return dflt;
            v = v * 10 + (s[i] - '0');
            lastDigit = true;
        } else if (s[i] == '_' && lastDigit && i + 1 < e) {
            lastDigit = false;
        } else {
            return dflt;
        }
    }
    if (!lastDigit)
        return dflt;
    return neg ? -v : v;
}

std::string formDecode(const std::string& s) {
    auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) && hex(s[i + 2])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Where the lab ladder proxy starts (remote_fpga_ladder_proxy 9d09488b): urllib parse_qs (fields
// split on '&', blank values skipped, '+' and %XX decoded), parse_int takes the first value through
// int() and falls back to 0 (:204-211), then offset_ms = max(0, ...) (:277). No #fragment on the wire.
int64_t proxyOffsetMs(const std::string& url) {
    const auto q = url.find('?');
    if (q == std::string::npos)
        return 0;
    const auto hash = url.find('#');
    const std::string qs = url.substr(q + 1, (hash == std::string::npos ? url.size() : hash) - q - 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        if (amp == std::string::npos)
            amp = qs.size();
        const std::string field = qs.substr(pos, amp - pos);
        const auto eq = field.find('=');
        if (eq != std::string::npos && eq + 1 < field.size() &&
            formDecode(field.substr(0, eq)) == "offset_ms") {
            const int64_t n = pyIntOr(formDecode(field.substr(eq + 1)), 0);
            return n < 0 ? 0 : n;
        }
        pos = amp + 1;
    }
    return 0;
}

// ---- F-B test-side helpers: written independently of the code under test ----
namespace fb {

std::string pctDecode(const std::string& s) {
    auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) && hex(s[i + 2])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The outer query split into fields (empty fields kept), without the #fragment.
std::vector<std::string> outerFields(const std::string& url) {
    std::vector<std::string> f;
    const auto q = url.find('?');
    if (q == std::string::npos)
        return f;
    const auto hash = url.find('#');
    const std::string qs = url.substr(q + 1, (hash == std::string::npos ? url.size() : hash) - q - 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        if (amp == std::string::npos)
            amp = qs.size();
        f.push_back(qs.substr(pos, amp - pos));
        pos = amp + 1;
    }
    return f;
}

std::string srcEnc(const std::string& url) {
    for (const auto& field : outerFields(url))
        if (field.rfind("src=", 0) == 0)
            return field.substr(4);
    return "<none>";
}

std::string srcOf(const std::string& url) { return pctDecode(srcEnc(url)); }

// The outer URL with src= blanked and every offset_ms field dropped: what F-B must not change.
std::string outerSkeleton(const std::string& url) {
    const auto q = url.find('?');
    std::string out = url.substr(0, q == std::string::npos ? url.size() : q) + "?";
    for (const auto& field : outerFields(url)) {
        if (field.rfind("offset_ms=", 0) == 0)
            continue;
        out += (field.rfind("src=", 0) == 0 ? std::string("src=<SRC>") : field) + "&";
    }
    const auto hash = url.find('#');
    return out + (hash == std::string::npos ? std::string() : url.substr(hash));
}

// The o<ms> of a relay src's path, as written ("" if the path does not end in /o<digits>).
std::string relayO(const std::string& src) {
    const auto qh = src.find_first_of("?#", src.find("://") == std::string::npos ? 0 : src.find("://") + 3);
    const std::string path = src.substr(0, qh == std::string::npos ? src.size() : qh);
    const auto slash = path.rfind("/o");
    if (slash == std::string::npos || slash + 2 >= path.size())
        return "";
    const std::string d = path.substr(slash + 2);
    for (char c : d)
        if (c < '0' || c > '9')
            return "";
    return d;
}

// The relay src with its o<ms> digits blanked: what F-B must not change inside a relay src.
std::string relaySkeleton(const std::string& src) {
    const std::string o = relayO(src);
    if (o.empty())
        return src;
    const auto qh = src.find_first_of("?#", src.find("://") + 3);
    const size_t pathEnd = qh == std::string::npos ? src.size() : qh;
    return src.substr(0, pathEnd - o.size()) + "<MS>" + src.substr(pathEnd);
}

// A universal src's query split around the key offset (exact name at a field start): its values,
// and the src without those fields (what F-B must not change inside a universal src).
std::vector<std::string> srcOffsets(const std::string& src) {
    std::vector<std::string> v;
    for (const auto& field : outerFields(src))
        if (field.rfind("offset=", 0) == 0)
            v.push_back(field.substr(7));
    return v;
}

std::string universalSkeleton(const std::string& src) {
    const auto q = src.find('?');
    if (q == std::string::npos)
        return src;
    std::string out = src.substr(0, q) + "?";
    for (const auto& field : outerFields(src))
        if (field.rfind("offset=", 0) != 0)
            out += field + "&";
    const auto hash = src.find('#');
    return out + (hash == std::string::npos ? std::string() : src.substr(hash));
}

#ifndef FB_TEETH_BASE_PROBE
// plex_resolve.cpp urlEncodeQuery as it was at 8d52ace6 :365-377, pinned verbatim (tooth (e)).
std::string oldUrlEncodeQuery(const std::string& s) {
    std::ostringstream o;
    o << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o << static_cast<char>(c);
        else {
            o << '%' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c)
              << std::dec;
        }
    }
    return o.str();
}
#endif

}  // namespace fb

}  // namespace

int main() {
    using namespace misterplex::transcode_offset;
    const std::string u0 =
        "http://pms:32400/video/:/transcode/universal/start.ts?path=%2Flibrary%2Fmetadata%2F7"
        "&maxVideoBitrate=2500&videoQuality=60&videoResolution=624x480";
    const std::string l0 =
        "http://192.0.2.21:18767/ladder.ts?src=http%3A%2F%2Fpms%2Fparts%2F7&w=624&h=480&fps=24000%2F1001";
    const std::string d = "http://pms:32400/library/parts/7/1/file.mp4?X-Plex-Token=t";

    // PMS universal: whole seconds, rounded half-up, plus the seek-safe clamps.
    {
        const std::string s = withSeekSafeTranscodeParams(u0, 123456);
        expect(param(s, "offset") == "123", "universal 123456 ms -> offset=123, got " + param(s, "offset"));
        expect(param(s, "maxVideoBitrate") == "350" && param(s, "videoQuality") == "18" &&
                   param(s, "fastSeek") == "1",
               "universal seek clamps 350/18 + fastSeek=1: " + s);
        expect(param(withSeekSafeTranscodeParams(u0, 1500), "offset") == "2", "1500 ms -> offset=2");
        const std::string again = withSeekSafeTranscodeParams(s, 60000);
        expect(param(again, "offset") == "60" && count(again, "offset") == 1,
               "universal retarget replaces offset=: " + again);
        const std::string zero = withSeekSafeTranscodeParams(s, 0);
        expect(param(zero, "offset") == "0" && count(zero, "offset") == 1, "universal seek to 0 -> offset=0");
        expect(urlHasUniversalOffset(s) && !urlHasUniversalOffset(u0), "urlHasUniversalOffset (universal)");
    }
    // Lab ladder: offset_ms is the proxy's -ss start, no PMS clamps.
    {
        const std::string first = withSeekSafeTranscodeParams(l0, 30000);
        expect(param(first, "offset_ms") == "30000" && count(first, "offset_ms") == 1,
               "ladder first seek appends offset_ms=30000: " + first);
        expect(param(first, "maxVideoBitrate") == "<none>" && param(first, "fastSeek") == "<none>",
               "ladder gets no PMS clamps");
        // The audited bug: a URL that already carries offset_ms kept the old start.
        const std::string second = withSeekSafeTranscodeParams(first, 60000);
        expect(param(second, "offset_ms") == "60000" && count(second, "offset_ms") == 1,
               "ladder second seek retargets offset_ms to 60000 (dbcc42fa kept 30000): " + second);
        const std::string back = withSeekSafeTranscodeParams(second, 0);
        expect(param(back, "offset_ms") == "0" && count(back, "offset_ms") == 1,
               "ladder seek to 0 sets offset_ms=0 (dbcc42fa kept 60000): " + back);
        expect(withSeekSafeTranscodeParams(l0, 0) == l0, "ladder seek to 0 on a fresh URL is unchanged");
        expect(urlHasUniversalOffset(first) && !urlHasUniversalOffset(l0), "urlHasUniversalOffset (ladder)");
        const std::string frag = withSeekSafeTranscodeParams(l0 + "#t", 5000);
        expect(frag.size() > 2 && frag.substr(frag.size() - 2) == "#t" && param(frag.substr(0, frag.size() - 2), "offset_ms") == "5000",
               "fragment preserved: " + frag);
    }
    // Direct Part: not a universal or ladder URL; the demuxer seeks it (seekable HTTP).
    expect(withSeekSafeTranscodeParams(d, 5000) == d, "direct Part URL unchanged");

    // doPlay start retarget: compared with the start the URL already carries.
    expect(retargetStartUrl(u0, 0) == u0, "universal resolved for 0, start 0: unchanged");
    expect(retargetStartUrl(u0, -8000) == u0,
           "negative cast offset (normalized to 0) leaves the URL alone, no seek clamps");
    expect(retargetStartUrl(l0, 0) == l0, "ladder resolved for 0, start 0: unchanged");
    {
        const std::string resumed = retargetStartUrl(l0, 1234567);
        expect(param(resumed, "offset_ms") == "1234567",
               "continue watching (cast offset absent) bakes the viewOffset into the ladder URL: " + resumed);
        const std::string universal = retargetStartUrl(u0, 1234567);
        expect(param(universal, "offset") == "1235" && param(universal, "maxVideoBitrate") == "350",
               "continue watching on PMS universal: offset=1235 + seek clamps: " + universal);
        const std::string at60 = withSeekSafeTranscodeParams(l0, 60000);
        const std::string scrubbed = retargetStartUrl(at60, 120000);
        expect(param(scrubbed, "offset_ms") == "120000" && count(scrubbed, "offset_ms") == 1,
               "scrub during resolve retargets 60000 -> 120000: " + scrubbed);
        expect(retargetStartUrl(at60, 60000) == at60, "ladder already at the start: unchanged");
        const std::string u123 = u0 + "&offset=123";
        expect(retargetStartUrl(u123, 123456) == u123,
               "universal already asking for that second keeps its parameters (no clamps added)");
        const std::string u124 = retargetStartUrl(u123, 123600);
        expect(param(u124, "offset") == "124" && count(u124, "offset") == 1,
               "universal start moved to the next second: offset=124: " + u124);
        const std::string direct5 = l0 + "&offset_ms=5000";
        const std::string direct0 = retargetStartUrl(direct5, 0);
        expect(param(direct0, "offset_ms") == "0" && count(direct0, "offset_ms") == 1,
               "a passed-through ladder URL whose baked start differs from the play start follows "
               "the play start: " + direct0);
        expect(retargetStartUrl(d, 5000) == d, "direct Part: unchanged (demuxer seeks)");
        // A sub-half-second start on a universal URL without offset=: PMS starts at 0 either way;
        // offset=0 is made explicit (no seek clamps) so the demuxer does not seek the live TS.
        const std::string u499 = retargetStartUrl(u0, 499);
        expect(param(u499, "offset") == "0" && param(u499, "maxVideoBitrate") == "2500" &&
                   param(u499, "fastSeek") == "<none>",
               "universal start 499 ms: offset=0, start ladder kept: " + u499);
        const std::string u500 = retargetStartUrl(u0, 500);
        expect(param(u500, "offset") == "1" && param(u500, "maxVideoBitrate") == "350",
               "universal start 500 ms: offset=1 with seek clamps: " + u500);
        const std::string uz = u0 + "&offset=0";
        expect(retargetStartUrl(uz, 499) == uz, "universal offset=0 already present, start 499 ms: unchanged");
    }

    // Review follow-up (review, c65b6078): duplicate, blank and malformed offset_ms. Whatever
    // the URL carried, after retargetStartUrl the proxy must start where the play starts.
    {
        const std::vector<std::string> carried = {
            "", "&offset_ms=5000", "&offset_ms=5000&offset_ms=9000", "&offset_ms=9000&offset_ms=5000",
            "&offset_ms=&offset_ms=5000", "&offset_ms=5000&offset_ms=", "&offset_ms=", "&offset_ms=abc",
            "&offset_ms=5000x", "&offset_ms=5000.0", "&offset_ms=5_000", "&offset_ms=-5000",
            "&offset_ms=+5000", "&offset_ms=%2B5000", "&offset_ms=005000", "&xoffset_ms=5000",
            "&offset_ms=5000#t"};
        for (const auto& c : carried) {
            for (const int64_t s : {int64_t{0}, int64_t{5}, int64_t{499}, int64_t{5000}, int64_t{9000},
                                    int64_t{60000}}) {
                const std::string out = retargetStartUrl(l0 + c, s);
                expect(proxyOffsetMs(out) == s,
                       "ladder [" + c + "] start " + std::to_string(s) + ": proxy starts at " +
                           std::to_string(proxyOffsetMs(out)) + ": " + out);
            }
        }
        const std::string dup = l0 + "&offset_ms=5000&offset_ms=9000";
        expect(retargetStartUrl(dup, 5000) == dup, "ladder duplicate offset_ms: the first is the start, unchanged");
        const std::string dup9 = retargetStartUrl(dup, 9000);
        expect(param(dup9, "offset_ms") == "9000" && count(dup9, "offset_ms") == 2,
               "ladder duplicate offset_ms: only the first is rewritten (the proxy reads the first): " + dup9);
        expect(queryParamInt(l0 + "&offset_ms=5000x", "offset_ms") == -1 &&
                   queryParamInt(l0 + "&offset_ms=", "offset_ms") == -1 &&
                   queryParamInt(l0 + "&offset_ms=-5000", "offset_ms") == -5000 &&
                   queryParamInt(l0, "offset_ms") == 0 &&
                   queryParamInt(l0 + "&xoffset_ms=7", "offset_ms") == 0,
               "queryParamInt: junk/blank -1, signed value kept, absent 0, key boundary");
    }
    // Universal offset= (no PMS model): the first key is read and rewritten, malformed values
    // become a clean whole second.
    {
        const std::string dup = u0 + "&offset=10&offset=20";
        expect(retargetStartUrl(dup, 10000) == dup, "universal duplicate offset=: the first (10) is the start");
        const std::string moved = retargetStartUrl(dup, 20000);
        expect(param(moved, "offset") == "20" && count(moved, "offset") == 2 &&
                   param(moved, "maxVideoBitrate") == "350",
               "universal duplicate offset=: the first is retargeted, the second left: " + moved);
        const std::string frac = retargetStartUrl(u0 + "&offset=12.5", 12000);
        expect(param(frac, "offset") == "12" && count(frac, "offset") == 1,
               "universal offset=12.5 is not read as 12, it is rewritten: " + frac);
        const std::string junk = retargetStartUrl(u0 + "&offset=abc", 0);
        expect(param(junk, "offset") == "0" && count(junk, "offset") == 1, "universal offset=abc -> offset=0: " + junk);
        const std::string blank = u0 + "&offset=";
        expect(retargetStartUrl(blank, 0) == blank, "universal blank offset= at start 0: unchanged");
        const std::string blank3 = retargetStartUrl(blank, 3000);
        expect(param(blank3, "offset") == "3" && count(blank3, "offset") == 1,
               "universal blank offset= at 3 s -> offset=3: " + blank3);
    }
    // doPlay start bounds (main.cpp): [0, duration]; an unknown duration caps at one week.
    {
        expect(clampStartMs(-5, 0) == 0 && clampStartMs(-5, 60000) == 0, "negative start -> 0");
        expect(clampStartMs(5000, 0) == 5000, "unknown duration keeps a real start");
        expect(clampStartMs(INT64_MAX, 0) == kMaxUnknownDurationStartMs, "unknown duration caps INT64_MAX at one week");
        expect(clampStartMs(kMaxUnknownDurationStartMs + 1, -1) == kMaxUnknownDurationStartMs,
               "a negative duration counts as unknown");
        expect(clampStartMs(8000000, 7200000) == 7200000 && clampStartMs(INT64_MAX, 7200000) == 7200000,
               "known duration clamps to the duration");
        expect(clampStartMs(7200000, 7200000) == 7200000 && clampStartMs(0, 0) == 0, "bounds are inclusive");
        // Whole-second rounding without signed overflow (seekMs does not clamp an unknown duration).
        const std::string maxSec = std::to_string(INT64_MAX / 1000 + 1);
        expect(roundMsToSec(INT64_MAX) == INT64_MAX / 1000 + 1 && roundMsToSec(1499) == 1 &&
                   roundMsToSec(1500) == 2 && roundMsToSec(0) == 0 && roundMsToSec(-1) == 0,
               "roundMsToSec half-up, 0 at or below 0");
        expect(param(withSeekSafeTranscodeParams(u0, INT64_MAX), "offset") == maxSec &&
                   param(retargetStartUrl(u0, INT64_MAX), "offset") == maxSec,
               "universal offset at INT64_MAX ms does not wrap");
    }

    // ---- F-B teeth (T-BROWSE-DOUBLE-SEEK fix F-B, PREREG-FB rev1.1 s3; needles pinned) ----
    {
        using std::to_string;
        // Vectors from the producers (H264 n2). The outer ladder is composed as resolve does
        // (plex_resolve.cpp :2440-2442) with the producer's own urlEncodeQuery. The relay src is
        // the helper's own format (mpx_menu.py :1004 @675c9b09, a Python producer): served for
        // real in ds3 as GET /menu/tsrc/local/1/o95000.
        const std::string kProxy = "http://192.0.2.21:18767";
        auto ladderOf = [&](const std::string& src) {
            return kProxy + "/ladder.ts?src=" + misterplex::urlEncodeQuery(src) +
                   "&w=624&h=480&br=1000&fps=24000/1001";
        };
        const std::string rSrc0 = "http://127.0.0.1:18769/menu/tsrc/local/1/o0";
        const std::string rSrc95 = "http://127.0.0.1:18769/menu/tsrc/local/1/o95000";
        const std::string lr0 = ladderOf(rSrc0);
        const std::string lr95 = ladderOf(rSrc95);
        const auto q95 = lr95.find('?');
        const auto w95 = lr95.find("&w=");
        const std::string lrm1 = lr95.substr(0, q95 + 1) + "offset_ms=95000&" + lr95.substr(q95 + 1);
        const std::string lrm2 = lr95.substr(0, w95) + "&offset_ms=95000" + lr95.substr(w95);
        const std::string lrm3 = lr95 + "&offset_ms=95000";
        const std::string lrq = ladderOf(rSrc95 + "?x=1");
        const std::string lrs = ladderOf(rSrc95 + "/");
        // Commit B (UNIVERSAL arm, PROVISIONAL): the owner transcode src from resolve's own
        // producer (plex_resolve.cpp ownerTranscodeLadderSrc, the :2403-2412 composition) on the
        // real 480p profile; offset= only when > 0 (buildUniversalTranscodeUrl).
        misterplex::WeakLadder fit;
        const bool fitOk = misterplex::applyPlexTranscodeProfile("480p", fit);
        const std::string uSrc0 = misterplex::ownerTranscodeLadderSrc(
            "http://127.0.0.1:32400", "/library/metadata/1", "tok", "fbsession0001", 0, fit, false);
        const std::string uSrc95 = misterplex::ownerTranscodeLadderSrc(
            "http://127.0.0.1:32400", "/library/metadata/1", "tok", "fbsession0001", 95000, fit, false);
        const std::string lu0 = ladderOf(uSrc0);
        const std::string lu95 = ladderOf(uSrc95);
        const std::string lp0 = lu0 + "&offset_ms=95000";
        const std::string lu95m = lu95 + "&offset_ms=95000";
        const std::string luv = ladderOf(uSrc0 + "&viewOffset=30000&startOffset=30");
        const std::vector<std::pair<const char*, std::string>> printed = {
            {"lr0", lr0}, {"lr95", lr95}, {"lrm1", lrm1}, {"lrm2", lrm2}, {"lrm3", lrm3},
            {"lrq", lrq}, {"lrs", lrs}, {"lu0", lu0}, {"lu95", lu95}, {"lp0", lp0},
            {"lu95m", lu95m}, {"luv", luv}, {"l0", l0}};
        for (const auto& v : printed)
            std::printf("FB vector %s: %s\n", v.first, v.second.c_str());
        expect(fitOk && uSrc0.find("/video/:/transcode/universal/start.mp4?") != std::string::npos &&
                   fb::srcOffsets(uSrc0).empty() &&
                   fb::srcOffsets(uSrc95) == std::vector<std::string>{"95"},
               "FB(a) universal carrier: the producer vectors: " + uSrc0 + " | " + uSrc95);

        using Fn = std::string (*)(const std::string&, int64_t);
        const std::vector<std::pair<const char*, Fn>> fns = {
            {"seek", &withSeekSafeTranscodeParams}, {"retarget", &retargetStartUrl}};
        auto at = [](const char* f, const std::string& x, int64_t t) {
            return std::string(f) + "(" + x + ", " + to_string(t) + ")";
        };

        // (a) Live srcs, relay rows.
        for (const auto& x : {lr0, lr95}) {
            for (const int64_t t : {int64_t{95000}, int64_t{125000}}) {
                for (const auto& f : fns) {
                    const std::string out = f.second(x, t);
                    expect(fb::relayO(fb::srcOf(out)) == to_string(t),
                           "FB(a) relay carrier: " + at(f.first, x, t) + " -> " + out);
                    expect(count(out, "offset_ms") == 0 && proxyOffsetMs(out) == 0,
                           "FB(a) no offset_ms: " + at(f.first, x, t) + " -> " + out);
                    expect(fb::outerSkeleton(out) == fb::outerSkeleton(x) &&
                               fb::relaySkeleton(fb::srcOf(out)) == fb::relaySkeleton(fb::srcOf(x)),
                           "FB(a) other bytes: " + at(f.first, x, t) + " -> " + out);
                }
            }
        }
        expect(retargetStartUrl(lr95, 95000) == lr95,
               "FB(a) P1 already there: relay at 95000 -> " + retargetStartUrl(lr95, 95000));

        // (a) Live srcs, universal rows (commit B).
        for (const auto& x : {lu0, lu95, lp0, lu95m}) {
            for (const int64_t t : {int64_t{95000}, int64_t{125000}}) {
                for (const auto& f : fns) {
                    const std::string out = f.second(x, t);
                    expect(fb::srcOffsets(fb::srcOf(out)) == std::vector<std::string>{to_string(roundMsToSec(t))},
                           "FB(a) universal carrier: " + at(f.first, x, t) + " -> " + out);
                    expect(count(out, "offset_ms") == 0 && proxyOffsetMs(out) == 0,
                           "FB(a) no offset_ms: " + at(f.first, x, t) + " -> " + out);
                    expect(fb::outerSkeleton(out) == fb::outerSkeleton(x) &&
                               fb::universalSkeleton(fb::srcOf(out)) == fb::universalSkeleton(fb::srcOf(x)),
                           "FB(a) other bytes: " + at(f.first, x, t) + " -> " + out);
                }
            }
        }
        expect(retargetStartUrl(lu95, 95000) == lu95,
               "FB(a) P1 already there: owner transcode at 95000 -> " + retargetStartUrl(lu95, 95000));
        {
            const std::string out = retargetStartUrl(lp0, 95000);
            expect(fb::srcOffsets(fb::srcOf(out)) == std::vector<std::string>{"95"} && count(out, "offset_ms") == 0,
                   "FB(a) P0 strip: " + out);
        }
        expect(count(retargetStartUrl(lu95m, 95000), "offset_ms") == 0,
               "FB(a) R2 needs no offset_ms: " + retargetStartUrl(lu95m, 95000));
        for (const int64_t ms : {int64_t{0}, int64_t{1}, int64_t{499}, int64_t{500}, int64_t{999},
                                 int64_t{1000}, int64_t{95499}, int64_t{95500}, int64_t{125000}}) {
            expect(roundMsToSec(ms) == misterplex::universalOffsetSeconds(ms),
                   "FB(a) rounding: " + to_string(ms) + " ms: " + to_string(roundMsToSec(ms)) + " vs " +
                       to_string(misterplex::universalOffsetSeconds(ms)));
        }
        // (a+) n3 (i): viewOffset= / startOffset= are not the carrier.
        for (const int64_t t : {int64_t{95000}, int64_t{125000}}) {
            for (const auto& f : fns) {
                const std::string out = f.second(luv, t);
                expect(fb::srcOffsets(fb::srcOf(out)) == std::vector<std::string>{to_string(roundMsToSec(t))} &&
                           fb::universalSkeleton(fb::srcOf(out)) == fb::universalSkeleton(fb::srcOf(luv)) &&
                           count(out, "offset_ms") == 0,
                       "FB(a+) exact name: " + at(f.first, luv, t) + " -> " + out);
            }
        }

        // (a+) Exact-name parsing (H264 n3): offset_ms at any position (ii); the relay form (iii).
        for (const auto& x : {lrm1, lrm2, lrm3}) {
            for (const int64_t t : {int64_t{95000}, int64_t{125000}}) {
                for (const auto& f : fns) {
                    const std::string out = f.second(x, t);
                    expect(count(out, "offset_ms") == 0 && out == f.second(lr95, t),
                           "FB(a+) offset_ms position: " + at(f.first, x, t) + " -> " + out);
                }
            }
        }
        for (const int64_t t : {int64_t{95000}, int64_t{125000}}) {
            for (const auto& f : fns) {
                const std::string q = f.second(lrq, t);
                expect(fb::srcOf(q) == "http://127.0.0.1:18769/menu/tsrc/local/1/o" + to_string(t) + "?x=1" &&
                           count(q, "offset_ms") == 0,
                       "FB(a+) relay form: a query after o<ms> is RELAY: " + at(f.first, lrq, t) + " -> " + q);
                const std::string s = f.second(lrs, t);
                expect(s == lrs + "&offset_ms=" + to_string(t),
                       "FB(a+) relay form: a trailing '/' is PART: " + at(f.first, lrs, t) + " -> " + s);
            }
        }

        // (b) Part src: no regression (the expects above keep their own messages).
        for (const int64_t t : {int64_t{95000}, int64_t{125000}}) {
            for (const auto& f : fns) {
                const std::string out = f.second(l0, t);
                expect(param(out, "offset_ms") == to_string(t) && count(out, "offset_ms") == 1,
                       "FB(b) part: " + at(f.first, l0, t) + " -> " + out);
            }
        }

        // (c) Seek to 0.
        for (const auto& f : fns) {
            const std::string out = f.second(lr95, 0);
            expect(fb::srcOf(out) == rSrc0 && count(out, "offset_ms") == 0,
                   "FB(c) seek to 0: " + at(f.first, lr95, 0) + " -> " + out);
            const std::string uo = f.second(lu95, 0);
            expect(fb::srcOffsets(fb::srcOf(uo)) == std::vector<std::string>{"0"} && count(uo, "offset_ms") == 0,
                   "FB(c) seek to 0: " + at(f.first, lu95, 0) + " -> " + uo);
        }
        expect(withSeekSafeTranscodeParams(l0, 0) == l0 &&
                   param(withSeekSafeTranscodeParams(l0 + "&offset_ms=5000", 0), "offset_ms") == "0",
               "FB(c) seek to 0: a Part ladder keeps today's behaviour");

        // (d) Idempotence.
        for (const auto& x : {lr0, lr95, lrm1, lrm2, lrm3, lrq, lrs, lu0, lu95, lp0, lu95m, luv, l0}) {
            for (const int64_t t : {int64_t{0}, int64_t{95000}, int64_t{125000}}) {
                for (const auto& f : fns) {
                    const std::string once = f.second(x, t);
                    expect(f.second(once, t) == once,
                           "FB(d) idempotent: " + at(f.first, x, t) + " -> " + once + " -> " + f.second(once, t));
                }
            }
        }

#ifndef FB_TEETH_BASE_PROBE
        // (e) Codec: the shared encoder and resolve's urlEncodeQuery equal the pinned old body on
        // every byte; every vector's src round-trips; a src that does not re-encode to the same bytes
        // is left PART (today's behaviour).
        for (int b = 0; b < 256; ++b) {
            const std::string s(1, static_cast<char>(b));
            expect(misterplex::urlEncodeQueryComponent(s) == fb::oldUrlEncodeQuery(s) &&
                       misterplex::urlEncodeQuery(s) == fb::oldUrlEncodeQuery(s),
                   "FB(e) codec: byte " + to_string(b));
        }
        for (const auto& v : printed) {
            std::string d;
            const std::string enc = fb::srcEnc(v.second);
            expect(misterplex::urlDecodeQueryComponent(enc, &d) && misterplex::urlEncodeQueryComponent(d) == enc,
                   std::string("FB(e) codec: round trip of ") + v.first);
        }
        {
            std::string lower = lr95;
            const auto p = lower.find("%2F");
            lower.replace(p, 3, "%2f");
            expect(withSeekSafeTranscodeParams(lower, 125000) == lower + "&offset_ms=125000",
                   "FB(e) codec: a non-canonical src is not rewritten: " + withSeekSafeTranscodeParams(lower, 125000));
        }
#endif
    }

    {
        // GCAST YouTube: a ladder over gcast-xcode /yt.ts carries its start in start_ms= (live remux).
        const std::string yt = "http://p:18767/ladder.ts?src=" +
                               misterplex::urlEncodeQueryComponent("http://x:18780/yt.ts?v=aqz-KE-bpKQ&start_ms=0") +
                               "&w=624&h=480&br=4000&gcast=1&fps=src";
        const std::string want = "http://p:18767/ladder.ts?src=" +
                                 misterplex::urlEncodeQueryComponent("http://x:18780/yt.ts?v=aqz-KE-bpKQ&start_ms=23566") +
                                 "&w=624&h=480&br=4000&gcast=1&fps=src";
        expect(misterplex::transcode_offset::withSeekSafeTranscodeParams(yt, 23566) == want, "YT seek: carrier start_ms, no offset_ms: " +
               misterplex::transcode_offset::withSeekSafeTranscodeParams(yt, 23566));
        expect(misterplex::transcode_offset::withSeekSafeTranscodeParams(yt + "&offset_ms=23566", 30000).find("offset_ms") == std::string::npos,
               "YT seek drops an outer offset_ms");
        expect(misterplex::transcode_offset::retargetStartUrl(yt, 0) == yt, "YT at start keeps the URL");
        expect(misterplex::transcode_offset::ladderYtRemuxSrc(yt) && misterplex::transcode_offset::ladderYtRemuxSrc(want),
               "ladderYtRemuxSrc: yt.ts ladder carries its start (no demux seek)");
        expect(misterplex::transcode_offset::retargetStartUrl(yt, 23566) == want, "YT retarget -> " + misterplex::transcode_offset::retargetStartUrl(yt, 23566));
        // Not a yt remux: other path, start_ms as a substring of another key, junk digits.
        const std::string other = "http://p:18767/ladder.ts?src=" +
                                  misterplex::urlEncodeQueryComponent("http://x:18780/yt.tsx?v=a&start_ms=0") + "&w=1";
        expect(misterplex::transcode_offset::withSeekSafeTranscodeParams(other, 5000).find("offset_ms=5000") != std::string::npos,
               "yt.tsx stays PART (offset_ms)");
        const std::string sub = "http://p:18767/ladder.ts?src=" +
                                misterplex::urlEncodeQueryComponent("http://x:18780/yt.ts?v=a&xstart_ms=0") + "&w=1";
        expect(misterplex::transcode_offset::withSeekSafeTranscodeParams(sub, 5000).find("offset_ms=5000") != std::string::npos,
               "xstart_ms is not the carrier");
        const std::string junk = "http://p:18767/ladder.ts?src=" +
                                 misterplex::urlEncodeQueryComponent("http://x:18780/yt.ts?v=a&start_ms=5x") + "&w=1";
        expect(misterplex::transcode_offset::withSeekSafeTranscodeParams(junk, 5000).find("offset_ms=5000") != std::string::npos,
               "start_ms=5x is not a carrier");
        expect(!misterplex::transcode_offset::ladderYtRemuxSrc(other) && !misterplex::transcode_offset::ladderYtRemuxSrc(junk) &&
                   !misterplex::transcode_offset::ladderYtRemuxSrc("http://x:18780/yt.ts?v=a&start_ms=0"),
               "ladderYtRemuxSrc: only a /ladder.ts over /yt.ts?start_ms=<digits>");
    }

    // NeedsTranscode retry: 2217 ms is offset=2 on a universal URL, and the demux
    // must not seek those 2217 ms again (a live TS seek returns no frame).
    {
        const std::string at = retargetStartUrl(u0, 2217);
        expect(param(at, "offset") == "2", "2217 ms -> offset=2, got " + param(at, "offset"));
        expect(demuxSeekMs(at, 2217) == 0, "universal offset=2: no second demux seek");
        expect(demuxSeekMs(u0, 2217) == 2217, "universal with no offset= still seeks");
        expect(demuxSeekMs(d, 2217) == 2217, "direct Part seeks 2217 ms");
        const std::string src =
            "http://127.0.0.1:32400/video/:/transcode/universal/start.mp4?path=%2Flibrary%2Fmetadata%2F1&offset=2";
        const std::string ladder = "http://192.0.2.21:18767/ladder.ts?src=" + misterplex::urlEncodeQuery(src) +
                                   "&w=624&h=480";
        expect(liveStartBaked(ladder) && demuxSeekMs(ladder, 2217) == 0,
               "live universal ladder (no outer offset_ms): no second demux seek");
        expect(param(withSeekSafeTranscodeParams(u0, 1500), "offset") == "2", "1500 ms -> offset=2 still");
    }

    if (fails) {
        std::fprintf(stderr, "test_transcode_offset: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_transcode_offset: OK (universal offset= whole seconds half-up + seek clamps, "
                "replace on retarget; ladder offset_ms retargets on every seek incl. 0; direct Part "
                "unchanged; doPlay start retarget for viewOffset / scrub-during-resolve; duplicate, "
                "blank and malformed offsets end where the proxy starts; start clamp and rounding "
                "without overflow)\n");
    return 0;
}
