#pragma once
// On-device OpenSubtitles.com REST API v1 fallback (no helper server).
//
// OPENSUBTITLES_API_KEY (misterplex.conf) or env OPENSUBTITLES_API_KEY /
// MPX_OPENSUBTITLES_API_KEY. NEVER log the key (key_present=0/1 only).
// OPENSUBS_FALLBACK (conf/env) defaults on (cache on FAT works offline; search/download need a key).
// Never mutates Plex server state (no PMS uploads or stream selection writes).
// Optional OPENSUBTITLES_USERNAME / OPENSUBTITLES_PASSWORD → POST /api/v1/login.
//
// Cache: /media/fat/misterplex/subs/<ratingKey|imdb>-en.srt

#include "track_select.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <errno.h>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace misterplex {
namespace opensubs {

inline bool envFlagOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

inline bool envFlagOff(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "0") == 0 || std::strcmp(buf, "false") == 0 || std::strcmp(buf, "no") == 0 ||
           std::strcmp(buf, "off") == 0;
}

// Never returns the secret. Empty when unset.
inline std::string apiKey() {
    if (const char* e = std::getenv("MPX_OPENSUBTITLES_API_KEY"); e && e[0])
        return e;
    if (const char* e = std::getenv("OPENSUBTITLES_API_KEY"); e && e[0])
        return e;
    return {};
}

inline bool keyPresent() { return !apiKey().empty(); }

// Explicit OFF wins. Otherwise on (cached srt on FAT does not require a key).
inline bool fallbackEnabled() {
#ifdef MUT_OPENSUBS_FALLBACK_OFF
    return false;
#else
    if (const char* e = std::getenv("MPX_OPENSUBS_FALLBACK"); e && e[0]) {
        if (envFlagOff(e))
            return false;
    }
    if (const char* e = std::getenv("OPENSUBS_FALLBACK"); e && e[0]) {
        if (envFlagOff(e))
            return false;
    }
    return true;
#endif
}

inline bool networkFallbackEnabled() { return fallbackEnabled() && keyPresent(); }

// Serve a cached OpenSubtitles.com file without an API key (#221).
inline bool shouldServeOpenSubsFromCache(bool fallbackOn, bool shouldQuery, bool cacheReadable) {
    return fallbackOn && shouldQuery && cacheReadable;
}

inline std::string username() {
    if (const char* e = std::getenv("MPX_OPENSUBTITLES_USERNAME"); e && e[0])
        return e;
    if (const char* e = std::getenv("OPENSUBTITLES_USERNAME"); e && e[0])
        return e;
    return {};
}

inline std::string password() {
    if (const char* e = std::getenv("MPX_OPENSUBTITLES_PASSWORD"); e && e[0])
        return e;
    if (const char* e = std::getenv("OPENSUBTITLES_PASSWORD"); e && e[0])
        return e;
    return {};
}

inline void logKeyPresenceOnce() {
    static std::atomic<int> once{0};
    if (once.exchange(1) != 0)
        return;
    if (!keyPresent())
        std::fprintf(stderr, "misterplexd: opensubtitles: no API key (set OPENSUBTITLES_API_KEY)\n");
    else
        std::fprintf(stderr, "misterplexd: opensubtitles: key_present=1\n");
}

struct Guids {
    std::string imdb;   // digits only, no "tt"
    std::string tmdb;   // digits
    std::string title;
    std::string year;   // 4 digits when known
};

inline Guids parseGuidsFromXml(const std::string& xml) {
    Guids g;
    size_t sp = 0;
    while (sp < xml.size()) {
        const auto at = xml.find("<Guid ", sp);
        if (at == std::string::npos)
            break;
        const auto end = xml.find('>', at);
        if (end == std::string::npos)
            break;
        const std::string tag = xml.substr(at, end - at);
        const std::string id = xmlTagAttr(tag, "id");
        sp = end + 1;
        if (id.rfind("imdb://", 0) == 0) {
            std::string v = id.substr(7);
            if (v.rfind("tt", 0) == 0)
                v = v.substr(2);
            for (char c : v) {
                if (std::isdigit(static_cast<unsigned char>(c)))
                    g.imdb.push_back(c);
            }
        } else if (id.rfind("tmdb://", 0) == 0) {
            for (char c : id.substr(7)) {
                if (std::isdigit(static_cast<unsigned char>(c)))
                    g.tmdb.push_back(c);
            }
        }
    }
    const std::string videoOpen = [&]() {
        const auto p = xml.find("<Video");
        if (p == std::string::npos)
            return std::string();
        const auto e = xml.find('>', p);
        if (e == std::string::npos)
            return std::string();
        return xml.substr(p, e - p);
    }();
    if (!videoOpen.empty()) {
        g.title = xmlTagAttr(videoOpen, "title");
        g.year = xmlTagAttr(videoOpen, "year");
        if (g.year.size() != 4)
            g.year.clear();
    }
    return g;
}

