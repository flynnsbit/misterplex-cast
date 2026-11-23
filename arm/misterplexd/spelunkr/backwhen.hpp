#pragma once
// BackWhenTV (https://backwhen.stream/tv) as a Spelunkr guide section.
//
// Public APIs only: /api/lineup and /api/schedules. The official web player
// streams Internet Archive files (and a few first-party backwhen.stream MP4s)
// with no login. This code does the same: it does not re-host video, does not
// spoof Origin, and does not play Pluto. Pluto live streams are listed on the
// official apps only (the /tv page excludes them) and arrive via a third-party
// HLS proxy; they are flagged UNSUPPORTED rather than tuned.
//
// H.264 goes to the existing MisterPlex H.264 core. Non-H.264, audio-only
// (the WeatherStar bed), over-cap resolution (above kRemoteDirectMaxMbs, so
// 1080p), and too-high fps (mbs*fps above kRemoteDirectMaxMbps, or fps>30)
// are flagged and never transcoded or downscaled.

#include "spelunkr/broadcast.hpp"
#include "spelunkr/schedule.hpp"
#include "libmisterplex/pluto_live.hpp"
#include "libmisterplex/spelunkr_sections.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {
namespace spelunkr {

inline constexpr const char* kBackWhenOrigin = "https://backwhen.stream";
inline constexpr const char* kBackWhenLineupUrl = "https://backwhen.stream/api/lineup";
inline constexpr const char* kBackWhenSchedulesUrl = "https://backwhen.stream/api/schedules?pods=1&hours=6";

// Official Back When numbers start at 1 and collide with Spelunkr dials.
// Dial 1000+official is unique and deterministic (official CH 8 -> 1008).
inline constexpr int kBackWhenDialBase = 1000;
inline constexpr int kBackWhenFetchTimeoutSec = 5;
inline constexpr int kBackWhenRetrySec = 30;
inline constexpr int kBackWhenRetryMaxSec = 600;  // cap exponential backoff at 10 min
inline constexpr int64_t kBackWhenMaxHorizonSec = 7 * 24 * 3600;
inline constexpr int64_t kBackWhenMaxSleepSec = 24 * 3600;
inline constexpr int kBackWhenRefreshLeadSec = 600;  // refresh 10 min before valid_until
inline constexpr size_t kBackWhenMaxBody = 4u << 20;

inline int backWhenDial(int officialNum) {
    if (officialNum <= 0 || officialNum > std::numeric_limits<int>::max() - kBackWhenDialBase)
        return 0;
    return kBackWhenDialBase + officialNum;
}

// Exponential backoff for a short retry: 30, 60, 120, ... capped at 10 min.
// failStreak 0 or 1 uses the base delay.
inline int64_t backWhenRetryDelaySec(int failStreak) {
    const int n = failStreak < 1 ? 1 : failStreak;
    int64_t d = kBackWhenRetrySec;
    for (int i = 1; i < n; ++i) {
        if (d > static_cast<int64_t>(kBackWhenRetryMaxSec) / 2)
            return kBackWhenRetryMaxSec;
        d *= 2;
        if (d >= kBackWhenRetryMaxSec)
            return kBackWhenRetryMaxSec;
    }
    return d;
}

inline int64_t addUnixClamped(int64_t a, int64_t b) {
    int64_t o = 0;
    if (!addInt64(a, b, o))
        return std::numeric_limits<int64_t>::max();
    return o;
}

// True when the next wake should be a short retry (failed fetch, or horizon already due).
inline bool backWhenNeedsRetry(int64_t validUntil, int64_t nowSec, bool lastOk) {
    if (!lastOk)
        return true;
    if (validUntil <= 0)
        return false;
    int64_t at = 0;
    if (!addInt64(validUntil, -static_cast<int64_t>(kBackWhenRefreshLeadSec), at) || at <= nowSec)
        return true;
    return false;
}

// Seconds to sleep until nextUnix, always in [1, kBackWhenMaxSleepSec].
inline int64_t backWhenSleepSec(int64_t nextUnix, int64_t nowSec) {
    if (nowSec < 0)
        nowSec = 0;
    int64_t s = 0;
    if (nextUnix <= nowSec || !addInt64(nextUnix, -nowSec, s) || s < 1)
        return 1;
    if (s > kBackWhenMaxSleepSec)
        return kBackWhenMaxSleepSec;
    return s;
}

// Next unix-second to fetch. lastOk=false -> short retry with backoff.
// validUntil<=0 -> hourly. Far-future valid_until is clamped so sleep math
// cannot overflow steady_clock (nanoseconds).
inline int64_t backWhenNextFetchUnix(int64_t validUntil, int64_t nowSec, bool lastOk,
                                    int failStreak = 0) {
    if (nowSec < 0)
        nowSec = 0;
    const int64_t retryAt = addUnixClamped(nowSec, backWhenRetryDelaySec(failStreak));
    if (!lastOk)
        return retryAt;
    if (validUntil <= 0)
        return addUnixClamped(nowSec, 3600);
    const int64_t maxVu = addUnixClamped(nowSec, kBackWhenMaxHorizonSec);
    if (validUntil > maxVu)
        validUntil = maxVu;
    int64_t at = 0;
    if (!addInt64(validUntil, -static_cast<int64_t>(kBackWhenRefreshLeadSec), at))
        return retryAt;
    if (at <= nowSec)
        return retryAt;
    const int64_t maxWake = addUnixClamped(nowSec, kBackWhenMaxSleepSec);
    if (at > maxWake)
        return maxWake;
    return at;
}

inline std::string lowerAscii(const char* e) {
    std::string s = e ? e : "";
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline bool backwhenHostAllowed(const std::string& url) {
    if (url.rfind("https://backwhen.stream/", 0) == 0)
        return true;
    if (url.rfind("https://www.backwhen.stream/", 0) == 0)
        return true;
    return false;
}

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

inline std::string percentDecode(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexNibble(s[i + 1]);
            const int lo = hexNibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                o += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        o += s[i];
    }
    return o;
}

inline std::string urlPath(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return {};
    const auto slash = url.find('/', scheme + 3);
    if (slash == std::string::npos)
        return {};
    std::string p = url.substr(slash);
    const auto q = p.find('?');
    if (q != std::string::npos)
        p.resize(q);
    const auto h = p.find('#');
    if (h != std::string::npos)
        p.resize(h);
    return p;
}

// archive.org/download/{id}/{file} or *.archive.org/.../items/{id}/{file}
inline bool parseArchiveItemUrl(const std::string& url, std::string& id, std::string& file) {
    id.clear();
    file.clear();
    const std::string h = urlHost(url);
    if (h != "archive.org" && h != "www.archive.org") {
        const char* suf = ".archive.org";
        const size_t n = std::strlen(suf);
        if (!(h.size() > n && h.compare(h.size() - n, n, suf) == 0))
            return false;
    }
    const std::string path = urlPath(url);
    const char* keys[] = {"/download/", "/items/"};
    size_t pos = std::string::npos;
    size_t keyLen = 0;
    for (const char* k : keys) {
        const auto p = path.find(k);
        if (p != std::string::npos) {
            pos = p;
            keyLen = std::strlen(k);
            break;
        }
    }
    if (pos == std::string::npos)
        return false;
    std::string rest = path.substr(pos + keyLen);
    while (!rest.empty() && rest[0] == '/')
        rest.erase(rest.begin());
    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size())
        return false;
    id = percentDecode(rest.substr(0, slash));
    file = percentDecode(rest.substr(slash + 1));
    if (id.empty() || file.empty() || id.find('/') != std::string::npos)
        return false;
    return true;
}

