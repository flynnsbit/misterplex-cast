#pragma once
// RAM thumb LRU (≤12 MB) + disk LRU under /media/fat/misterplex/browse/.
// Pre-scaled RGB24 at tile pixel size. Drop RAM when playback starts.

#include "libmisterplex/browse_shell.hpp"
#include "libmisterplex/browse_shell_model.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <array>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace misterplex {

struct BrowseThumb {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> rgb; // packed RGB24
};

inline std::string browseThumbKey(const std::string& serverId, const std::string& ratingKey, int w,
                                  int h) {
    return serverId + "|" + ratingKey + "|" + std::to_string(w) + "x" + std::to_string(h);
}

struct BrowseThumbLookup {
    std::array<std::string_view, 5> parts;
    char size[32];
    BrowseThumbLookup(std::string_view server, std::string_view ratingKey, int w, int h) {
        const int n = std::snprintf(size, sizeof size, "%dx%d", w, h);
        parts = {server, "|", ratingKey, "|", std::string_view(size, static_cast<std::size_t>(n))};
    }
    int compare(std::string_view key) const {
        std::size_t i = 0;
        for (auto part : parts) {
            for (unsigned char c : part) {
                if (i == key.size())
                    return 1;
                const auto other = static_cast<unsigned char>(key[i++]);
                if (c != other)
                    return c < other ? -1 : 1;
            }
        }
        return i == key.size() ? 0 : -1;
    }
};

struct BrowseThumbLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a < b; }
    bool operator()(std::string_view a, const BrowseThumbLookup& b) const { return b.compare(a) > 0; }
    bool operator()(const BrowseThumbLookup& a, std::string_view b) const { return a.compare(b) < 0; }
};

inline std::string browseCacheComponent(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_')
            out.push_back(static_cast<char>(c));
        else {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out.empty() ? "_" : out;
}

class BrowseRamCache {
public:
    explicit BrowseRamCache(std::size_t maxBytes = 0)
        : maxBytes_(std::min(maxBytes ? maxBytes : browseShellRamCacheBytes(),
                             static_cast<std::size_t>(12u * 1024u * 1024u))) {}

    void setMaxBytes(std::size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        maxBytes_ = std::min(n, static_cast<std::size_t>(12u * 1024u * 1024u));
        evict();
    }
    std::size_t maxBytes() const { std::lock_guard<std::mutex> lk(mu_); return maxBytes_; }
    std::size_t bytes() const {
        std::lock_guard<std::mutex> lk(mu_);
        return bytes_;
    }
    int gets() const { std::lock_guard<std::mutex> lk(mu_); return gets_; }
    int puts() const { std::lock_guard<std::mutex> lk(mu_); return puts_; }

    bool contains(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.find(key) != map_.end();
    }

    bool get(const std::string& key, BrowseThumb& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(key);
        if (it == map_.end())
            return false;
        ++gets_;
        it->second.last = ++clock_;
        out = it->second.thumb;
        return true;
    }

    template<class Paint>
    bool paint(std::string_view server, std::string_view ratingKey, int w, int h, Paint&& fn) {
        std::lock_guard<std::mutex> lk(mu_);
        const BrowseThumbLookup key(server, ratingKey, w, h);
        auto it = map_.find(key);
        if (it == map_.end())
            return false;
        ++gets_;
        it->second.last = ++clock_;
        fn(it->second.thumb);
        return true;
    }

    void put(const std::string& key, BrowseThumb thumb) {
        std::lock_guard<std::mutex> lk(mu_);
        ++puts_;
        auto it = map_.find(key);
        if (it != map_.end()) {
            bytes_ -= it->second.bytes;
            map_.erase(it);
        }
        Ent e;
        e.thumb = std::move(thumb);
        e.bytes = e.thumb.rgb.size();
        e.last = ++clock_;
        bytes_ += e.bytes;
        map_.emplace(key, std::move(e));
        evict();
    }

    void dropAll() {
        std::lock_guard<std::mutex> lk(mu_);
        map_.clear();
        bytes_ = 0;
    }

private:
    struct Ent {
        BrowseThumb thumb;
        std::size_t bytes = 0;
        uint64_t last = 0;
    };
    void evict() {
        while (bytes_ > maxBytes_ && !map_.empty()) {
            auto oldest = map_.begin();
            for (auto it = map_.begin(); it != map_.end(); ++it) {
                if (it->second.last < oldest->second.last)
                    oldest = it;
            }
            bytes_ -= oldest->second.bytes;
            map_.erase(oldest);
        }
    }
    std::size_t maxBytes_ = 0;
    std::size_t bytes_ = 0;
    uint64_t clock_ = 0;
    int gets_ = 0;
    int puts_ = 0;
    mutable std::mutex mu_;
    std::map<std::string, Ent, BrowseThumbLess> map_;
};

inline bool browseMkdirP(const std::string& path) {
    if (path.empty())
        return false;
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur.empty() || cur == "/")
                continue;
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    return true;
}

