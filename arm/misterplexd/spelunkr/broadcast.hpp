#pragma once
// Public Spelunkr broadcast feed (spelunkr.tv/broadcast/). No supabase. No Origin spoof.
// Item filename is `file` (happy-trees) or `mp4` (saturday-morning). Both are observed.

#include "gcast/mini_json.hpp"

#include <cctype>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace misterplex {
namespace spelunkr {

inline constexpr const char* kBroadcastOrigin = "https://spelunkr.tv";
inline constexpr const char* kBroadcastBase = "https://spelunkr.tv/broadcast/";
inline constexpr const char* kArchiveDownload = "https://archive.org/download/";
inline constexpr const char* kArchiveMetadata = "https://archive.org/metadata/";
// Empty ffmpeg UA → archive.org 400. Lavf is what host ffprobe sends.
inline constexpr const char* kArchiveUserAgent = "Lavf/61.7.100";

inline std::string urlHost(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos)
        return {};
    std::string h = url.substr(scheme + 3);
    const auto slash = h.find('/');
    if (slash != std::string::npos)
        h.resize(slash);
    const auto colon = h.find(':');
    if (colon != std::string::npos)
        h.resize(colon);
    return h;
}

inline bool hostAllowed(const std::string& url) {
    if (url.rfind("https://spelunkr.tv/", 0) == 0)
        return true;
    const std::string h = urlHost(url);
    if (h == "archive.org" || h == "www.archive.org")
        return true;
    const char* suf = ".archive.org";
    const size_t n = std::strlen(suf);
    return h.size() > n && h.compare(h.size() - n, n, suf) == 0;
}

inline std::string percentEncodePath(const std::string& s) {
    std::string o;
    o.reserve(s.size() * 3);
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            o += char(c);
        } else if (c == '/') {
            o += '/';
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

inline std::string archiveUrl(const std::string& id, const std::string& file) {
    if (id.empty() || file.empty() || id.find('/') != std::string::npos)
        return {};
    return std::string(kArchiveDownload) + percentEncodePath(id) + "/" + percentEncodePath(file);
}

inline std::string archiveMetadataUrl(const std::string& id) {
    if (id.empty() || id.find('/') != std::string::npos)
        return {};
    return std::string(kArchiveMetadata) + percentEncodePath(id);
}

// First 8 bytes of an MP4/QuickTime box. Empty 302 bodies fail this.
inline bool looksLikeMp4Head(const std::string& body) {
    if (body.size() < 8)
        return false;
    const char* t = body.data() + 4;
    return std::memcmp(t, "ftyp", 4) == 0 || std::memcmp(t, "mdat", 4) == 0 ||
           std::memcmp(t, "moov", 4) == 0 || std::memcmp(t, "wide", 4) == 0 ||
           std::memcmp(t, "free", 4) == 0;
}

// P3: a richer classification of the same 512-byte preflight buffer. FrontMoov is the
// fast-start layout (moov visible before mdat); TailMoov is ftyp -> optional free -> mdat
// with no moov seen in the buffer, which on archive.org exports means the moov sits after
// the (often huge) mdat at the end of the file — a live scan there is expensive. The walk
// stays inside the buffer: a box whose declared size would run past it, or a 0/1 (extends-
// to-EOF / 64-bit largesize) box, ends the walk at that box.
enum class Mp4HeadKind { NotMp4, FrontMoov, TailMoov };

inline Mp4HeadKind classifyMp4Head(const std::string& body) {
    if (body.size() < 8)
        return Mp4HeadKind::NotMp4;
    std::vector<std::string> types;
    size_t pos = 0;
    while (pos + 8 <= body.size()) {
        const auto* b = reinterpret_cast<const unsigned char*>(body.data() + pos);
        const uint32_t boxSize = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        types.emplace_back(body.data() + pos + 4, 4);
        if (boxSize < 8)  // 0 (extends to EOF), 1 (64-bit largesize), or garbage: stop the walk here
            break;
        pos += boxSize;
    }
    if (types.empty() ||
        (types[0] != "ftyp" && types[0] != "mdat" && types[0] != "moov" && types[0] != "wide" &&
         types[0] != "free"))
        return Mp4HeadKind::NotMp4;
    for (const auto& t : types)
        if (t == "moov")
            return Mp4HeadKind::FrontMoov;
    if (types[0] != "ftyp")
        return Mp4HeadKind::FrontMoov;  // no moov seen but not the ftyp->mdat shape: assume fast-start
    size_t i = 1;
    if (i < types.size() && types[i] == "free")
        ++i;
    if (i < types.size() && types[i] == "mdat")
        return Mp4HeadKind::TailMoov;
    return Mp4HeadKind::FrontMoov;
}

inline std::string jsonStr(const airplay::Json* j) {
    return j ? j->scalar() : std::string();
}

inline int64_t jsonInt(const airplay::Json* j, int64_t def = 0) {
    if (!j)
        return def;
    if (j->type == airplay::Json::Num && !j->str.empty()) {
        char* end = nullptr;
        const long long v = std::strtoll(j->str.c_str(), &end, 10);
        if (end && *end == '\0')
            return v;
        return static_cast<int64_t>(j->num);
    }
    if (j->type == airplay::Json::Str && !j->str.empty()) {
        char* end = nullptr;
        return std::strtoll(j->str.c_str(), &end, 10);
    }
    return def;
}

inline bool archiveHostToken(const std::string& d1) {
    if (d1.empty() || d1.size() > 253 || d1.find('/') != std::string::npos)
        return false;
    for (unsigned char c : d1) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-'))
            return false;
    }
    return d1.find("..") == std::string::npos;
}