inline bool jsonTruthy(const airplay::Json* j) {
    if (!j)
        return false;
    if (j->type == airplay::Json::Bool)
        return j->b;
    if (j->type == airplay::Json::Num)
        return jsonInt(j, 0) != 0;
    if (j->type == airplay::Json::Str) {
        const std::string s = jsonStr(j);
        return s == "1" || s == "true" || s == "TRUE";
    }
    return false;
}

inline bool titleLooksLikeAd(const std::string& t) {
    std::string u;
    u.reserve(t.size());
    for (unsigned char c : t)
        u += static_cast<char>(std::toupper(c));
    return u.rfind("COMMERCIAL BREAK", 0) == 0;
}

struct BackWhenChannel {
    std::string id;
    std::string name;
    int num = 0;
    std::string kind;
    std::string cat;
    std::string blurb;
    std::string pluto;
    bool pods = false;
};

struct BackWhenLineup {
    int version = 0;
    int64_t anchor = 0;
    std::vector<BackWhenChannel> channels;
};

struct BackWhenItem {
    std::string title;
    std::string url;
    std::string fallback;
    int durationSec = 0;
    int64_t startUnix = 0;
    int64_t endUnix = 0;
    bool ad = false;
};

struct BackWhenSchedule {
    std::string id;
    std::vector<BackWhenItem> items;
};

