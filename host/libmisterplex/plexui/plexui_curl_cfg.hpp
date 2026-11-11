#pragma once
// curl -K config for plexui caller headers (X-Plex-Token etc.): never on argv / ps (#253).
// Header-only so tests/unit/test_plexhttpget_injection.cpp can exercise it without the plexui link deps.
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {
namespace plexui {

// RFC 7230 token: rejects CR/LF/NUL, ':' ' ' '"' and other controls in header NAMES so a name cannot inject a
// curl config line (#264 review nit 1). MUT_PLEXUI_HDR_NAME_UNCHECKED restores the old non-empty-only check.
inline bool plexuiHeaderNameOk(const std::string& n) {
    if (n.empty())
        return false;
#ifndef MUT_PLEXUI_HDR_NAME_UNCHECKED
    for (unsigned char c : n) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c != 0 && std::strchr("!#$%&'*+-.^_`|~", c)))
            continue;
        return false;
    }
#endif
    return true;
}

inline std::string plexuiCurlHeaderConfig(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string out;
    for (const auto& h : headers) {
        if (!plexuiHeaderNameOk(h.first) || h.second.find('\n') != std::string::npos ||
            h.second.find('\r') != std::string::npos || h.second.find('\0') != std::string::npos)
            continue;
        out += "header = \"";
        const std::string line = h.first + ": " + h.second;
        for (char c : line) {
            if (c == '"' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        out += "\"\n";
    }
    return out;
}

} // namespace plexui
} // namespace misterplex