inline bool archiveDirToken(const std::string& dir) {
    if (dir.empty() || dir[0] != '/' || dir.find("..") != std::string::npos)
        return false;
    for (unsigned char c : dir) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-' || c == '_' || c == '/'))
            return false;
    }
    return true;
}

inline bool scanJsonStringField(const std::string& body, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\":\"";
    const auto p = body.find(pat);
    if (p == std::string::npos)
        return false;
    const auto start = p + pat.size();
    const auto e = body.find('"', start);
    if (e == std::string::npos || e == start)
        return false;
    out = body.substr(start, e - start);
    return true;
}

// archive.org/metadata/{id} root fields d1 + dir. Direct item URL supports Range;
// /download/ 302s and a Range GET on the 302 is empty → libav "Invalid data found".
inline bool parseArchiveLocator(const std::string& body, std::string& d1, std::string& dir,
                                const airplay::Json* parsed = nullptr) {
    d1.clear();
    dir.clear();
    airplay::Json root;
    if (!parsed && airplay::parseJson(body, root))
        parsed = &root;
    if (parsed && parsed->type == airplay::Json::Obj) {
        d1 = jsonStr(parsed->get("d1"));
        dir = jsonStr(parsed->get("dir"));
    }
    if (d1.empty())
        scanJsonStringField(body, "d1", d1);
    if (dir.empty())
        scanJsonStringField(body, "dir", dir);
    return archiveHostToken(d1) && archiveDirToken(dir);
}

inline std::string archiveItemUrl(const std::string& d1, const std::string& dir, const std::string& file) {
    if (!archiveHostToken(d1) || !archiveDirToken(dir) || file.empty())
        return {};
    std::string path = dir;
    if (path.back() != '/')
        path += '/';
    const std::string u = "https://" + d1 + path + percentEncodePath(file);
    return hostAllowed(u) ? u : std::string();
}

inline std::string resolveArchiveItemUrl(const std::string& metadataBody, const std::string& file) {
    std::string d1, dir;
    if (!parseArchiveLocator(metadataBody, d1, dir))
        return {};
    return archiveItemUrl(d1, dir, file);
}

