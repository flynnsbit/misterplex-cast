#pragma once
#include "yt_cache.hpp"
// YouTube lounge captions onto the HDMI caption band (YT_CAPTIONS, default off).
//
// The phone's getSubtitlesTrack is already acked. setSubtitlesTrack was not
// parsed, and the Owenb caption clock only paints SOFT_SUBS cues, so a cast
// never showed text. Flag on: yt-dlp (the same binary as playback) writes the
// signed subtitle file and this clock paints it. Flag off: the board is not
// consulted, the set command stays unparsed, and SOFT_SUBS is unchanged.
//
// An unsigned www.youtube.com/api/timedtext URL is not a caption source.
// Glass on that URL was HTTP 429 and the board never armed. No second clock:
// linesFor is read from onCaptionClock(mediaMs).

#include "airplay/ns.hpp"
#include "gcast/lounge_proto.hpp"
#include "libmisterplex/subtitle_cues.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {
namespace airplay {
namespace yt_captions {

struct YtCaptionRequest {
    std::string videoId;
    std::string lang;
    std::string kind;  // "asr" or empty
};

// yt-dlp argv that writes a subtitle file next to outStem (yt-dlp appends
// .lang.vtt). Same watch URL and player-client extractor arg as a media
// resolve. Not an unsigned /api/timedtext URL: that path is HTTP 429.
inline std::vector<std::string> captionYtdlpArgs(const YtCaptionRequest& r, const std::string& outStem,
                                                 const char* extractorArgs) {
#ifdef MUT_LP_CAPTIONS_UNSIGNED
    (void)outStem;
    (void)extractorArgs;
    if (!lounge::validVideoId(r.videoId) || !lounge::captionLangOk(r.lang))
        return {};
    std::string url = "https://www.youtube.com/api/timedtext?v=" + r.videoId + "&lang=" + r.lang + "&fmt=vtt";
    if (r.kind == "asr")
        url += "&kind=asr";
    return {url};
#else
    if (!lounge::validVideoId(r.videoId) || !lounge::captionLangOk(r.lang) || outStem.empty() ||
        outStem.find("..") != std::string::npos)
        return {};
    std::vector<std::string> args;
    yt_dlp::appendYtCacheArgs(args);
    args.emplace_back("--skip-download");
    args.emplace_back("--no-playlist");
    args.emplace_back("--no-warnings");
    args.emplace_back("--write-subs");
    args.emplace_back("--write-auto-subs");
    args.emplace_back("--sub-langs");
    // Exactly the requested language (the 10/31 hand run: --sub-langs en). "en.*"
    // pulled every en-* track, one more HTTP fetch (and 429 chance) each.
    args.emplace_back(r.lang);
    args.emplace_back("--sub-format");
    args.emplace_back("vtt/srv3/best");
    if (extractorArgs && extractorArgs[0]) {
        args.emplace_back("--extractor-args");
        args.emplace_back(extractorArgs);
    }
    args.emplace_back("-o");
    args.emplace_back(outStem);
    args.emplace_back(std::string("https://www.youtube.com/watch?v=") + r.videoId);
    return args;
#endif
}

// True only for the yt-dlp subtitle argv. An unsigned timedtext URL is not.
inline bool captionArgsAreYtdlp(const std::vector<std::string>& args) {
    bool skip = false, autoSubs = false, watch = false, timed = false, dashO = false;
    for (const auto& a : args) {
        if (a == "--skip-download")
            skip = true;
        if (a == "--write-auto-subs")
            autoSubs = true;
        if (a == "-o")
            dashO = true;
        if (a.find("watch?v=") != std::string::npos)
            watch = true;
        if (a.find("api/timedtext") != std::string::npos)
            timed = true;
    }
    return skip && autoSubs && watch && dashO && !timed;
}

inline void appendUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) {
        o.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

inline bool utf8Scalar(unsigned cp) {
    return cp >= 32 && cp <= 0xFFFF && (cp < 0xD800 || cp > 0xDFFF);
}

inline void decodeXmlEntities(std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            o.push_back(s[i++]);
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) {
            o.push_back(s[i++]);
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") o.push_back('&');
        else if (ent == "lt") o.push_back('<');
        else if (ent == "gt") o.push_back('>');
        else if (ent == "quot") o.push_back('"');
        else if (ent == "apos" || ent == "#39") o.push_back('\'');
        else if (ent.size() > 2 && ent[0] == '#' && ent[1] == 'x') {
            unsigned v = 0;
            bool ok = true;
            for (size_t k = 2; k < ent.size(); ++k) {
                unsigned char c = static_cast<unsigned char>(ent[k]);
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else { ok = false; break; }
            }
            if (ok && utf8Scalar(v)) appendUtf8(o, v);
        } else if (ent.size() > 1 && ent[0] == '#') {
            unsigned v = 0;
            bool ok = true;
            for (size_t k = 1; k < ent.size(); ++k) {
                if (ent[k] < '0' || ent[k] > '9') { ok = false; break; }
                v = v * 10 + static_cast<unsigned>(ent[k] - '0');
            }
            if (ok && utf8Scalar(v)) appendUtf8(o, v);
        } else {
            o.push_back(s[i++]);
            continue;
        }
        i = semi + 1;
    }
    s.swap(o);
}

inline int64_t xmlAttrInt(const std::string& tag, const char* key) {
    const std::string k = std::string(key) + "=";
    const size_t p0 = tag.find(k);
    if (p0 == std::string::npos)
        return -1;
    size_t p = p0 + k.size();
    if (p < tag.size() && (tag[p] == '"' || tag[p] == '\''))
        ++p;
    int64_t v = 0;
    bool any = false;
    while (p < tag.size() && tag[p] >= '0' && tag[p] <= '9') {
        any = true;
        v = v * 10 + (tag[p] - '0');
        if (v > 7LL * 86400000LL)
            return -1;
        ++p;
    }
    return any ? v : -1;
}

inline misterplex::CueList parseSrv3Cues(std::string body) {
    misterplex::CueList out;
    if (body.size() > misterplex::kSubtitleCueBytesCap) {
        body.resize(misterplex::kSubtitleCueBytesCap);
        out.overflow = true;
    }
    size_t i = 0;
    while (i < body.size()) {
        const size_t open = body.find("<p", i);
        if (open == std::string::npos)
            break;
        if (open + 2 < body.size() && body[open + 2] != ' ' && body[open + 2] != '>' && body[open + 2] != '\t' &&
            body[open + 2] != '\n' && body[open + 2] != '\r') {
            i = open + 2;
            continue;
        }
        const size_t gt = body.find('>', open);
        if (gt == std::string::npos)
            break;
        const std::string tag = body.substr(open, gt - open);
        const int64_t t = xmlAttrInt(tag, "t");
        const int64_t d = xmlAttrInt(tag, "d");
        const size_t close = body.find("</p>", gt);
        std::string text = body.substr(gt + 1, close == std::string::npos ? std::string::npos : close - (gt + 1));
        i = close == std::string::npos ? body.size() : close + 4;
        if (t < 0 || d <= 0)
            continue;
        for (size_t b = 0; b + 4 <= text.size();) {
            if ((text[b] == '<' && (text.compare(b, 4, "<br>") == 0 || text.compare(b, 4, "<br ") == 0)) ||
                (text[b] == '<' && text.compare(b, 5, "<br/>") == 0)) {
                const size_t end = text.find('>', b);
                if (end == std::string::npos)
                    break;
                text.replace(b, end - b + 1, "\n");
                continue;
            }
            ++b;
        }
        decodeXmlEntities(text);
        text = misterplex::stripSrtTags(std::move(text));
        misterplex::SubtitleCue c;
        c.startMs = t;
        c.endMs = t + d;
        misterplex::splitCueLines(text, c.line1, c.line2);
        if (c.line1.empty() && c.line2.empty())
            continue;
        if (!misterplex::pushCue(out, std::move(c)))
            break;
    }
    return out;
}

inline bool jsonStringAt(const std::string& s, size_t& i, std::string& out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"')
            return true;
        if (c == '\\' && i < s.size()) {
            char e = s[i++];
            if (e == 'n') out.push_back('\n');
            else if (e == 't') out.push_back(' ');
            else if (e == 'u' && i + 4 <= s.size()) {
                unsigned v = 0;
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    unsigned char h = static_cast<unsigned char>(s[i++]);
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= h - '0';
                    else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                    else ok = false;
                }
                if (ok && utf8Scalar(v))
                    appendUtf8(out, v);
            } else
                out.push_back(e);
            continue;
        }
        if (static_cast<unsigned char>(c) >= 0x20)
            out.push_back(c);
    }
    return false;
}

