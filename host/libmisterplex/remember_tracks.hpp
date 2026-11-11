#pragma once
// REMEMBER_TRACKS (misterplex.conf) / MPX_REMEMBER_TRACKS (env wins). Default off.
//
// Off or unset: play and re-resolve match this tip. show_tracks.txt (language /
// codec / forced) is unchanged. This file is not read for a pick.
//
// On (1/on/yes/true): a Program-menu or setStreams pick is stored on this MiSTer
// in remember_tracks.txt, keyed the same way as show memory (episode
// grandparentRatingKey, otherwise ratingKey). The next episode of that show
// restores the choice. The saved subtitleStreamID / audioStreamID is used when
// that part still has it. Otherwise the part's stream with the same language,
// codec, and title is used, and that part's id is what playback opens. The
// matched subtitle is written onto the PLAY URL the way an explicit setStreams
// pick is: burned (subtitles=burn and that subtitleStreamID) when the overlay
// will not show it, or subtitleStreamID=0 when the overlay will. The timeline
// echoes that id. Subtitles Off is subtitleStreamID=0 on the URL and stays off.
//
// The per-play 14:9 picture toggle is cleared at the start of each play.
// This store does not keep it. REMEMBER_PICTURE is the picture-mode store.
//
// No PMS write-back. track_pref_server_sync is a separate knob.

#include "libmisterplex/track_select.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <sys/stat.h>
#include <vector>

