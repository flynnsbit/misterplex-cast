// GCAST P6: deterministic mutation fuzzer for the gcast parsers (no libFuzzer on the box; g++ ASan/UBSan).
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iarm/misterplexd -Ihost \
//       tests/gcast/fuzz_parsers.cpp -o /tmp/fuzz && FUZZ_ITERS=200000 FUZZ_SEED=1 /tmp/fuzz
// Targets: lounge ChunkParser/decodeBatch/toEvent, mini_json, dial parseHead/handle/dialSearchMx/formDecode,
// http parseRespHead/ChunkedDecoder, cast_resolve ytCastId/planCast/parseProbe/titleFromUrl, gcast_osd.
// Invariants beyond "no sanitizer report": framed chunks <= kMaxChunk, events carry only valid video ids,
// dial replies only launch a valid id, osd text stays printable and capped.
#include "gcast/cast_resolve.hpp"
#include "gcast/dial_proto.hpp"
#include "gcast/http_util.hpp"
#include "gcast/lounge_proto.hpp"
#include "gcast/mini_json.hpp"
#include "libmisterplex/gcast_osd.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace misterplex::gcast;
static uint64_t rs = 1;
static uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static size_t rn(size_t n) { return n ? rnd() % n : 0; }
static long violations = 0;
#define INV(c) do { if (!(c)) { ++violations; std::fprintf(stderr, "INVARIANT %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static const char* kTokens[] = {"[", "]", "{", "}", ",", ":", "\"", "\\u0000", "\\ud800", "1e999", "-", "null",
    "true", "\r\n", "\n", "0", "ffffffff", "99999999", ";", "%", "%zz", "&", "=", "?", "yt:", "v=", "videoId",
    "setPlaylist", "seekTo", "currentTime", "dQw4w9WgXcQ", "Content-Length: ", "Transfer-Encoding: chunked",
    "HTTP/1.1 200 OK", "POST /apps/YouTube HTTP/1.1", "MX: ", "ST: urn:dial-multiscreen-org:service:dial:1"};
static std::string mutate(std::string s) {
    const int ops = 1 + static_cast<int>(rn(8));
    for (int i = 0; i < ops; ++i) {
        switch (rn(7)) {
        case 0: if (!s.empty()) s[rn(s.size())] ^= static_cast<char>(1u << rn(8)); break;
        case 1: s.insert(rn(s.size() + 1), 1, static_cast<char>(rn(256))); break;
        case 2: if (!s.empty()) s.erase(rn(s.size()), 1 + rn(8)); break;
        case 3: s.insert(rn(s.size() + 1), kTokens[rn(sizeof kTokens / sizeof *kTokens)]); break;
        case 4: if (!s.empty()) s.resize(rn(s.size())); break;
        case 5: if (!s.empty()) { size_t a = rn(s.size()); s.insert(rn(s.size() + 1), s.substr(a, rn(64))); } break;
        case 6: if (s.size() < 1 << 16) s.insert(rn(s.size() + 1), std::string(rn(4096), static_cast<char>(rn(256)))); break;
        }
    }
    return s;
}
static const std::vector<std::string> kLounge = {
    "[[0,[\"c\",\"SID1\",\"\",8]],[1,[\"S\",\"GS1\"]],[2,[\"setPlaylist\",{\"videoId\":\"dQw4w9WgXcQ\",\"currentTime\":\"12.5\",\"videoIds\":\"dQw4w9WgXcQ,aqz-KE-bpKQ\",\"currentIndex\":\"0\",\"listId\":\"RQ1\"}]]]",
    "[[3,[\"seekTo\",{\"newTime\":\"120\"}]],[4,[\"pause\"]],[5,[\"play\"]],[6,[\"stopVideo\"]],[7,[\"noop\"]]]",
    "[[8,[\"remoteConnected\",{\"name\":\"Pixel \\u00e9\",\"app\":\"android\"}]],[9,[\"setVolume\",{\"volume\":\"50\"}]]]"};
static const std::vector<std::string> kDial = {
    "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\nST: urn:dial-multiscreen-org:service:dial:1\r\n\r\n",
    "GET /dd.xml HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /apps/YouTube HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 40\r\n\r\n",
    "GET /apps/YouTube HTTP/1.1\r\nOrigin: https://www.youtube.com\r\n\r\n",
    "DELETE /apps/YouTube/run HTTP/1.1\r\n\r\n"};
static const std::vector<std::string> kDialBody = {"pairingCode=abc-123&v=dQw4w9WgXcQ&t=30", "v=bad&t=-1", ""};
static const std::vector<std::string> kHttp = {
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nX-A: b\r\n\r\n",
    "HTTP/1.1 400 Unknown SID\r\nContent-Length: 0\r\n\r\n"};