inline int64_t jsonIntAfterKey(const std::string& s, size_t keyAt, const char* key) {
    const size_t n = std::strlen(key);
    if (keyAt + n > s.size() || s.compare(keyAt, n, key) != 0)
        return -1;
    size_t i = keyAt + n;
    while (i < s.size() && (s[i] == ' ' || s[i] == '"'))
        ++i;
    if (i >= s.size() || s[i] != ':')
        return -1;
    ++i;
    while (i < s.size() && (s[i] == ' '))
        ++i;
    bool neg = false;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    int64_t v = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        any = true;
        v = v * 10 + (s[i] - '0');
        if (v > 7LL * 86400000LL)
            return -1;
        ++i;
    }
    if (!any)
        return -1;
    return neg ? -v : v;
}

inline misterplex::CueList parseJson3Cues(std::string body) {
    misterplex::CueList out;
    if (body.size() > misterplex::kSubtitleCueBytesCap) {
        body.resize(misterplex::kSubtitleCueBytesCap);
        out.overflow = true;
    }
    size_t i = 0;
    while (i < body.size()) {
        const size_t key = body.find("\"tStartMs\"", i);
        if (key == std::string::npos)
            break;
        const int64_t t = jsonIntAfterKey(body, key + 1, "tStartMs\"");
        const size_t next = body.find("\"tStartMs\"", key + 10);
        const size_t end = next == std::string::npos ? body.size() : next;
        const int64_t d = [&]() {
            const size_t dk = body.find("\"dDurationMs\"", key);
            if (dk == std::string::npos || dk >= end)
                return int64_t(-1);
            return jsonIntAfterKey(body, dk + 1, "dDurationMs\"");
        }();
        std::string text;
        size_t u = key;
        while (u < end) {
            const size_t uk = body.find("\"utf8\"", u);
            if (uk == std::string::npos || uk >= end)
                break;
            size_t p = uk + 6;
            while (p < end && body[p] != ':')
                ++p;
            if (p >= end)
                break;
            ++p;
            std::string seg;
            if (!jsonStringAt(body, p, seg))
                break;
            text += seg;
            u = p;
        }
        i = end;
        if (t < 0)
            continue;
        const int64_t dur = d > 0 ? d : 2000;
        misterplex::SubtitleCue c;
        c.startMs = t;
        c.endMs = t + dur;
        misterplex::splitCueLines(text, c.line1, c.line2);
        if (c.line1.empty() && c.line2.empty())
            continue;
        if (!misterplex::pushCue(out, std::move(c)))
            break;
    }
    return out;
}