namespace misterplex {

inline bool rememberTracksEnvOn(const char* e) {
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

// Default off. MPX_REMEMBER_TRACKS wins over REMEMBER_TRACKS when it is set. Unset is off.
inline bool rememberTracksEnabled() {
#ifdef MUT_REMEMBER_TRACKS_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_REMEMBER_TRACKS"); e && e[0])
        return rememberTracksEnvOn(e);
    return rememberTracksEnvOn(std::getenv("REMEMBER_TRACKS"));
#endif
}

// One saved Program-menu / setStreams choice for a show.
struct RememberedChoice {
    std::string showKey;
    bool hasAudio = false;
    int audioStreamId = 0;
    std::string audioLanguage;
    std::string audioCodec;
    std::string audioTitle;
    bool hasSubtitle = false;
    int subtitleStreamId = 0; // 0 with hasSubtitle and subtitlesOff: the user turned subs off
    bool subtitlesOff = false;
    std::string subtitleLanguage;
    std::string subtitleCodec;
    bool subtitleForced = false;
    std::string subtitleTitle;
};

inline constexpr const char* kRememberMagic = "MPXREMEMBER 1";
inline constexpr int kRememberFields = 13;

inline std::string rememberClip(std::string s) {
    if (s.size() > kMaxShowPrefField)
        s.resize(kMaxShowPrefField);
    return s;
}

inline std::string rememberStreamTitle(const MediaStream& s) {
    return rememberClip(!s.displayTitle.empty() ? s.displayTitle : s.title);
}

inline bool rememberTitleMatch(const std::string& stored, const MediaStream& s) {
    if (stored.empty())
        return false;
    const std::string want = trackLower(stored);
    if (!s.displayTitle.empty() && trackLower(s.displayTitle) == want)
        return true;
    if (!s.title.empty() && trackLower(s.title) == want)
        return true;
    const std::string shown = trackLower(rememberStreamTitle(s));
    return !shown.empty() && shown == want;
}

inline bool rememberSplit(const std::string& line, std::vector<std::string>& f) {
    f.clear();
    std::string cur;
    bool esc = false;
    for (char c : line) {
        if (esc) {
            if (c == 'n')
                cur.push_back('\n');
            else if (c == 'r')
                cur.push_back('\r');
            else
                cur.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '|') {
            f.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        if (c == '\r')
            continue;
        cur.push_back(c);
    }
    if (esc)
        return false;
    f.push_back(std::move(cur));
    return true;
}

inline bool rememberFieldOk(const std::string& s) { return showPrefFieldOk(s); }

inline std::string rememberEncode(const RememberedChoice& c) {
    if (c.showKey.empty() || !rememberFieldOk(c.showKey) || !rememberFieldOk(c.audioLanguage) ||
        !rememberFieldOk(c.audioCodec) || !rememberFieldOk(c.audioTitle) || !rememberFieldOk(c.subtitleLanguage) ||
        !rememberFieldOk(c.subtitleCodec) || !rememberFieldOk(c.subtitleTitle))
        return {};
    if (c.hasAudio && c.audioStreamId <= 0)
        return {};
    if (!c.hasAudio && c.audioStreamId != 0)
        return {};
    if (c.hasSubtitle && c.subtitlesOff && c.subtitleStreamId != 0)
        return {};
    if (c.hasSubtitle && !c.subtitlesOff && c.subtitleStreamId <= 0)
        return {};
    if (!c.hasSubtitle && (c.subtitleStreamId != 0 || c.subtitlesOff))
        return {};
    return showPrefEscape(c.showKey) + "|" + (c.hasAudio ? "1" : "0") + "|" + std::to_string(c.audioStreamId) + "|" +
           showPrefEscape(c.audioLanguage) + "|" + showPrefEscape(c.audioCodec) + "|" + showPrefEscape(c.audioTitle) +
           "|" + (c.hasSubtitle ? "1" : "0") + "|" + std::to_string(c.subtitleStreamId) + "|" +
           (c.subtitlesOff ? "1" : "0") + "|" + showPrefEscape(c.subtitlesOff ? "" : c.subtitleLanguage) + "|" +
           showPrefEscape(c.subtitlesOff ? "" : c.subtitleCodec) + "|" + (c.subtitleForced ? "1" : "0") + "|" +
           showPrefEscape(c.subtitlesOff ? "" : c.subtitleTitle);
}

inline bool rememberDecode(const std::string& line, RememberedChoice& out) {
    std::vector<std::string> f;
    if (!rememberSplit(line, f) || static_cast<int>(f.size()) != kRememberFields)
        return false;
    for (const std::string& field : f)
        if (!rememberFieldOk(field))
            return false;
    if (f[0].empty() || (f[1] != "0" && f[1] != "1") || (f[6] != "0" && f[6] != "1") ||
        (f[8] != "0" && f[8] != "1") || (f[11] != "0" && f[11] != "1"))
        return false;
    int audioId = 0;
    int subId = 0;
    if (!trackParseInt(f[2], audioId) || !trackParseInt(f[7], subId))
        return false;
    RememberedChoice c;
    c.showKey = f[0];
    c.hasAudio = f[1] == "1";
    c.audioStreamId = audioId;
    c.audioLanguage = canonLanguage(f[3]);
    c.audioCodec = trackLower(f[4]);
    c.audioTitle = f[5];
    c.hasSubtitle = f[6] == "1";
    c.subtitleStreamId = subId;
    c.subtitlesOff = f[8] == "1";
    c.subtitleLanguage = c.subtitlesOff ? std::string() : canonLanguage(f[9]);
    c.subtitleCodec = c.subtitlesOff ? std::string() : trackLower(f[10]);
    c.subtitleForced = !c.subtitlesOff && f[11] == "1";
    c.subtitleTitle = c.subtitlesOff ? std::string() : f[12];
    if (rememberEncode(c).empty())
        return false;
    out = std::move(c);
    return true;
}

inline void rememberApplyAudio(TrackPick& p, const MediaStream& a) {
    p.valid = true;
    p.audioStreamId = a.id;
    p.audioIndex = a.index;
    p.audioLanguage = a.language;
    p.audioCodec = a.codec;
    p.fromPreference = true;
}

inline void rememberApplySubtitle(TrackPick& p, const MediaStream& s) {
    p.valid = true;
    p.fromPreference = true;
    p.rememberedSubtitle = true;
    p.subtitlesOff = false;
    p.subtitleStreamId = s.id;
    p.subtitleIndex = s.index;
    p.subtitleLanguage = s.language;
    p.subtitleCodec = s.codec;
    p.subtitleForced = s.forced;
    p.packetText = subtitleIsPacketTextTrack(s);
    const bool soft = subtitleIsSoftNow(s);
    p.imageSubtitleSkipped = !soft;
    p.imageNote = soft ? std::string()
                       : "subtitles (" + (s.codec.empty() ? std::string("embedded") : s.codec) +
                             ") are burned by PMS";
}

inline void rememberClearSubtitle(TrackPick& p) {
    p.valid = true;
    p.fromPreference = true;
    p.rememberedSubtitle = true;
    p.subtitlesOff = true;
    p.subtitleStreamId = 0;
    p.subtitleIndex = -1;
    p.subtitleLanguage.clear();
    p.subtitleCodec.clear();
    p.subtitleForced = false;
    p.packetText = false;
    p.imageSubtitleSkipped = false;
    p.imageNote.clear();
}

inline const MediaStream* matchRememberedAudio(const StreamCatalog& cat, const RememberedChoice& mem, int partId) {
    const bool needLang = !mem.audioLanguage.empty();
    const bool needTitle = !needLang && !mem.audioTitle.empty();
    if (!needLang && !needTitle)
        return nullptr;
    const MediaStream* best = nullptr;
    int bestScore = -1;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Audio || !track_detail::streamInPart(s, partId))
            continue;
        if (needLang && !languagesMatch(s.language, mem.audioLanguage) &&
            !languagesMatch(s.languageRaw, mem.audioLanguage))
            continue;
        if (needTitle && !rememberTitleMatch(mem.audioTitle, s))
            continue;
        int score = 0;
        if (!mem.audioCodec.empty() && trackLower(s.codec) == mem.audioCodec)
            score += 4;
        if (rememberTitleMatch(mem.audioTitle, s))
            score += 8;
        if (!best || score > bestScore) {
            best = &s;
            bestScore = score;
        }
    }
    return best;
}

inline const MediaStream* matchRememberedSubtitle(const StreamCatalog& cat, const RememberedChoice& mem, int partId) {
    const bool needLang = !mem.subtitleLanguage.empty();
    const bool needTitle = !needLang && !mem.subtitleTitle.empty();
    const bool codecOnly = !needLang && !needTitle && !mem.subtitleCodec.empty();
    if (!needLang && !needTitle && !codecOnly)
        return nullptr;
    const MediaStream* best = nullptr;
    int bestScore = -1;
    int codecHits = 0;
    const MediaStream* codecOnlyHit = nullptr;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || !track_detail::streamInPart(s, partId))
            continue;
#ifdef MUT_REMEMBER_TRACKS_SKIP_IMAGE
        if (subtitleCodecIsImage(s.codec))
            continue;
#endif
        if (codecOnly) {
            if (!mem.subtitleCodec.empty() && trackLower(s.codec) == mem.subtitleCodec &&
                s.forced == mem.subtitleForced) {
                ++codecHits;
                codecOnlyHit = &s;
            }
            continue;
        }
        if (needLang && !languagesMatch(s.language, mem.subtitleLanguage) &&
            !languagesMatch(s.languageRaw, mem.subtitleLanguage))
            continue;
        if (needTitle && !rememberTitleMatch(mem.subtitleTitle, s))
            continue;
        int score = 0;
        if (!mem.subtitleCodec.empty() && trackLower(s.codec) == mem.subtitleCodec)
            score += 4;
        if (mem.subtitleForced == s.forced)
            score += 3;
        if (rememberTitleMatch(mem.subtitleTitle, s))
            score += 8;
        if (!best || score > bestScore) {
            best = &s;
            bestScore = score;
        }
    }
    if (codecOnly)
        return codecHits == 1 ? codecOnlyHit : nullptr;
    return best;
}

