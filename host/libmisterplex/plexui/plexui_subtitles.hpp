#pragma once
// Browse subtitle picker rows. Image / embedded tracks are listed but not applied.

#include "libmisterplex/track_select.hpp"
#include "plexui_types.hpp"

#include <string>
#include <vector>

namespace misterplex {
namespace plexui {

inline std::vector<SubtitlePickerRow> subtitlePickerRows(const std::vector<PlexSubtitleTrack>& tracks) {
    std::vector<SubtitlePickerRow> rows;
    SubtitlePickerRow off;
    off.streamId = 0;
    off.label = "Off";
    off.selectable = true;
    rows.push_back(off);
    for (const PlexSubtitleTrack& t : tracks) {
        SubtitlePickerRow r;
        r.streamId = t.id;
        r.image = t.image;
        r.selectable = t.softText;
        r.label = t.displayTitle;
        if (r.label.empty()) {
            r.label = t.language.empty() ? "Unknown" : t.language;
            if (!t.codec.empty())
                r.label += " " + t.codec;
            if (t.forced)
                r.label += " forced";
            if (t.hearingImpaired)
                r.label += " SDH";
        }
        if (!t.softText)
            r.label += t.image ? " (image — later)" : " (embedded — later)";
        rows.push_back(std::move(r));
    }
    return rows;
}

inline std::vector<SubtitlePickerRow> subtitlePickerRows(const PlexItem& item) {
    return subtitlePickerRows(item.subtitles);
}

inline PlexSubtitleTrack plexSubtitleFromStream(const MediaStream& s) {
    PlexSubtitleTrack t;
    t.id = s.id;
    t.codec = s.codec;
    t.language = s.language.empty() ? s.languageRaw : s.language;
    t.displayTitle = subtitlePickerLabel(s);
    t.selected = s.selected;
    t.forced = s.forced;
    t.hearingImpaired = s.hearingImpaired;
    t.image = subtitleCodecIsImage(s.codec);
    t.softText = subtitleIsSoftNow(s);
    return t;
}

inline std::vector<PlexSubtitleTrack> plexSubtitlesFromCatalog(const StreamCatalog& cat) {
    std::vector<PlexSubtitleTrack> out;
    for (const MediaStream& s : cat.streams) {
        if (s.kind == StreamKind::Subtitle)
            out.push_back(plexSubtitleFromStream(s));
    }
    return out;
}

inline ShowPreference preferenceFromSubtitleRow(const PlexItem& item, const SubtitlePickerRow& row,
                                                const PlexSubtitleTrack* track) {
    ShowPreference p;
    p.showKey = itemShowKey(item);
    if (row.streamId <= 0 || !row.selectable || !track) {
        p.subtitlesOff = true;
        return p;
    }
    p.subtitlesOff = false;
    p.subtitleLanguage = track->language;
    p.subtitleCodec = track->codec;
    p.subtitleForced = track->forced;
    return p;
}

inline const PlexSubtitleTrack* findSubtitleTrack(const PlexItem& item, int id) {
    for (const PlexSubtitleTrack& t : item.subtitles) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

inline int pickerRowForStreamId(const std::vector<SubtitlePickerRow>& rows, int streamId) {
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[static_cast<size_t>(i)].streamId == streamId)
            return i;
    }
    return 0;
}

} // namespace plexui
} // namespace misterplex
