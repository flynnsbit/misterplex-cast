#pragma once

// TV seasons, episodes, and the play queue the on-device player steps.
// Pure helpers: no heap beyond the vectors the caller owns, no tokens in logs.

#include "libmisterplex/browse_v2.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "plexui_types.hpp"

namespace misterplex {
namespace plexui {

inline std::string episodeCode(int season, int episode) {
    if (season <= 0 && episode <= 0)
        return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "S%02dE%02d", std::max(0, season), std::max(0, episode));
    return buf;
}

// On Deck / Continue Watching: in-progress only. Finished titles drop viewOffset
// (or sit at duration); those must not occupy a home row tile.
inline bool continueWatchingEligible(int64_t viewOffsetMs, int runtimeMs) {
#ifdef MUT_CW_KEEP_FINISHED
    (void)viewOffsetMs;
    (void)runtimeMs;
    return true;
#else
    if (viewOffsetMs <= 0)
        return false;
    if (runtimeMs > 0 && viewOffsetMs >= runtimeMs)
        return false;
    return true;
#endif
}

inline bool continueWatchingEligible(const PlexItem& it) {
    return continueWatchingEligible(it.viewOffsetMs, it.runtimeMs);
}

inline bool continueWatchingTitle(const std::string& title) {
    std::string low = title;
    for (char& c : low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return low.find("continue") != std::string::npos;
}

// Home tile caption: show name + S01E02 for episodes, else the PMS title.
inline std::string hubItemLabel(const PlexItem& it) {
    const std::string code = episodeCode(it.parentIndex, it.index);
    if (code.empty())
        return it.title;
    const std::string show = !it.grandparentTitle.empty() ? it.grandparentTitle : it.title;
    if (show.empty() || show == it.title)
        return code;
    return show + "  " + code;
}

inline std::string formatRunTime(int ms) {
    if (ms <= 0)
        return {};
    const int totalSec = ms / 1000;
    if (totalSec < 60)
        return std::to_string(std::max(1, totalSec)) + "s";
    const int minutes = totalSec / 60;
    const int hours = minutes / 60;
    if (hours > 0)
        return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
    return std::to_string(minutes) + "m";
}

enum class WatchMark : uint8_t { None, Progress, Watched };

inline WatchMark watchMark(int viewCount, int64_t viewOffsetMs) {
    if (viewOffsetMs > 0)
        return WatchMark::Progress;
    if (viewCount > 0)
        return WatchMark::Watched;
    return WatchMark::None;
}

// Where Back lands. Hidden is cast-ready (the UI is not on screen).
enum class TvPlace : uint8_t {
    Hidden,
    Servers,
    Home,
    Grid,
    Show,
    Season,
    Episode,
    UpNext,
};

// playingHidden: a plexui play is on screen and the UI is not. The first Back
// stays on the episode list and shows it; it does not pop.
inline TvPlace backPlace(TvPlace cur, bool fromGrid, bool playingHidden) {
    if (playingHidden && (cur == TvPlace::Episode || cur == TvPlace::UpNext))
        return TvPlace::Episode;
    switch (cur) {
    case TvPlace::UpNext:
        return TvPlace::Episode;
    case TvPlace::Episode:
        return TvPlace::Season;
    case TvPlace::Season:
        return TvPlace::Show;
    case TvPlace::Show:
        return fromGrid ? TvPlace::Grid : TvPlace::Home;
    case TvPlace::Grid:
        return TvPlace::Home;
    case TvPlace::Home:
        return TvPlace::Servers;
    case TvPlace::Servers:
    case TvPlace::Hidden:
        return TvPlace::Hidden;
    }
    return TvPlace::Hidden;
}

struct PlexQueueItem {
    std::string ratingKey;
    std::string key;
    std::string playQueueItemId;
    std::string title;
    std::string type; // "clip" is a pre-roll; queue_step.hpp still decides natural EOF
    int parentIndex = 0;
    int index = 0;
    int runtimeMs = 0;
};

struct PlexPlayQueue {
    bool ok = false;
    std::string id;
    std::string version;
    std::string selectedItemId;
    std::vector<PlexQueueItem> items;
};

inline std::string playQueueUri(const std::string& machineId, const std::string& ratingKey) {
    return "server://" + machineId + "/com.plexapp.plugins.library/library/metadata/" + ratingKey;
}

// Exact ratingKey only. queueIndexFor falls back to the selected or first row, which is
// the wrong test for "is this title already in the queue".
inline int queueExactIndex(const PlexPlayQueue& q, const std::string& ratingKey) {
    if (ratingKey.empty())
        return -1;
    for (size_t i = 0; i < q.items.size(); ++i) {
        if (q.items[i].ratingKey == ratingKey)
            return static_cast<int>(i);
    }
    return -1;
}

// The requested episode wins. Otherwise the container's selected item, else the first row.
inline int queueIndexFor(const PlexPlayQueue& q, const std::string& ratingKey) {
    if (!ratingKey.empty()) {
        for (size_t i = 0; i < q.items.size(); ++i) {
            if (q.items[i].ratingKey == ratingKey)
                return static_cast<int>(i);
        }
    }
    if (!q.selectedItemId.empty()) {
        for (size_t i = 0; i < q.items.size(); ++i) {
            if (q.items[i].playQueueItemId == q.selectedItemId)
                return static_cast<int>(i);
        }
    }
    return q.items.empty() ? -1 : 0;
}

inline int queueStepIndex(int current, int delta, int size) {
    if (size <= 0 || current < 0)
        return -1;
    const int dest = current + delta;
    if (dest < 0 || dest >= size)
        return -1;
    return dest;
}

inline PlexPlayQueue parsePlayQueueXml(const std::string& xml) {
    PlexPlayQueue q;
    for (const auto& el : parseXmlStartTags(xml)) {
        if (el.name == "MediaContainer") {
            q.id = el.get("playQueueID");
            if (q.id.empty())
                q.id = el.get("playQueueId");
            q.version = el.get("playQueueVersion");
            q.selectedItemId = el.get("playQueueSelectedItemID");
            if (q.selectedItemId.empty())
                q.selectedItemId = el.get("playQueueSelectedItemId");
            continue;
        }
        if (el.name != "Video" && el.name != "Episode" && el.name != "Track")
            continue;
        PlexQueueItem it;
        it.ratingKey = el.get("ratingKey");
        it.key = el.get("key");
        it.playQueueItemId = el.get("playQueueItemID");
        if (it.playQueueItemId.empty())
            it.playQueueItemId = el.get("playQueueItemId");
        it.title = el.get("title");
        it.type = el.get("type");
        it.parentIndex = std::atoi(el.get("parentIndex").c_str());
        it.index = std::atoi(el.get("index").c_str());
        it.runtimeMs = std::atoi(el.get("duration").c_str());
        if (it.key.empty() && !it.ratingKey.empty())
            it.key = "/library/metadata/" + it.ratingKey;
        if (!it.ratingKey.empty() || !it.playQueueItemId.empty())
            q.items.push_back(std::move(it));
    }
    q.ok = !q.id.empty() && !q.items.empty();
    return q;
}

enum class UpNextAction : uint8_t { Wait, Advance, Cancel };

struct UpNextState {
    bool showing = false;
    int secondsLeft = 0;
    bool cancelFocused = false;
};

inline void armUpNextState(UpNextState& s, int seconds) {
    s.showing = true;
    s.secondsLeft = seconds > 0 ? seconds : 1;
    s.cancelFocused = false;
}

inline UpNextAction tickUpNext(UpNextState& s) {
    if (!s.showing)
        return UpNextAction::Wait;
    if (s.secondsLeft > 0)
        --s.secondsLeft;
    if (s.secondsLeft <= 0) {
        s.showing = false;
        return UpNextAction::Advance;
    }
    return UpNextAction::Wait;
}

// vertical < 0 moves to Play now, vertical > 0 moves to Cancel.
inline UpNextAction upNextInput(UpNextState& s, bool select, bool back, int vertical) {
    if (!s.showing)
        return UpNextAction::Wait;
    if (back) {
        s.showing = false;
        return UpNextAction::Cancel;
    }
    if (vertical > 0)
        s.cancelFocused = true;
    else if (vertical < 0)
        s.cancelFocused = false;
    if (!select)
        return UpNextAction::Wait;
    s.showing = false;
    return s.cancelFocused ? UpNextAction::Cancel : UpNextAction::Advance;
}

} // namespace plexui
} // namespace misterplex
