#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace misterplex {
namespace plexui {

struct PlexConnection {
    std::string protocol;
    std::string address;
    std::string port;
    bool local = false;
    bool relay = false;
    std::string uri;
};

struct PlexServer {
    std::string name;
    std::string sourceTitle;
    std::string clientIdentifier;
    std::string machineIdentifier;
    std::string accessToken;
    bool owned = false;
    std::vector<PlexConnection> connections;
    // Chosen base URL (no trailing slash), e.g. http://127.0.0.1:32400
    std::string baseUrl;
    // OB port 0011: no address answered (all probes failed): listed greyed "Offline", never picked.
    bool offline = false;
    // #328: non-empty = accessToken is the conf token and may go ONLY to a base whose
    // /identity machineIdentifier equals this id (learned tokenless, never from an address).
    std::string confTokenId;
};

// Fixture rows used by unit tests. A machine hostname is not a reason to skip a server.
inline bool isFixturePlexServerName(const std::string& name) {
    std::string n(name.size(), '\0');
    for (size_t i = 0; i < name.size(); ++i)
        n[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
#ifdef MUT_NODE_WORKER_FILTER
    if (n.find("build-host") != std::string::npos)
        return true;
#endif
    return n.find("native test") != std::string::npos || n.find("misterplex native") != std::string::npos;
}

// Sidebar already paints the MiSTerPlex logo. Drop a leading "MiSTerPlex "
// so "Native build-host" survives ellipsize instead of "MiSTerPlex N…".
inline std::string stripMisterPlexPrefix(const std::string& title) {
    if (title.size() < 10)
        return title;
    std::string n(title.size(), '\0');
    for (size_t i = 0; i < title.size(); ++i)
        n[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(title[i])));
    if (n.compare(0, 10, "misterplex") != 0)
        return title;
    size_t i = 10;
    while (i < n.size() && (n[i] == ' ' || n[i] == '-' || n[i] == '_' || n[i] == ':'))
        ++i;
    if (i >= title.size())
        return title;
    return title.substr(i);
}

// One audio stream of the item's first Part (PMS Stream streamType=2).
struct PlexAudioTrack {
    int id = 0;
    std::string codec;
    std::string language;
    std::string displayTitle;
    bool selected = false;
    bool playable = true; // false: greyed out in the picker, cannot be chosen
};

struct PlexSubtitleTrack {
    int id = 0;
    std::string codec;
    std::string language;
    std::string displayTitle;
    bool selected = false;
    bool forced = false;
    bool hearingImpaired = false;
    bool image = false;
    bool softText = false;
};

struct PlexItem {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string thumb;
    std::string art;
    std::string theme; // /library/metadata/<id>/theme... empty when the document has none
    std::string updatedAt;
    std::string summary;
    std::string year;
    std::string contentRating;
    std::string tagline;
    std::string genres;
    std::string type;
    int runtimeMs = 0;
    float rating = 0.f;
    float audienceRating = 0.f;
    int64_t viewOffsetMs = 0;
    // PMS display aspect ("1.33", "1.78", "4:3"). Empty when the document has none.
    // FILTER_4_3 reads this. The grid still lists the item when that flag is off.
    std::string aspectRatio;
    // TV children. Zero / empty on movies and hubs, so a grid of films stays the same size.
    int index = 0;
    int parentIndex = 0;
    int leafCount = 0;
    int viewCount = 0;
    std::string parentTitle;
    std::string grandparentTitle;
    std::string grandparentRatingKey;
    std::vector<PlexSubtitleTrack> subtitles;
    std::vector<PlexAudioTrack> audios;
};

struct AudioPickerRow {
    int streamId = 0; // 0 = default (daemon / PMS choice)
    std::string label;
    bool selectable = true;
};

struct SubtitlePickerRow {
    int streamId = 0; // 0 = Off
    std::string label;
    bool selectable = true;
    bool image = false;
};

// Movies and episodes with a catalog can open the picker. Shows/seasons stay Seasons-only.
inline bool itemShowsSubtitleButton(const PlexItem& it) {
    return it.type != "show" && it.type != "season" && !it.subtitles.empty();
}

// An audio choice only exists with two or more audio streams.
inline bool itemShowsAudioButton(const PlexItem& it) {
    return it.type != "show" && it.type != "season" && it.audios.size() >= 2;
}

// Play next / Add to queue on movie and episode Detail (#56). Shows stay Seasons-only.
inline bool itemShowsQueueButtons(const PlexItem& it) {
    return it.type == "movie" || it.type == "episode";
}

inline int detailButtonCount(const PlexItem& it) {
    if (it.type == "show")
        return 1;
    int n = 1;
    if (it.viewOffsetMs > 0)
        ++n;
    if (itemShowsSubtitleButton(it))
        ++n;
    if (itemShowsAudioButton(it))
        ++n;
    if (itemShowsQueueButtons(it))
        n += 2;
    return n;
}

inline int playFromStartButtonIndex(const PlexItem& it) {
    return it.viewOffsetMs > 0 ? 1 : -1;
}

inline int subtitleButtonIndex(const PlexItem& it) {
    if (!itemShowsSubtitleButton(it))
        return -1;
    return it.viewOffsetMs > 0 ? 2 : 1;
}

// Audio stays after Subtitles so the subtitle index never moves. Queue buttons follow.
inline int audioButtonIndex(const PlexItem& it) {
    if (!itemShowsAudioButton(it))
        return -1;
    int i = it.viewOffsetMs > 0 ? 2 : 1;
    if (itemShowsSubtitleButton(it))
        ++i;
    return i;
}

inline int playNextButtonIndex(const PlexItem& it) {
    if (!itemShowsQueueButtons(it))
        return -1;
    return detailButtonCount(it) - 2;
}

inline int addToQueueButtonIndex(const PlexItem& it) {
    if (!itemShowsQueueButtons(it))
        return -1;
    return detailButtonCount(it) - 1;
}

inline std::string itemShowKey(const PlexItem& it) {
    if ((it.type == "episode" || !it.grandparentRatingKey.empty()) && !it.grandparentRatingKey.empty())
        return it.grandparentRatingKey;
    return it.ratingKey;
}

struct PlexHubRow {
    std::string title;
    std::string hubKey;
    std::vector<PlexItem> items;
};

struct PlexSection {
    std::string key;
    std::string title;
    std::string type;
};

enum class SidebarRowKind : uint8_t { Library, Server, SignIn, HomeUser, ServerPicker };

struct SidebarLibraryEntry {
    std::string title;
    std::string subtitle;
    std::string type;
    std::string sectionKey;
    int serverIndex = 0;
    SidebarRowKind kind = SidebarRowKind::Library;
};

struct GridCategoryChip {
    enum class Kind : uint8_t { All, Collection, Genre, Aspect43 };
    Kind kind = Kind::All;
    std::string title;
    std::string filterId;
};

enum class ViewId : uint8_t {
    ServerList,
    Home,
    LibraryGrid,
    Detail,
    SeasonList,
    EpisodeList,
    UpNext,
    LinkSignIn,
    SubtitlePicker,
    AudioPicker,
    HomeUsers,
    HomePin,
};

// UiFocus::sidebar on Home: 0 = content (hub rows; the Home pill stays the aligned
// item), 1+ = sidebar library/server entry (index+1), kSidebarHomeFocus = the
// sidebar itself has focus on Home (LEFT past the first hub tile, or UP from
// the first entry).
constexpr int kSidebarHomeFocus = -1;

struct UiFocus {
    int row = 0;
    int col = 0;
    int sidebar = 0; // Home: 0=content, kSidebarHomeFocus=Home item, 1+=section index+1
    int detailButton = 0; // 0=Play/Resume, then Play from start / Subs / Audio / queue
    bool gridOnChips = false;
    int gridChip = 0;
    // AZ_JUMP. The bar is a sibling of the chip row. Flag off leaves both at rest.
    bool gridOnLetters = false;
    int gridLetter = 0; // slot: 0 = all, 1..26 = A..Z, 27 = '#'
};

} // namespace plexui
} // namespace misterplex
