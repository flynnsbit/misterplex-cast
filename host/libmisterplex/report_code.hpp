#pragma once
// FEATURE-GAP E16: a couch "send a report" short code.
//
// REPORT_CODE (misterplex.conf) / MPX_REPORT_CODE (env wins). Default off.
// Unset, empty, 0, off, false, and no stay off.
//
// Off: the Ready-card hint is unchanged, the phone status page is unchanged,
// and no pack is written.
// On: one short code is minted for the boot. The OwenB Ready card gains a
// "Report CODE" row (HDMI, no phone). The phone status page shows the same
// code. A redacted copy of the daemon log is written under the report
// directory. Nothing is uploaded and no new secret is required.
//
// The R1 logo stamp keeps the base ready line. The OwenB card is the row
// that splits on newlines.

#include "../../arm/misterplexd/log_redact.hpp"
#include "plex_link_label.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {

inline bool reportCodeEnvOn(const char* e) {
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

// Default off. MPX_REPORT_CODE wins over REPORT_CODE when it is set.
inline bool reportCodeEnabled() {
#ifdef MUT_REPORT_CODE_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_REPORT_CODE"); e && e[0])
        return reportCodeEnvOn(e);
    return reportCodeEnvOn(std::getenv("REPORT_CODE"));
#endif
}

// 32 symbols, no 0/O/1/I/L. Six characters, deterministic from seed.
inline constexpr char kReportAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

inline bool validReportCode(const std::string& code) {
    if (code.size() != 6)
        return false;
    for (const unsigned char c : code) {
        if (std::strchr(kReportAlphabet, static_cast<char>(c)) == nullptr)
            return false;
    }
    return true;
}

inline std::string reportShortCode(std::uint64_t seed) {
#ifdef MUT_REPORT_CODE_EMPTY
    (void)seed;
    return {};
#else
    std::uint64_t x = seed ^ 0xA5A5A5A5A5A5A5A5ull;
    std::string out(6, '2');
    for (int i = 0; i < 6; ++i) {
        x = x * 0x9E3779B97F4A7C15ull + 0xBF58476D1CE4E5B9ull;
        out[static_cast<std::size_t>(i)] = kReportAlphabet[(x >> 59) & 31u];
    }
    return out;
#endif
}

inline std::string& bootReportSlot() {
    static std::string code;
    return code;
}

inline std::string bootReportCode() { return bootReportSlot(); }

// Flag off, or a code that is not six alphabet characters: hint unchanged.
// Flag on: append "Report CODE" as its own row. Empty hint becomes "Ready".
inline std::string reportReadyHint(const std::string& hint, const std::string& code) {
    if (!reportCodeEnabled() || !validReportCode(code))
        return hint;
#ifdef MUT_REPORT_CODE_NO_HINT
    return hint;
#else
    // #228: the plex.tv/link row owns the card. A second, six-character "Report" code next to
    // the four-character PIN reads as the link code, so the report row stays off that screen.
#ifndef MUT_PLEXLINK_REPORT_OVER_LINK
    if (hint.find(kPlexTvLinkLabel) != std::string::npos)
        return hint;
#endif
#ifndef MUT_PLEXLINK_REPORT_OVER_ERROR
    if (hint.find(kPlexTvLinkErrorLine) != std::string::npos)   // the error row owns it too
        return hint;
#endif
    const std::string row = std::string("Report ") + code;
    if (hint.find(row) != std::string::npos)
        return hint;
    if (hint.empty())
        return std::string("Ready\n") + row;
    return hint + "\n" + row;
#endif
}

inline std::string reportPhoneDl(const std::string& code) {
#ifdef MUT_REPORT_CODE_NO_PHONE
    (void)code;
    return {};
#else
    if (!validReportCode(code))
        return {};
    return "<dt>report</dt><dd>" + code + "</dd>\n";
#endif
}

inline std::string reportPhonePre(const std::string& code) {
#ifdef MUT_REPORT_CODE_NO_PHONE
    (void)code;
    return {};
#else
    if (!validReportCode(code))
        return {};
    return "report=" + code + "\n";
#endif
}

// Empty when there is no code, so the status log keeps its eleven fields.
inline std::string reportPhoneLog(const std::string& code) {
#ifdef MUT_REPORT_CODE_NO_PHONE
    (void)code;
    return {};
#else
    if (!validReportCode(code))
        return {};
    return "\nreport=" + code;
#endif
}

struct ReportSource {
    std::string name;
    std::string body;
};

struct ReportPack {
    std::string code;
    std::string path;
    bool ok = false;
};

inline bool reportEmailChar(unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

inline std::string redactReportEmails(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '@')
            continue;
        std::size_t b = i;
        while (b > 0 && reportEmailChar(static_cast<unsigned char>(s[b - 1])))
            --b;
        if (b == i)
            continue;
        std::size_t e = i + 1;
        bool dot = false;
        while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '.' || s[e] == '-')) {
            if (s[e] == '.')
                dot = true;
            ++e;
        }
        if (!dot || e < i + 4)
            continue;
        s.replace(b, e - b, "REDACTED");
        i = b + 8;
    }
    return s;
}

