// GET /misterplex/status and POST /misterplex/update (issue #57 page, issue #59 start).
//
// The status page is one static HTML document from the companion HTTP server, same request-line
// pattern as /misterplex/mast: GET only, no query or fragment, fixed fields, printable ASCII,
// capped, no secrets, no external assets, readable without JavaScript.
//
// Updates default OFF (Facts::canUpdate false): no Update button, the page says
// "update: not available", and POST answers 503. startUpdate stays false. The router never
// fetches or swaps; it only asks the caller to start.
//
// When the daemon has a starter (canUpdate), an idle page shows one button. A POST whose body
// is nonce=<per-boot hex> sets startUpdate and answers 202. Playing is 409 and does not start.
// A browser Origin other than http://<Host> is 403. An empty Origin still needs the nonce.
// The caller rotates the nonce only after the updater accepts the start. Nothing here is a timer.
//
// No tokens, no PMS URLs that carry a token, no email. The renderer scrubs those out of every
// free-text field before the page or the log line is built.
#pragma once

#include "libmisterplex/report_code.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {
namespace phone_status {

constexpr const char* kStatusPath = "/misterplex/status";
constexpr const char* kUpdatePath = "/misterplex/update";
constexpr size_t kFieldMax = 160;
constexpr size_t kScanMax = 8192;
constexpr size_t kUpdateBodyCap = 256;
constexpr size_t kNonceHex = 32;
constexpr const char* kUpdatePhrase = "update: not available";
constexpr const char* kHtmlType = "text/html; charset=us-ascii";

struct Facts {
    std::string version;
    std::string commit;
    std::string md5;
    std::string core;
    std::string pairing;
    std::string title;
    std::string position;
    std::string mode;
    std::string helper;
    std::string newer;
    bool playing = false;
    bool canUpdate = false;  // daemon registered a self-update starter (issue #59); default off
    std::string nonce;
    std::string reportCode;  // empty unless REPORT_CODE armed a short code; the page omits the row
};

struct Reply {
    int code = 200;
    const char* contentType = "text/plain";
    std::string body;
    bool startUpdate = false;  // true only for an accepted idle POST while canUpdate
};

struct BodyNeed {
    bool read = false;
    bool tooLarge = false;
    size_t more = 0;
};

inline size_t fieldCap() {
#ifdef MUT_PHONE_STATUS_NO_CAP  // test-only mutant: free text is not truncated
    return static_cast<size_t>(1) << 20;
#else
    return kFieldMax;
#endif
}

inline bool hexDigit(unsigned char c) { return std::isxdigit(c) != 0; }

inline char asciiLower(unsigned char c) {
    return static_cast<char>(std::tolower(c));
}

inline bool ciAt(const std::string& s, size_t i, const char* key) {
    for (; *key; ++key, ++i) {
        if (i >= s.size() || asciiLower(static_cast<unsigned char>(s[i])) !=
                                 asciiLower(static_cast<unsigned char>(*key)))
            return false;
    }
    return true;
}

// A token assignment starting at i: x-plex-token / access_token / auth_token / token,
// then '=' or '%3d', then a value. Longer keys are tried first.
inline bool tokenSpan(const std::string& s, size_t i, size_t* end) {
    if (i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])))
        return false;
    const char* keys[] = {"x-plex-token", "access_token", "auth_token", "token", nullptr};
    size_t klen = 0;
    bool matched = false;
    for (int ki = 0; keys[ki]; ++ki) {
        if (!ciAt(s, i, keys[ki]))
            continue;
        const size_t n = std::strlen(keys[ki]);
        if (i + n < s.size() && std::isalnum(static_cast<unsigned char>(s[i + n])))
            continue;
        klen = n;
        matched = true;
        break;
    }
    if (!matched)
        return false;
    size_t j = i + klen;
    bool eq = false;
    if (j < s.size() && s[j] == '=') {
        eq = true;
        ++j;
    } else if (j + 2 < s.size() && s[j] == '%' && s[j + 1] == '3' &&
               (s[j + 2] == 'D' || s[j + 2] == 'd')) {
        eq = true;
        j += 3;
    }
    if (!eq)
        return false;
    while (j < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[j]);
        if (c <= ' ' || c == '&' || c == '#' || c == '"' || c == '\'' || c == '<' || c == '>' ||
            c == '?')
            break;
        ++j;
    }
    *end = j;
    return true;
}