struct ArchiveFileMeta {
    int64_t size = -1;
    std::string etag;
    int width = -1;
    int height = -1;
};

struct ArchiveItemMeta {
    std::string d1, dir;
    std::unordered_map<std::string, ArchiveFileMeta> files;
};

// GBE #625 P3-6 / P2-1: keep .mp4/.m4v/.mov rows (ch12's item has 2400 files: thumbs, xml,
// torrents). Dropping .m4v/.mov lost size/etag, so DeadTailMoov never fired on ch28/36/53/204.
inline bool archiveMetaKeepsRow(const std::string& name) {
    if (name.size() < 4)
        return false;
    std::string ext = name.substr(name.size() - 4);
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4" || ext == ".m4v" || ext == ".mov";
}

inline bool parseArchiveItemMeta(const std::string& body, ArchiveItemMeta& out) {
    out = {};
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj) {
        // GBE #625 P3-5: a body that does not parse (e.g. truncated at maxBody) keeps the old
        // scanJsonStringField locator fallback: rows stay empty, so every file is "dims unknown".
        return parseArchiveLocator(body, out.d1, out.dir);
    }
    if (!parseArchiveLocator(body, out.d1, out.dir, &root))
        return false;
    const auto dimension = [](const airplay::Json* j) {
        const std::string s = jsonStr(j);
        if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos)
            return -1;
        const int64_t n = jsonInt(j, -1);
        return n > 0 && n <= INT_MAX ? static_cast<int>(n) : -1;
    };
    const airplay::Json* files = root.get("files");
    if (files && files->type == airplay::Json::Arr) {
        for (const auto& f : files->arr) {
            const std::string name = jsonStr(f.get("name"));
            if (f.type != airplay::Json::Obj || name.empty() || !archiveMetaKeepsRow(name))
                continue;
            ArchiveFileMeta row;
            const int64_t size = jsonInt(f.get("size"), -1);
            if (size > 0)
                row.size = size;
            row.etag = jsonStr(f.get("md5"));
            row.width = dimension(f.get("width"));
            row.height = dimension(f.get("height"));
            out.files.emplace(name, std::move(row));
        }
    }
    return true;
}

// P2 header cache: archive.org/metadata/{id} "files" entries carry {"name":...,"size":"123","md5":...}.
// size is a decimal string in that API; md5 doubles as an ETag-equivalent content fingerprint (archive.org
// does not send a real ETag on the direct item URL). False: no matching file entry (size stays -1, etag
// stays empty — the caller then keys the header cache on (url,size) only, or skips caching if size is
// also unknown).
inline bool parseArchiveFileMeta(const std::string& body, const std::string& file, int64_t& size,
                                 std::string& etag) {
    size = -1;
    etag.clear();
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj)
        return false;
    const airplay::Json* files = root.get("files");
    if (!files || files->type != airplay::Json::Arr)
        return false;
    for (const auto& f : files->arr) {
        if (f.type != airplay::Json::Obj || jsonStr(f.get("name")) != file)
            continue;
        const int64_t sz = jsonInt(f.get("size"), -1);
        if (sz > 0)
            size = sz;
        etag = jsonStr(f.get("md5"));
        return size > 0 || !etag.empty();
    }
    return false;
}

struct VersionPtr {
    int v = 0;
    std::string manifest;
    std::string details;
    int64_t epochMs = 0;
    int64_t effectiveAtMs = 0;
    int startIndex = 0;
};

struct Channel {
    std::string id;
    std::string name;
    int dial = 0;
    std::string color;
    std::string family;
    std::vector<VersionPtr> versions;
};

struct Lineup {
    int engine = 0;
    std::string generatedAt;
    std::string build;
    std::string snapshot;
    std::vector<Channel> channels;
};

struct Item {
    std::string id;    // Archive identifier
    std::string file;  // filename inside the item
    int64_t moov = 0;
    int durationSec = 0;
    std::string title;
    std::string year;
};