inline std::string redactReportBody(std::string s) {
#ifdef MUT_REPORT_CODE_SKIP_REDACT
    return s;
#else
    return redactReportEmails(redactUrlTokens(std::move(s)));
#endif
}

inline std::string readReportTail(const std::string& path, std::size_t cap) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ::close(fd);
        return {};
    }
    const off_t sz = st.st_size;
    off_t start = 0;
    if (static_cast<unsigned long long>(sz) > cap)
        start = sz - static_cast<off_t>(cap);
    if (::lseek(fd, start, SEEK_SET) < 0) {
        ::close(fd);
        return {};
    }
    std::string s;
    s.resize(static_cast<std::size_t>(sz - start));
    std::size_t got = 0;
    while (got < s.size()) {
        const ssize_t n = ::read(fd, &s[got], s.size() - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return {};
        }
        if (n == 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    s.resize(got);
    if (start > 0) {
        const auto nl = s.find('\n');
        if (nl == std::string::npos)
            return {};
        s.erase(0, nl + 1);
    }
    return s;
}

inline bool ensureReportDir(const std::string& dir) {
    if (dir.empty() || dir.find("..") != std::string::npos)
        return false;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    struct stat st;
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool writeReportFile(const std::string& path, const std::string& body) {
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0)
        return false;
    std::size_t off = 0;
    while (off < body.size()) {
        const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
}

inline std::string reportPackDir() {
    if (const char* e = std::getenv("MPX_REPORT_DIR"); e && e[0])
        return e;
    return "/media/fat/misterplex/reports";
}

inline std::vector<std::string> reportLogPaths() {
    std::vector<std::string> paths;
    if (const char* e = std::getenv("MPX_REPORT_LOG"); e && e[0])
        paths.emplace_back(e);
    paths.emplace_back("/media/fat/misterplex/misterplexd.log");
    return paths;
}

inline std::uint64_t reportSeed() {
    if (const char* e = std::getenv("MPX_REPORT_SEED"); e && e[0]) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(e, &end, 10);
        if (end && end != e && *end == '\0' && v != 0)
            return static_cast<std::uint64_t>(v);
    }
    std::uint64_t s = 0;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::size_t got = 0;
        while (got < sizeof s) {
            const ssize_t n = ::read(fd, reinterpret_cast<char*>(&s) + got, sizeof s - got);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            got += static_cast<std::size_t>(n);
        }
        ::close(fd);
        if (got == sizeof s && s != 0)
            return s;
    }
    const std::uint64_t t = static_cast<std::uint64_t>(std::time(nullptr));
    const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
    return (t << 32) ^ (pid * 0x9E3779B97F4A7C15ull) ^ 0xC0FFEEULL;
}

// Flag off: clears the boot code and writes nothing.
// Flag on: mints the code, then writes one redacted pack. The code stays set
// when the directory cannot be created, so the card can still show it.
inline ReportPack prepareBootReport() {
    ReportPack p;
    if (!reportCodeEnabled()) {
        bootReportSlot().clear();
        return p;
    }
    p.code = reportShortCode(reportSeed());
    if (!validReportCode(p.code)) {
        bootReportSlot().clear();
        return p;
    }
    bootReportSlot() = p.code;
#ifdef MUT_REPORT_CODE_NO_PACK
    return p;
#else
    std::string body = "misterplex report\ncode=" + p.code + "\nredacted=1\n";
    int sources = 0;
    for (const std::string& path : reportLogPaths()) {
        const std::string tail = readReportTail(path, 256u * 1024u);
        if (tail.empty())
            continue;
        body += "===== ";
        body += redactReportBody(path);
        body += " =====\n";
        body += redactReportBody(tail);
        if (body.empty() || body.back() != '\n')
            body.push_back('\n');
        ++sources;
    }
    if (sources == 0)
        body += "sources=0\n";
    std::string dir = reportPackDir();
    if (!ensureReportDir(dir)) {
        if (std::getenv("MPX_REPORT_DIR") != nullptr)
            return p;
        dir = "/tmp/misterplex-reports";
        if (!ensureReportDir(dir))
            return p;
    }
    p.path = dir;
    if (p.path.back() != '/')
        p.path.push_back('/');
    p.path += p.code;
    p.path += ".txt";
    p.ok = writeReportFile(p.path, body);
    if (!p.ok)
        p.path.clear();
    return p;
#endif
}

} // namespace misterplex