// Flag off: current, unchanged. Flag on: exact stream id on this part, else the
// matching stream's id (language, then codec, then title). Subtitles Off sticks.
inline TrackPick applyRememberedTracks(const StreamCatalog& cat, const RememberedChoice& mem, TrackPick current,
                                       int partId) {
#ifndef MUT_REMEMBER_TRACKS_OFF_STILL_APPLIES
    if (!rememberTracksEnabled())
        return current;
#endif
#ifdef MUT_REMEMBER_TRACKS_IGNORED
    (void)cat;
    (void)mem;
    (void)partId;
    return current;
#endif
    TrackPick p = current;
    if (mem.hasAudio) {
        const MediaStream* a = findStreamById(cat, mem.audioStreamId, StreamKind::Audio, partId);
#ifndef MUT_REMEMBER_TRACKS_SAME_ID_ONLY
        if (!a)
            a = matchRememberedAudio(cat, mem, partId);
#endif
        if (a)
            rememberApplyAudio(p, *a);
    }
    if (mem.hasSubtitle) {
        if (mem.subtitlesOff) {
#ifdef MUT_REMEMBER_TRACKS_DROP_OFF
            (void)p;
#else
            rememberClearSubtitle(p);
#endif
        } else {
            const MediaStream* s = findStreamById(cat, mem.subtitleStreamId, StreamKind::Subtitle, partId);
#ifndef MUT_REMEMBER_TRACKS_SAME_ID_ONLY
            if (!s)
                s = matchRememberedSubtitle(cat, mem, partId);
#endif
            if (s)
                rememberApplySubtitle(p, *s);
        }
    }
    return p;
}

