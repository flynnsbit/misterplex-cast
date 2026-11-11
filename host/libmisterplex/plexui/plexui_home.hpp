#pragma once

// Plex Home profile list and PIN switch. The plex.tv account token
// (PLEXTV_TOKEN) lists users and posts the switch. The switched token is a
// separate session. This is not the plex.tv/link PIN exchange.

#include "libmisterplex/browse_v2.hpp"
#include "plexui_link.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace misterplex {
namespace plexui {

struct HomeUser {
    std::string id;
    std::string title;
    std::string thumb;
    bool protectedPin = false;
    bool admin = false;
    bool restricted = false;
    bool guest = false;
};

struct HomeSwitch {
    bool ok = false;
    bool unauthorized = false;
    std::string id;
    std::string authToken;
};

// Empty token clears the saved profile. A set token is the switched user,
// never the plex.tv account token and never the PMS PLEX_TOKEN.
struct HomeSession {
    std::string id;
    std::string title;
    std::string token;
};

inline constexpr const char* kHomeUsersUrl = "https://plex.tv/api/v2/home/users";
inline constexpr const char* kHomeUsersLegacyUrl = "https://plex.tv/api/home/users";

inline std::string homeSwitchUrl(const std::string& id) {
    return "https://plex.tv/api/v2/home/users/" + id + "/switch";
}

inline std::string homeSwitchLegacyUrl(const std::string& id) {
    return "https://plex.tv/api/home/users/" + id + "/switch";
}

// Plex Home ids are numeric. A non-digit cannot be placed in a URL path.
inline bool homeIdOk(const std::string& id) {
    if (id.empty() || id.size() > 32)
        return false;
    for (unsigned char c : id) {
        if (!std::isdigit(c))
            return false;
    }
    return true;
}

// One conf line. Drops controls and '=' so a title cannot break the file.
inline std::string homeTitleForConf(const std::string& title) {
    std::string out;
    for (unsigned char c : title) {
        if (c < 0x20 || c == '=' || c == '#')
            continue;
        out.push_back(static_cast<char>(c));
        if (out.size() >= 64)
            break;
    }
    if (out.empty())
        return "User";
    return out;
}

inline bool homePinOk(const std::string& pin) {
#ifdef MUT_HOME_ACCEPT_SHORT_PIN
    if (pin.empty())
        return false;
#else
    if (pin.size() != 4)
        return false;
#endif
    for (unsigned char c : pin) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

inline bool homeSwitchNeedsPin(const HomeUser& user) {
#ifdef MUT_HOME_SKIP_PIN
    (void)user;
    return false;
#else
    return user.protectedPin;
#endif
}

// The library token. An empty profile keeps the account token, which is the
// tip path for a single-user or no-Home account.
inline std::string activeLibraryToken(const std::string& accountToken, const std::string& homeToken) {
    if (!homeToken.empty())
        return homeToken;
    return accountToken;
}

inline std::string homeSessionLog(const HomeSession& session) {
    if (session.token.empty())
        return "plexui: home profile cleared";
    return "plexui: home profile id=" + session.id + " title=" + session.title;
}

inline std::string jsonObjectSlice(const std::string& body, size_t open) {
    if (open >= body.size() || body[open] != '{')
        return {};
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < body.size(); ++i) {
        const char c = body[i];
        if (inStr) {
            if (c == '\\' && i + 1 < body.size()) {
                ++i;
                continue;
            }
            if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0)
                return body.substr(open, i - open + 1);
        }
    }
    return {};
}

inline bool jsonFlag(const std::string& obj, const char* key) {
    const std::string v = jsonScalar(obj, key);
    return v == "1" || v == "true";
}

inline std::vector<HomeUser> parseHomeUsersJson(const std::string& body) {
    std::vector<HomeUser> out;
    const size_t key = body.find("\"users\"");
    if (key == std::string::npos)
        return out;
    const size_t bracket = body.find('[', key);
    if (bracket == std::string::npos)
        return out;
    size_t i = bracket + 1;
    while (i < body.size() && out.size() < 32) {
        while (i < body.size() && body[i] != '{' && body[i] != ']')
            ++i;
        if (i >= body.size() || body[i] == ']')
            break;
        const std::string obj = jsonObjectSlice(body, i);
        if (obj.empty())
            break;
        i += obj.size();
        HomeUser u;
        u.id = jsonScalar(obj, "id");
        u.title = jsonScalar(obj, "title");
        if (u.title.empty())
            u.title = jsonScalar(obj, "username");
        if (u.title.empty())
            u.title = jsonScalar(obj, "friendlyName");
        u.thumb = jsonScalar(obj, "thumb");
        u.protectedPin = jsonFlag(obj, "protected");
        u.admin = jsonFlag(obj, "admin");
        u.restricted = jsonFlag(obj, "restricted");
        u.guest = jsonFlag(obj, "guest");
        if (u.guest || !homeIdOk(u.id))
            continue;
        if (u.title.empty())
            u.title = "User";
        out.push_back(std::move(u));
    }
    return out;
}

inline bool xmlFlag(const XmlElement& el, const char* key) {
    const std::string v = el.get(key);
    return v == "1" || v == "true";
}

inline std::vector<HomeUser> parseHomeUsersXml(const std::string& body) {
    std::vector<HomeUser> out;
    for (const auto& el : parseXmlStartTags(body)) {
        if (el.name != "User" && el.name != "user")
            continue;
        if (out.size() >= 32)
            break;
        HomeUser u;
        u.id = el.get("id");
        u.title = el.get("title");
        if (u.title.empty())
            u.title = el.get("username");
        if (u.title.empty())
            u.title = el.get("friendlyName");
        u.thumb = el.get("thumb");
        u.protectedPin = xmlFlag(el, "protected");
        u.admin = xmlFlag(el, "admin");
        u.restricted = xmlFlag(el, "restricted");
        u.guest = xmlFlag(el, "guest");
        if (u.guest || !homeIdOk(u.id))
            continue;
        if (u.title.empty())
            u.title = "User";
        out.push_back(std::move(u));
    }
    return out;
}

inline std::vector<HomeUser> parseHomeUsers(const std::string& body) {
    if (body.find("<User") != std::string::npos || body.find("<user") != std::string::npos)
        return parseHomeUsersXml(body);
    return parseHomeUsersJson(body);
}

inline HomeSwitch parseHomeSwitch(const std::string& body, int httpStatus) {
    HomeSwitch sw;
    if (httpStatus == 401 || httpStatus == 403) {
        sw.unauthorized = true;
        return sw;
    }
    if (httpStatus < 200 || httpStatus >= 300)
        return sw;
    std::string tok = xmlAttr(body, "authToken");
    if (tok.empty())
        tok = xmlAttr(body, "authenticationToken");
    sw.id = xmlAttr(body, "id");
    if (tok.empty())
        tok = jsonScalar(body, "authToken");
    if (tok.empty())
        tok = jsonScalar(body, "authenticationToken");
    if (sw.id.empty())
        sw.id = jsonScalar(body, "id");
    if (!plexAccountTokenOk(tok))
        return sw;
    sw.authToken = std::move(tok);
    sw.ok = true;
    return sw;
}

} // namespace plexui
} // namespace misterplex