// WebVTT, YouTube srv3 XML, or json3. Anything else (an HTML error page) is
// an empty list: the track is not armed.
inline misterplex::CueList parseYtCaptionBody(std::string body) {
    body = misterplex::stripUtf8Bom(std::move(body));
    if (misterplex::bodyLooksWebVtt(body))
        return misterplex::parseWebVttCues(std::move(body));
    if (body.find("<p") != std::string::npos && body.find("t=") != std::string::npos)
        return parseSrv3Cues(std::move(body));
    if (body.find("\"tStartMs\"") != std::string::npos)
        return parseJson3Cues(std::move(body));
    return {};
}

// First subtitle file in dir that actually parses. A 429 HTML page saved beside
// it is not cues, so it is skipped. Empty when yt-dlp wrote nothing.
inline std::string captionTextInDir(const std::string& dir) {
    if (dir.empty() || dir.find("..") != std::string::npos)
        return {};
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return {};
    std::string best;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == ".." || name.find('/') != std::string::npos)
            continue;
        const bool vtt = name.size() >= 4 && name.compare(name.size() - 4, 4, ".vtt") == 0;
        const bool srv = name.size() >= 5 && name.compare(name.size() - 5, 5, ".srv3") == 0;
        if (!vtt && !srv)
            continue;
        const std::string path = dir + "/" + name;
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (st.st_size <= 0 || static_cast<size_t>(st.st_size) > misterplex::kSubtitleCueBytesCap)
            continue;
        std::ifstream in(path);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (parseYtCaptionBody(body).cues.empty())
            continue;
        best = std::move(body);
        break;
    }
    ::closedir(d);
    return best;
}

inline void removeCaptionDir(const std::string& dir) {
    static const char kPrefix[] = "/tmp/mpx-ytcap-";
    if (dir.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0)
        return;
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        if (name == "." || name == ".." || name.find('/') != std::string::npos)
            continue;
        ::unlink((dir + "/" + name).c_str());
    }
    ::closedir(d);
    ::rmdir(dir.c_str());
}