inline bool catalogHasTextSubtitle(const StreamCatalog& cat, int partId) {
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle)
            continue;
        if (partId > 0 && s.partId > 0 && s.partId != partId)
            continue;
        if (subtitleCodecIsImage(s.codec))
            continue;
        if (subtitleCodecIsSoftFetchable(s.codec) || subtitleCodecIsText(s.codec))
            return true;
    }
    return false;
}

// #217: OpenSubtitles is queried (cache or network) only when the play has no
// text subtitle of its own: no PMS sidecar srt/ass/vtt and no embedded text
// stream for this part. An image-only catalog (PGS/VobSub) still qualifies.
// An explicit Off (pendingSubtitleId == 0) never queries.
inline bool shouldQueryOpenSubs(const StreamCatalog& cat, int partId, bool subtitlesOff, int pendingSubtitleId) {
#ifndef MUT_OPENSUBS_IGNORE_TEXT_SUB
    if (catalogHasTextSubtitle(cat, partId))
        return false;
#else
    (void)cat;
    (void)partId;
#endif
    if (subtitlesOff && pendingSubtitleId == 0)
        return false;
    return true;
}

inline std::string cacheDir() { return "/media/fat/misterplex/subs"; }

inline std::string cachePath(const std::string& ratingKey, const std::string& imdb, const std::string& lang) {
    std::string stem;
    if (!ratingKey.empty()) {
        stem = ratingKey;
    } else if (!imdb.empty()) {
        stem = imdb;
    } else {
        return {};
    }
    for (unsigned char c : stem) {
        if (!(std::isalnum(c) || c == '-' || c == '_'))
            return {};
    }
    std::string l = lang.empty() ? "en" : lang;
    for (char& c : l)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l.size() < 2 || l.size() > 3)
        l = "en";
    return cacheDir() + "/" + stem + "-" + l + ".srt";
}

