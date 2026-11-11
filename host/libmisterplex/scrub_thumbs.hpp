#pragma once
// SCRUB_THUMBS (misterplex.conf) / MPX_SCRUB_THUMBS (env wins). Default off.
//
// Unset is off. On (1/on/yes/true): a play whose first PMS <Part> carries
// indexes="sd" or "hd" can show that BIF frame on the seek bar. Off, or a
// part with no index, does not fetch and does not paint a preview.
//
// The bytes are a Roku BIF. Live PMS HTTP serves them at
// /library/parts/{id}/indexes/{sd|hd} with no ".bif" suffix (a ".bif" URL is
// 404). On-disk names like index-sd.bif are not the request path. This header
// parses the body. It does not fetch and it does not draw.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace misterplex {
namespace scrub_thumbs {

inline constexpr std::size_t kBifCap = 8u * 1024u * 1024u;
inline constexpr int kFrameCap = 4096;

struct PartIndex {
    int partId = 0;
    std::string kind; // "sd", "hd", or empty
};

struct BifFrame {
    int64_t atMs = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct BifIndex {
    bool ok = false;
    std::string bytes;
    std::vector<BifFrame> frames;
};

// Shared by the play that armed the fetch and the detached GET. A newer play
// bumps gen so a late body is dropped.
struct ScrubSlot {
    std::mutex mu;
    uint64_t gen = 0;
    bool keep = false;
    BifIndex index;
};

inline std::string xmlAttr(std::string_view tag, std::string_view name) {
    const std::string key = std::string(name) + "=\"";
    std::size_t p = 0;
    while ((p = tag.find(key, p)) != std::string_view::npos) {
        const bool boundary = p == 0 || tag[p - 1] == ' ' || tag[p - 1] == '\t' || tag[p - 1] == '\n' ||
                              tag[p - 1] == '\r';
        if (boundary) {
            p += key.size();
            const auto e = tag.find('"', p);
            if (e == std::string_view::npos)
                return {};
            return std::string(tag.substr(p, e - p));
        }
        p += key.size();
    }
    return {};
}

inline int parseId(const std::string& s) {
    if (s.empty())
        return 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (!end || end == s.c_str() || *end != '\0' || v <= 0 || v > 2000000000L)
        return 0;
    return static_cast<int>(v);
}

// "sd", "hd", "sd,hd", "HD" → prefer sd. Anything else is no index.
inline std::string indexKind(std::string raw) {
    for (char& c : raw)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool sd = false, hd = false;
    std::size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == ','))
            ++i;
        std::size_t j = i;
        while (j < raw.size() && raw[j] != ',' && raw[j] != ' ' && raw[j] != '\t')
            ++j;
        const std::string tok = raw.substr(i, j - i);
        if (tok == "sd")
            sd = true;
        else if (tok == "hd")
            hd = true;
        i = j;
    }
    if (sd)
        return "sd";
    if (hd)
        return "hd";
    return {};
}

// The first <Part> in the metadata document (the part partIdFromXml uses).
inline PartIndex partIndexFromXml(std::string_view xml) {
#ifdef MUT_SCRUB_THUMBS_IGNORE_INDEX
    (void)xml;
    return {};
#else
    const auto p = xml.find("<Part");
    if (p == std::string_view::npos)
        return {};
    if (p + 5 < xml.size()) {
        const char n = xml[p + 5];
        if (n != ' ' && n != '\t' && n != '\n' && n != '\r' && n != '>' && n != '/')
            return {};
    }
    const auto gt = xml.find('>', p);
    if (gt == std::string_view::npos)
        return {};
    const std::string_view open = xml.substr(p, gt - p);
    PartIndex out;
    out.partId = parseId(xmlAttr(open, "id"));
    out.kind = indexKind(xmlAttr(open, "indexes"));
    return out;
#endif
}