inline bool urlSpan(const std::string& s, size_t i, size_t* end) {
    if (i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])))
        return false;
    size_t j = 0;
    if (ciAt(s, i, "https://"))
        j = i + 8;
    else if (ciAt(s, i, "http://"))
        j = i + 7;
    else
        return false;
    const size_t body = j;
    while (j < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[j]);
        if (c <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == ')')
            break;
        ++j;
    }
    if (j == body)
        return false;
    *end = j;
    return true;
}

inline bool urlHasSecret(const std::string& url) {
    if (url.find('@') != std::string::npos)
        return true;
    for (size_t i = 0; i < url.size(); ++i) {
        size_t end = 0;
        if (tokenSpan(url, i, &end))
            return true;
    }
    return false;
}

inline bool emailSpan(const std::string& s, size_t i, size_t* end) {
    auto localChar = [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
    };
    if (i > 0 && localChar(static_cast<unsigned char>(s[i - 1])))
        return false;
    if (i >= s.size() || !std::isalnum(static_cast<unsigned char>(s[i])))
        return false;
    size_t j = i;
    while (j < s.size() && localChar(static_cast<unsigned char>(s[j])))
        ++j;
    if (j >= s.size() || s[j] != '@' || j == i)
        return false;
    const size_t dom = ++j;
    bool dot = false;
    while (j < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '-')) {
        if (s[j] == '.')
            dot = true;
        ++j;
    }
    if (!dot || j - dom < 4)
        return false;
    const size_t tld = s.rfind('.', j - 1);
    if (tld == std::string::npos || tld < dom || j - tld - 1 < 2)
        return false;
    *end = j;
    return true;
}

// Tokens, token-bearing URLs, and email addresses become the word redacted.
inline std::string scrubSecrets(const std::string& in) {
#ifdef MUT_PHONE_STATUS_ECHO_SECRET  // test-only mutant: secrets are copied through
    return in.size() > kScanMax ? in.substr(0, kScanMax) : in;
#else
    const std::string s = in.size() > kScanMax ? in.substr(0, kScanMax) : in;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t end = 0;
        if (urlSpan(s, i, &end)) {
            const std::string url = s.substr(i, end - i);
            if (urlHasSecret(url))
                out += "redacted";
            else
                out += url;
            i = end;
            continue;
        }
        if (tokenSpan(s, i, &end)) {
            out += "redacted";
            i = end;
            continue;
        }
        if (emailSpan(s, i, &end)) {
            out += "redacted";
            i = end;
            continue;
        }
        out += s[i++];
    }
    return out;
#endif
}

