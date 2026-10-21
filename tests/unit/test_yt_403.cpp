// One 403 re-resolve, then play. A second 403 does not ask again.
#include "gcast/yt_403.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            ++g_fail;                                                                              \
            std::cerr << "FAIL " << __LINE__ << " " << #c << "\n";                                 \
        }                                                                                          \
    } while (0)

int main() {
    CHECK(misterplex::gcast::ytOpenIs403("Server returned 403 Forbidden"));
    CHECK(!misterplex::gcast::ytOpenIs403("connection timed out"));
    CHECK(misterplex::gcast::yt403ShouldReresolve("HTTP 403", 0));
    CHECK(!misterplex::gcast::yt403ShouldReresolve("HTTP 403", 1));
    CHECK(!misterplex::gcast::yt403ShouldReresolve("404", 0));

    int calls = 0;
    std::string url = "https://example.invalid/old";
    std::string audio;
    auto cb = [&](std::string& u, std::string& a) {
        ++calls;
        u = "https://example.invalid/new";
        a = "https://example.invalid/audio";
        return true;
    };
    std::string err = "Server returned 403 Forbidden";
    CHECK(misterplex::gcast::yt403ShouldReresolve(err, 0));
    CHECK(cb(url, audio));
    CHECK(url == "https://example.invalid/new");
    CHECK(!misterplex::gcast::yt403ShouldReresolve(err, calls));

    std::ifstream in("host/libmisterplex/owenb_session.cpp");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    CHECK(src.find("yt403ShouldReresolve") != std::string::npos);
    CHECK(src.find("redactUrlTokens(req.url)") != std::string::npos);
    const auto raw = src.find("\"open \" + req.url");
    CHECK(raw == std::string::npos);
    std::ifstream mp("arm/misterplexd/media_player.cpp");
    std::stringstream mps;
    mps << mp.rdbuf();
    const std::string player = mps.str();
    CHECK(player.find("castOpenFailEndsIdle") != std::string::npos);
    CHECK(player.find("ui.clearError()") != std::string::npos);
    CHECK(player.find("castOpenIdleOnFail_ = true") != std::string::npos);

    if (g_fail) {
        std::cerr << "test_yt_403: " << g_fail << " failed\n";
        return 1;
    }
    std::cout << "test_yt_403: ok\n";
    return 0;
}