inline std::string bifUrl(std::string base, int partId, const std::string& kind) {
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base.empty() || partId <= 0 || (kind != "sd" && kind != "hd"))
        return {};
    // Live PMS: 200 and BIF magic on this path. Appending ".bif" is 404.
    return base + "/library/parts/" + std::to_string(partId) + "/indexes/" + kind;
}

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Roku BIF: magic, version 0, image count, timestamp multiplier (ms = ts * multiplier),
// 64-byte header, then count+1 index rows (the last timestamp is 0xFFFFFFFF).
inline BifIndex parseBif(std::string bytes) {
    BifIndex out;
    if (bytes.size() < 64 || bytes.size() > kBifCap)
        return out;
    static constexpr uint8_t kMagic[8] = {0x89, 'B', 'I', 'F', 0x0d, 0x0a, 0x1a, 0x0a};
    const auto* b = reinterpret_cast<const uint8_t*>(bytes.data());
    if (std::memcmp(b, kMagic, 8) != 0)
        return out;
    if (rd32(b + 8) != 0)
        return out;
    const uint32_t count = rd32(b + 12);
    if (count == 0 || count > static_cast<uint32_t>(kFrameCap))
        return out;
    uint32_t mult = rd32(b + 16);
    if (mult == 0)
        mult = 1000;
    if (mult > 60000u)
        return out;
    const std::size_t indexBytes = (static_cast<std::size_t>(count) + 1u) * 8u;
    if (64u + indexBytes > bytes.size())
        return out;
    struct Row {
        uint32_t ts;
        uint32_t off;
    };
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(count) + 1u);
    for (uint32_t i = 0; i <= count; ++i) {
        const uint8_t* p = b + 64 + static_cast<std::size_t>(i) * 8u;
        rows.push_back(Row{rd32(p), rd32(p + 4)});
    }
    if (rows.back().ts != 0xffffffffu)
        return out;
    const uint32_t endOff = rows.back().off;
    if (endOff > bytes.size())
        return out;
    const uint32_t indexEnd = static_cast<uint32_t>(64u + indexBytes);
    std::vector<BifFrame> frames;
    frames.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = rows[i].off;
        const uint32_t next = rows[i + 1].off;
        if (off < indexEnd || next < off || next > endOff)
            return out;
        const uint32_t len = next - off;
        if (len < 2)
            continue;
        if (b[off] != 0xff || b[off + 1] != 0xd8)
            continue;
        const uint64_t ms = uint64_t(rows[i].ts) * uint64_t(mult);
        if (ms > uint64_t(std::numeric_limits<int64_t>::max()))
            continue;
        if (!frames.empty() && static_cast<int64_t>(ms) < frames.back().atMs)
            return out;
        BifFrame f;
        f.atMs = static_cast<int64_t>(ms);
        f.offset = off;
        f.length = len;
        frames.push_back(f);
    }
    if (frames.empty())
        return out;
    out.ok = true;
    out.bytes = std::move(bytes);
    out.frames = std::move(frames);
    return out;
}

// Greatest frame at or before seekMs. Before the first frame, that frame.
// -1 when the index is empty. The sentinel is not a frame.
inline int frameIndexAt(const BifIndex& idx, int64_t seekMs) {
#ifdef MUT_SCRUB_THUMBS_DROP_FRAME
    (void)idx;
    (void)seekMs;
    return -1;
#else
    if (!idx.ok || idx.frames.empty())
        return -1;
    if (seekMs < 0)
        seekMs = 0;
    int best = 0;
    for (int i = 0; i < static_cast<int>(idx.frames.size()); ++i) {
        if (idx.frames[static_cast<std::size_t>(i)].atMs <= seekMs)
            best = i;
        else
            break;
    }
    return best;
#endif
}

inline std::vector<uint8_t> frameJpeg(const BifIndex& idx, int64_t seekMs, int64_t& atMs) {
    atMs = -1;
    const int i = frameIndexAt(idx, seekMs);
    if (i < 0)
        return {};
    const BifFrame& f = idx.frames[static_cast<std::size_t>(i)];
    if (static_cast<std::size_t>(f.offset) + f.length > idx.bytes.size())
        return {};
    atMs = f.atMs;
    const auto* p = reinterpret_cast<const uint8_t*>(idx.bytes.data()) + f.offset;
    return std::vector<uint8_t>(p, p + f.length);
}

} // namespace scrub_thumbs

inline bool scrubThumbsEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// Default off. MPX_SCRUB_THUMBS wins over SCRUB_THUMBS when it is set. Unset is off.
inline bool scrubThumbsEnabled() {
#ifdef MUT_SCRUB_THUMBS_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_SCRUB_THUMBS"); e && e[0])
        return scrubThumbsEnvOn(e);
    return scrubThumbsEnvOn(std::getenv("SCRUB_THUMBS"));
#endif
}

// Flag off, or no frame, does not paint. The seek path calls this with the
// JPEG it found (empty when the part has no BIF).
inline bool scrubPreviewWanted(bool enabled, bool haveFrame) {
#ifdef MUT_SCRUB_THUMBS_WHEN_OFF
    (void)enabled;
    return haveFrame;
#else
    return enabled && haveFrame;
#endif
}

} // namespace misterplex
