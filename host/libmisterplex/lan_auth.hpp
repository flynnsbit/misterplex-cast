#pragma once
// LAN_AUTH: LAN shared-secret auth for the companion control routes and the
// transcode helper (#62/#63 nits, supersedes #140).
//
// LAN_AUTH (misterplex.conf) / MPX_LAN_AUTH (env wins). Default off. Unset,
// empty, 0, off, false, and no stay off. Off: nothing is checked or sent.
//
// On:
//  - Secret: env MPX_LAN_AUTH_SECRET, else conf LAN_AUTH_SECRET, else the first
//    line of LAN_AUTH_SECRET_FILE (default /media/fat/misterplex/lan_auth.secret).
//    When none exists the daemon generates the file on the card from
//    /dev/urandom (mode 600). Nothing is compiled in. The value is never logged.
//  - Perms: a secret file open to group/other (mode & 077) is refused, EXCEPT
//    on FAT/vfat/exFAT (statfs magic) where modes are synthetic (MiSTer
//    /media/fat reads 755, chmod is a no-op). The card is the only store that
//    persists and survives MiSTer Linux updates, so the secret stays there.
//  - Companion: /misterplex/* control routes need header X-MPX-Auth: <secret>
//    unless the peer is loopback. GET /misterplex/status and GET /misterplex/mast
//    stay open (read-only). Plex companion routes (/player/*, /resources,
//    /timeline, /player/navigation/uiSize, ...) are exempt: the Plex app and
//    Plex Web cannot carry this secret; they keep the existing LAN peer ACL +
//    Origin/Host guard. GH#46 uiSize is companion, not /misterplex.
//  - Helper: the daemon sends X-MPX-Auth on every helper request (health,
//    probe, admit, play URL). The helper refuses a non-loopback peer without it.
//    AirPlay and YouTube go through the daemon, so they keep working.
// Do not set this in STAGE conf.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace misterplex {
namespace lan_auth {

inline constexpr const char* kHeader = "X-MPX-Auth";
inline constexpr const char* kSecretEnv = "MPX_LAN_AUTH_SECRET";
inline constexpr const char* kDefaultSecretFile = "/media/fat/misterplex/lan_auth.secret";

inline bool envOn(const char* e) {
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

// Default off. MPX_LAN_AUTH wins over LAN_AUTH when it is set.
inline bool enabled() {
#ifdef MUT_LAN_AUTH_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_LAN_AUTH"); e && e[0])
        return envOn(e);
    return envOn(std::getenv("LAN_AUTH"));
#endif
}

// 16..128 chars from the URL-token set; CR/LF/space/quotes rejected.
inline bool tokenOk(const std::string& s) {
    if (s.size() < 16 || s.size() > 128)
        return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '~' || c == '-'))
            return false;
    return true;
}

inline bool constEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

inline constexpr unsigned long kMsdosMagic = 0x4d44UL;      // vfat / msdos
inline constexpr unsigned long kExfatMagic = 0x2011BAB0UL;  // kernel exfat

inline bool fsMagicIsFat(unsigned long m) { return m == kMsdosMagic || m == kExfatMagic; }

// Pure policy: is a secret file with this mode on this fs usable?
inline bool permsOk(unsigned mode, bool onFat) {
#ifdef MUT_LAN_AUTH_FAT_RELAX_ALWAYS
    (void)onFat;
    (void)mode;
    return true;  // mutant: relax on every fs
#else
    if (onFat) {
        // FAT has no real modes; chmod cannot fix it. The mode is the mount
        // fmask: 755 is fine, but group/world-writable means the mount lets
        // other users rewrite the secret (#222).
#ifdef MUT_LAN_AUTH_FAT_ALLOW_WW
        return true;
#else
        return (mode & 022u) == 0;
#endif
    }
    return (mode & 077u) == 0;
#endif
}

// Owner policy (#222): root or the daemon's own euid. On FAT the owner is the
// mount uid=, so a card mounted for another user is refused too.
inline bool ownerOk(unsigned uid, unsigned euid) {
#ifdef MUT_LAN_AUTH_OWNER_ANY
    (void)uid;
    (void)euid;
    return true;
#else
    return uid == 0 || uid == euid;
#endif
}

inline bool pathOnFat(const std::string& path) {
    struct statfs fs {};
    if (::statfs(path.c_str(), &fs) != 0)
        return false;
    return fsMagicIsFat(static_cast<unsigned long>(fs.f_type) & 0xffffffffUL);
}

// Which check refused the secret file (#222 nit: the log names it).
enum class SecretFileCheck { Ok, NotRegular, Owner, ModeOffFat, FatWritable };

