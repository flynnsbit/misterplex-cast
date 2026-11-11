#pragma once
// CHAPTERS (misterplex.conf) / MPX_CHAPTERS (env wins). Default off.
//
// PMS already returns chapter markers on the metadata GET used for a play
// (<Chapter startTimeOffset= endTimeOffset= tag= index=>, milliseconds from
// the start of the item). Unset or off does not ask for them and does not
// seek. On (1/on/yes/true): that GET adds includeChapters=1 and the Program
// menu can seek to a marker's startTimeOffset.
//
// This header does not fetch. Offsets are the ones PMS put on the Chapter
// tag. A second <Video> is ignored. At most 128 chapters, earliest first.

#include "plex_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace misterplex {
namespace chapters {

inline constexpr int kChapterCap = 128;
inline constexpr int64_t kPrevRestartMs = 3000;

struct Chapter {
    int index = 0;
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string title;
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

// One pass, including &#NN; / &#xHH;. Do not unescape the result again.
inline std::string xmlUnescape(std::string s) { return misterplex::xmlUnescape(std::move(s)); }

inline bool parseMs(const std::string& s, int64_t& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    out = std::strtoll(s.c_str(), &end, 10);
    return end && end != s.c_str() && *end == '\0';
}

inline std::string_view firstVideoSpan(std::string_view xml) {
    const auto open = xml.find("<Video");
    if (open == std::string_view::npos)
        return {};
    const auto end = xml.find("</Video>", open);
    if (end == std::string_view::npos)
        return xml.substr(open);
    return xml.substr(open, end - open);
}

// Chapters inside the first <Video>. Drop a missing or negative start, and a
// start past a known duration. endTimeOffset is optional; when it runs past
// duration it is clamped. Marker tags are not chapters.
inline std::vector<Chapter> parseChapters(std::string_view metadataXml, int64_t durationMs) {
#ifdef MUT_CHAPTERS_DROP_PARSE
    (void)metadataXml;
    (void)durationMs;
    return {};
#else
    const std::string_view video = firstVideoSpan(metadataXml);
    if (video.empty())
        return {};
    std::vector<Chapter> out;
    std::size_t p = 0;
    while ((p = video.find("<Chapter", p)) != std::string_view::npos && out.size() < 256) {
        const auto gt = video.find('>', p);
        if (gt == std::string_view::npos)
            break;
        const std::string_view open = video.substr(p, gt - p);
        p = gt + 1;
        // <Chapters ...> is not a marker.
        if (open.size() > 8) {
            const char n = open[8];
            if (n != ' ' && n != '\t' && n != '\n' && n != '\r' && n != '/' && n != '>')
                continue;
        }
        int64_t start = 0;
        if (!parseMs(xmlAttr(open, "startTimeOffset"), start) || start < 0)
            continue;
        if (durationMs > 0 && start > durationMs)
            continue;
        Chapter c;
        c.startMs = start;
        int64_t end = 0;
        if (parseMs(xmlAttr(open, "endTimeOffset"), end) && end > start) {
            if (durationMs > 0 && end > durationMs)
                end = durationMs;
            if (end > start)
                c.endMs = end;
        }
        int64_t index = 0;
        if (parseMs(xmlAttr(open, "index"), index) && index > 0 && index < 100000)
            c.index = static_cast<int>(index);
        std::string title = xmlAttr(open, "tag");
        if (title.empty())
            title = xmlAttr(open, "title");
        c.title = xmlUnescape(std::move(title));
        if (c.title.size() > 200)
            c.title.resize(200);
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(), [](const Chapter& a, const Chapter& b) {
        if (a.startMs != b.startMs)
            return a.startMs < b.startMs;
        return a.index < b.index;
    });
    if (static_cast<int>(out.size()) > kChapterCap)
        out.resize(static_cast<std::size_t>(kChapterCap));
    int ordinal = 1;
    for (Chapter& c : out) {
        if (c.title.empty())
            c.title = "Chapter " + std::to_string(c.index > 0 ? c.index : ordinal);
        ++ordinal;
    }
    return out;
#endif
}

// Last chapter whose start is at or before pos. -1 when pos is before them all.
inline int chapterIndexAt(const std::vector<Chapter>& ch, int64_t posMs) {
    if (posMs < 0)
        posMs = 0;
    int cur = -1;
    for (int i = 0; i < static_cast<int>(ch.size()); ++i) {
        if (ch[static_cast<std::size_t>(i)].startMs <= posMs)
            cur = i;
        else
            break;
    }
    return cur;
}

inline int64_t currentChapterStart(const std::vector<Chapter>& ch, int64_t posMs) {
    const int i = chapterIndexAt(ch, posMs);
    if (i < 0)
        return -1;
    return ch[static_cast<std::size_t>(i)].startMs;
}

// The following marker. Before the first marker, that marker. -1 at the last.
inline int64_t nextChapterStart(const std::vector<Chapter>& ch, int64_t posMs) {
    if (ch.empty())
        return -1;
    const int cur = chapterIndexAt(ch, posMs);
    if (cur < 0)
        return ch[0].startMs;
    if (cur + 1 < static_cast<int>(ch.size()))
        return ch[static_cast<std::size_t>(cur + 1)].startMs;
    return -1;
}

// More than kPrevRestartMs into the current marker: its start. Otherwise the
// previous marker. -1 when already near the start of the first marker.
inline int64_t prevChapterStart(const std::vector<Chapter>& ch, int64_t posMs) {
    if (ch.empty() || posMs < 0)
        return -1;
    const int cur = chapterIndexAt(ch, posMs);
    if (cur < 0)
        return -1;
    const Chapter& here = ch[static_cast<std::size_t>(cur)];
    if (posMs - here.startMs > kPrevRestartMs)
        return here.startMs;
    if (cur > 0)
        return ch[static_cast<std::size_t>(cur - 1)].startMs;
    return -1;
}

} // namespace chapters

inline bool chaptersEnvOn(const char* e) {
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

// Default off. MPX_CHAPTERS wins over CHAPTERS when it is set. Unset is off.
inline bool chaptersEnabled() {
#ifdef MUT_CHAPTERS_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_CHAPTERS"); e && e[0])
        return chaptersEnvOn(e);
    return chaptersEnvOn(std::getenv("CHAPTERS"));
#endif
}

// Flag off, or a missing target, does not seek. The Program menu calls this
// with the marker start it stored.
inline int64_t chapterSeekTarget(bool enabled, int64_t startMs) {
#ifdef MUT_CHAPTERS_SEEK_WHEN_OFF
    (void)enabled;
    if (startMs < 0)
        return -1;
    return startMs;
#else
    if (!enabled || startMs < 0)
        return -1;
    return startMs;
#endif
}

} // namespace misterplex