inline std::string asciiCap(const std::string& s) {
    std::string out;
    const size_t cap = fieldCap();
    out.reserve(std::min(s.size(), cap));
    for (const unsigned char c : s) {
        if (out.size() >= cap)
            break;
        out += (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?';
    }
    return out;
}

inline std::string freeText(const std::string& s) { return asciiCap(scrubSecrets(s)); }

inline std::string htmlEscape(const std::string& s) {
#ifdef MUT_PHONE_STATUS_NO_ESCAPE  // test-only mutant: fields are inserted raw
    return s;
#else
    std::string o;
    o.reserve(s.size());
    for (const unsigned char c : s) {
        switch (c) {
        case '&':
            o += "&amp;";
            break;
        case '<':
            o += "&lt;";
            break;
        case '>':
            o += "&gt;";
            break;
        case '"':
            o += "&quot;";
            break;
        case '\'':
            o += "&#39;";
            break;
        default:
            o += static_cast<char>(c);
            break;
        }
    }
    return o;
#endif
}

inline bool knownMode(const std::string& s) {
    return s == "playing" || s == "paused" || s == "stopped" || s == "buffering" || s == "ended";
}

inline std::string whitelistMode(const std::string& s) {
    if (s.empty())
        return "stopped";
    return knownMode(s) ? s : "unknown";
}

inline std::string whitelistWord(const std::string& s, const char* a, const char* b, const char* c) {
    if (s == a || s == b || (c && s == c))
        return s;
    return "unknown";
}

inline bool dottedVersion(const std::string& s) {
    if (s.empty() || s.size() > 32)
        return false;
    bool dot = false;
    bool digit = false;
    for (const unsigned char c : s) {
        if (c == '.') {
            if (!digit)
                return false;
            dot = true;
            digit = false;
            continue;
        }
        if (!std::isdigit(c))
            return false;
        digit = true;
    }
    return digit && dot;
}

inline bool hexString(const std::string& s, size_t n) {
    if (s.size() != n)
        return false;
    for (const unsigned char c : s)
        if (!hexDigit(c))
            return false;
    return true;
}

inline std::string whitelistVersion(const std::string& s) {
    return dottedVersion(s) ? s : "unknown";
}

inline std::string whitelistCommit(const std::string& s) {
    if (s == "unknown")
        return s;
    if (s.size() < 7 || s.size() > 64)
        return "unknown";
    for (const unsigned char c : s)
        if (!hexDigit(c))
            return "unknown";
    return s;
}

inline std::string whitelistMd5(const std::string& s) {
    if (s == "unknown")
        return s;
    return hexString(s, 32) ? s : "unknown";
}

inline std::string whitelistCore(const std::string& s) {
    if (s == "unknown" || s == "none")
        return s;
    if (s.size() == 10 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && hexString(s.substr(2), 8))
        return s;
    return "unknown";
}

inline std::string whitelistPosition(const std::string& s) {
    if (s.empty())
        return "0:00";
    for (const unsigned char c : s) {
        if (!(std::isdigit(c) || c == ':' || c == ' ' || c == '/'))
            return "0:00";
    }
    return s.size() > kFieldMax ? s.substr(0, kFieldMax) : s;
}

struct View {
    std::string version;
    std::string commit;
    std::string md5;
    std::string core;
    std::string pairing;
    std::string title;
    std::string position;
    std::string mode;
    std::string helper;
    std::string newer;
    std::string nonce;
    bool playing = false;
    bool canUpdate = false;
};

inline bool showTitle(const std::string& mode) {
    return mode == "playing" || mode == "paused" || mode == "buffering" || mode == "ended";
}

inline View present(const Facts& in) {
    View v;
    v.version = whitelistVersion(in.version);
    v.commit = whitelistCommit(in.commit);
    v.md5 = whitelistMd5(in.md5);
    v.core = whitelistCore(in.core);
    v.pairing = freeText(in.pairing);
    if (v.pairing.empty())
        v.pairing = "unknown";
    v.mode = whitelistMode(in.mode);
    v.position = whitelistPosition(in.position);
    v.helper = whitelistWord(in.helper, "up", "down", "unknown");
    v.newer = whitelistWord(in.newer, "yes", "no", "unknown");
    if (showTitle(v.mode)) {
        v.title = freeText(in.title);
        if (v.title.empty())
            v.title = "(none)";
    } else {
        v.title = "(none)";
    }
    v.nonce = hexString(in.nonce, kNonceHex) ? in.nonce : "none";
    v.playing = in.playing;
    v.canUpdate = in.canUpdate;
    return v;
}

// "not available" unless a starter is registered. Playing never offers a tap (issue #59).
inline const char* updateDetail(bool canUpdate, bool playing) {
    if (!canUpdate)
        return "not available";
    if (playing)
        return "refused while playing";
    return "tap to start";
}

inline std::string statusLogLine(const Facts& in) {
    const View v = present(in);
    return "phone status\nversion=" + v.version + "\ncommit=" + v.commit + "\nmd5=" + v.md5 +
           "\ncore=" + v.core + "\npairing=" + v.pairing + "\ntitle=" + v.title +
           "\nposition=" + v.position + "\nmode=" + v.mode + "\nhelper=" + v.helper +
           "\nnewer=" + v.newer + "\nupdate=" + updateDetail(v.canUpdate, v.playing) +
           reportPhoneLog(in.reportCode);
}

inline std::string renderPage(const Facts& in) {
    const View v = present(in);
    const auto row = [](const char* name, const std::string& value) {
        return std::string("<dt>") + name + "</dt><dd>" + htmlEscape(value) + "</dd>\n";
    };
    std::string html;
    html.reserve(1400);
    html += "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"us-ascii\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>MisterPlex status</title><style>"
            "body{margin:16px;font:18px/1.4 sans-serif;background:#111;color:#eee}"
            "h1{font-size:1.25em;margin:0 0 .4em}"
            "dt{margin-top:.75em;font-weight:bold}dd{margin:0;overflow-wrap:anywhere}"
            "pre{white-space:pre-wrap;overflow-wrap:anywhere;font:14px/1.35 monospace}"
            "</style></head><body>\n<h1>MisterPlex</h1>\n<dl>\n";
    html += row("daemon", v.version);
    html += row("commit", v.commit);
    html += row("md5", v.md5);
    html += row("core", v.core);
    html += row("pairing", v.pairing);
    html += row("title", v.title);
    html += row("position", v.position);
    html += row("mode", v.mode);
    html += row("helper", v.helper);
    html += row("newer", v.newer);
    html += row("update", updateDetail(v.canUpdate, v.playing));
    html += reportPhoneDl(in.reportCode);
    html += "</dl>\n<p>";
    html += std::string("update: ") + updateDetail(v.canUpdate, v.playing);
    html += "</p>\n";
#ifndef MUT_PHONE_STATUS_NO_BUTTON  // test-only mutant: updates-on idle page has no Update control
    if (v.canUpdate && !v.playing && v.nonce.size() == kNonceHex) {
        html += "<form method=\"post\" action=\"/misterplex/update\">"
                "<input type=\"hidden\" name=\"nonce\" value=\"";
        html += htmlEscape(v.nonce);
        html += "\"><button type=\"submit\">Update</button></form>\n";
    }
#endif
    html += "<pre>\n";
    html += "version=" + htmlEscape(v.version) + "\n";
    html += "commit=" + htmlEscape(v.commit) + "\n";
    html += "md5=" + htmlEscape(v.md5) + "\n";
    html += "core=" + htmlEscape(v.core) + "\n";
    html += "pairing=" + htmlEscape(v.pairing) + "\n";
    html += "title=" + htmlEscape(v.title) + "\n";
    html += "position=" + htmlEscape(v.position) + "\n";
    html += "mode=" + htmlEscape(v.mode) + "\n";
    html += "helper=" + htmlEscape(v.helper) + "\n";
    html += "newer=" + htmlEscape(v.newer) + "\n";
    html += std::string("update=") + updateDetail(v.canUpdate, v.playing) + "\n";
    html += reportPhonePre(in.reportCode);
    html += "nonce=" + htmlEscape(v.nonce) + "\n";
    html += "</pre>\n";
#ifdef MUT_PHONE_STATUS_SHOW_BUTTON  // test-only mutant: an Update control is emitted
    html += "<button>Update</button>\n";
#endif
    html += "</body></html>\n";
    return html;
}

inline std::string formatClock(int64_t ms) {
    if (ms < 0)
        ms = 0;
    const int64_t total = ms / 1000;
    const int64_t h = total / 3600;
    const int m = static_cast<int>((total % 3600) / 60);
    const int sec = static_cast<int>(total % 60);
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof buf, "%lld:%02d:%02d", static_cast<long long>(h), m, sec);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d", m, sec);
    return buf;
}