inline bool parseBackWhenChannel(const airplay::Json& j, BackWhenChannel& o) {
    if (j.type != airplay::Json::Obj)
        return false;
    o.id = jsonStr(j.get("id"));
    o.name = jsonStr(j.get("name"));
    const int64_t num = jsonInt(j.get("num"));
    if (num <= 0 || num > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return false;
    o.num = static_cast<int>(num);
    o.kind = jsonStr(j.get("kind"));
    o.cat = jsonStr(j.get("cat"));
    o.blurb = jsonStr(j.get("blurb"));
    o.pluto = jsonStr(j.get("pluto"));
    o.pods = jsonTruthy(j.get("pods"));
    return !o.id.empty();
}

inline bool parseBackWhenLineup(const std::string& body, BackWhenLineup& out) {
    out = BackWhenLineup();
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj)
        return false;
    out.version = static_cast<int>(jsonInt(root.get("version")));
    out.anchor = jsonInt(root.get("anchor"));
    const airplay::Json* ch = root.get("channels");
    if (!ch || ch->type != airplay::Json::Arr)
        return false;
    for (const auto& c : ch->arr) {
        BackWhenChannel chn;
        if (parseBackWhenChannel(c, chn))
            out.channels.push_back(std::move(chn));
    }
    return !out.channels.empty();
}

inline bool parseBackWhenItem(const airplay::Json& j, BackWhenItem& o) {
    if (j.type != airplay::Json::Obj)
        return false;
    o.title = jsonStr(j.get("t"));
    o.url = jsonStr(j.get("u"));
    o.fallback = jsonStr(j.get("f"));
    const int64_t d = jsonInt(j.get("d"));
    if (d <= 0 || d > static_cast<int64_t>(std::numeric_limits<int>::max()))
        return false;
    o.durationSec = static_cast<int>(d);
    o.startUnix = jsonInt(j.get("s"));
    o.endUnix = jsonInt(j.get("e"));
    o.ad = jsonTruthy(j.get("a")) || titleLooksLikeAd(o.title);
    if (o.startUnix < 0 || (o.startUnix <= 0 && o.url.empty()))
        return false;
    if (o.startUnix > 0) {
        if (o.startUnix > kMaxSecRepresentableAsMs)
            return false;
        if (o.endUnix != 0) {
            if (o.endUnix <= o.startUnix || o.endUnix > kMaxSecRepresentableAsMs)
                return false;
            int64_t dummy = 0;
            if (!secToMs(o.endUnix - o.startUnix, dummy))
                return false;
        } else {
            int64_t end = 0;
            if (!addInt64(o.startUnix, static_cast<int64_t>(o.durationSec), end) ||
                end <= o.startUnix || end > kMaxSecRepresentableAsMs)
                return false;
        }
    }
    return true;
}

