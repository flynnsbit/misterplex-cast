// rc2: LAN-configurable helper endpoints (mpx-menu :18769, airplay-xcode :18780).
// One key, MPX_LAB_HOST=<host>, points both at a LAN helper box; each service keeps its own
// full-URL override. Env wins over /media/fat/linux/misterplex.conf. Pure: lookups are injected.
//   ladder : removed (#275). misterplexd plays via PMS only; ladder env/conf keys are ignored.
//   menu   : MPX_MENU_URL (env) > MPX_MENU_BASE (env) + /menu > MPX_MENU_URL (conf)
//            > http://MPX_LAB_HOST:MPX_LAB_MENU_PORT/menu > "" (no helper: PMS sections only). No built-in IP.
//   xcode  : AIRPLAY_XCODE (env, empty = none) > GCAST_XCODE (env, compat alias, empty = none)
//            > AIRPLAY_XCODE (conf) > GCAST_XCODE (conf) > http://MPX_LAB_HOST:MPX_LAB_XCODE_PORT > "".
#pragma once
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace misterplex::lab_endpoints {

using Lookup = std::function<std::optional<std::string>(const char* key)>;   // nullopt = key unset

constexpr int kMenuPort = 18769, kXcodePort = 18780;

struct Endpoints {
    std::string host;                 // normalized MPX_LAB_HOST ("" = unset)
    std::string menu, xcode;  // "" = not configured
    std::string menuSrc, xcodeSrc;
    std::vector<std::string> notes;   // rejected values
};

// "http://203.0.113.55/" -> "203.0.113.55". Hostnames, IPv4 and [IPv6] only; anything else -> "".
inline std::string normalizeHost(std::string v) {
    for (const char* p : {"http://", "https://"})
        if (v.rfind(p, 0) == 0)
            v = v.substr(std::string(p).size());
    while (!v.empty() && (v.back() == '/' || v.back() == ' '))
        v.pop_back();
    while (!v.empty() && v.front() == ' ')
        v.erase(0, 1);
    if (v.empty() || v.size() > 253)
        return {};
    const bool v6 = v.front() == '[';
    for (char c : v) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || (v6 && (c == ':' || c == '[' || c == ']'));
#ifndef MUT_LE_ANY_HOST
        if (!ok)
            return {};
#else
        (void)ok;
#endif
    }
    if (v6 && v.back() != ']')
        return {};
    return v;
}

inline int parsePort(const std::optional<std::string>& v, int def, const char* key, std::vector<std::string>& notes) {
    if (!v || v->empty())
        return def;
    char* end = nullptr;
    const long n = std::strtol(v->c_str(), &end, 10);
    if (!end || *end != '\0' || n < 1 || n > 65535) {
        notes.push_back(std::string(key) + "=" + *v + " ignored (not a port)");
        return def;
    }
    return static_cast<int>(n);
}

inline Endpoints resolve(const Lookup& env, const Lookup& conf) {
    Endpoints e;
    const auto pick = [&](const char* key) -> std::optional<std::string> {
        if (auto v = env(key); v && !v->empty())
            return v;
        if (auto v = conf(key); v && !v->empty())
            return v;
        return std::nullopt;
    };
    if (auto raw = pick("MPX_LAB_HOST")) {
        e.host = normalizeHost(*raw);
        if (e.host.empty())
            e.notes.push_back("MPX_LAB_HOST=" + *raw + " ignored (not a host)");
    }
    const int mp = parsePort(pick("MPX_LAB_MENU_PORT"), kMenuPort, "MPX_LAB_MENU_PORT", e.notes);
    const int xp = parsePort(pick("MPX_LAB_XCODE_PORT"), kXcodePort, "MPX_LAB_XCODE_PORT", e.notes);
    const auto at = [&](int port) { return "http://" + e.host + ":" + std::to_string(port); };

    if (auto v = env("MPX_MENU_URL"); v && !v->empty()) {
        e.menu = *v;
        e.menuSrc = "env MPX_MENU_URL";
    } else if (auto b = env("MPX_MENU_BASE"); b && !b->empty()) {
        std::string s = *b;
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        e.menu = s + "/menu";
        e.menuSrc = "env MPX_MENU_BASE";
    } else if (auto c = conf("MPX_MENU_URL"); c && !c->empty()) {
        e.menu = *c;
        e.menuSrc = "conf MPX_MENU_URL";
    } else if (!e.host.empty()) {
        e.menu = at(mp) + "/menu";
        e.menuSrc = "MPX_LAB_HOST";
    } else {
        e.menuSrc = "unset (PMS sections only)";
    }

    const auto takeXcodeEnv = [&](const char* key) -> bool {
#ifndef MUT_LE_XCODE_EMPTY_FALLS_THROUGH
        if (auto v = env(key)) {
#else
        if (auto v = env(key); v && !v->empty()) {
#endif
            e.xcode = *v;
            e.xcodeSrc = v->empty() ? std::string("env ") + key + " (empty: none)"
                                    : std::string("env ") + key;
            return true;
        }
        return false;
    };
    if (takeXcodeEnv("AIRPLAY_XCODE") || takeXcodeEnv("GCAST_XCODE")) {
        // primary key wins, including an explicit empty (none). GCAST_XCODE is the compat alias.
    } else if (auto c = conf("AIRPLAY_XCODE"); c && !c->empty()) {
        e.xcode = *c;
        e.xcodeSrc = "conf AIRPLAY_XCODE";
    } else if (auto c = conf("GCAST_XCODE"); c && !c->empty()) {
        e.xcode = *c;
        e.xcodeSrc = "conf GCAST_XCODE";
    } else if (!e.host.empty()) {
        e.xcode = at(xp);
        e.xcodeSrc = "MPX_LAB_HOST";
    } else {
        e.xcodeSrc = "unset";
    }
    return e;
}

inline std::string logLine(const Endpoints& e) {
    std::string s = "misterplexd: LAN endpoints";
    s += " lab_host=" + (e.host.empty() ? std::string("-") : e.host);
    s += " menu=" + (e.menu.empty() ? std::string("-") : e.menu) + " [" + e.menuSrc + "]";
    s += " xcode=" + (e.xcode.empty() ? std::string("-") : e.xcode) + " [" + e.xcodeSrc + "]";
    for (const auto& n : e.notes)
        s += "; " + n;
    return s;
}

} // namespace misterplex::lab_endpoints
