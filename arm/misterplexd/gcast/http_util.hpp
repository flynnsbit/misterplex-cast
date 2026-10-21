// HTTP/1.1 client-side helpers for the gcast HTTPS client (GCAST P3): response
// head parsing and a streaming chunked-transfer decoder. Pure, so they are
// unit-tested and fuzzed on the host; https_client.cpp does the TLS I/O.
#pragma once
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace misterplex {
namespace gcast {
namespace http {

constexpr size_t kMaxHead = 32 * 1024;

struct RespHead {
    int status = 0;
    std::string reason;
    std::map<std::string, std::string> h;  // lower-case names
    std::string get(const std::string& k) const { auto it = h.find(k); return it == h.end() ? std::string() : it->second; }
};

inline std::string lc(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }

// raw = everything up to (not including) the blank line.
inline bool parseRespHead(const std::string& raw, RespHead& out) {
    if (raw.size() > kMaxHead || raw.compare(0, 7, "HTTP/1.") != 0) return false;
    size_t eol = raw.find("\r\n");
    std::string line = raw.substr(0, eol == std::string::npos ? raw.size() : eol);
    size_t sp = line.find(' ');
    if (sp == std::string::npos || sp + 4 > line.size()) return false;
    for (int i = 1; i <= 3; ++i) if (!std::isdigit((unsigned char)line[sp + i])) return false;
    out.status = std::atoi(line.substr(sp + 1, 3).c_str());
    out.reason = sp + 5 <= line.size() ? line.substr(sp + 5) : std::string();
    size_t pos = eol == std::string::npos ? raw.size() : eol + 2;
    int n = 0;
    while (pos < raw.size()) {
        size_t e = raw.find("\r\n", pos);
        if (e == std::string::npos) e = raw.size();
        if (++n > 100) return false;
        std::string l = raw.substr(pos, e - pos);
        size_t c = l.find(':');
        if (c != std::string::npos) {
            size_t v = c + 1;
            while (v < l.size() && (l[v] == ' ' || l[v] == '\t')) ++v;
            std::string key = lc(l.substr(0, c));
            std::string val = l.substr(v);
            auto it = out.h.find(key);
            if (it == out.h.end()) out.h[key] = val; else it->second += ", " + val;
        }
        pos = e + 2;
    }
    return true;
}

// Streaming chunked decoder. feed() appends decoded bytes to out; returns false on
// malformed framing. done() once the terminating 0-chunk (and its CRLF) is seen.
class ChunkedDecoder {
public:
    bool feed(const char* p, size_t n, std::string& out) {
        if (bad_) return false;
        for (size_t i = 0; i < n; ++i) {
            char c = p[i];
            switch (st_) {
            case Size:
                if (c == '\r') break;
                if (c == '\n') {
                    if (line_.empty()) { bad_ = true; return false; }
                    std::string hex = line_.substr(0, line_.find(';'));
                    for (char x : hex) if (!std::isxdigit((unsigned char)x)) { bad_ = true; return false; }
#ifndef MUT_HTTP_NO_CHUNKCAP
                    if (hex.size() > 7) { bad_ = true; return false; }  // > 256 MiB chunk
#endif
                    left_ = std::strtoul(hex.c_str(), nullptr, 16);
                    line_.clear();
                    st_ = left_ ? Data : Trailer;
                    break;
                }
                if (line_.size() > 64) { bad_ = true; return false; }
                line_ += c;
                break;
            case Data: {
                size_t take = n - i < left_ ? n - i : left_;
                out.append(p + i, take);
                left_ -= take; i += take - 1;
                if (left_ == 0) st_ = DataCr;
                break;
            }
            case DataCr:
                if (c == '\r') break;
                if (c != '\n') { bad_ = true; return false; }
                st_ = Size;
                break;
            case Trailer:
                if (c == '\r') break;
                if (c == '\n') { if (line_.empty()) { st_ = Done; return true; } line_.clear(); break; }
                if (line_.size() > 1024) { bad_ = true; return false; }
                line_ += c;
                break;
            case Done:
                return true;
            }
        }
        return true;
    }
    bool done() const { return st_ == Done; }
    bool bad() const { return bad_; }
private:
    enum St { Size, Data, DataCr, Trailer, Done } st_ = Size;
    std::string line_;
    size_t left_ = 0;
    bool bad_ = false;
};

}  // namespace http
}  // namespace gcast
}  // namespace misterplex
