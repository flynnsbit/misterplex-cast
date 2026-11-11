#pragma once
#include "airplay/ns.hpp"
// One silent re-resolve when a YouTube open dies with HTTP 403 (an expired
// googlevideo URL). A second 403 is not retried here.

#include <string>

namespace misterplex {
namespace airplay {

inline bool ytOpenIs403(const std::string& err) {
    return err.find("403") != std::string::npos;
}

// used is how many re-resolves this open has already spent. Only the first
// 403 is silent.
inline bool yt403ShouldReresolve(const std::string& err, int used) {
    return used < 1 && ytOpenIs403(err);
}

}  // namespace airplay
}  // namespace misterplex
