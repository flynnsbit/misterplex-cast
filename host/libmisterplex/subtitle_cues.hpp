#pragma once
// Phase-1 text cues: sidecar SRT and flattened ASS/SSA Dialogue lines.
// Host-testable. No libass, no libav subtitle decoder. 1 MiB hard cap, separate
// from the PlexUI 0012 art budget.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace misterplex {

inline constexpr size_t kSubtitleCueBytesCap = 1024u * 1024u;

struct SubtitleCue {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string line1;
    std::string line2;
};

struct CueList {
    std::vector<SubtitleCue> cues;
    size_t bytes = 0;
    bool overflow = false;     // hit the 1 MiB cap; first N cues kept
    bool hadOverrides = false; // ASS {\\...} stripped
    bool emptyFile = false;
};

struct CaptionLines {
    std::string line1;
    std::string line2;
    bool armed = false;
    int cueIndex = -1;  // 0-based into CueList; -1 = no cue at this mediaMs
    bool empty() const { return line1.empty() && line2.empty(); }
};

// One-shot dump the glass runner can grep or GET /misterplex/caption.
// Overlay band is FPGA raster pixels. HDMI scaler mode (e.g. Unknown 2200x513p)
// is not an input — captions do not read it.
inline std::string captionDebugLine(int64_t streamId, int cueIndex, int64_t mediaMs, bool armed,
                                    const std::string& line1, const std::string& line2, int bandX,
                                    int bandY, int bandW, int bandH, int overlayW, int overlayH) {
    std::string text = line1;
    if (!line2.empty()) {
        if (!text.empty())
            text += " / ";
        text += line2;
    }
    for (char& c : text) {
        if (c == '"' || c == '\n' || c == '\r')
            c = ' ';
    }
    char buf[768];
    std::snprintf(buf, sizeof buf,
                  "misterplexd: caption dump stream=%lld cue=%d mediaMs=%lld armed=%d text=\"%s\" "
                  "band=%d,%d,%d,%d overlay=%dx%d hdmi_mode=unused",
                  static_cast<long long>(streamId), cueIndex, static_cast<long long>(mediaMs),
                  armed ? 1 : 0, text.c_str(), bandX, bandY, bandW, bandH, overlayW, overlayH);
    return buf;
}

inline std::string& captionDumpStore() {
    static std::string s =
        "misterplexd: caption dump stream=0 cue=-1 mediaMs=0 armed=0 text=\"\" "
        "band=0,0,0,0 overlay=0x0 hdmi_mode=unused";
    return s;
}

inline void publishCaptionDump(const std::string& line) {
    if (captionDumpStore() == line)
        return;
    captionDumpStore() = line;
    std::fprintf(stderr, "%s\n", line.c_str());
}

// Cue clock: media time of the picture, never the bar / hold target.
inline int64_t captionClockMs(int64_t mediaMs, int64_t holdTargetMs) {
    (void)holdTargetMs;
    return mediaMs;
}

inline std::string cueTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

inline std::string stripUtf8Bom(std::string s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        s.erase(0, 3);
    return s;
}

inline std::string stripSrtTags(std::string s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            const auto e = s.find('>', i + 1);
            if (e == std::string::npos) {
                o.push_back(s[i++]);
                continue;
            }
            i = e + 1;
            continue;
        }
        o.push_back(s[i++]);
    }
    return o;
}

inline std::string stripAssOverrides(const std::string& s, bool& stripped) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '{') {
            const auto e = s.find('}', i + 1);
            if (e != std::string::npos) {
                stripped = true;
                i = e + 1;
                continue;
            }
        }
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'N' || s[i + 1] == 'n')) {
            o.push_back('\n');
            i += 2;
            continue;
        }
        o.push_back(s[i++]);
    }
    return o;
}

