// mpx::caBundlePath() is header-only. First non-empty regular file wins:
//   $MPX_CA_BUNDLE, the shipped bundle, the distro store, then the two
//   files stock MiSTer (a lab MiSTer) actually has (cacert.pem and cert.pem).
// An empty file is skipped. kCaBundlePaths / caBundlePathPick are the
// same helpers the plexui-link-signin tests call.
#include "ca_bundle.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

static int g_fails = 0;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                              \
            std::fprintf(stderr, __VA_ARGS__);                                                     \
            std::fprintf(stderr, "\n");                                                            \
        }                                                                                          \
    } while (0)

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const std::string hdr = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/ca_bundle.hpp");
    const char* order[] = {
        "MPX_CA_BUNDLE",
        "/media/fat/linux/misterplex-cacert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/certs/cacert.pem",
        "/etc/ssl/cert.pem",
    };
    size_t at = 0;
    for (const char* s : order) {
        const size_t found = hdr.find(s, at);
        CHECK(found != std::string::npos, "missing %s", s);
        if (found != std::string::npos)
            at = found + std::strlen(s);
    }
    CHECK(hdr.find("ca_bundle.cpp") == std::string::npos, "ca_bundle.cpp must not come back");
    CHECK(hdr.find("inline std::string caBundlePath()") != std::string::npos, "inline caBundlePath");
    CHECK(hdr.find("kCaBundlePaths") != std::string::npos, "kCaBundlePaths");
    CHECK(hdr.find("caBundlePathPick") != std::string::npos, "caBundlePathPick");
    CHECK(hdr.find("S_ISREG") != std::string::npos && hdr.find("st_size > 0") != std::string::npos,
          "usable file is a non-empty regular file");
    CHECK(hdr.find("access(") == std::string::npos, "access(R_OK) must not select the bundle");

    const char* saved = std::getenv("MPX_CA_BUNDLE");
    const std::string savedCopy = saved ? saved : "";
    char tmpl[] = "/tmp/mpx-ca-XXXXXX";
    const int fd = ::mkstemp(tmpl);
    CHECK(fd >= 0, "mkstemp");
    if (fd >= 0)
        ::close(fd);
    ::setenv("MPX_CA_BUNDLE", tmpl, 1);
    CHECK(!mpx::caBundleFileUsable(tmpl), "empty env file is not usable");
    const char* rest[] = {
        "/media/fat/linux/misterplex-cacert.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/certs/cacert.pem",
        "/etc/ssl/cert.pem",
    };
    std::string expect;
    for (const char* p : rest) {
        if (mpx::caBundleFileUsable(p)) {
            expect = p;
            break;
        }
    }
    CHECK(mpx::caBundlePath() == expect, "empty MPX_CA_BUNDLE is skipped, got %s want %s",
          mpx::caBundlePath().c_str(), expect.c_str());
    {
        std::ofstream filled(tmpl);
        filled << "cert\n";
    }
    CHECK(mpx::caBundleFileUsable(tmpl), "non-empty env file is usable");
    CHECK(mpx::caBundlePath() == tmpl, "env path wins, got %s", mpx::caBundlePath().c_str());

    ::setenv("MPX_CA_BUNDLE", "/no/such/misterplex-ca.pem", 1);
    CHECK(mpx::caBundlePath() == expect, "missing env falls through, got %s want %s",
          mpx::caBundlePath().c_str(), expect.c_str());
    ::unsetenv("MPX_CA_BUNDLE");
    CHECK(mpx::caBundlePath() == expect, "unset env");
    ::setenv("MPX_CA_BUNDLE", "", 1);
    CHECK(mpx::caBundlePath() == expect, "empty env");

    char shipDir[] = "/tmp/mpx-ca-ship-XXXXXX";
    CHECK(::mkdtemp(shipDir) != nullptr, "mkdtemp");
    const std::string emptyShip = std::string(shipDir) + "/misterplex-cacert.pem";
    const std::string nextCand = std::string(shipDir) + "/ca-certificates.crt";
    {
        std::ofstream empty(emptyShip);
        std::ofstream next(nextCand);
        next << "cert\n";
    }
    const char* shipOrder[] = {emptyShip.c_str(), nextCand.c_str()};
    const std::string skipped = mpx::caBundlePathPick(nullptr, shipOrder, 2, mpx::caBundleFileUsable);
    CHECK(skipped == nextCand, "empty misterplex-cacert.pem falls through, got %s", skipped.c_str());
    ::unlink(emptyShip.c_str());
    ::unlink(nextCand.c_str());
    ::rmdir(shipDir);
    ::unlink(tmpl);
    if (saved)
        ::setenv("MPX_CA_BUNDLE", savedCopy.c_str(), 1);
    else
        ::unsetenv("MPX_CA_BUNDLE");

    if (g_fails) {
        std::fprintf(stderr, "test_ca_bundle: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("test_ca_bundle: OK\n");
    return 0;
}
