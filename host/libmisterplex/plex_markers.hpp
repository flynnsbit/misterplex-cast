#pragma once
// Plex <Marker> children of one <Video> (Skip Intro / Skip Credits / Up Next).
// Host-testable. No I/O. Multi-part items yield no markers (offsets are unverified).

#include "plex_text.hpp"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace misterplex::markers {

enum class Kind : uint8_t { Intro, Credits, Commercial };

struct Marker {
    Kind kind = Kind::Intro;
    int64_t startMs = 0;
    int64_t endMs = 0;
    bool isFinal = false;
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

// One pass (numeric &#NN; &#xHH; and the named entities). A second call would turn
// `&amp;lt;` into `<`; callers unescape here and nowhere else.
inline std::string xmlUnescape(std::string s) { return misterplex::xmlUnescape(std::move(s)); }

inline std::string_view firstVideoSpan(std::string_view xml) {
    const auto open = xml.find("<Video");
    if (open == std::string_view::npos)
        return {};
    const auto end = xml.find("</Video>", open);
    if (end == std::string_view::npos)
        return xml.substr(open);
    return xml.substr(open, end - open);
}

// Markers inside the first <Video>, cap 16. Drop end<=start, start<0, or start>duration.
// Clamp end to duration. A second <Part> drops the whole set.
inline std::vector<Marker> parseMarkers(std::string_view metadataXml, int64_t durationMs) {
    const std::string_view video = firstVideoSpan(metadataXml);
    if (video.empty()) {
#ifdef MUT_MARKERS_NO_MARKER_SHOWS
        return {Marker{Kind::Intro, 0, 60000, false}};
#else
        return {};
#endif
    }
    int parts = 0;
    for (std::size_t p = 0; (p = video.find("<Part", p)) != std::string_view::npos; p += 5)
        ++parts;
    if (parts > 1)
        return {};

    std::vector<Marker> out;
    std::size_t p = 0;
    bool saw = false;
    while ((p = video.find("<Marker", p)) != std::string_view::npos && out.size() < 16) {
        saw = true;
        const auto gt = video.find('>', p);
        if (gt == std::string_view::npos)
            break;
        const std::string_view open = video.substr(p, gt - p);
        p = gt + 1;
        std::string type = xmlAttr(open, "type");
        for (char& c : type)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        Kind kind;
        if (type == "intro")
            kind = Kind::Intro;
        else if (type == "credits" || type == "credit")
            kind = Kind::Credits;
        else if (type == "commercial")
            kind = Kind::Commercial;
        else
            continue;
        const std::string ss = xmlAttr(open, "startTimeOffset");
        const std::string es = xmlAttr(open, "endTimeOffset");
        if (ss.empty() || es.empty())
            continue;
        const int64_t start = std::strtoll(ss.c_str(), nullptr, 10);
        int64_t end = std::strtoll(es.c_str(), nullptr, 10);
        if (start < 0 || end <= start)
            continue;
        if (durationMs > 0 && start > durationMs)
            continue;
        if (durationMs > 0 && end > durationMs)
            end = durationMs;
        if (end <= start)
            continue;
        const std::string fin = xmlAttr(open, "final");
        const bool isFinal = fin == "1" || fin == "true";
        out.push_back(Marker{kind, start, end, isFinal});
    }
#ifdef MUT_MARKERS_NO_MARKER_SHOWS
    if (!saw)
        return {Marker{Kind::Intro, 0, 60000, false}};
#else
    (void)saw;
#endif
    return out;
}

// final="1" wins. Otherwise the last credits marker whose end is within 5 s of duration.
inline const Marker* finalCredits(const std::vector<Marker>& m, int64_t durationMs) {
    const Marker* flagged = nullptr;
    const Marker* near = nullptr;
    for (const Marker& mk : m) {
        if (mk.kind != Kind::Credits)
            continue;
        if (mk.isFinal)
            flagged = &mk;
        if (durationMs > 0 && mk.endMs <= durationMs && durationMs - mk.endMs <= 5000)
            near = &mk;
    }
    return flagged ? flagged : near;
}

// end + 500 ms. A universal transcode rounds that target up to a whole second.
inline int64_t adjustSkipTarget(int64_t markerEndMs, bool universalTranscode) {
    if (markerEndMs < 0)
        return 0;
    int64_t target = markerEndMs;
    if (target < INT64_MAX - 500)
        target += 500;
    if (!universalTranscode || target <= 0)
        return target;
    const int64_t sec = (target + 999) / 1000;
    if (sec > INT64_MAX / 1000)
        return target;
    return sec * 1000;
}

} // namespace misterplex::markers
