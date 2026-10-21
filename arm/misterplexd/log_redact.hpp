#pragma once
// Central log redaction for misterplexd.
//
// Secrets must never reach misterplexd.log. Feed every log write through
// LogLineAssembler (or MediaPlayer::log / Companion::log, which use it) so a
// token split across two writes is redacted as one line. Do NOT apply this to
// live playback argv/URLs — only to the logged copy.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace misterplex {

namespace redact_detail {

inline char lowerCh(char c) {
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c - 'A' + 'a');
    return c;
}

inline bool ciAt(const std::string& s, size_t pos, const char* lit) {
    const size_t n = std::strlen(lit);
    if (pos + n > s.size())
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (lowerCh(s[pos + i]) != lowerCh(lit[i]))
            return false;
    }
    return true;
}

inline bool isAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// A key may follow start-of-string, a non-alphanumeric, or an encoded '&'.
inline bool keyBoundary(const std::string& s, size_t pos) {
    if (pos == 0)
        return true;
    if (pos >= 5 && ciAt(s, pos - 5, "%2526"))
        return true;
    if (pos >= 3 && ciAt(s, pos - 3, "%26"))
        return true;
    return !isAlnum(s[pos - 1]);
}

// Longer keys first so a short key ("token") does not win over a longer one.
inline const char* const* secretKeyList() {
    static const char* kKeys[] = {
        "x-plex-token",
        "myplexaccesstoken",
        "authenticationtoken",
        "access_token",
        "accesstoken",
        "authtoken",
        "plex_token",
        "plextoken",
        "bearer",
        "token",
        nullptr,
    };
    return kKeys;
}

// A write that ends in the middle of a key ("myPlexAcce" / "ssToken=") must be
// held. A full key with no separator is not held: the value has not started.
inline bool endsWithStrictKeyPrefix(const std::string& s) {
    const char* const* keys = secretKeyList();
    for (int ki = 0; keys[ki]; ++ki) {
        const char* key = keys[ki];
        const size_t klen = std::strlen(key);
        if (klen < 5 || s.size() < 4)
            continue;
        const size_t hi = std::min(klen - 1, s.size());
        for (size_t len = hi; len >= 4; --len) {
            const size_t pos = s.size() - len;
            if (!keyBoundary(s, pos))
                continue;
            bool match = true;
            for (size_t i = 0; i < len; ++i) {
                if (lowerCh(s[pos + i]) != lowerCh(key[i])) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
    }
    return false;
}

inline size_t valueEnd(const std::string& s, size_t begin) {
    size_t i = begin;
    while (i < s.size()) {
        if (s[i] == '&' || s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == '"' ||
            s[i] == '\'')
            return i;
        if (ciAt(s, i, "%2526") || ciAt(s, i, "%26"))
            return i;
        ++i;
    }
    return s.size();
}

} // namespace redact_detail

// Replace secret VALUES, keep keys and separators. Covers:
//   X-Plex-Token=SECRET / X-Plex-Token: SECRET
//   accessToken="SECRET" / "accessToken": "SECRET" / accessToken='SECRET'
//   case variants (x-plex-token, X-PLEX-TOKEN, access_token, accessToken, authToken)
//   percent-encoded and double-encoded '=' (%3D, %253D) and '&' / &amp; / %26 / %2526
// Byte-identical when no known secret key is present.
inline std::string redactSensitive(std::string s) {
#ifdef LOG_REDACT_FAULT_IDENTITY
    // Intentional fault for EXPECTED_RED mutation tests only.
    return s;
#else
    using namespace redact_detail;
    const char* const* kKeys = secretKeyList();
    // Longer separators first so %253D is not read as %3D.
    static const char* kSeps[] = {"%253d", "%3d", "=", ":"};
    static constexpr const char kRedacted[] = "REDACTED";

    size_t i = 0;
    while (i < s.size()) {
        bool hit = false;
        for (int ki = 0; kKeys[ki]; ++ki) {
            const char* key = kKeys[ki];
            const size_t klen = std::strlen(key);
            if (!ciAt(s, i, key) || !keyBoundary(s, i))
                continue;
            size_t j = i + klen;
            // JSON: "accessToken": "secret" — the key's closing quote sits before ':'.
            if (j < s.size() && s[j] == '"')
                ++j;
            // "Authorization: Bearer SECRET" — the space is the separator, not padding.
            const bool bearerSpace = std::strcmp(key, "bearer") == 0 && j < s.size() && s[j] == ' ';
            if (!bearerSpace) {
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
                    ++j;
            }
            const char* sep = nullptr;
            if (!bearerSpace) {
                for (const char* cand : kSeps) {
                    if (ciAt(s, j, cand)) {
                        sep = cand;
                        break;
                    }
                }
            }
            if (!sep && !bearerSpace)
                continue;
            if (bearerSpace)
                ++j;
            else
                j += std::strlen(sep);
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
                ++j;
            size_t valBegin = j;
            size_t valEnd = j;
            if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
                const char q = s[j];
                valBegin = j + 1;
                valEnd = s.find(q, valBegin);
                if (valEnd == std::string::npos)
                    valEnd = s.size();
            } else {
                valEnd = valueEnd(s, valBegin);
            }
            s.replace(valBegin, valEnd - valBegin, kRedacted);
            i = valBegin + (sizeof(kRedacted) - 1);
            hit = true;
            break;
        }
        if (!hit)
            ++i;
    }
    return s;
#endif
}

