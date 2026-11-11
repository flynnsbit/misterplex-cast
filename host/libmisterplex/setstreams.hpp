#pragma once
// FEATURE-GAP #1+#4 / Tier1 #1: Program-menu subtitle and audio picks.
//
// SETSTREAMS (misterplex.conf) / MPX_SETSTREAMS (env wins). Default off.
// Off: the Program menu does not grow Subtitles or Audio rows, so it matches
// the parent tip. Companion /player/playback/setStreams and the PMS universal
// URL (audioStreamID, subtitles=burn, subtitleStreamID) already live on that
// tip and stay ungated — flag off does not change them.
// On: during a play the menu lists this part's streams. A pick is the same
// setStreams request Plex Web sends. The player re-resolves at the current
// offset. subtitleStreamID 0 is off. Image codecs (PGS, VobSub, and the same
// family) go out as subtitles=burn. This header does not paint soft text.

#include "libmisterplex/track_select.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex {

inline bool setStreamsEnvOn(const char* e) {
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

// Default off. MPX_SETSTREAMS wins over SETSTREAMS when it is set.
inline bool setStreamsEnabled() {
#ifdef MUT_SETSTREAMS_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_SETSTREAMS"); e && e[0])
        return setStreamsEnvOn(e);
    return setStreamsEnvOn(std::getenv("SETSTREAMS"));
#endif
}

struct ProgramStreamRow {
    int id = 0;
    std::string label;
    bool current = false;
};

inline std::string programStreamLabel(const MediaStream& s, bool current) {
    std::string label = subtitlePickerLabel(s);
    if (current)
        label += "  *";
    return label;
}

// Off, then every subtitle on this part, including image/PGS. Empty when the
// part has no subtitle stream (no Off-only row).
inline std::vector<ProgramStreamRow> programSubtitleRows(const StreamCatalog& cat, int partId, int currentId) {
    std::vector<ProgramStreamRow> tracks;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || s.id <= 0 || !track_detail::streamInPart(s, partId))
            continue;
#ifdef MUT_SETSTREAMS_PGS_HIDDEN
        if (subtitleCodecIsImage(s.codec))
            continue;
#endif
        ProgramStreamRow r;
        r.id = s.id;
        r.current = currentId > 0 && s.id == currentId;
        r.label = programStreamLabel(s, r.current);
        tracks.push_back(std::move(r));
    }
    if (tracks.empty())
        return tracks;
    ProgramStreamRow off;
    off.id = 0;
    off.current = currentId <= 0;
    off.label = off.current ? "Off  *" : "Off";
    tracks.insert(tracks.begin(), std::move(off));
    return tracks;
}

// Audio streams on this part. There is no Off row: audioStreamID 0 is not a stream.
inline std::vector<ProgramStreamRow> programAudioRows(const StreamCatalog& cat, int partId, int currentId) {
    std::vector<ProgramStreamRow> tracks;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Audio || s.id <= 0 || !track_detail::streamInPart(s, partId))
            continue;
        ProgramStreamRow r;
        r.id = s.id;
        r.current = currentId > 0 && s.id == currentId;
        r.label = programStreamLabel(s, r.current);
        tracks.push_back(std::move(r));
    }
    return tracks;
}

} // namespace misterplex