inline void splitCueLines(const std::string& text, std::string& a, std::string& b) {
    a.clear();
    b.clear();
    std::string cur;
    auto flush = [&](const std::string& line) {
        const std::string t = cueTrim(line);
        if (t.empty())
            return;
        if (a.empty())
            a = t;
        else if (b.empty())
            b = t;
    };
    for (char c : text) {
        if (c == '\n') {
            flush(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    flush(cur);
}

inline bool parseSrtTime(const std::string& raw, int64_t& ms) {
    int h = 0, m = 0, s = 0, frac = 0;
    char sep = 0;
    if (std::sscanf(raw.c_str(), "%d:%d:%d%c%d", &h, &m, &s, &sep, &frac) != 5)
        return false;
    if (sep != ',' && sep != '.')
        return false;
    if (frac < 0)
        return false;
    if (sep == '.') {
        if (frac < 100)
            frac *= 10;
        else if (frac >= 1000)
            frac /= 10;
    }
    ms = (static_cast<int64_t>(h) * 3600 + static_cast<int64_t>(m) * 60 + s) * 1000 + frac;
    return ms >= 0;
}

inline bool parseSrtTimes(const std::string& line, int64_t& startMs, int64_t& endMs) {
    const auto arrow = line.find("-->");
    if (arrow == std::string::npos)
        return false;
    return parseSrtTime(cueTrim(line.substr(0, arrow)), startMs) &&
           parseSrtTime(cueTrim(line.substr(arrow + 3)), endMs);
}

inline size_t cueFootprint(const SubtitleCue& c) {
    return sizeof(SubtitleCue) + c.line1.size() + c.line2.size();
}

inline bool pushCue(CueList& out, SubtitleCue c) {
    if (c.endMs < c.startMs)
        std::swap(c.endMs, c.startMs);
    const size_t add = cueFootprint(c);
    if (out.bytes + add > kSubtitleCueBytesCap) {
        out.overflow = true;
        return false;
    }
    out.bytes += add;
    out.cues.push_back(std::move(c));
    return true;
}

inline CueList parseSrtCues(std::string body) {
    CueList out;
    body = stripUtf8Bom(std::move(body));
    if (cueTrim(body).empty()) {
        out.emptyFile = true;
        return out;
    }
    if (body.size() > kSubtitleCueBytesCap) {
        body.resize(kSubtitleCueBytesCap);
        out.overflow = true;
    }
    std::vector<std::string> lines;
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r')
            cur.pop_back();
        lines.push_back(std::move(cur));
    }

    size_t i = 0;
    while (i < lines.size()) {
        while (i < lines.size() && cueTrim(lines[i]).empty())
            ++i;
        if (i >= lines.size())
            break;
        int dummy = 0;
        if (std::sscanf(cueTrim(lines[i]).c_str(), "%d", &dummy) == 1 &&
            lines[i].find("-->") == std::string::npos)
            ++i;
        if (i >= lines.size())
            break;
        int64_t t0 = 0, t1 = 0;
        if (!parseSrtTimes(lines[i], t0, t1)) {
            ++i;
            continue;
        }
        ++i;
        std::string text;
        while (i < lines.size() && !cueTrim(lines[i]).empty()) {
            if (!text.empty())
                text.push_back('\n');
            text += stripSrtTags(lines[i]);
            ++i;
        }
        SubtitleCue c;
        c.startMs = t0;
        c.endMs = t1;
        splitCueLines(text, c.line1, c.line2);
        if (!pushCue(out, std::move(c)))
            break;
    }
    return out;
}

inline bool parseAssTime(const std::string& raw, int64_t& ms) {
    int h = 0, m = 0, s = 0, cs = 0;
    if (std::sscanf(cueTrim(raw).c_str(), "%d:%d:%d.%d", &h, &m, &s, &cs) != 4)
        return false;
    ms = (static_cast<int64_t>(h) * 3600 + static_cast<int64_t>(m) * 60 + s) * 1000 +
         static_cast<int64_t>(cs) * 10;
    return ms >= 0;
}

// Dialogue: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
inline bool parseAssDialogue(const std::string& line, SubtitleCue& c, bool& stripped) {
    const auto colon = line.find(':');
    if (colon == std::string::npos)
        return false;
    const std::string rest = line.substr(colon + 1);
    int commas = 0;
    size_t start = 0;
    std::string fields[9];
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == ',') {
            if (commas < 9)
                fields[commas] = rest.substr(start, i - start);
            ++commas;
            start = i + 1;
            if (commas == 9)
                break;
        }
    }
    if (commas < 9)
        return false;
    if (!parseAssTime(fields[1], c.startMs) || !parseAssTime(fields[2], c.endMs))
        return false;
    const std::string text = stripAssOverrides(rest.substr(start), stripped);
    splitCueLines(text, c.line1, c.line2);
    return true;
}

