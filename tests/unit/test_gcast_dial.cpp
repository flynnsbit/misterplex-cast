// Unit tests for gcast/dial_proto.hpp. Mutants: MUT_DIAL_ANY_ST, MUT_DIAL_NO_BODYCAP.
#include "gcast/dial_proto.hpp"
#include <cstdio>
using namespace misterplex::gcast::dial;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    std::string ms = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 9\r\nST: urn:dial-multiscreen-org:service:dial:1\r\n\r\n";
    CHECK(dialSearchMx(ms) == 5);
    CHECK(dialSearchMx("M-SEARCH * HTTP/1.1\r\nMAN: \"ssdp:discover\"\r\nST: ssdp:all\r\n\r\n") == 1);
    CHECK(dialSearchMx("M-SEARCH * HTTP/1.1\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n") == 0);
    CHECK(dialSearchMx("NOTIFY * HTTP/1.1\r\nNT: x\r\n\r\n") == 0);
    CHECK(dialSearchMx("garbage") == 0);
    CHECK(dialSearchMx(std::string(9000, 'A')) == 0);
    std::string r = ssdpResponse("192.0.2.12", 8008, "u-1", 7);
    CHECK(r.find("LOCATION: http://192.0.2.12:8008/dd.xml\r\n") != std::string::npos && r.find("USN: uuid:u-1::urn:dial") != std::string::npos);
    CHECK(deviceDescXml("A<b>&\"", "u").find("A&lt;b&gt;&amp;&quot;") != std::string::npos);

    Ctx c; c.name = "MiSTer"; c.uuid = "u-1"; c.screenId = "scr"; c.appUrl = "http://1.2.3.4:8008/apps/";
    Head h; CHECK(parseHead("GET /dd.xml HTTP/1.1\r\nHost: x\r\n\r\n", h));
    Reply rp = handle(h, "", c); CHECK(rp.status == 200 && rp.applicationUrl == c.appUrl && rp.body.find("<friendlyName>MiSTer<") != std::string::npos);
    Head g; parseHead("GET /apps/YouTube HTTP/1.1\r\n\r\n", g); rp = handle(g, "", c);
    CHECK(rp.status == 200 && rp.body.find("<state>stopped</state>") != std::string::npos && rp.body.find("<screenId>scr</screenId>") != std::string::npos);
    Head p; parseHead("POST /apps/YouTube HTTP/1.1\r\nContent-Length: 40\r\n\r\n", p);
    rp = handle(p, "pairingCode=abc-123&v=dQw4w9WgXcQ&t=12.5&theme=cl", c);
    CHECK(rp.status == 201 && rp.launch && rp.pairingCode == "abc-123" && rp.videoId == "dQw4w9WgXcQ" && rp.startMs == 12500);
    CHECK(rp.location == "http://1.2.3.4:8008/apps/YouTube/run");
    rp = handle(p, "v=../../etc&t=-5", c); CHECK(rp.launch && rp.videoId.empty() && rp.startMs == 0);
    rp = handle(p, std::string(5000, 'x'), c); CHECK(rp.status == 413 && !rp.launch);
    Head d; parseHead("DELETE /apps/YouTube/run HTTP/1.1\r\n\r\n", d); c.running = true; rp = handle(d, "", c); CHECK(rp.status == 200 && rp.stop);
    Ctx cold = c; cold.youtubeReady = false; cold.running = true;
    rp = handle(p, "pairingCode=abc-123&v=dQw4w9WgXcQ&t=12.5", cold);
    CHECK(rp.status == 404 && !rp.launch && rp.videoId.empty() && rp.pairingCode.empty());
    rp = handle(g, "", cold); CHECK(rp.status == 404 && rp.body.empty());
    rp = handle(d, "", cold); CHECK(rp.status == 404 && !rp.stop);
    rp = handle(h, "", cold); CHECK(rp.status == 200 && rp.body.find("<friendlyName>MiSTer<") != std::string::npos);
    Head z; parseHead("GET /etc/passwd HTTP/1.1\r\n\r\n", z); CHECK(handle(z, "", c).status == 404);
    Head bad; CHECK(!parseHead("GET /\r\n\r\n", bad)); CHECK(!parseHead("GET / FTP/1\r\n\r\n", bad));
    auto f = formDecode("a=1+2&b=%41%zz&&c"); CHECK(f["a"] == "1 2" && f["b"] == "A%zz" && f.count("c"));
    std::printf("test_gcast_dial: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