static const std::vector<std::string> kChunked = {"5\r\nhello\r\n0\r\n\r\n", "a;ext=1\r\n0123456789\r\n0\r\n\r\n"};
static const std::vector<std::string> kUrls = {"yt:dQw4w9WgXcQ", "http://192.0.2.5:8000/a/b%20c.mp4?x=1",
    "https://cdn.example/x.m3u8", "yt:../../etc", "file:///etc/passwd", "rtsp://h/s", ""};
static const std::vector<std::string> kProbe = {"ok=1 w=1920 h=1080 vcodec=h264 audio=1 dur=212000 title=Big%20Buck",
    "{\"ok\":1,\"w\":640,\"h\":360,\"title\":\"x\"}"};
template <class V> static const std::string& pick(const V& v) { return v[rn(v.size())]; }
static void fuzzLounge(const std::string& in) {
    lounge::ChunkParser cp;   // framed: "<len>\n<payload>", fed in random splits
    std::string framed = std::to_string(in.size()) + "\n" + in;
    if (rn(4) == 0) framed = mutate(framed);
    std::vector<std::string> chunks;
    for (size_t i = 0; i < framed.size();) {
        size_t n = 1 + rn(64);
        if (!cp.feed(framed.data() + i, std::min(n, framed.size() - i), chunks)) break;
        i += n;
    }
    for (auto& c : chunks) INV(c.size() <= lounge::kMaxChunk);
    chunks.push_back(in);
    for (auto& c : chunks) {
        std::vector<lounge::Message> msgs;
        lounge::decodeBatch(c, msgs);
        lounge::Channel ch;
        for (auto& m : msgs) {
            ch.accept(m);
            lounge::Event e = lounge::toEvent(m);
            if (e.kind == lounge::Ev::Play) INV(lounge::validVideoId(e.videoId));
            if (e.kind == lounge::Ev::Seek || e.kind == lounge::Ev::Play) INV(e.ms >= 0);
        }
    }
}
static void fuzzJson(const std::string& in) { Json j; parseJson(in, j); (void)j.scalar(); (void)j.get("a"); }
static void fuzzDial(const std::string& head, const std::string& body) {
    dial::Head h;
    if (dial::parseHead(head, h)) {
        dial::Ctx c{"MiSTerPlex", "uuid-1", "screen-1", "http://192.0.2.2:8008/apps/", rn(2) == 0};
        dial::Reply r = dial::handle(h, body, c);
        if (r.launch) INV(dial::validVid(r.videoId) || r.videoId.empty());
        INV(r.startMs >= 0);
    }
    (void)dial::dialSearchMx(head);
    (void)dial::formDecode(body);
}
static void fuzzHttp(const std::string& head, const std::string& chunked) {
    http::RespHead r;
    http::parseRespHead(head, r);
    http::ChunkedDecoder d;
    std::string out;
    for (size_t i = 0; i < chunked.size();) {
        size_t n = 1 + rn(32);
        if (!d.feed(chunked.data() + i, std::min(n, chunked.size() - i), out)) break;
        i += n;
    }
    INV(out.size() <= chunked.size());
}
static void fuzzCast(const std::string& url, const std::string& probe) {
    std::string id;
    if (ytCastId(url, id)) INV(lounge::validVideoId(id));
    CastPlan p = planCast(url, rn(2) == 0, rn(2) ? "http://192.0.2.3:18780" : "", 624, 480, 2500,
                          static_cast<int64_t>(rn(1000000)));
    (void)p;
    ProbeInfo pi = parseProbe(probe);
    const std::string np = misterplex::gcast_osd::gcastNowPlaying(rn(2) ? "YouTube" : "AirPlay",
                                                                 !pi.title.empty() ? pi.title : titleFromUrl(url, "AirPlay"));
    INV(np.size() <= 60);
    for (unsigned char c : np) INV(c >= 0x20 && c < 0x7f);
}
int main() {
    const char* it = std::getenv("FUZZ_ITERS");
    const char* sd = std::getenv("FUZZ_SEED");
    const long iters = it ? std::atol(it) : 20000;
    rs = sd ? std::strtoull(sd, nullptr, 10) * 2 + 1 : 1;
    const unsigned long long seed0 = rs;
    for (long i = 0; i < iters; ++i) {
        fuzzLounge(mutate(pick(kLounge)));
        fuzzJson(mutate(pick(kLounge)));
        fuzzDial(mutate(pick(kDial)), mutate(pick(kDialBody)));
        fuzzHttp(mutate(pick(kHttp)), mutate(pick(kChunked)));
        fuzzCast(mutate(pick(kUrls)), mutate(pick(kProbe)));
    }
    std::printf("fuzz_parsers iters=%ld seed=%llu violations=%ld\n", iters, seed0, violations);
    return violations ? 1 : 0;
}
