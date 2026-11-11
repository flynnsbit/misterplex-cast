#pragma once
// FEATURE-GAP E19: A–Z jump on a PlexUI library grid.
//
// AZ_JUMP (misterplex.conf) / MPX_AZ_JUMP (env wins over AZ_JUMP).
// Default off. Off: the library grid has no letter bar and library queries
// stay on sort=titleSort + the GRID_PAGE container window. On: UP from the
// category chips opens an A–Z bar. Select loads that letter with PMS
// firstCharacter on the same /library/sections/<id>/all query. * clears the
// letter. # is titles that do not start with A–Z. GRID_PAGE still pages the
// filtered grid. Search, collections, seasons, and playback are not part of
// this flag.

#include "grid_page.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {

constexpr int kAzJumpSlots = 28; // 0 = all, 1..26 = A..Z, 27 = '#'

inline bool azJumpEnvOn(const char* e) {
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

// Default off. MPX_AZ_JUMP wins over AZ_JUMP when it is set.
inline bool azJumpEnabled() {
#ifdef MUT_AZ_JUMP_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_AZ_JUMP"); e && e[0])
        return azJumpEnvOn(e);
    return azJumpEnvOn(std::getenv("AZ_JUMP"));
#endif
}

// UP from the chip row opens the bar only while the flag is on.
inline bool azJumpEnterFromChips() {
#if defined(MUT_AZ_JUMP_NO_ENTER)
    return false;
#else
    return azJumpEnabled();
#endif
}

// 0 when the character is not a letter and not '#'.
inline char azJumpNormalize(char letter) {
    if (letter == '#')
        return '#';
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    if (c >= 'A' && c <= 'Z')
        return c;
    return 0;
}

// Letter actually placed on the wire. Flag off is no letter.
inline char azJumpWireLetter(char letter) {
    const char n = azJumpNormalize(letter);
#if defined(MUT_AZ_JUMP_OFF_STILL_FILTERS)
    return n;
#elif defined(MUT_AZ_JUMP_IGNORE_LETTER)
    return 0;
#else
    if (!azJumpEnabled())
        return 0;
    return n;
#endif
}

// Slot 0 clears the filter. Slots 1..26 are A..Z. Slot 27 is '#'.
inline char azJumpSlotLetter(int index) {
    if (index <= 0 || index >= kAzJumpSlots)
        return 0;
    if (index >= 27)
        return '#';
    return static_cast<char>('A' + index - 1);
}

inline int azJumpSlotForLetter(char letter) {
    const char n = azJumpNormalize(letter);
    if (n == '#')
        return kAzJumpSlots - 1;
    if (n >= 'A' && n <= 'Z')
        return 1 + (n - 'A');
    return 0;
}

inline std::string azJumpSlotLabel(int index) {
    const char letter = azJumpSlotLetter(index);
    if (!letter)
        return "*";
    return std::string(1, letter);
}

// Seven labels with the focused slot in brackets. Wraps at the ends.
inline std::string azJumpBarText(int index) {
    if (index < 0)
        index = 0;
    if (index >= kAzJumpSlots)
        index = kAzJumpSlots - 1;
    std::string line;
    for (int d = -3; d <= 3; ++d) {
        int i = index + d;
        i %= kAzJumpSlots;
        if (i < 0)
            i += kAzJumpSlots;
        const std::string lab = azJumpSlotLabel(i);
        if (!line.empty())
            line += ' ';
        if (d == 0)
            line += "[" + lab + "]";
        else
            line += lab;
    }
    return line;
}

// Empty when the flag is off or the letter is not A–Z or '#'.
inline std::string azJumpQuerySuffix(char letter) {
    const char w = azJumpWireLetter(letter);
    if (!w)
        return {};
    if (w == '#')
        return "&firstCharacter=%23";
    std::string s = "&firstCharacter=";
    s.push_back(w);
    return s;
}

// GRID_PAGE still owns the container window. The letter is only appended.
inline std::string azJumpDecorate(const std::string& query, char letter) {
    return query + azJumpQuerySuffix(letter);
}

} // namespace misterplex
