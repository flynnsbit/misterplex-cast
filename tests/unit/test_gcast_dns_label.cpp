// GCAST: AirPlay/RAOP instance name sanitized into one DNS label (release a lab MiSTer: dotted --name split by mdnsd).
#include "gcast/dns_label.hpp"
#include <cstdio>
#include <string>
using namespace misterplex::gcast;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool singleLabel(const std::string& s) { return s.find('.') == std::string::npos && !s.empty() && s.size() <= 63; }
int main() {
    CHECK(airplayInstanceName("MiSTerPlex-192.0.2.110") == "MiSTerPlex-192-0-2-110");   // MUT_DNSL_KEEP_DOTS
    CHECK(airplayInstanceName("MiSTerPlex-192.0.2.12") == "MiSTerPlex-192-0-2-229");
    CHECK(airplayInstanceName("Living Room") == "Living Room");
    CHECK(airplayInstanceName("mister_1") == "mister_1");
    CHECK(airplayInstanceName("a.b") == "a-b");
    CHECK(airplayInstanceName("x/y:z@w\\q") == "x-y-z-w-q");
    CHECK(airplayInstanceName("caf\xc3\xa9") == "caf");           // non-ASCII -> '-', trailing '-' trimmed
    CHECK(airplayInstanceName("..mister..") == "mister");
    CHECK(airplayInstanceName("") == "MiSTerPlex");
    CHECK(airplayInstanceName("...") == "MiSTerPlex");
    CHECK(airplayInstanceName("  ") == "MiSTerPlex");
    CHECK(airplayInstanceName("MiSTer.local") == "MiSTer-local");
    // Cap: name <= 50 so "<12hex>@name" (RAOP) is <= 63.                                MUT_DNSL_NO_CAP
    const std::string longName(80, 'a');
    CHECK(airplayInstanceName(longName).size() == kAirplayNameMax);
    CHECK(kAirplayNameMax + kRaopPrefixBytes == 63);
    CHECK(sanitizeDnsLabel(longName).size() == 63);
    CHECK(sanitizeDnsLabel(std::string(49, 'b') + ".c") == std::string(49, 'b') + "-c");
    CHECK(airplayInstanceName(std::string(50, 'd') + "-e") == std::string(50, 'd'));  // trailing '-' after cut
    for (const char* n : {"MiSTerPlex-192.0.2.5", "a.b.c.d.e.f", "x", "Ünïcødé.name.local"})
        CHECK(singleLabel(airplayInstanceName(n)) && singleLabel("0123456789ab@" + airplayInstanceName(n)));
    std::printf("gcast_dns_label: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