inline bool parseBackWhenSchedules(const std::string& body, std::vector<BackWhenSchedule>& out,
                                  int64_t* validUntil = nullptr) {
    out.clear();
    if (validUntil)
        *validUntil = 0;
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj)
        return false;
    auto clampVu = [](int64_t v) -> int64_t {
        if (v <= 0 || v > kMaxSecRepresentableAsMs)
            return 0;
        return v;
    };
    if (validUntil) {
        *validUntil = clampVu(jsonInt(root.get("valid_until")));
    }
    const airplay::Json* sch = root.get("schedules");
    if (!sch || sch->type != airplay::Json::Obj)
        return false;
    for (const auto& kv : sch->obj) {
        if (kv.second.type != airplay::Json::Obj)
            continue;
        BackWhenSchedule sc;
        sc.id = kv.first;
        const std::string idField = jsonStr(kv.second.get("id"));
        if (!idField.empty())
            sc.id = idField;
        if (validUntil) {
            const int64_t sv = jsonInt(kv.second.get("valid_until"));
            if (sv > 0 && sv <= kMaxSecRepresentableAsMs && (*validUntil <= 0 || sv < *validUntil))
                *validUntil = sv;
        }
        const airplay::Json* items = kv.second.get("items");
        if (!items || items->type != airplay::Json::Arr)
            continue;
        for (const auto& it : items->arr) {
            BackWhenItem item;
            if (parseBackWhenItem(it, item))
                sc.items.push_back(std::move(item));
        }
        if (!sc.id.empty())
            out.push_back(std::move(sc));
    }
    return !out.empty();
}

inline const BackWhenSchedule* findBackWhenSchedule(const std::vector<BackWhenSchedule>& S, const std::string& id) {
    for (const auto& s : S)
        if (s.id == id)
            return &s;
    return nullptr;
}

inline Item flaggedPlaceholder(const std::string& title, const std::string& reason) {
    Item it;
    it.title = title;
    it.durationSec = 3600;
    it.startUnix = 1;
    it.endUnix = kMaxSecRepresentableAsMs;
    it.playable = false;
    it.flag = reason;
    return it;
}

inline Item backWhenShowItem(const BackWhenItem& src) {
    Item it;
    it.title = src.title;
    it.durationSec = src.durationSec;
    it.startUnix = src.startUnix;
    it.endUnix = src.endUnix;
    if (it.endUnix <= 0) {
        int64_t end = 0;
        if (addInt64(src.startUnix, static_cast<int64_t>(src.durationSec), end) &&
            end > src.startUnix && end <= kMaxSecRepresentableAsMs)
            it.endUnix = end;
    }
    it.directUrl = src.url;
    it.fallbackUrl = src.fallback;
    it.ad = src.ad;
    it.playable = true;
    std::string id, file;
    if (parseArchiveItemUrl(src.url, id, file) || parseArchiveItemUrl(src.fallback, id, file)) {
        it.id = id;
        it.file = file;
    }
    return it;
}