inline const char* secretFileCheckMsg(SecretFileCheck c) {
    switch (c) {
    case SecretFileCheck::NotRegular: return "missing or not a regular file";
    case SecretFileCheck::Owner: return "owner not root or the daemon user";
    case SecretFileCheck::ModeOffFat: return "mode open to group/other off-FAT (chmod 600)";
    case SecretFileCheck::FatWritable: return "FAT group/other write (fix mount fmask)";
    default: return "ok";
    }
}

inline SecretFileCheck secretFileCheck(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return SecretFileCheck::NotRegular;
    if (!ownerOk(static_cast<unsigned>(st.st_uid), static_cast<unsigned>(::geteuid())))
        return SecretFileCheck::Owner;
    const bool fat = pathOnFat(path);
    if (!permsOk(static_cast<unsigned>(st.st_mode), fat))
        return fat ? SecretFileCheck::FatWritable : SecretFileCheck::ModeOffFat;
    return SecretFileCheck::Ok;
}

inline bool secretFilePermsOk(const std::string& path) {
    return secretFileCheck(path) == SecretFileCheck::Ok;
}

inline std::string readSecretFile(const std::string& path) {
    if (::access(path.c_str(), F_OK) == 0) {
        const SecretFileCheck c = secretFileCheck(path);
        if (c != SecretFileCheck::Ok) {
            std::fprintf(stderr, "misterplexd: LAN_AUTH secret file refused: %s\n", secretFileCheckMsg(c));
            return {};
        }
    }
    std::ifstream in(path);

    std::string line;
    if (!in || !std::getline(in, line))
        return {};
    line = trim(line);
    return tokenOk(line) ? line : std::string();
}

// Generate a 48-hex-char secret on the card. Returns it, or empty on failure.
inline std::string generateSecretFile(const std::string& path) {
    int r = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (r < 0)
        return {};
    unsigned char raw[24];
    const ssize_t got = ::read(r, raw, sizeof raw);
    ::close(r);
    if (got != static_cast<ssize_t>(sizeof raw))
        return {};
    static const char hx[] = "0123456789abcdef";
    std::string s;
    for (unsigned char c : raw) {
        s += hx[c >> 4];
        s += hx[c & 15];
    }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return {};
    const std::string line = s + "\n";
    const bool ok = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    ::close(fd);
    return ok ? s : std::string();
}

// Resolve the secret (env, conf value, file, else generate the file) and
// publish it as MPX_LAN_AUTH_SECRET so helpers on this card see the same one.
// Returns true when a valid secret is active. Only called when enabled().
inline bool adoptSecret(const std::string& confValue, const std::string& filePath, bool generate) {
    if (const char* e = std::getenv(kSecretEnv); e && tokenOk(e))
        return true;
    std::string s = trim(confValue);
    if (!tokenOk(s))
        s = readSecretFile(filePath);
    if (s.empty() && generate)
        s = generateSecretFile(filePath);
    if (!tokenOk(s))
        return false;
    return ::setenv(kSecretEnv, s.c_str(), 1) == 0;
}

inline std::string secret() {
    const char* e = std::getenv(kSecretEnv);
    return (e && tokenOk(e)) ? std::string(e) : std::string();
}

// Companion route policy. path excludes the query string.
inline bool companionRouteNeedsAuth(const std::string& method, const std::string& path) {
#ifdef MUT_LAN_AUTH_WRONG_CONTEXT
    // mutant: gates Plex companion routes (breaks Plex app casting), skips ours
    (void)method;
    return path.compare(0, 8, "/player/") == 0;
#else
    if (path.compare(0, 11, "/misterplex") != 0)
        return false;  // Plex companion protocol: exempt (Plex app / Plex Web)
    if (method == "GET" && (path == "/misterplex/status" || path == "/misterplex/mast"))
        return false;  // read-only status
    return true;
#endif
}

// One request. headerValue is the X-MPX-Auth value ("" when missing).
// Returns 0 allowed, 401 missing header, 403 wrong header.
inline int companionCheck(bool on, const std::string& sec, bool peerLoopback, const std::string& method,
                          const std::string& path, const std::string& headerValue) {
#ifdef MUT_LAN_AUTH_OFF_STILL_BINDS
    on = true;
#endif
#ifdef MUT_LAN_AUTH_ON_IGNORED
    on = false;
#endif
    if (!on || peerLoopback || !companionRouteNeedsAuth(method, path))
        return 0;
    if (headerValue.empty())
        return 401;
    if (sec.empty() || !constEq(headerValue, sec))
        return 403;
    return 0;
}

// "X-MPX-Auth: <secret>\r\n" for daemon->helper requests; empty when off.
inline std::string helperWireLine() {
    if (!enabled())
        return {};
    const std::string s = secret();
    return s.empty() ? std::string() : std::string(kHeader) + ": " + s + "\r\n";
}

}  // namespace lan_auth
}  // namespace misterplex
