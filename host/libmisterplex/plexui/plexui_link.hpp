#pragma once

// plex.tv/link PIN sign-in. Parsers only: the HTTP calls live in the controller,
// and nothing here prints a token.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace misterplex {
namespace plexui {

struct PlexPin {
    bool ok = false;
    std::string id;
    std::string code;
    std::string authToken;
    int expiresIn = 0;
};

inline std::string jsonScalar(const std::string& body, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t i = body.find(pat);
    if (i == std::string::npos)
        return {};
    i += pat.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
        ++i;
    if (i >= body.size() || body[i] != ':')
        return {};
    ++i;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])))
        ++i;
    if (i >= body.size())
        return {};
    if (body.compare(i, 4, "null") == 0)
        return {};
    if (body[i] == '"') {
        std::string out;
        for (++i; i < body.size(); ++i) {
            if (body[i] == '\\' && i + 1 < body.size()) {
                out.push_back(body[++i]);
                continue;
            }
            if (body[i] == '"')
                break;
            out.push_back(body[i]);
        }
        return out;
    }
    size_t end = i;
    while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != ']' &&
           !std::isspace(static_cast<unsigned char>(body[end])))
        ++end;
    return body.substr(i, end - i);
}

inline std::string xmlAttr(const std::string& body, const std::string& key) {
    const std::string pat = key + "=\"";
    const size_t a = body.find(pat);
    if (a == std::string::npos)
        return {};
    const size_t b = body.find('"', a + pat.size());
    if (b == std::string::npos)
        return {};
    return body.substr(a + pat.size(), b - (a + pat.size()));
}

inline PlexPin parsePinResponse(const std::string& body) {
    PlexPin pin;
    if (body.find('<') != std::string::npos && body.find('{') == std::string::npos) {
        pin.id = xmlAttr(body, "id");
        pin.code = xmlAttr(body, "code");
        pin.authToken = xmlAttr(body, "authToken");
        const std::string exp = xmlAttr(body, "expiresIn");
        if (!exp.empty())
            pin.expiresIn = std::atoi(exp.c_str());
    } else {
        pin.id = jsonScalar(body, "id");
        pin.code = jsonScalar(body, "code");
        pin.authToken = jsonScalar(body, "authToken");
        const std::string exp = jsonScalar(body, "expiresIn");
        if (!exp.empty())
            pin.expiresIn = std::atoi(exp.c_str());
    }
    if (pin.expiresIn <= 0)
        pin.expiresIn = 900;
    pin.ok = !pin.id.empty() && !pin.code.empty();
    return pin;
}

inline bool pinAuthed(const PlexPin& pin) { return !pin.authToken.empty(); }

// plex.tv account tokens are alphanumeric plus '-', '_', and '.' (JWT-style).
// Anything else is rejected so a value cannot break a conf line.
inline bool plexAccountTokenOk(const std::string& tok) {
    if (tok.empty())
        return false;
    for (unsigned char c : tok) {
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.')
            return false;
    }
    return true;
}

// True when a log line would reveal the token. Callers skip the line.
inline bool logMentionsSecret(const std::string& line, const std::string& secret) {
    return !secret.empty() && line.find(secret) != std::string::npos;
}

// QR for the fixed URL https://plex.tv/link (version 2, medium ECC, quiet zone 1).
// 27x27 modules, row-major, '1' dark. Packed size is the bitmap; kLinkQrModules is
// the draw size (GH#23). MUT_QR_MODULES_1 shrinks the draw; MUT_QR_BLANK blanks it.
constexpr int kLinkQrPacked = 27;
#ifdef MUT_QR_MODULES_1
constexpr int kLinkQrModules = 1;
#else
constexpr int kLinkQrModules = kLinkQrPacked;
#endif
constexpr char kLinkQr[kLinkQrPacked * kLinkQrPacked + 1] =
    "000000000000000000000000000"
    "011111110000010100011111110"
    "010000010100100000010000010"
    "010111010101100111010111010"
    "010111010101001011010111010"
    "010111010001001001010111010"
    "010000010000001101010000010"
    "011111110101010101011111110"
    "000000000110000011000000000"
    "010000010110011100110011100"
    "000101100110111111001111100"
    "011110111010101010100010110"
    "010000100010010111010010010"
    "000001111000001110010000010"
    "010000100010011011001000100"
    "010000010101001110111110110"
    "010101001001011100111011010"
    "010111010011010111111101000"
    "000000000111000011000100000"
    "011111110000011101010100010"
    "010000010001100111000100010"
    "010111010011100011111101110"
    "010111010001011000110000110"
    "010111010010001101100011010"
    "010000010000011001001100010"
    "011111110100111011110010010"
    "000000000000000000000000000";

inline char linkQrModule(int x, int y) {
#ifdef MUT_QR_BLANK
    (void)x;
    (void)y;
    return '0';
#else
    if (x < 0 || y < 0 || x >= kLinkQrPacked || y >= kLinkQrPacked)
        return '0';
    return kLinkQr[y * kLinkQrPacked + x];
#endif
}

} // namespace plexui
} // namespace misterplex
