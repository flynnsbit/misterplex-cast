// #207 follow-up: the OB core's direct-play decode envelope, the "first picture" truth test, and
// redaction for logged PMS error bodies. Header-only so tests/unit/test_ob_decode_envelope.cpp can run it.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>

namespace misterplex {

struct ObDirectSource {
    std::string codec;    // Stream@codec / Media@videoCodec, lower case
    std::string profile;  // Stream@profile, lower case
    int width = 0, height = 0;
    int level = 0;        // Stream@level (31 = 3.1); 0 unknown
    int bitrateKbps = 0;  // 0 unknown
};

// Conservative FPGA direct-play caps: 1,620 macroblocks (720x576, the 0.1-0.10 RC_REF_SIZE cap),
// 8-bit 4:2:0 profiles only, level <= 4.1, <= 8 Mbps.  lab BISECT-BLACK-1030: 712x400 834k decodes,
// 1280x692 High 11.7 Mbps gives audio over black.
constexpr int kObDirectMaxMbs = 1620;
constexpr int kObDirectMaxLevel = 41;
constexpr int kObDirectMaxKbps = 8000;

inline bool obDirectDecodable(const ObDirectSource& s) {
#ifdef MUT_ENV_CODEC_IGNORED
    (void)0;
#else
    if (s.codec != "h264" && s.codec != "avc" && s.codec != "avc1")
        return false;
#endif
    if (s.profile.find("10") != std::string::npos || s.profile.find("422") != std::string::npos ||
        s.profile.find("444") != std::string::npos)
        return false;
    if (s.width <= 0 || s.height <= 0)
        return false;  // unknown size: not provably decodable
    const int mbs = ((s.width + 15) / 16) * ((s.height + 15) / 16);
#ifndef MUT_ENV_SIZE_IGNORED
    if (mbs > kObDirectMaxMbs)
        return false;
#else
    (void)mbs;
#endif
    if (s.level > kObDirectMaxLevel)
        return false;
    if (s.bitrateKbps > kObDirectMaxKbps)
        return false;
    return true;
}

// Flag (REMOTE_H264_TRANSCODE) on + REMOTE server: never hand the core a direct Part it cannot decode.
// LAN plays are never changed by the flag (review #208 nit 1).
inline bool directFallbackAllowed(bool flagOn, bool wan, bool decodable) {
#if defined(MUT_ENV_OFF_BLOCKS)
    (void)flagOn; (void)wan; return decodable;
#elif defined(MUT_ENV_ON_IGNORED)
    (void)flagOn; (void)wan; (void)decodable; return true;
#elif defined(MUT_ENV_LOCAL_GATED)
    (void)wan; return !flagOn || decodable;
#else
    return !(flagOn && wan) || decodable;
#endif
}

// "first picture" only when the STC sample landed AND the decoder reconstructed a picture this stream.
inline bool firstPictureReal(bool stcSample, uint32_t picsNow, uint32_t picsAtOpen) {
#ifdef MUT_FIRSTPIC_STC_ONLY
    (void)picsNow; (void)picsAtOpen; return stcSample;
#else
    return stcSample && picsNow != picsAtOpen;
#endif
}

// A PMS error body for the log: tokens REDACTED, newlines flattened, cut to maxBytes.
inline std::string redactPlexBody(const std::string& in, std::size_t maxBytes = 400) {
    std::string s = in.substr(0, maxBytes);
    // Any "...token" name (X-Plex-Token, token, accessToken, authToken; any case) followed by
    // '=', URL-encoded "%3D", or ':' (optional quote/space): the value is REDACTED (review #208 nit 2).
    std::size_t p = 0;
    while (p < s.size()) {
        std::size_t q = p;
        while (q < s.size() && std::tolower(static_cast<unsigned char>(s[q])) != 't')
            ++q;
        if (q + 5 > s.size())
            break;
        bool tok = true;
        for (int i = 0; i < 5; ++i)
            if (std::tolower(static_cast<unsigned char>(s[q + i])) != "token"[i])
                tok = false;
        p = q + 1;
        if (!tok)
            continue;
        std::size_t v = q + 5;
        if (v < s.size() && s[v] == '"')
            ++v;
        if (v < s.size() && (s[v] == '=' || s[v] == ':'))
            ++v;
#ifndef MUT_REDACT_NO_PCT
        else if (v + 3 <= s.size() && s[v] == '%' && s[v + 1] == '3' && (s[v + 2] == 'D' || s[v + 2] == 'd'))
            v += 3;
#endif
        else
            continue;
        while (v < s.size() && (s[v] == ' ' || s[v] == '"'))
            ++v;
        std::size_t e = v;
        while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '-' || s[e] == '_'))
            ++e;
#ifndef MUT_REDACT_OFF
        if (e > v)
            s.replace(v, e - v, "REDACTED");
#endif
        p = v + 8;
    }
    for (char& c : s)
        if (c == '\n' || c == '\r')
            c = ' ';
    if (in.size() > maxBytes)
        s += "...";
    return s;
}

}  // namespace misterplex