// Pluto / board / weather get a single flagged placeholder (not played).
// Ads and COMMERCIAL BREAK titles are omitted (video-only, like Spelunkr bumps).
inline bool buildBackWhenSection(const BackWhenLineup& lu, const std::vector<BackWhenSchedule>& schedules,
                                 Lineup& out, std::vector<Manifest>& mans) {
    out = Lineup();
    mans.clear();
    for (const auto& src : lu.channels) {
        Channel ch;
        ch.id = src.id;
        ch.name = src.name;
        ch.dial = backWhenDial(src.num);
        ch.family = src.cat;
        ch.section = kBackWhenSection;
        ch.kind = src.kind;
        Manifest man;
        man.channel = src.id;
        man.absoluteClock = true;
        if (src.kind == "pluto") {
#ifdef MUT_PLUTO_DEFAULT_ON
            const bool plutoOn = true;
#else
            const bool plutoOn = misterplex::plutoLiveEnabled();
#endif
            if (!plutoOn) {
                man.items.push_back(flaggedPlaceholder(
                    "UNSUPPORTED: Pluto",
                    "UNSUPPORTED: Pluto live streams are not on backwhen.stream/tv"));
            } else if (src.pluto.empty()) {
                man.items.push_back(flaggedPlaceholder("UNSUPPORTED: Pluto", "UNSUPPORTED: Pluto missing channel id"));
            } else {
                Item it;
                it.title = src.name.empty() ? std::string("Pluto") : src.name;
                it.id = "pluto";
                it.file = src.pluto;
                it.playable = true;
                it.durationSec = 86400;
                it.startUnix = 1;
                it.endUnix = kMaxSecRepresentableAsMs;
                man.items.push_back(std::move(it));
            }
        } else if (src.kind == "board") {
            man.items.push_back(flaggedPlaceholder("COMMUNITY BOARD", "UNSUPPORTED: not a stream"));
        } else if (src.kind == "weather") {
            man.items.push_back(flaggedPlaceholder(
                "Local forecast", "UNSUPPORTED: audio-only WeatherStar bed"));
        } else if (src.kind == "ia") {
            const BackWhenSchedule* sc = findBackWhenSchedule(schedules, src.id);
            if (sc) {
                for (const auto& it : sc->items) {
                    if (it.ad)
                        continue;
                    man.items.push_back(backWhenShowItem(it));
                }
            }
            if (man.items.empty()) {
                man.items.push_back(flaggedPlaceholder("NO PROGRAMME", "UNSUPPORTED: no playable schedule"));
            }
        } else {
            man.items.push_back(flaggedPlaceholder("UNSUPPORTED", "UNSUPPORTED: unknown channel kind"));
        }
        VersionPtr ver;
        ver.v = 1;
        ver.manifest = "backwhen:" + src.id;  // preloaded; never fetched
        ver.epochMs = 1;
        if (!man.items.empty()) {
            int64_t epoch = 0;
            const int64_t st = std::max<int64_t>(1, man.items[0].startUnix);
            if (secToMs(st, epoch))
                ver.epochMs = epoch;
        }
        ver.startIndex = 0;
        ch.versions.push_back(ver);
        out.channels.push_back(std::move(ch));
        mans.push_back(std::move(man));
    }
    return !out.channels.empty();
}

// Drop items whose window has ended. Invalid windows on playable items are
// dropped too. If nothing remains, leave a single unplayable placeholder so
// the channel does not loop a stale first entry.
inline int pruneExpiredBackWhenItems(Manifest& man, int64_t nowSec) {
    if (nowSec <= 0 || !manifestIsAbsolute(man))
        return 0;
    std::vector<Item> kept;
    kept.reserve(man.items.size());
    for (auto& it : man.items) {
        if (!it.playable) {
            kept.push_back(std::move(it));
            continue;
        }
        int64_t st = 0, en = 0;
        if (!itemWindowSec(it, st, en) || en <= nowSec)
            continue;
        kept.push_back(std::move(it));
    }
    const int dropped = static_cast<int>(man.items.size()) - static_cast<int>(kept.size());
    man.items = std::move(kept);
    if (man.items.empty())
        man.items.push_back(flaggedPlaceholder("NO PROGRAMME", "UNSUPPORTED: schedule expired"));
    return dropped;
}

inline bool loadBackWhenSection(const std::string& lineupBody, const std::string& schedBody,
                               Lineup& out, std::vector<Manifest>& mans, int64_t nowSec = 0,
                               int64_t* validUntil = nullptr) {
    BackWhenLineup lu;
    std::vector<BackWhenSchedule> sch;
    if (!parseBackWhenLineup(lineupBody, lu))
        return false;
    parseBackWhenSchedules(schedBody, sch, validUntil);
    if (!buildBackWhenSection(lu, sch, out, mans))
        return false;
    if (nowSec > 0) {
        for (auto& man : mans)
            pruneExpiredBackWhenItems(man, nowSec);
    }
    return true;
}

// ---- Service-switch speed: on-disk copy of the last good Back When section ----
// A Plex->Spelunkr service switch restarts the daemon and used to block the
// last-channel resume on two live TLS GETs (lineup + schedules, ~3 s on the
// MiSTer). The bodies of the last good fetch are persisted next to the header
// cache; a restart loads them from disk, resumes at once, and the normal live
// fetch refreshes the section in the background. A cache is used only while
// its own valid_until is still in the future, so an expired schedule is never
// tuned from disk. Public data only; nothing here is secret.
inline std::string backWhenCachePath() {
    if (const char* e = std::getenv("MPX_SPELUNKR_BW_CACHE"); e && *e)
        return e;
    return "/media/fat/linux/misterplex-cache/spelunkr-backwhen.cache";
}

