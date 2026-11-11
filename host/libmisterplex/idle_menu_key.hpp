#pragma once
// Keyboard P on the Ready idle screen.
//
// IDLE_MENU_KEY (misterplex.conf) / MPX_IDLE_MENU_KEY (env wins).
// Default off. Unset, empty, 0, off, false, and no stay off.
//
// On (1/on/yes/true): PS/2 set-2 P (make code 0x4D, no E0 prefix) is Aux
// while the Ready card is up (nothing playing, library and PlexUI hidden).
// That opens the Program menu, the same panel as pad Y, including Safe area.
// Off: P is not a navigation. M, Tab, and F12 stay Menu. Pad Y stays Aux.
//
// In a play, Aux is Skip Back, so P is not Aux there. While PlexUI or the
// library is up, Aux belongs to that page (PlexUI: UI size), so P is not
// Aux there either. Do not set this in STAGE conf.

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace misterplex {

inline bool idleMenuKeyEnvOn(const char* e) {
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

// Default off. MPX_IDLE_MENU_KEY wins over IDLE_MENU_KEY when it is set.
inline bool idleMenuKeyEnabled() {
#ifdef MUT_IDLE_MENU_KEY_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_IDLE_MENU_KEY"); e && e[0])
        return idleMenuKeyEnvOn(e);
    return idleMenuKeyEnvOn(std::getenv("IDLE_MENU_KEY"));
#endif
}

} // namespace misterplex
