#pragma once
// #258 fallback: PMS Stream@index is not the demuxer's stream index when the
// opened file is a remux or a transcode with a different layout. Match the
// chosen embedded SRT/ASS to a text stream in that file. A universal URL that
// dropped the track (subtitleStreamID=0) is asked to keep it. Burn stays off
// unless the caller already opted in.

#include "track_select.hpp"

#include <string>
#include <vector>

namespace misterplex {

struct OpenedTextStream {
    int index = -1;
    std::string codec;
    std::string language;
    std::string title;
    bool forced = false;
};

struct PlexTextIdentity {
    int plexIndex = -1;
    std::string codec;
    std::string language;
    std::string title;
    bool forced = false;
    int textOrdinal = -1; // nth embedded text stream in the Plex catalog, or -1
};

inline std::string textCodecFamily(const std::string& codec) {
    const std::string c = trackLower(codec);
    if (c == "srt" || c == "subrip" || c == "text" || c == "mov_text")
        return "text";
    if (c == "ass" || c == "ssa")
        return "ass";
    if (c == "webvtt" || c == "vtt")
        return "vtt";
    return c;
}

// Codec family must match when the pick names one. A language mismatch
// excludes the track when both sides have a language. MUT_SOFT_SUB_NO_LANG_GATE
// keeps the codec gate and ignores language, so an index-shaped or codec-only
// choice can pick French for English.
inline bool openedTextEligible(const PlexTextIdentity& pick, const OpenedTextStream& s) {
    if (!pick.codec.empty() && textCodecFamily(pick.codec) != textCodecFamily(s.codec))
        return false;
#ifdef MUT_SOFT_SUB_NO_LANG_GATE
    (void)pick;
    return true;
#else
    if (!pick.language.empty() && !s.language.empty() && !languagesMatch(pick.language, s.language))
        return false;
    return true;
#endif
}

// Container index to arm, or -1 when the opened file has no matching text track.
// The Plex index is not a demuxer index. Title, forced, and ordinal only
// break ties inside the same codec and language.
// MUT_SOFT_SUB_RAW_INDEX uses MediaStream.index and misses a shifted layout.
inline int mapEmbeddedTextIndex(const PlexTextIdentity& pick, const std::vector<OpenedTextStream>& opened) {
#ifdef MUT_SOFT_SUB_RAW_INDEX
    (void)opened;
    return pick.plexIndex;
#else
    if (pick.plexIndex < 0 && pick.codec.empty() && pick.language.empty() && pick.textOrdinal < 0)
        return -1;
    int best = -1;
    int bestScore = -1;
    int ordinal = 0;
    for (const OpenedTextStream& s : opened) {
        if (!openedTextEligible(pick, s)) {
            ++ordinal;
            continue;
        }
        int score = 0;
        if (!pick.title.empty() && trackLower(pick.title) == trackLower(s.title))
            score += 2;
        if (pick.forced == s.forced)
            score += 1;
        if (pick.textOrdinal >= 0 && pick.textOrdinal == ordinal)
            score += 4;
        if (score > bestScore) {
            bestScore = score;
            best = s.index;
        }
        ++ordinal;
    }
    return best;
#endif
}

// Universal transcode that stripped the subtitle (subtitleStreamID=0) is asked
// to keep that stream. burn is the existing SUBTITLE_MODE=burn opt-in, default off.
inline std::string urlKeepingEmbeddedText(const std::string& url, int subtitleStreamId, bool burn) {
    if (subtitleStreamId <= 0 || !trackUrlIsUniversal(url))
        return url;
    std::string u = trackWithQuery(url, "subtitleStreamID", std::to_string(subtitleStreamId));
    return trackWithQuery(u, "subtitles", burn ? "burn" : "embedded");
}

// nth soft-text stream in the catalog, or -1 when this stream is not one of them.
inline int embeddedTextOrdinal(const StreamCatalog& cat, const MediaStream& stream) {
    int n = 0;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || !subtitleCodecIsSoftText(s.codec))
            continue;
        if (s.id == stream.id && s.index == stream.index)
            return n;
        ++n;
    }
    return -1;
}

inline PlexTextIdentity plexTextIdentityFrom(const MediaStream& stream, const StreamCatalog& cat) {
    PlexTextIdentity id;
    id.plexIndex = stream.index;
    id.codec = stream.codec;
    id.language = stream.language.empty() ? stream.languageRaw : stream.language;
    id.title = stream.title.empty() ? stream.displayTitle : stream.title;
    id.forced = stream.forced;
    id.textOrdinal = embeddedTextOrdinal(cat, stream);
    return id;
}

} // namespace misterplex
