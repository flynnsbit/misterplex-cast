#pragma once
// BURN_SUBS_FIX (conf) / MPX_BURN_SUBS_FIX (env wins). Default off.
//
// Off: play URLs match tip 890ec6a6. A picked SRT/ASS/SSA stays off the
// universal URL (no subtitles=burn); the soft overlay paints them. mov_text
// and image picks that already burn still leave directPlay=1 on the PlexOB
// ladder, so PMS does not burn.
//
// On: a picked subtitle the overlay will not paint (PGS/VobSub/image, and
// mov_text when SOFT_SUBS is off) is subtitles=burn plus that subtitleStreamID
// on the universal start URL and on the decision URL, and directPlay=0 /
// directStream=0. PMS burns only during a video transcode. Soft text the
// overlay paints is never burned. REMEMBER_TRACKS and FORCED_SUBS picks use
// the same rule. CHAPTERS does not touch the URL.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace misterplex {

inline bool burnSubsFixEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// N6: a legacy conf line BURN_SUBS_FIX=1 (also on/yes/true) is an explicit opt-in,
// so an upgraded box keeps burning. Unset, empty, and 0 are off. The shipped
// example leaves the key commented, so a new conf stays off. Env is separate
// and wins when the caller has already set it.
inline bool burnSubsFixConfOptIn(const char* confValue) {
#ifdef MUT_FU14_LEGACY_CONF_IGNORED
    (void)confValue;
    return false;
#else
    return burnSubsFixEnvOn(confValue);
#endif
}

// Default off. MPX_BURN_SUBS_FIX wins over BURN_SUBS_FIX when it is set.
inline bool burnSubsFixEnabled() {
#ifdef MUT_BURN_SUBS_FIX_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_BURN_SUBS_FIX"); e && e[0])
        return burnSubsFixEnvOn(e);
    return burnSubsFixEnvOn(std::getenv("BURN_SUBS_FIX"));
#endif
}

// AUTO_BURN (conf) / MPX_AUTO_BURN (env wins over AUTO_BURN). Truth table, env
// and conf use the same keys once main.cpp has copied an unset env from conf:
//   explicit AUTO_BURN=1                         burn ON
//   explicit AUTO_BURN=0                         burn OFF (wins over legacy)
//   AUTO_BURN unset and BURN_SUBS_FIX=1          burn ON (legacy opt-in)
//   neither set                                  burn OFF
// An explicit 0 is a set value, not "unset". Empty is unset.
inline bool autoBurnEnabled() {
#ifdef MUT_AUTO_BURN_DEFAULT_ON
    return true;
#else
    auto explicitOn = [](const char* e, bool* present) -> bool {
        if (!e || !e[0]) {
            *present = false;
            return false;
        }
        *present = true;
        return burnSubsFixEnvOn(e);
    };
    bool present = false;
    if (const char* e = std::getenv("MPX_AUTO_BURN")) {
        const bool on = explicitOn(e, &present);
        if (present) {
#ifdef MUT_AUTO0_NOT_WINNING
            if (!on && burnSubsFixEnabled())
                return true;
#endif
#ifdef MUT_AUTO1_IGNORED
            if (on)
                return false;
#endif
            return on;
        }
    }
    if (const char* e = std::getenv("AUTO_BURN")) {
        const bool on = explicitOn(e, &present);
        if (present) {
#ifdef MUT_AUTO0_NOT_WINNING
            if (!on && burnSubsFixEnabled())
                return true;
#endif
#ifdef MUT_AUTO1_IGNORED
            if (on)
                return false;
#endif
            return on;
        }
    }
#if defined(MUT_NEITHER_MEANS_ON)
    return true;
#elif defined(MUT_LEGACY_IGNORED_WHEN_AUTO_UNSET)
    return false;
#else
    return burnSubsFixEnabled();
#endif
#endif
}

// A burn on the wire must be a video transcode. directPlay=1 lets PMS
// return the source and skip the burn.
inline bool burnSubsFixForcesTranscode(bool burn) {
#ifdef MUT_BURN_SUBS_FIX_KEEP_DIRECT
    (void)burn;
    return false;
#else
    return burnSubsFixEnabled() && burn;
#endif
}

