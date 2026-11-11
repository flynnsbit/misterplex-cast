#pragma once
// Browse audio picker rows. A greyed-out row is listed but cannot be chosen.
//
// Every real audio codec is playable: the daemon asks PMS universal for the picked
// audioStreamID and PMS transcodes it (the daemon itself decodes only AAC / MP3 / PCM).
// A track with no stream id cannot be addressed, so it is the one case that is greyed.

#include "libmisterplex/track_select.hpp"
#include "plexui_types.hpp"

#include <string>
#include <vector>

namespace misterplex {
namespace plexui {

// True when the daemon can ask PMS for this stream (a usable id).
inline bool audioTrackSelectable(const PlexAudioTrack& t) {
    return t.id > 0 && t.playable;
}

inline std::string audioPickerLabel(const PlexAudioTrack& t) {
    std::string label = t.displayTitle;
    if (label.empty()) {
        label = t.language.empty() ? "Unknown" : t.language;
        if (!t.codec.empty())
            label += " " + t.codec;
    }
    if (!audioTrackSelectable(t))
        label += " (unsupported — later)";
    return label;
}

inline std::vector<AudioPickerRow> audioPickerRows(const std::vector<PlexAudioTrack>& tracks) {
    std::vector<AudioPickerRow> rows;
    for (const PlexAudioTrack& t : tracks) {
        AudioPickerRow r;
        r.streamId = t.id;
        r.selectable = audioTrackSelectable(t);
        r.label = audioPickerLabel(t);
        rows.push_back(std::move(r));
    }
    return rows;
}

inline std::vector<AudioPickerRow> audioPickerRows(const PlexItem& item) {
    return audioPickerRows(item.audios);
}

inline PlexAudioTrack plexAudioFromStream(const MediaStream& s) {
    PlexAudioTrack t;
    t.id = s.id;
    t.codec = s.codec;
    t.language = s.language.empty() ? s.languageRaw : s.language;
    t.displayTitle = subtitlePickerLabel(s);
    t.selected = s.selected;
    t.playable = s.id > 0;
    return t;
}

// Audio streams of the first Part (the one the daemon plays), in catalog order.
inline std::vector<PlexAudioTrack> plexAudiosFromCatalog(const StreamCatalog& cat, int partId = 0) {
    std::vector<PlexAudioTrack> out;
    for (const MediaStream& s : cat.streams) {
        if (s.kind == StreamKind::Audio && track_detail::streamInPart(s, partId))
            out.push_back(plexAudioFromStream(s));
    }
    return out;
}

inline const PlexAudioTrack* findAudioTrack(const PlexItem& item, int id) {
    for (const PlexAudioTrack& t : item.audios) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

// Row index for a stream id; falls back to the first selectable row, else 0.
inline int audioRowForStreamId(const std::vector<AudioPickerRow>& rows, int streamId) {
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (rows[static_cast<size_t>(i)].streamId == streamId && rows[static_cast<size_t>(i)].selectable)
            return i;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (rows[static_cast<size_t>(i)].selectable)
            return i;
    return 0;
}

// Opening row: the user's earlier pick, else the PMS-selected track.
inline int audioInitialRow(const PlexItem& item, int pickedId) {
    const auto rows = audioPickerRows(item);
    if (pickedId > 0)
        return audioRowForStreamId(rows, pickedId);
    for (const PlexAudioTrack& t : item.audios)
        if (t.selected && audioTrackSelectable(t))
            return audioRowForStreamId(rows, t.id);
    return audioRowForStreamId(rows, 0);
}

} // namespace plexui
} // namespace misterplex