class BrowseDiskCache {
public:
    BrowseDiskCache() : dir_(browseShellDir()), maxBytes_(browseShellDiskCacheBytes()) {}
    explicit BrowseDiskCache(std::string dir, std::size_t maxBytes)
        : dir_(std::move(dir)), maxBytes_(maxBytes) {}
    ~BrowseDiskCache() { flushTouches(); }

    const std::string& dir() const { return dir_; }
    std::size_t maxBytes() const { return maxBytes_; }

    std::string serversPath() const {
        std::string d = dir_;
        if (!d.empty() && d.back() != '/')
            d.push_back('/');
        return d + "servers.json";
    }

    std::string thumbPath(const std::string& serverId, const std::string& ratingKey, int w, int h) const {
        std::string d = dir_;
        if (!d.empty() && d.back() != '/')
            d.push_back('/');
        d += "thumbs/";
        d += browseCacheComponent(serverId);
        d += "/";
        d += browseCacheComponent(ratingKey);
        d += "_";
        d += std::to_string(w);
        d += "x";
        d += std::to_string(h);
        d += ".rgb";
        return d;
    }

    bool loadServersJson(std::string& out) const {
        std::ifstream in(serversPath(), std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    bool saveServersJson(const std::string& json) {
        browseMkdirP(dir_);
        const std::string path = serversPath();
        const std::string tmp = path + ".tmp";
        struct stat previous {};
        const std::size_t oldBytes = ::stat(path.c_str(), &previous) == 0 ?
                                    static_cast<std::size_t>(previous.st_size) : 0;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (!out)
                return false;
        }
        if (::rename(tmp.c_str(), path.c_str()) != 0)
            return false;
        if (bytesKnown_) {
            cachedBytes_ = cachedBytes_ - std::min(cachedBytes_, oldBytes) + json.size();
            evict();
        }
        return true;
    }

    bool loadThumb(const std::string& serverId, const std::string& ratingKey, int w, int h,
                   BrowseThumb& out) const {
        const std::string path = thumbPath(serverId, ratingKey, w, h);
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u;
        out.w = w;
        out.h = h;
        out.rgb.resize(n);
        in.read(reinterpret_cast<char*>(out.rgb.data()), static_cast<std::streamsize>(n));
        if (in.gcount() != static_cast<std::streamoff>(n))
            return false;
        pendingTouches_[path] = true;
        if (pendingTouches_.size() >= 32)
            flushTouches();
        return true;
    }

    bool saveThumb(const std::string& serverId, const std::string& ratingKey, int w, int h,
                   const BrowseThumb& t) {
        if (t.rgb.empty() || w <= 0 || h <= 0)
            return false;
        std::string d = dir_;
        if (!d.empty() && d.back() != '/')
            d.push_back('/');
        d += "thumbs/";
        d += browseCacheComponent(serverId);
        browseMkdirP(d);
        const std::string path = thumbPath(serverId, ratingKey, w, h);
        const std::string tmp = path + ".tmp";
        if (!bytesKnown_) {
            cachedBytes_ = dirBytes();
            bytesKnown_ = true;
        }
        struct stat previous {};
        const std::size_t oldBytes = ::stat(path.c_str(), &previous) == 0 ?
                                    static_cast<std::size_t>(previous.st_size) : 0;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(reinterpret_cast<const char*>(t.rgb.data()),
                      static_cast<std::streamsize>(t.rgb.size()));
            if (!out)
                return false;
        }
        if (::rename(tmp.c_str(), path.c_str()) != 0)
            return false;
        cachedBytes_ = cachedBytes_ - std::min(cachedBytes_, oldBytes) + t.rgb.size();
        evict();
        return true;
    }

    void evict() {
        if (bytesKnown_ && cachedBytes_ <= maxBytes_)
            return;
        flushTouches();
        std::vector<DiskEnt> files;
        std::size_t total = 0;
        walk(dir_, files, total);
        cachedBytes_ = total;
        bytesKnown_ = true;
        if (total <= maxBytes_)
            return;
        std::sort(files.begin(), files.end(),
                  [](const DiskEnt& a, const DiskEnt& b) { return a.mtime < b.mtime; });
        const std::size_t lowWater = maxBytes_ - maxBytes_ * 15 / 100;
        for (const DiskEnt& f : files) {
            if (total <= lowWater)
                break;
            if (f.path.size() >= 12 && f.path.compare(f.path.size() - 12, 12, "servers.json") == 0)
                continue;
            if (::unlink(f.path.c_str()) == 0)
                total -= f.bytes;
        }
        cachedBytes_ = total;
    }

