// Unit tests for gcast/http_util.hpp. Mutant: MUT_HTTP_NO_CHUNKCAP.
#include "gcast/http_util.hpp"
#include <cstdio>
using namespace misterplex::gcast::http;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    RespHead h;
    CHECK(parseRespHead("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nSet-Cookie: a\r\nset-cookie: b\r\nTransfer-Encoding: chunked", h));
    CHECK(h.status == 200 && h.reason == "OK" && h.get("content-type") == "text/plain" && h.get("set-cookie") == "a, b");
    RespHead u; CHECK(parseRespHead("HTTP/1.1 400 Unknown SID", u) && u.status == 400 && u.reason == "Unknown SID");
    RespHead b; CHECK(!parseRespHead("HTTP/1.1 2x0 OK", b)); CHECK(!parseRespHead("SIP/2.0 200 OK", b)); CHECK(!parseRespHead("HTTP/1.1", b));
    { ChunkedDecoder d; std::string out, s = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-T: 1\r\n\r\n";
      for (char c : s) CHECK(d.feed(&c, 1, out));
      CHECK(out == "hello world" && d.done()); }
    { ChunkedDecoder d; std::string out, s = "1a\r\n" + std::string(26, 'z') + "\r\n0\r\n\r\n";
      CHECK(d.feed(s.data(), s.size(), out) && out.size() == 26 && d.done()); }
    { ChunkedDecoder d; std::string out, s = "zz\r\n"; CHECK(!d.feed(s.data(), s.size(), out)); CHECK(d.bad()); }
    { ChunkedDecoder d; std::string out, s = "3\r\nabcX"; CHECK(!d.feed(s.data(), s.size(), out)); }
    { ChunkedDecoder d; std::string out, s = "FFFFFFFF\r\n"; CHECK(!d.feed(s.data(), s.size(), out)); }
    { ChunkedDecoder d; std::string out, s = "\r\n"; CHECK(!d.feed(s.data(), s.size(), out)); }
    { ChunkedDecoder d; std::string out, s(100, '1'); CHECK(!d.feed(s.data(), s.size(), out)); }
    std::printf("test_gcast_http: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