// #324/#329: the burn subtitle part PUT (PUT /library/parts/N?subtitleStreamID=) changes the
// server's stored selection for every user of that media, so it runs only when
//   1. BURN_SUBS_FIX is explicitly on (opt-in, default off), and
//   2. the play server's MACHINE ID (its /identity machineIdentifier) is the clientIdentifier
//      of a plex.tv device the signed-in user owns (owned=1). #329: keyed on the machine id,
//      never host:port, so a shared server on a reused IP/port is not taken for an owned one.
// Unknown server (no id, no plex.tv row, no sign-in) counts as not owned.
inline std::string burnPutHostKey(const std::string& url) {
    std::string s = url;
    const auto sch = s.find("://");
    if (sch != std::string::npos)
        s = s.substr(sch + 3);
    const auto end = s.find_first_of("/?#");
    if (end != std::string::npos)
        s = s.substr(0, end);
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
class BurnPutOwners {
public:
    // ids: clientIdentifier of owned devices. hosts: their connections (MUT_329_HOST_KEY only).
    void set(const std::vector<std::string>& ids, const std::vector<std::string>& hosts = {}) {
        std::lock_guard<std::mutex> g(mu_);
        ids_.clear();
        hosts_.clear();
        for (const auto& i : ids)
            if (!i.empty())
                ids_.insert(i);
        for (const auto& h : hosts)
            if (!h.empty())
                hosts_.insert(burnPutHostKey(h));
    }
    bool ownedId(const std::string& id) {
        std::lock_guard<std::mutex> g(mu_);
        return !id.empty() && ids_.count(id) != 0;
    }
    bool ownedHost(const std::string& baseUrl) {
        const std::string k = burnPutHostKey(baseUrl);
        std::lock_guard<std::mutex> g(mu_);
        return !k.empty() && hosts_.count(k) != 0;
    }
private:
    std::mutex mu_;
    std::set<std::string> ids_;
    std::set<std::string> hosts_;
};
inline BurnPutOwners& burnPutOwners() {
    static BurnPutOwners o;
    return o;
}
// base -> machineIdentifier ("" unknown). plex_resolve.cpp installs a tokenless /identity lookup.
inline std::function<std::string(const std::string&)>& burnPutIdentityLookup() {
    static std::function<std::string(const std::string&)> f;
    return f;
}
enum class BurnPutGate { Allowed, OptInOff, NotOwner };
inline BurnPutGate burnPartPutGate(const std::string& plexBase) {
#if !defined(MUT_324_DEFAULT_ON) && !defined(MUT_BURN_SUBS_SELECT_IGNORE_FLAG)
    if (!burnSubsFixEnabled())
        return BurnPutGate::OptInOff;
#endif
#if defined(MUT_324_NO_OWNER_CHECK)
    (void)plexBase;
#elif defined(MUT_329_HOST_KEY)
    if (!burnPutOwners().ownedHost(plexBase))
        return BurnPutGate::NotOwner;
#else
    const auto& look = burnPutIdentityLookup();
    const std::string id = (look && !plexBase.empty()) ? look(plexBase) : std::string();
    if (burnPutOwners().ownedId(id))
        return BurnPutGate::Allowed;
#ifndef MUT_366_NO_EMPTY_ID_HOST
    // Identity lookup can fail on plex.direct TLS while the host is the
    // plex.tv connection URI of an owned device (GH#366 glass).
    if (id.empty() && burnPutOwners().ownedHost(plexBase))
        return BurnPutGate::Allowed;
#endif
    return BurnPutGate::NotOwner;
#endif
    return BurnPutGate::Allowed;
}
// plex.tv connection URIs are https://A-B-C-D.<hash>.plex.direct:32400. The
// part PUT is plain HTTP on the LAN (#340); HTTPS is refused. Map that host
// to http://A.B.C.D:32400 so an owned plex.direct play can PUT.
inline std::string plexDirectLanHttp(const std::string& url) {
#ifdef MUT_366_NO_LAN_REWRITE
    (void)url;
    return {};
#else
    if (url.rfind("https://", 0) != 0)
        return {};
    const std::string rest = url.substr(8);
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
    const auto dot = hostport.find('.');
    if (dot == std::string::npos)
        return {};
    const std::string head = hostport.substr(0, dot);
    const std::string tail = hostport.substr(dot); // .hash.plex.direct:port
    if (tail.find(".plex.direct") == std::string::npos)
        return {};
    std::string ip = head;
    for (char& c : ip)
        if (c == '-')
            c = '.';
    int a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(ip.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) != 4)
        return {};
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255)
        return {};
    std::string port = "32400";
    const auto colon = tail.rfind(':');
    if (colon != std::string::npos)
        port = tail.substr(colon + 1);
    if (port.empty())
        port = "32400";
    return "http://" + ip + ":" + port + path;
#endif
}

inline const char* burnPutSkipReason(BurnPutGate g) {
    return g == BurnPutGate::OptInOff ? "burn PUT skipped (opt-in off)"
           : g == BurnPutGate::NotOwner ? "burn PUT skipped (not owner)"
                                        : "";
}

}  // namespace misterplex