inline CueList parseAssCues(std::string body) {
    CueList out;
    body = stripUtf8Bom(std::move(body));
    if (cueTrim(body).empty()) {
        out.emptyFile = true;
        return out;
    }
    if (body.size() > kSubtitleCueBytesCap) {
        body.resize(kSubtitleCueBytesCap);
        out.overflow = true;
    }
    std::string cur;
    auto handle = [&](std::string line) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string t = cueTrim(line);
        if (t.size() < 9)
            return;
        std::string head = t.substr(0, 9);
        for (char& ch : head)
            ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        if (head != "dialogue:")
            return;
        SubtitleCue c;
        if (!parseAssDialogue(t, c, out.hadOverrides))
            return;
        pushCue(out, std::move(c));
    };
    for (char c : body) {
        if (c == '\n') {
            handle(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        handle(std::move(cur));
    return out;
}

// Last cue that contains mediaMs wins (overlapping).
inline CaptionLines captionAt(const CueList& list, int64_t mediaMs) {
    CaptionLines o;
    for (size_t i = 0; i < list.cues.size(); ++i) {
        const SubtitleCue& c = list.cues[i];
        if (mediaMs >= c.startMs && mediaMs < c.endMs) {
            o.line1 = c.line1;
            o.line2 = c.line2;
            o.cueIndex = static_cast<int>(i);
        }
    }
    return o;
}

// True when the body is a WebVTT file (BOM already tolerated). Sidecar SRT stays on parseSrtCues.
inline bool bodyLooksWebVtt(const std::string& body) {
    std::string s = stripUtf8Bom(body);
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        ++i;
    return s.compare(i, 6, "WEBVTT") == 0;
}

// WebVTT cue timings. Hours may be omitted (MM:SS.mmm). Cue settings after the end time are ignored.
inline bool parseVttTimes(const std::string& line, int64_t& startMs, int64_t& endMs) {
    const auto arrow = line.find("-->");
    if (arrow == std::string::npos)
        return false;
    auto padHours = [](std::string t) {
        t = cueTrim(t);
        int colons = 0;
        for (char c : t)
            if (c == ':')
                ++colons;
        if (colons == 1)
            t = "00:" + t;
        return t;
    };
    std::string right = cueTrim(line.substr(arrow + 3));
    const auto sp = right.find(' ');
    if (sp != std::string::npos)
        right = right.substr(0, sp);
    return parseSrtTime(padHours(line.substr(0, arrow)), startMs) && parseSrtTime(padHours(right), endMs);
}

inline CueList parseWebVttCues(std::string body) {
    CueList out;
    body = stripUtf8Bom(std::move(body));
    if (cueTrim(body).empty()) {
        out.emptyFile = true;
        return out;
    }
    if (body.size() > kSubtitleCueBytesCap) {
        body.resize(kSubtitleCueBytesCap);
        out.overflow = true;
    }
    std::vector<std::string> lines;
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r')
            cur.pop_back();
        lines.push_back(std::move(cur));
    }
    bool header = true;
    size_t i = 0;
    while (i < lines.size()) {
        if (header) {
            if (cueTrim(lines[i]).empty())
                header = false;
            ++i;
            continue;
        }
        while (i < lines.size() && cueTrim(lines[i]).empty())
            ++i;
        if (i >= lines.size())
            break;
        const std::string head = cueTrim(lines[i]);
        if (head.rfind("NOTE", 0) == 0 || head.rfind("STYLE", 0) == 0 || head.rfind("REGION", 0) == 0) {
            ++i;
            while (i < lines.size() && !cueTrim(lines[i]).empty())
                ++i;
            continue;
        }
        if (head.find("-->") == std::string::npos && i + 1 < lines.size() && lines[i + 1].find("-->") != std::string::npos)
            ++i;
        if (i >= lines.size())
            break;
        int64_t t0 = 0, t1 = 0;
        if (!parseVttTimes(lines[i], t0, t1)) {
            ++i;
            continue;
        }
        ++i;
        std::string text;
        while (i < lines.size() && !cueTrim(lines[i]).empty()) {
            if (!text.empty())
                text.push_back('\n');
            text += stripSrtTags(lines[i]);
            ++i;
        }
        SubtitleCue c;
        c.startMs = t0;
        c.endMs = t1;
        splitCueLines(text, c.line1, c.line2);
        if (!pushCue(out, std::move(c)))
            break;
    }
    return out;
}

// One decoded subtitle payload (plain text or one ASS Dialogue line) to at most two lines.
// Not FFmpeg clients/common/subs.c.
inline std::string flattenDecodedSubtitleText(std::string raw) {
    bool stripped = false;
    if (raw.find("Dialogue:") != std::string::npos) {
        SubtitleCue c;
        if (parseAssDialogue(raw, c, stripped)) {
            if (c.line2.empty())
                return c.line1;
            return c.line1 + "\n" + c.line2;
        }
    }
    raw = stripAssOverrides(stripSrtTags(std::move(raw)), stripped);
    std::string a, b;
    splitCueLines(raw, a, b);
    if (b.empty())
        return a;
    return a + "\n" + b;
}

} // namespace misterplex