inline constexpr const char* kBackWhenCacheMagic = "MPXBW1";

// valid_until must be known and still ahead of now.
inline bool backWhenCacheFresh(int64_t validUntil, int64_t nowSec) {
    return validUntil > 0 && nowSec > 0 && validUntil > nowSec;
}

inline bool writeBackWhenCache(const std::string& path, const std::string& lineupBody,
                               const std::string& schedBody) {
    if (path.empty() || lineupBody.empty() || schedBody.empty() ||
        lineupBody.size() > kBackWhenMaxBody || schedBody.size() > kBackWhenMaxBody)
        return false;
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = path.substr(0, slash);
        // One missing level (the misterplex-cache dir) is created; deeper is not our business.
        ::mkdir(dir.c_str(), 0755);
    }
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const int hdr = std::fprintf(f, "%s %zu %zu\n", kBackWhenCacheMagic, lineupBody.size(), schedBody.size());
    bool ok = hdr > 0 && std::fwrite(lineupBody.data(), 1, lineupBody.size(), f) == lineupBody.size() &&
              std::fwrite(schedBody.data(), 1, schedBody.size(), f) == schedBody.size();
    ok = (std::fflush(f) == 0) && ok;
    if (ok)
        ::fsync(::fileno(f));
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

// Reads and validates the framing only; the caller parses the bodies.
inline bool readBackWhenCache(const std::string& path, std::string& lineupBody, std::string& schedBody) {
    lineupBody.clear();
    schedBody.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[16] = {0};
    size_t n1 = 0, n2 = 0;
    char nl = 0;
    bool ok = std::fscanf(f, "%15s %zu %zu%c", magic, &n1, &n2, &nl) == 4 && nl == '\n' &&
              std::strcmp(magic, kBackWhenCacheMagic) == 0 && n1 > 0 && n2 > 0 && n1 <= kBackWhenMaxBody &&
              n2 <= kBackWhenMaxBody;
    if (ok) {
        std::string a(n1, '\0'), b(n2, '\0');
        ok = std::fread(&a[0], 1, n1, f) == n1 && std::fread(&b[0], 1, n2, f) == n2 &&
             std::fgetc(f) == EOF;  // exact length: a truncated or padded file is rejected
        if (ok) {
            lineupBody = std::move(a);
            schedBody = std::move(b);
        }
    }
    std::fclose(f);
    return ok;
}

// Disk cache -> section, only when still fresh. False leaves every output untouched.
inline bool loadBackWhenDiskCache(const std::string& path, int64_t nowSec, Lineup& out,
                                  std::vector<Manifest>& mans, int64_t* validUntil = nullptr) {
    std::string lu, sc;
    if (!readBackWhenCache(path, lu, sc))
        return false;
    Lineup tmp;
    std::vector<Manifest> tmpMans;
    int64_t vu = 0;
    if (!loadBackWhenSection(lu, sc, tmp, tmpMans, nowSec, &vu) || !backWhenCacheFresh(vu, nowSec))
        return false;
    out = std::move(tmp);
    mans = std::move(tmpMans);
    if (validUntil)
        *validUntil = vu;
    return true;
}

// SPELUNKR_BACKWHEN defaults on when SPELUNKR is on; 0/off/false/no disables
// (ASCII case-insensitive, so "Off" and "FALSE" actually disable it).
inline bool backWhenSectionWanted() {
    const char* e = std::getenv("SPELUNKR_BACKWHEN");
    if (!e || !*e)
        return true;
    const std::string v = lowerAscii(e);
    if (v == "0" || v == "n" || v == "no" || v == "f" || v == "false" || v == "off")
        return false;
    return true;
}

}  // namespace spelunkr
}  // namespace misterplex
