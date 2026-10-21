#pragma once

#include <cctype>
#include <string>

namespace misterplex {

// /library/metadata/123 -> "123" (Web often omits ratingKey= on cast URLs).
inline std::string ratingKeyFromKey(const std::string& key) {
    const std::string marker = "/library/metadata/";
    auto pos = key.find(marker);
    if (pos == std::string::npos)
        return {};
    pos += marker.size();
    size_t end = pos;
    while (end < key.size() && std::isdigit(static_cast<unsigned char>(key[end])))
        ++end;
    if (end == pos)
        return {};
    return key.substr(pos, end - pos);
}

} // namespace misterplex
