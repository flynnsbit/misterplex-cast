#include "companion.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    using misterplex::PlayRequest;
    using misterplex::titlePlayPolicy;

    PlayRequest lib;
    lib.key = "/library/metadata/9";
    lib.protocol = "http";
    require(titlePlayPolicy(lib) == "PROXY", "/library protocol=http must be PROXY");
    lib.protocol = "https";
    require(titlePlayPolicy(lib) == "PROXY", "/library protocol=https must be PROXY");
    lib.protocol = "inline";
    require(titlePlayPolicy(lib) == "PROXY", "/library protocol=inline must be PROXY");

    PlayRequest t;
    t.key = "testsrc";
    t.protocol = "http";
    require(titlePlayPolicy(t) == "DIRECT", "testsrc must be DIRECT");
    t.key = "test";
    require(titlePlayPolicy(t) == "DIRECT", "test must be DIRECT");

    PlayRequest url;
    url.key = "http://127.0.0.1/media.mkv";
    url.protocol = "http";
    require(titlePlayPolicy(url) == "DIRECT", "http URL key must be DIRECT");
    url.key = "https://example/x";
    require(titlePlayPolicy(url) == "DIRECT", "https URL key must be DIRECT");

    PlayRequest path;
    path.key = "/media/fat/video.mkv";
    path.protocol = "http";
    require(titlePlayPolicy(path) == "DIRECT", "local path must be DIRECT");

    PlayRequest pq;
    pq.key = "/playQueues/3";
    require(titlePlayPolicy(pq) == "PROXY", "playQueues must be PROXY");

    std::printf("test_companion_title_policy: OK\n");
    return 0;
}