// What the universal PLAY URL should carry for a remembered subtitle.
// -1 omits the parameter (the tip path, and a pick this flag did not apply).
// 0 is Off. >0 with burn is the explicit image / overlay-off path: PMS burns
// that id and the timeline reads it off the URL. >0 is not left as "omit".
struct RememberWire {
    bool burn = false;
    int subtitleStreamId = -1;
};

inline RememberWire rememberSubtitleWire(const TrackPick& pick, const StreamCatalog* cat = nullptr,
                                         int partId = 0) {
    RememberWire w;
    if (!pick.rememberedSubtitle)
        return w;
#ifdef MUT_REMEMBER_TRACKS_OMIT_SUB_URL
    // The pick still carries the id the resolve log prints. The PLAY URL and
    // the timeline keep the omitted subtitle. This is the glass failure.
    return w;
#endif
    if (pick.subtitlesOff || pick.subtitleStreamId <= 0) {
        w.subtitleStreamId = 0;
        w.burn = false;
        return w;
    }
    // Overlay on: the sink paints soft text. The URL carries subtitleStreamID=0
    // so PMS does not also burn it. The timeline keeps the pick id via the sink.
    // #201-r3: with the catalog, ask the player's own kind function. SRT/ASS never burn.
    const bool overlay = cat ? !subtitlePickMayBurn(*cat, pick, partId)
                             : ((softOverlayAllowed() && subtitleCodecKeepsDirectPlay(pick.subtitleCodec) &&
                                 !pick.imageSubtitleSkipped) ||
                                subtitleCodecIsSoftText(pick.subtitleCodec));
    if (overlay) {
        w.burn = false;
        w.subtitleStreamId = 0;
        return w;
    }
    // Same gate as subtitlePickMayBurn: explicit AUTO_BURN wins, and a legacy
    // BURN_SUBS_FIX=1 is on only when AUTO_BURN is unset.
    if (!autoBurnEnabled()) {
        w.burn = false;
        w.subtitleStreamId = pick.subtitleStreamId;
        return w;
    }
    w.burn = true;
    w.subtitleStreamId = pick.subtitleStreamId;
    return w;
}

struct RememberTrackLoad {
    bool loaded = false;
    bool recovered = false;
    size_t kept = 0;
    size_t skipped = 0;
};