inline bool cacheReadable(const std::string& path) {
    if (path.empty())
        return false;
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

inline bool ensureCacheDir() {
    struct stat st {};
    if (::stat(cacheDir().c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    ::mkdir("/media/fat/misterplex", 0755);
    return ::mkdir(cacheDir().c_str(), 0755) == 0 || errno == EEXIST;
}

// Atomic write: tmp + fsync + rename. Any failure unlinks the tmp (no truncated .srt left on FAT).
inline bool writeCacheFile(const std::string& path, const std::string& body) {
    if (path.empty() || body.empty())
        return false;
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    size_t off = 0;
    bool ok = true;
    while (off < body.size()) {
        const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) { ok = false; break; }
        off += static_cast<size_t>(n);
    }
    if (ok && ::fsync(fd) != 0)
        ok = false;
    if (::close(fd) != 0)
        ok = false;
    if (ok && ::rename(tmp.c_str(), path.c_str()) == 0)
        return true;
    ::unlink(tmp.c_str());
    return false;
}

// Cache cap (#221 review nit 2): ~50 MB total and 500 files, oldest mtime evicted first.
constexpr uint64_t kCacheMaxBytes = 50ull * 1024 * 1024;
constexpr size_t kCacheMaxFiles = 500;
constexpr const char* kMaxSrtBytesArg = "2M";   // curl --max-filesize for the CDN body

struct CacheEntry {
    std::string path;
    int64_t mtime = 0;
    uint64_t size = 0;
};

// Pure: entries to delete so the rest fit maxBytes and maxFiles. Oldest mtime first (ties: path).
inline std::vector<std::string> pickEvictions(std::vector<CacheEntry> e, uint64_t maxBytes, size_t maxFiles) {
    std::sort(e.begin(), e.end(), [](const CacheEntry& a, const CacheEntry& b) {
#ifndef MUT_OPENSUBS_EVICT_NEWEST
        return a.mtime != b.mtime ? a.mtime < b.mtime : a.path < b.path;
#else
        return a.mtime != b.mtime ? a.mtime > b.mtime : a.path > b.path;
#endif
    });
    uint64_t total = 0;
    for (const auto& x : e)
        total += x.size;
    size_t count = e.size();
    std::vector<std::string> out;
    for (const auto& x : e) {
        if (total <= maxBytes && count <= maxFiles)
            break;
        out.push_back(x.path);
        total -= x.size;
        --count;
    }
    return out;
}
// Scan dir for *.srt (stale *.srt.tmp always removed) and evict down to the cap. Returns files removed.
inline size_t evictCacheDir(const std::string& dir, uint64_t maxBytes, size_t maxFiles) {
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return 0;
    std::vector<CacheEntry> e;
    size_t removed = 0;
    while (dirent* de = ::readdir(d)) {
        const std::string n = de->d_name;
        const std::string full = dir + "/" + n;
        auto ends = [&](const std::string& suf) {
            return n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0;
        };
        struct stat st {};
        if (::lstat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (ends(".srt.tmp")) {
            if (::unlink(full.c_str()) == 0)
                ++removed;
        } else if (ends(".srt")) {
            e.push_back({full, static_cast<int64_t>(st.st_mtime), static_cast<uint64_t>(st.st_size)});
        }
    }
    ::closedir(d);
    for (const auto& p : pickEvictions(std::move(e), maxBytes, maxFiles))
        if (::unlink(p.c_str()) == 0)
            ++removed;
    return removed;
}

inline bool writeCache(const std::string& path, const std::string& body) {
    if (path.empty() || body.empty() || !ensureCacheDir())
        return false;
    if (!writeCacheFile(path, body))
        return false;
    evictCacheDir(cacheDir(), kCacheMaxBytes, kCacheMaxFiles);
    return true;
}

// curl -K config text for request headers (#221 / #253): Api-Key and Bearer go through -K (stdin or
// /dev/fd), never argv (visible in ps). Lines with CR/LF/NUL are dropped (no header injection).
inline std::string curlHeaderConfig(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string out;
    for (const auto& kv : headers) {
        const std::string line = kv.first + ": " + kv.second;
        bool bad = kv.first.empty();
        for (char c : line)
            if (c == '\r' || c == '\n' || c == '\0')
                bad = true;
        if (bad)
            continue;
        out += "header = \"";
        for (char c : line) {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        out += "\"\n";
    }
    return out;
}

// argv for OpenSubtitles REST headers: only "-K" and a path ("-" = stdin, or /dev/fd/N). Secrets live
// in the config bytes, never on argv. cfgPath is ignored under MUT_OPENSUBS_KEY_ON_ARGV (old -H leak).
inline std::vector<std::string> opensubsHeaderArgs(const std::vector<std::pair<std::string, std::string>>& headers,
                                                   const std::string& cfgPath = "-") {
#ifndef MUT_OPENSUBS_KEY_ON_ARGV
    (void)headers;
    return {"-K", cfgPath};
#else
    (void)cfgPath;
    std::vector<std::string> a;
    for (const auto& kv : headers)
        a.insert(a.end(), {"-H", kv.first + ": " + kv.second});
    return a;
#endif
}

inline bool argvContains(const std::vector<std::string>& a, const std::string& needle) {
    if (needle.empty())
        return false;
    for (const auto& s : a)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}
// CDN link from /download JSON (nit 3): https only, host opensubtitles.com/.org or a subdomain, no userinfo/port.
inline bool subsLinkAllowed(const std::string& link) {
    const std::string pre = "https://";
    if (link.compare(0, pre.size(), pre) != 0)
        return false;
    const size_t e = link.find_first_of("/?#", pre.size());
    std::string host = link.substr(pre.size(), e == std::string::npos ? std::string::npos : e - pre.size());
    if (host.empty() || host.find_first_of("@:\\ ") != std::string::npos)
        return false;
    for (char& c : host)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const std::string dom : {"opensubtitles.com", "opensubtitles.org"}) {
        if (host == dom)
            return true;
        const std::string sub = "." + dom;
        if (host.size() > sub.size() && host.compare(host.size() - sub.size(), sub.size(), sub) == 0)
            return true;
    }
    return false;
}

// 406 backoff (nit 4): honor "reset_time_utc" ("YYYY-MM-DDTHH:MM:SS...") when present, else 24 h.
// Clamped to [now+60 s, now+25 h].
inline int64_t quotaBackoffUntil(const std::string& json, int64_t now) {
    int64_t until = now + 24 * 3600;
    const auto k = json.find("\"reset_time_utc\"");
    if (k != std::string::npos) {
        const auto colon = json.find(':', k);
        const auto q1 = colon == std::string::npos ? colon : json.find('"', colon + 1);
        int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
        if (q1 != std::string::npos &&
            std::sscanf(json.c_str() + q1 + 1, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &s) == 6) {
            struct tm t {};
            t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
            t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
            const int64_t r = static_cast<int64_t>(::timegm(&t));
            if (r > 0)
                until = r;
        }
    }
    return std::min(std::max(until, now + 60), now + 25 * 3600);
}

struct SearchHit {
    int64_t fileId = 0;
    std::string language;
};

// Minimal JSON dig for "file_id": <int> inside the first data[] entry.
inline std::vector<SearchHit> parseSearchHits(const std::string& json) {
    std::vector<SearchHit> hits;
    size_t sp = 0;
    while (sp < json.size()) {
        const auto key = json.find("\"file_id\"", sp);
        if (key == std::string::npos)
            break;
        size_t colon = json.find(':', key);
        if (colon == std::string::npos)
            break;
        size_t i = colon + 1;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])))
            ++i;
        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i]))) {
            sp = key + 8;
            continue;
        }
        int64_t id = 0;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) {
            id = id * 10 + (json[i] - '0');
            ++i;
        }
        if (id > 0) {
            SearchHit h;
            h.fileId = id;
            hits.push_back(h);
        }
        sp = i;
    }
    return hits;
}

