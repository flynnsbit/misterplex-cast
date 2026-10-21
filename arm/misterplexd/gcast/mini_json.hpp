// Minimal JSON reader for the YouTube Lounge channel (GCAST P3).
// Read-only, bounded depth, no exceptions. Numbers are kept as double and as
// their source text so ids survive. Not a general-purpose library.
#pragma once
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {
namespace gcast {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;  // Str value, or the literal text of a Num
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const std::string& k) const {
        if (type != Obj) return nullptr;
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    // String form of a scalar: strings verbatim, numbers as source text.
    std::string scalar() const {
        if (type == Str || type == Num) return str;
        if (type == Bool) return b ? "true" : "false";
        return std::string();
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}
    bool parse(Json& out) {
        pos_ = 0;
        if (!value(out, 0)) return false;
        ws();
        return pos_ == s_.size();
    }
private:
    const std::string& s_;
    size_t pos_ = 0;
    static constexpr int kMaxDepth = 32;

    void ws() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_; }
    bool lit(const char* w) {
        size_t n = 0; while (w[n]) ++n;
        if (s_.compare(pos_, n, w) != 0) return false;
        pos_ += n; return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { o += c; continue; }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
            case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
            case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {
                unsigned cp; if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                    pos_ += 2; unsigned lo; if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp); break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') {
            v.type = Json::Obj; ++pos_; ws();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            for (;;) {
                ws(); std::string k; if (!string(k)) return false;
                ws(); if (pos_ >= s_.size() || s_[pos_] != ':') return false; ++pos_;
                Json child; if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                ws(); if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Arr; ++pos_; ws();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            for (;;) {
                Json child; if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws(); if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Json::Str; return string(v.str); }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = pos_;
            if (s_[pos_] == '-') ++pos_;
            while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            v.type = Json::Num; v.str = s_.substr(st, pos_ - st);
            char* end = nullptr; v.num = std::strtod(v.str.c_str(), &end);
            return end && *end == '\0' && v.str != "-";
        }
        return false;
    }
};

inline bool parseJson(const std::string& s, Json& out) { out = Json(); return JsonReader(s).parse(out); }

}  // namespace gcast
}  // namespace misterplex
