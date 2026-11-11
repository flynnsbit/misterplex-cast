#pragma once
// Persisted YouTube Lounge screen_id. A copied SD card or install that reused
// another box's /media/fat/misterplex/yt_screen_id made two receivers advertise
// the same screenId, so a phone cast to one MiSTer was delivered to the other
// (GH#390: "Loading YouTube..." on the box the user is watching).
//
// File v2:
//   uuid <dial-uuid>
//   <screenId>
// A v1 file (bare screenId) or a uuid mismatch is treated as empty so this
// process generates a new Lounge id.
#include <cctype>
#include <sstream>
#include <string>

namespace misterplex {
namespace airplay {

inline bool ytScreenIdPlausible(const std::string& sid) {
    if (sid.size() < 8 || sid.size() > 64)
        return false;
    for (unsigned char c : sid) {
        if (!(std::isalnum(c) || c == '-' || c == '_'))
            return false;
    }
    return true;
}

inline std::string ytScreenIdPersistText(const std::string& uuid, const std::string& sid) {
    return "uuid " + uuid + "\n" + sid + "\n";
}

// Returns the screenId to keep, or empty to call generate_screen_id.
inline std::string ytScreenIdFromFile(const std::string& text, const std::string& uuid) {
    if (uuid.empty())
        return {};
    std::istringstream in(text);
    std::string line1, line2;
    if (!std::getline(in, line1))
        return {};
    while (!line1.empty() && (line1.back() == '\r' || line1.back() == ' '))
        line1.pop_back();
    const std::string prefix = "uuid ";
    if (line1.size() <= prefix.size() || line1.compare(0, prefix.size(), prefix) != 0)
        return {};   // v1 bare screenId, or junk
    const std::string got = line1.substr(prefix.size());
    if (got != uuid)
        return {};
    if (!std::getline(in, line2))
        return {};
    while (!line2.empty() && (line2.back() == '\r' || line2.back() == ' '))
        line2.pop_back();
    if (!ytScreenIdPlausible(line2))
        return {};
    return line2;
}

}  // namespace airplay
}  // namespace misterplex
