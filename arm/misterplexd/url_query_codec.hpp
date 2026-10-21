#pragma once
// Query-component percent codec, shared by resolve (plex_resolve.cpp urlEncodeQuery builds the
// lab ladder's src=) and the ladder start retarget (transcode_offset.hpp). With one encoder, a src
// that the retarget re-encodes is byte-identical to the one resolve built (T-BROWSE-DOUBLE-SEEK
// fix F-B, PREREG-FB rev1.1 s2).
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

namespace misterplex {

// Unreserved bytes (alphanumerics, '-', '_', '.', '~') stay as they are; every other byte becomes
// %XX in upper-case hex. Moved verbatim from plex_resolve.cpp urlEncodeQuery (8d52ace6 :365-377).
inline std::string urlEncodeQueryComponent(const std::string& s) {
    std::ostringstream o;
    o << std::uppercase;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o << static_cast<char>(c);
        else {
            o << '%' << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c)
              << std::dec;
        }
    }
    return o.str();
}

// The inverse of urlEncodeQueryComponent: each %XX becomes its byte and every other byte is kept.
// '+' stays '+', because the encoder never writes '+' (it writes %2B). Returns false, leaving *out
// unspecified, on a '%' that is not followed by two hex digits.
inline bool urlDecodeQueryComponent(const std::string& s, std::string* out) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            r += s[i];
            continue;
        }
        if (i + 2 >= s.size())
            return false;
        const int hi = hexVal(s[i + 1]);
        const int lo = hexVal(s[i + 2]);
        if (hi < 0 || lo < 0)
            return false;
        r += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    *out = r;
    return true;
}

}  // namespace misterplex