inline std::string parseDownloadLink(const std::string& json) {
    const auto key = json.find("\"link\"");
    if (key == std::string::npos)
        return {};
    const auto colon = json.find(':', key);
    if (colon == std::string::npos)
        return {};
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos)
        return {};
    const auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1)
        return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

inline std::string parseLoginToken(const std::string& json) {
    const auto key = json.find("\"token\"");
    if (key == std::string::npos)
        return {};
    const auto colon = json.find(':', key);
    if (colon == std::string::npos)
        return {};
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos)
        return {};
    const auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1)
        return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

inline std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            o.push_back(static_cast<char>(c));
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 0x0f]);
        }
    }
    return o;
}

inline std::string buildSearchUrl(const Guids& g, const std::string& lang) {
    std::string url = "https://api.opensubtitles.com/api/v1/subtitles?languages=";
    url += pctEncode(lang.empty() ? "en" : lang);
    if (!g.imdb.empty()) {
        url += "&imdb_id=" + pctEncode(g.imdb);
    } else if (!g.tmdb.empty()) {
        url += "&tmdb_id=" + pctEncode(g.tmdb);
    } else if (!g.title.empty()) {
        url += "&query=" + pctEncode(g.title);
        if (!g.year.empty())
            url += "&year=" + pctEncode(g.year);
    } else {
        return {};
    }
    return url;
}

// Username + password are optional. Both present: /login for a Bearer (higher quota). Either missing: skip
// /login and download anonymously with the Api-Key alone (5/day per IP when the key allows anonymous downloads).
inline bool shouldLogin(const std::string& user, const std::string& pass) {
#ifdef MUT_OPENSUBS_LOGIN_ANON
    (void)user;
    (void)pass;
    return true;
#else
    return !user.empty() && !pass.empty();
#endif
}

enum class DownloadStatus { Ok, QuotaExceeded, Error };

// /download: 200 + "link" = Ok. 406 = download quota used up (anonymous 5/day per IP, or the account limit).
inline DownloadStatus classifyDownload(int httpCode, const std::string& json) {
#ifndef MUT_OPENSUBS_406_AS_ERROR
    if (httpCode == 406)
        return DownloadStatus::QuotaExceeded;
#endif
    if (httpCode == 200 && !parseDownloadLink(json).empty())
        return DownloadStatus::Ok;
    return DownloadStatus::Error;
}

inline std::string downloadBodyJson(int64_t fileId) {
    return std::string("{\"file_id\":") + std::to_string(fileId) + "}";
}

inline std::string loginBodyJson(const std::string& user, const std::string& pass) {
    // Values are not escaped beyond basic quote strip — callers pass conf values.
    auto scrub = [](std::string s) {
        for (char& c : s) {
            if (c == '"' || c == '\\' || c == '\n' || c == '\r')
                c = ' ';
        }
        return s;
    };
    return std::string("{\"username\":\"") + scrub(user) + "\",\"password\":\"" + scrub(pass) + "\"}";
}

}  // namespace opensubs
}  // namespace misterplex