// Run is given the yt-dlp argv and returns the subtitle file body (not a URL).
// Flag off does not call run. An unsigned timedtext argv is not run.
template <class Run>
inline misterplex::CueList loadYtCaptionCues(const YtCaptionRequest& r, Run run) {
#ifdef MUT_LP_CAPTIONS_NO_FETCH
    (void)r;
    (void)run;
    return {};
#else
    if (!lounge::ytCaptionsEnabled())
        return {};
    const std::vector<std::string> args = captionYtdlpArgs(r, "out", nullptr);
    if (!captionArgsAreYtdlp(args))
        return {};
    return parseYtCaptionBody(run(args));
#endif
}

// Cues for one YouTube play. The caption clock reads linesFor(epoch, mediaMs).
// A different epoch (the next play) does not see them. Seek of the same id
// keeps the cues and moves the epoch. A new id drops them.
class YtCaptionBoard {
public:
    void notePlay(uint64_t epoch, const std::string& videoId, bool keepCues) {
        std::lock_guard<std::mutex> lk(mu_);
        if (videoId.empty()) {
            clearLocked();
            return;
        }
        epoch_ = epoch;
#ifdef MUT_LP_CAPTIONS_KEEP_STALE
        videoId_ = videoId;
        (void)keepCues;
        return;
#else
        const bool same = keepCues && videoId_ == videoId;
        if (!same) {
            ++loadGen_;
            show_ = false;
            cues_ = {};
            videoId_ = videoId;
        }
#endif
    }
    void hide() {
        std::lock_guard<std::mutex> lk(mu_);
        ++loadGen_;
        show_ = false;
        cues_ = {};
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        clearLocked();
    }
    uint64_t beginLoad(const std::string& videoId) {
        std::lock_guard<std::mutex> lk(mu_);
        ++loadGen_;
        if (loadGen_ == 0)
            ++loadGen_;
        videoId_ = videoId;
        show_ = false;
        cues_ = {};
        return loadGen_;
    }
    // A load that will not finish (cap hit): hide only while it is still the
    // current load, so a newer load's board is never cleared.
    bool failLoad(uint64_t gen) {
        std::lock_guard<std::mutex> lk(mu_);
#ifndef MUT_LP_CAPTIONS_CAP_HIDE_ALL
        if (gen != loadGen_)
            return false;
#else
        (void)gen;
#endif
        ++loadGen_;
        show_ = false;
        cues_ = {};
        return true;
    }
    bool isCurrent(uint64_t gen) const {
        std::lock_guard<std::mutex> lk(mu_);
        return gen == loadGen_;
    }
    bool finishLoad(uint64_t gen, const std::string& videoId, misterplex::CueList cues) {
        std::lock_guard<std::mutex> lk(mu_);
        if (gen != loadGen_ || videoId.empty() || videoId != videoId_ || cues.cues.empty())
            return false;
        cues_ = std::move(cues);
        show_ = true;
        return true;
    }
    bool linesFor(uint64_t epoch, int64_t mediaMs, misterplex::CaptionLines& out) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (!show_ || epoch == 0 || epoch_ != epoch)
            return false;
        out = misterplex::captionAt(cues_, mediaMs);
        out.armed = true;
        return true;
    }
    bool showing() const {
        std::lock_guard<std::mutex> lk(mu_);
        return show_;
    }
    size_t cueCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return cues_.cues.size();
    }

private:
    void clearLocked() {
        ++loadGen_;
        show_ = false;
        cues_ = {};
        videoId_.clear();
        epoch_ = 0;
    }
    mutable std::mutex mu_;
    std::string videoId_;
    uint64_t epoch_ = 0;
    uint64_t loadGen_ = 1;
    bool show_ = false;
    misterplex::CueList cues_{};
};

inline YtCaptionBoard& board() {
    static YtCaptionBoard b;
    return b;
}

// The Owenb caption clock. Flag off returns false and leaves out alone, so
// the soft-sub band is unchanged. A selected track returns true even between
// cues (armed, empty lines) so the band stays reserved.
inline bool ytCaptionLinesForClock(uint64_t epoch, int64_t mediaMs, misterplex::CaptionLines& out) {
#ifdef MUT_LP_CAPTIONS_SKIP_PAINT
    (void)epoch;
    (void)mediaMs;
    (void)out;
    return false;
#else
    if (!lounge::ytCaptionsEnabled())
        return false;
    return board().linesFor(epoch, mediaMs, out);
#endif
}

}  // namespace yt_captions
}  // namespace airplay
}  // namespace misterplex