class RememberTrackStore {
public:
    RememberTrackLoad load(const std::string& path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = path;
        rows_.clear();
        dirty_ = false;
        RememberTrackLoad st;
        struct stat info;
        if (::stat(path.c_str(), &info) == 0 && static_cast<uint64_t>(info.st_size) > kMaxShowTrackFileBytes) {
            st.loaded = true;
            st.recovered = true;
            quarantineLocked(path);
            return st;
        }
        std::ifstream in(path);
        if (!in)
            return st;
        st.loaded = true;
        std::string magic;
        if (!std::getline(in, magic)) {
            st.recovered = true;
            quarantineLocked(path);
            return st;
        }
        if (!magic.empty() && magic.back() == '\r')
            magic.pop_back();
        if (magic != kRememberMagic) {
            st.recovered = true;
            in.close();
            quarantineLocked(path);
            return st;
        }
        std::string line;
        size_t bytes = magic.size();
        while (std::getline(in, line)) {
            bytes += line.size() + 1;
            if (bytes > kMaxShowTrackFileBytes) {
                st.recovered = true;
                rows_.clear();
                in.close();
                quarantineLocked(path);
                return st;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            RememberedChoice c;
            if (!rememberDecode(line, c)) {
                ++st.skipped;
                st.recovered = true;
                continue;
            }
            putLocked(std::move(c));
            ++st.kept;
        }
        if (st.recovered)
            saveLocked();
        return st;
    }

    std::optional<RememberedChoice> find(const std::string& showKey) const {
        std::lock_guard<std::mutex> g(mu_);
        for (const RememberedChoice& c : rows_)
            if (c.showKey == showKey)
                return c;
        return std::nullopt;
    }

    bool stage(RememberedChoice c) {
        if (rememberEncode(c).empty())
            return false;
        std::lock_guard<std::mutex> g(mu_);
        putLocked(std::move(c));
        dirty_ = true;
        return true;
    }

    // Merge one side of a pick into the show row. Flag off: no write.
    // audioId <= 0 does not clear a saved audio choice. subtitleId <= 0 stores Off.
    bool note(const std::string& showKey, const StreamCatalog& cat, int partId, int audioId, int subtitleId,
              bool noteAudio, bool noteSub) {
#ifndef MUT_REMEMBER_TRACKS_OFF_STILL_SAVES
        if (!rememberTracksEnabled())
            return false;
#endif
        if (showKey.empty() || (!noteAudio && !noteSub))
            return false;
        RememberedChoice c;
        if (const std::optional<RememberedChoice> existing = find(showKey))
            c = *existing;
        c.showKey = showKey;
        bool changed = false;
        if (noteAudio && audioId > 0) {
            if (const MediaStream* a = findStreamById(cat, audioId, StreamKind::Audio, partId)) {
                c.hasAudio = true;
                c.audioStreamId = a->id;
                c.audioLanguage = a->language;
                c.audioCodec = trackLower(a->codec);
                c.audioTitle = rememberStreamTitle(*a);
                changed = true;
            }
        }
        if (noteSub) {
            if (subtitleId <= 0) {
                c.hasSubtitle = true;
                c.subtitlesOff = true;
                c.subtitleStreamId = 0;
                c.subtitleLanguage.clear();
                c.subtitleCodec.clear();
                c.subtitleForced = false;
                c.subtitleTitle.clear();
                changed = true;
            } else if (const MediaStream* s = findStreamById(cat, subtitleId, StreamKind::Subtitle, partId)) {
                c.hasSubtitle = true;
                c.subtitlesOff = false;
                c.subtitleStreamId = s->id;
                c.subtitleLanguage = s->language;
                c.subtitleCodec = trackLower(s->codec);
                c.subtitleForced = s->forced;
                c.subtitleTitle = rememberStreamTitle(*s);
                changed = true;
            }
        }
        if (!changed)
            return false;
        return stage(std::move(c));
    }

    bool flush() {
        std::string path;
        std::string body;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!dirty_ || path_.empty())
                return true;
            dirty_ = false;
            path = path_;
            body = bodyLocked();
        }
        return writeShowTrackFile(path, body);
    }

    void setPath(std::string path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = std::move(path);
    }

    size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return rows_.size();
    }

private:
    void putLocked(RememberedChoice c) {
        if (c.showKey.empty())
            return;
        for (auto it = rows_.begin(); it != rows_.end(); ++it) {
            if (it->showKey == c.showKey) {
                rows_.erase(it);
                break;
            }
        }
        rows_.push_back(std::move(c));
        while (rows_.size() > kMaxShowPrefs)
            rows_.erase(rows_.begin());
    }

    std::string bodyLocked() const {
        std::string body = std::string(kRememberMagic) + "\n";
        for (const RememberedChoice& c : rows_) {
            const std::string line = rememberEncode(c);
            if (!line.empty())
                body += line + "\n";
        }
        return body;
    }

    bool saveLocked() {
        if (path_.empty())
            return false;
        dirty_ = false;
        return writeShowTrackFile(path_, bodyLocked());
    }

    void quarantineLocked(const std::string& path) {
        const std::string bad = path + ".bad";
        std::remove(bad.c_str());
        if (std::rename(path.c_str(), bad.c_str()) != 0) {
            std::ofstream out(path, std::ios::trunc);
            if (out)
                out << kRememberMagic << "\n";
        }
        rows_.clear();
        dirty_ = false;
    }

    mutable std::mutex mu_;
    std::string path_;
    std::vector<RememberedChoice> rows_;
    bool dirty_ = false;
};

inline RememberTrackStore& processRememberTracks() {
    static RememberTrackStore store;
    return store;
}

inline RememberTrackLoad loadProcessRememberTracks(const std::string& path) {
    return processRememberTracks().load(path);
}

} // namespace misterplex