inline std::string formatPosition(int64_t posMs, int64_t durMs) {
    if (durMs > 0)
        return formatClock(posMs) + " / " + formatClock(durMs);
    return formatClock(posMs);
}

inline std::string bootNonce() {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    unsigned char buf[16];
    size_t got = 0;
    while (got < sizeof buf) {
        const ssize_t n = ::read(fd, buf + got, sizeof buf - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    if (got != sizeof buf)
        return {};
    static const char* kHex = "0123456789abcdef";
    std::string out(kNonceHex, '0');
    for (size_t i = 0; i < sizeof buf; ++i) {
        out[i * 2] = kHex[buf[i] >> 4];
        out[i * 2 + 1] = kHex[buf[i] & 0x0f];
    }
    return out;
}

// --- MD5 of the running binary (one startup read). RFC 1321. ---

inline uint32_t md5Rot(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline void md5Block(uint32_t st[4], const uint8_t block[64]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int S[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22,
                              5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20,
                              4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                              6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = static_cast<uint32_t>(i);
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = static_cast<uint32_t>((5 * i + 1) % 16);
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = static_cast<uint32_t>((3 * i + 5) % 16);
        } else {
            f = c ^ (b | ~d);
            g = static_cast<uint32_t>((7 * i) % 16);
        }
        const uint32_t next = d;
        d = c;
        c = b;
        b = b + md5Rot(a + f + K[i] + M[g], S[i]);
        a = next;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

class Md5 {
public:
    void update(const uint8_t* p, size_t n) {
        bits_ += static_cast<uint64_t>(n) * 8;
        while (n > 0) {
            const size_t room = 64 - n_;
            const size_t take = n < room ? n : room;
            std::memcpy(buf_ + n_, p, take);
            n_ += take;
            p += take;
            n -= take;
            if (n_ == 64) {
                md5Block(st_, buf_);
                n_ = 0;
            }
        }
    }
    std::string hex() {
        uint8_t pad[64];
        const size_t n = n_;
        std::memcpy(pad, buf_, n);
        pad[n] = 0x80;
        if (n + 1 <= 56) {
            std::memset(pad + n + 1, 0, 56 - (n + 1));
            storeLen(pad + 56);
            md5Block(st_, pad);
        } else {
            std::memset(pad + n + 1, 0, 64 - (n + 1));
            md5Block(st_, pad);
            uint8_t last[64];
            std::memset(last, 0, 56);
            storeLen(last + 56);
            md5Block(st_, last);
        }
        static const char* kHex = "0123456789abcdef";
        std::string out(32, '0');
        for (int i = 0; i < 4; ++i) {
            uint32_t w = st_[i];
            for (int b = 0; b < 4; ++b) {
                const unsigned byte = w & 0xffu;
                const size_t at = static_cast<size_t>(i * 4 + b) * 2;
                out[at] = kHex[byte >> 4];
                out[at + 1] = kHex[byte & 0x0fu];
                w >>= 8;
            }
        }
        return out;
    }

private:
    void storeLen(uint8_t* p) const {
        uint64_t b = bits_;
        for (int i = 0; i < 8; ++i) {
            p[i] = static_cast<uint8_t>(b & 0xff);
            b >>= 8;
        }
    }
    uint32_t st_[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bits_ = 0;
    uint8_t buf_[64]{};
    size_t n_ = 0;
};

// Regular files only. A symlink is followed (the running binary is one). A FIFO is not read.
inline std::string fileMd5(const char* path) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return "unknown";
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return "unknown";
    }
    Md5 md;
    uint8_t buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return "unknown";
        }
        if (n == 0)
            break;
        md.update(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return md.hex();
}

inline std::string selfExeMd5() { return fileMd5("/proc/self/exe"); }

// --- request line ---

struct Target {
    std::string method;
    std::string path;
    bool query = false;
    bool ours = false;
    bool status = false;
    bool update = false;
};

inline Target parseTarget(const std::string& req) {
    Target t;
    const std::string line = req.substr(0, req.find_first_of("\r\n"));
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos || sp1 == 0)
        return t;
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string raw =
        line.substr(sp1 + 1, (sp2 == std::string::npos ? line.size() : sp2) - sp1 - 1);
    t.method = line.substr(0, sp1);
    const size_t q = raw.find_first_of("?#");
    t.query = q != std::string::npos;
    t.path = raw.substr(0, q);
    t.status = t.path == kStatusPath;
    t.update = t.path == kUpdatePath;
    t.ours = t.status || t.update;
    return t;
}

inline std::string headerBlock(const std::string& req) {
    const size_t p = req.find("\r\n\r\n");
    return p == std::string::npos ? req : req.substr(0, p);
}

inline std::string bodyBytes(const std::string& req) {
    const size_t p = req.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : req.substr(p + 4);
}

inline std::string headerField(const std::string& req, const char* name) {
    const std::string headers = headerBlock(req);
    const std::string wanted = name;
    size_t line = headers.find("\r\n");
    if (line == std::string::npos)
        return {};
    line += 2;
    while (line < headers.size()) {
        const size_t end = headers.find("\r\n", line);
        const size_t stop = end == std::string::npos ? headers.size() : end;
        if (stop == line)
            break;
        const size_t colon = headers.find(':', line);
        if (colon != std::string::npos && colon < stop) {
            bool eq = (colon - line) == wanted.size();
            for (size_t i = 0; eq && i < wanted.size(); ++i)
                eq = asciiLower(static_cast<unsigned char>(headers[line + i])) ==
                     asciiLower(static_cast<unsigned char>(wanted[i]));
            if (eq) {
                size_t v = colon + 1;
                while (v < stop && (headers[v] == ' ' || headers[v] == '\t'))
                    ++v;
                return headers.substr(v, stop - v);
            }
        }
        if (end == std::string::npos)
            break;
        line = end + 2;
    }
    return {};
}

inline bool parseContentLength(const std::string& req, size_t* out, bool* present) {
    const std::string v = headerField(req, "Content-Length");
    *present = !v.empty();
    if (v.empty())
        return false;
    if (v.size() > 9)
        return false;
    size_t n = 0;
    for (const unsigned char c : v) {
        if (!std::isdigit(c))
            return false;
        n = n * 10 + static_cast<size_t>(c - '0');
    }
    *out = n;
    return true;
}

inline BodyNeed updateBodyNeed(const std::string& req) {
    BodyNeed need;
    const Target t = parseTarget(req);
    if (!t.update || t.method != "POST" || t.query)
        return need;
    need.read = true;
    bool present = false;
    size_t len = 0;
    const bool ok = parseContentLength(req, &len, &present);
    if (present && !ok) {
        need.tooLarge = true;
        return need;
    }
    if (!present)
        return need;
    if (len > kUpdateBodyCap) {
        need.tooLarge = true;
        return need;
    }
    const size_t have = bodyBytes(req).size();
    if (have < len)
        need.more = len - have;
    return need;
}

// Empty Origin is the LAN curl path (the nonce is the check). A browser Origin must be
// http://<Host> exactly, so another LAN site that can read the page cannot submit the button.
inline bool updateOriginOk(const std::string& req) {
#ifdef MUT_PHONE_STATUS_START_ANY_ORIGIN  // test-only mutant: a foreign Origin may start
    (void)req;
    return true;
#else
    const std::string origin = headerField(req, "Origin");
    if (origin.empty())
        return true;
    const std::string host = headerField(req, "Host");
    if (host.empty())
        return false;
    const std::string pre = "http://";
    if (origin.size() != pre.size() + host.size())
        return false;
    for (size_t i = 0; i < pre.size(); ++i) {
        if (asciiLower(static_cast<unsigned char>(origin[i])) != pre[i])
            return false;
    }
    for (size_t i = 0; i < host.size(); ++i) {
        if (asciiLower(static_cast<unsigned char>(origin[pre.size() + i])) !=
            asciiLower(static_cast<unsigned char>(host[i])))
            return false;
    }
    return true;
#endif
}

inline bool nonceEqual(const std::string& got, const std::string& want) {
    if (!hexString(want, kNonceHex) || !hexString(got, kNonceHex))
        return false;
    unsigned diff = 0;
    for (size_t i = 0; i < kNonceHex; ++i)
        diff |= static_cast<unsigned char>(got[i]) ^ static_cast<unsigned char>(want[i]);
    return diff == 0;
}

inline Reply plain(int code, const char* body) { return Reply{code, "text/plain", body}; }

// The reply for one request, or nullopt when the path is neither status nor update.
inline std::optional<Reply> route(const std::string& req, const Facts& facts) {
    const Target t = parseTarget(req);
    if (!t.ours)
        return std::nullopt;
#ifndef MUT_PHONE_STATUS_ALLOW_QUERY  // test-only mutant: a query or fragment is served
    if (t.query)
        return plain(400, "no parameters\n");
#endif
    if (t.status) {
#ifndef MUT_PHONE_STATUS_ANY_VERB  // test-only mutant: any method is served
        if (t.method != "GET")
            return plain(405, "GET only\n");
#endif
        Reply r;
        r.code = 200;
        r.contentType = kHtmlType;
        r.body = renderPage(facts);
        return r;
    }
#ifndef MUT_PHONE_STATUS_ANY_VERB
    if (t.method != "POST")
        return plain(405, "POST only\n");
#endif
    bool lenPresent = false;
    size_t len = 0;
    const bool lenOk = parseContentLength(req, &len, &lenPresent);
    if (lenPresent && (!lenOk || len > kUpdateBodyCap))
        return plain(400, "body too large\n");
    std::string body = bodyBytes(req);
    if (lenPresent) {
        if (body.size() < len)
            return plain(400, "short body\n");
        body.resize(len);
    } else if (body.size() > kUpdateBodyCap) {
        return plain(400, "body too large\n");
    }
    if (!body.empty() && body.back() == '\n')
        body.pop_back();
    if (!body.empty() && body.back() == '\r')
        body.pop_back();
    if (body.empty())
        return plain(400, "nonce required\n");
    const std::string prefix = "nonce=";
    if (body.compare(0, prefix.size(), prefix) != 0 || body.find('&') != std::string::npos ||
        body.find(' ') != std::string::npos)
        return plain(400, "bad body\n");
    const std::string got = body.substr(prefix.size());
    if (got.empty())
        return plain(400, "nonce required\n");
#ifndef MUT_PHONE_STATUS_SKIP_NONCE  // test-only mutant: any nonce is accepted
    if (!nonceEqual(got, facts.nonce))
        return plain(403, "nonce mismatch\n");
#else
    (void)got;
#endif
#ifndef MUT_PHONE_STATUS_SKIP_BUSY  // test-only mutant: a playing session is not refused
    if (facts.playing)
        return plain(409, "refused: playing\n");
#endif
#ifndef MUT_PHONE_STATUS_NO_START  // test-only mutant: an idle updates-on POST does not ask to start
    if (facts.canUpdate) {
        if (!updateOriginOk(req))
            return plain(403, "origin refused\n");
        Reply r = plain(202, "update started\n");
        r.startUpdate = true;
        return r;
    }
#endif
    return plain(503, "update: not available\n");
}

}  // namespace phone_status
}  // namespace misterplex
