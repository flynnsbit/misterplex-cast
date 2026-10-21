// rc2: LAN helper endpoints (MPX_LAB_HOST + per-service overrides), env > conf, ladder conf keys ignored.
#include "lab_endpoints.hpp"
#include <cstdio>
#include <map>
#include <string>
using namespace misterplex::lab_endpoints;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
using KV = std::map<std::string, std::string>;
static Lookup L(const KV& m) {
    return [m](const char* k) -> std::optional<std::string> {
        auto it = m.find(k);
        if (it == m.end()) return std::nullopt;
        return it->second;
    };
}
static Endpoints R(const KV& env, const KV& conf = {}) { return resolve(L(env), L(conf)); }
int main() {
    auto e = R({});   // nothing set: direct PMS, no xcode, legacy lab menu default
    CHECK(e.ladder.empty() && e.xcode.empty() && e.menu.empty() && e.host.empty());
    e = R({{"MPX_LAB_HOST", "192.0.2.50"}});   // one key -> all three
    CHECK(e.ladder == "http://192.0.2.50:18767");
    CHECK(e.menu == "http://192.0.2.50:18769/menu");
    CHECK(e.xcode == "http://192.0.2.50:18780");
    e = R({{"MPX_LAB_HOST", "http://helper.lan/"}, {"MPX_LAB_XCODE_PORT", "9000"}, {"MPX_LAB_MENU_PORT", "x"}});
    CHECK(e.host == "helper.lan" && e.xcode == "http://helper.lan:9000" && e.menu == "http://helper.lan:18769/menu");
    CHECK(e.notes.size() == 1);
    e = R({{"MPX_LAB_HOST", "evil host;rm -rf"}});   // MUT_LE_ANY_HOST
    CHECK(e.host.empty() && e.ladder.empty() && e.xcode.empty() && !e.notes.empty());
    CHECK(normalizeHost("[fd00::5]") == "[fd00::5]" && normalizeHost("fd00::5").empty());
    e = R({{"MPX_LAB_HOST", "192.0.2.2"}, {"MPX_REMOTE_LADDER_PROXY", "http://l:1"}, {"MPX_MENU_URL", "http://m/menu/a1"},
           {"GCAST_XCODE", "http://x:2"}});
    CHECK(e.ladder == "http://l:1" && e.menu == "http://m/menu/a1" && e.xcode == "http://x:2");
    e = R({{"MPX_LAB_HOST", "192.0.2.2"}, {"MPX_REMOTE_LADDER_PROXY", ""}, {"GCAST_XCODE", ""}});
    CHECK(e.ladder.empty());
    CHECK(e.xcode.empty());   // MUT_LE_XCODE_EMPTY_FALLS_THROUGH
    e = R({{"MPX_LADDER_URL", "http://lu:7"}, {"MPX_REMOTE_LADDER_PROXY", "http://old:1"}, {"MPX_LAB_HOST", "192.0.2.2"}});
    CHECK(e.ladder == "http://lu:7" && e.ladderSrc == "env MPX_LADDER_URL");
    CHECK(R({{"MPX_LADDER_URL", ""}, {"MPX_LAB_HOST", "192.0.2.2"}}).ladder.empty());
    CHECK(logLine(R({})).find("menu=- [") != std::string::npos);
    CHECK(R({{"REMOTE_LADDER_PROXY", "http://old:3"}}).ladder == "http://old:3");
    e = R({}, {{"MPX_REMOTE_LADDER_PROXY", "http://stale:18767"}, {"REMOTE_LADDER_PROXY", "http://stale2"}});
    CHECK(e.ladder.empty());  // MUT_LE_CONF_LADDER
    e = R({}, {{"MPX_LAB_HOST", "192.0.2.60"}});
    CHECK(e.ladder == "http://192.0.2.60:18767" && e.xcode == "http://192.0.2.60:18780");
    e = R({{"MPX_LAB_HOST", "192.0.2.61"}}, {{"MPX_LAB_HOST", "192.0.2.60"}, {"GCAST_XCODE", "http://cx:5"}});
    CHECK(e.host == "192.0.2.61" && e.xcode == "http://cx:5");
    CHECK(R({}, {{"MPX_MENU_URL", "http://cm/menu"}}).menu == "http://cm/menu");
    CHECK(R({{"MPX_MENU_BASE", "http://b:1/"}}).menu == "http://b:1/menu");
    const std::string ll = logLine(R({{"MPX_LAB_HOST", "192.0.2.50"}}));
    CHECK(ll.find("lab_host=192.0.2.50") != std::string::npos && ll.find("[MPX_LAB_HOST]") != std::string::npos);
    std::printf("lab_endpoints: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