    std::size_t dirBytes() const {
        std::size_t total = 0;
        walkStub(dir_, total);
        return total;
    }

private:
    void flushTouches() const {
        for (const auto& entry : pendingTouches_)
            if (::utimensat(AT_FDCWD, entry.first.c_str(), nullptr, 0) != 0 && errno != ENOENT)
                std::fprintf(stderr, "browse-shell: cannot update thumbnail cache mtime: %s\n",
                             std::strerror(errno));
        pendingTouches_.clear();
    }
    struct DiskEnt {
        std::string path;
        std::size_t bytes = 0;
        long mtime = 0;
    };
    static void walk(const std::string& dir, std::vector<DiskEnt>& files, std::size_t& total) {
        DIR* d = ::opendir(dir.c_str());
        if (!d)
            return;
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            const std::string p = dir + (dir.back() == '/' ? "" : "/") + e->d_name;
            struct stat st {};
            if (::stat(p.c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                walk(p, files, total);
                continue;
            }
            DiskEnt f;
            f.path = p;
            f.bytes = static_cast<std::size_t>(st.st_size);
            f.mtime = static_cast<long>(st.st_mtime);
            total += f.bytes;
            files.push_back(std::move(f));
        }
        ::closedir(d);
    }
    static void walkStub(const std::string& dir, std::size_t& total) {
        DIR* d = ::opendir(dir.c_str());
        if (!d)
            return;
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            const std::string p = dir + (dir.back() == '/' ? "" : "/") + e->d_name;
            struct stat st {};
            if (::stat(p.c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                walkStub(p, total);
                continue;
            }
            total += static_cast<std::size_t>(st.st_size);
        }
        ::closedir(d);
    }
    std::string dir_;
    std::size_t maxBytes_ = 0;
    std::size_t cachedBytes_ = 0;
    bool bytesKnown_ = false;
    mutable std::unordered_map<std::string, bool> pendingTouches_;
};

inline std::string browseEscapeJson(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(static_cast<char>(c));
        } else if (c < 32) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

inline std::string browseServersToJson(const std::vector<BrowseServerRow>& rows, const std::string& lastUsed) {
    std::ostringstream o;
    o << "{\"lastUsed\":\"" << browseEscapeJson(lastUsed) << "\",\"servers\":[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& s = rows[i];
        if (i)
            o << ',';
        o << "{\"id\":\"" << browseEscapeJson(s.id) << "\",\"name\":\"" << browseEscapeJson(s.name)
          << "\",\"locality\":\"" << browseEscapeJson(s.locality) << "\",\"host\":\""
          << browseEscapeJson(s.host) << "\",\"status\":\"" << browseEscapeJson(s.status)
          << "\",\"baseUrl\":\"" << browseEscapeJson(s.baseUrl) << "\"}";
    }
    o << "]}";
    return o.str();
}

inline std::string browseJsonGet(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\":\"";
    const auto p = json.find(pat);
    if (p == std::string::npos)
        return {};
    auto i = p + pat.size();
    std::string v;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            const char escaped = json[i + 1];
            if (escaped == 'u' && i + 5 < json.size()) {
                const auto hex = json.substr(i + 2, 4);
                char* end = nullptr;
                const long cp = std::strtol(hex.c_str(), &end, 16);
                if (end == hex.c_str() + 4 && cp < 128) {
                    v.push_back(static_cast<char>(cp));
                    i += 6;
                    continue;
                }
            }
            v.push_back(escaped == 'n' ? '\n' : escaped == 'r' ? '\r' :
                        escaped == 't' ? '\t' : escaped);
            i += 2;
            continue;
        }
        v.push_back(json[i++]);
    }
    return v;
}

inline std::vector<BrowseServerRow> browseServersFromJson(const std::string& json, std::string& lastUsed) {
    lastUsed = browseJsonGet(json, "lastUsed");
    std::vector<BrowseServerRow> rows;
    std::size_t i = json.find("\"servers\"");
    if (i == std::string::npos)
        return rows;
    i = json.find('[', i);
    if (i == std::string::npos)
        return rows;
    ++i;
    while (i < json.size()) {
        const auto obj0 = json.find('{', i);
        if (obj0 == std::string::npos)
            break;
        std::size_t obj1 = obj0 + 1;
        bool quoted = false;
        for (; obj1 < json.size(); ++obj1) {
            if (quoted && json[obj1] == '\\') {
                ++obj1;
                continue;
            }
            if (json[obj1] == '"')
                quoted = !quoted;
            else if (!quoted && json[obj1] == '}')
                break;
        }
        if (obj1 >= json.size())
            break;
        const std::string obj = json.substr(obj0, obj1 - obj0 + 1);
        BrowseServerRow r;
        r.id = browseJsonGet(obj, "id");
        r.name = browseJsonGet(obj, "name");
        r.locality = browseJsonGet(obj, "locality");
        r.host = browseJsonGet(obj, "host");
        r.status = browseJsonGet(obj, "status");
        r.baseUrl = browseJsonGet(obj, "baseUrl");
        r.ok = r.status == "OK";
        r.lastUsed = !lastUsed.empty() && r.id == lastUsed;
        if (!r.name.empty())
            rows.push_back(std::move(r));
        i = obj1 + 1;
    }
    return rows;
}

} // namespace misterplex