// A URL or any log line. Same rules as redactSensitive, including percent-encoded
// separators. Only the logged copy.
inline std::string redactUrlTokens(std::string s) {
    return redactSensitive(std::move(s));
}

// True when `pending` ends in a known key plus a separator and the value has not
// arrived yet. The next write must be appended before redacting.
inline bool redactHoldPartial(const std::string& s) {
#ifdef LOG_REDACT_FAULT_IDENTITY
    (void)s;
    return false;
#else
    using namespace redact_detail;
    const char* const* kKeys = secretKeyList();
    static const char* kSeps[] = {"%253d", "%3d", "=", ":"};
    if (s.empty())
        return false;
    for (size_t pos = 0; pos < s.size(); ++pos) {
        for (int ki = 0; kKeys[ki]; ++ki) {
            const char* key = kKeys[ki];
            const size_t klen = std::strlen(key);
            if (!ciAt(s, pos, key) || !keyBoundary(s, pos))
                continue;
            size_t j = pos + klen;
            if (j < s.size() && s[j] == '"')
                ++j;
            // "Authorization: Bearer " + "SECRET": the space is the separator.
            // Skip it only after deciding to hold a trailing separator space.
            bool held = false;
            if (std::strcmp(key, "bearer") == 0 && j < s.size() &&
                (s[j] == ' ' || s[j] == '\t')) {
                size_t k = j;
                while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
                    ++k;
                if (k == s.size())
                    held = true;
            }
            if (!held) {
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
                    ++j;
            }
            for (const char* sep : kSeps) {
                if (held || !ciAt(s, j, sep))
                    continue;
                size_t k = j + std::strlen(sep);
                while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
                    ++k;
                if (k == s.size())
                    held = true;
            }
            if (held)
                return true;
        }
    }
    if (endsWithStrictKeyPrefix(s))
        return true;
    return false;
#endif
}

// Assembles log writes into lines, then redacts each line. A chunk that ends in
// `token=` (or %3D / %253D / ':') is held so the value in the next write is
// covered. A newline always flushes the line in front of it.
struct LogLineAssembler {
    std::string pending;

    std::vector<std::string> push(const std::string& chunk) {
        pending += chunk;
        std::vector<std::string> out;
        for (;;) {
            const auto nl = pending.find('\n');
            if (nl == std::string::npos)
                break;
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            out.push_back(redactUrlTokens(std::move(line)));
        }
        if (!pending.empty() && !redactHoldPartial(pending)) {
            out.push_back(redactUrlTokens(pending));
            pending.clear();
        }
        return out;
    }
};

// Process-wide stderr sink. Buffers a partial line, redacts, then writes.
// MediaPlayer::log and Companion::log share this. The mutex covers the pending
// line so two threads cannot move the same string. emitLogSink is a different
// process-wide buffer and has its own mutex.
class SyncedLogAssembler {
public:
    std::vector<std::string> push(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mu_);
        return asm_.push(chunk);
    }

private:
    std::mutex mu_;
    LogLineAssembler asm_;
};

inline void emitLogSink(const std::string& chunk) {
    static std::mutex mu;
    static LogLineAssembler asmbl;
    std::lock_guard<std::mutex> lock(mu);
    for (const std::string& line : asmbl.push(chunk))
        std::fprintf(stderr, "%s\n", line.c_str());
}

} // namespace misterplex