struct Manifest {
    int engine = 0;
    std::string channel;
    int version = 0;
    std::vector<Item> items;
};

inline bool parseVersionPtr(const airplay::Json& j, VersionPtr& o) {
    if (j.type != airplay::Json::Obj)
        return false;
    o.v = static_cast<int>(jsonInt(j.get("v")));
    o.manifest = jsonStr(j.get("manifest"));
    o.details = jsonStr(j.get("details"));
    o.epochMs = jsonInt(j.get("epochMs"));
    o.effectiveAtMs = jsonInt(j.get("effectiveAtMs"), o.epochMs);
    o.startIndex = static_cast<int>(jsonInt(j.get("startIndex")));
    return o.v > 0 && !o.manifest.empty() && o.epochMs > 0;
}

inline bool parseChannel(const airplay::Json& j, Channel& o) {
    if (j.type != airplay::Json::Obj)
        return false;
    o.id = jsonStr(j.get("id"));
    o.name = jsonStr(j.get("name"));
    o.dial = static_cast<int>(jsonInt(j.get("dial")));
    o.color = jsonStr(j.get("color"));
    o.family = jsonStr(j.get("family"));
    const airplay::Json* vs = j.get("versions");
    if (!vs || vs->type != airplay::Json::Arr)
        return !o.id.empty();
    for (const auto& v : vs->arr) {
        VersionPtr p;
        if (parseVersionPtr(v, p))
            o.versions.push_back(p);
    }
    return !o.id.empty();
}

inline bool parseLineup(const std::string& body, Lineup& out) {
    out = Lineup();
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj)
        return false;
    out.engine = static_cast<int>(jsonInt(root.get("engine")));
    out.generatedAt = jsonStr(root.get("generatedAt"));
    out.build = jsonStr(root.get("build"));
    out.snapshot = jsonStr(root.get("snapshot"));
    const airplay::Json* ch = root.get("channels");
    if (!ch || ch->type != airplay::Json::Arr)
        return false;
    for (const auto& c : ch->arr) {
        Channel chn;
        if (parseChannel(c, chn))
            out.channels.push_back(std::move(chn));
    }
    return !out.channels.empty();
}

inline bool parseItem(const airplay::Json& j, Item& o) {
    if (j.type != airplay::Json::Obj)
        return false;
    o.id = jsonStr(j.get("id"));
    o.file = jsonStr(j.get("file"));
    if (o.file.empty())
        o.file = jsonStr(j.get("mp4"));  // saturday-morning observed key
    o.moov = jsonInt(j.get("moov"));
    o.durationSec = static_cast<int>(jsonInt(j.get("duration")));
    o.title = jsonStr(j.get("title"));
    o.year = jsonStr(j.get("year"));
    return !o.id.empty() && !o.file.empty() && o.durationSec > 0;
}

inline bool parseManifest(const std::string& body, Manifest& out) {
    out = Manifest();
    airplay::Json root;
    if (!airplay::parseJson(body, root) || root.type != airplay::Json::Obj)
        return false;
    out.engine = static_cast<int>(jsonInt(root.get("engine")));
    out.channel = jsonStr(root.get("channel"));
    out.version = static_cast<int>(jsonInt(root.get("version")));
    const airplay::Json* items = root.get("items");
    if (!items || items->type != airplay::Json::Arr)
        return false;
    for (const auto& it : items->arr) {
        Item item;
        if (parseItem(it, item))
            out.items.push_back(std::move(item));
    }
    return !out.items.empty();
}

inline const Channel* findChannel(const Lineup& L, const std::string& id) {
    for (const auto& c : L.channels)
        if (c.id == id)
            return &c;
    return nullptr;
}

inline const VersionPtr* currentVersion(const Channel& c) {
    if (c.versions.empty())
        return nullptr;
    return &c.versions.back();
}

}  // namespace spelunkr
}  // namespace misterplex
