#pragma once
// Next episode for the Up Next card. GETs only: a bound play queue, or a read-only
// season walk (parent, then the next season, skipping specials). No playQueues POST,
// no scrobble, no stream prefetch.

#include "companion.hpp"
#include "plex_resolve.hpp"
#include "queue_step.hpp"

#include "libmisterplex/plex_markers.hpp"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace misterplex {

struct NextEpisode {
    std::string key;
    std::string ratingKey;
    std::string playQueueItemId;
    std::string title;
    std::string type;
    int index = -1;
    int parentIndex = -1;
};

inline std::vector<NextEpisode> episodesIn(const std::string& xml) {
    std::vector<NextEpisode> out;
    std::size_t pos = 0;
    while ((pos = xml.find("<Video", pos)) != std::string::npos) {
        const auto gt = xml.find('>', pos);
        if (gt == std::string::npos)
            break;
        const std::string open = xml.substr(pos, gt - pos);
        pos = gt + 1;
        NextEpisode e;
        e.key = markers::xmlUnescape(markers::xmlAttr(open, "key"));
        e.ratingKey = markers::xmlAttr(open, "ratingKey");
        e.playQueueItemId = markers::xmlAttr(open, "playQueueItemID");
        e.title = markers::xmlUnescape(markers::xmlAttr(open, "title"));
        e.type = markers::xmlAttr(open, "type");
        const std::string ix = markers::xmlAttr(open, "index");
        const std::string px = markers::xmlAttr(open, "parentIndex");
        if (!ix.empty())
            e.index = static_cast<int>(std::strtol(ix.c_str(), nullptr, 10));
        if (!px.empty())
            e.parentIndex = static_cast<int>(std::strtol(px.c_str(), nullptr, 10));
        if (e.key.empty() && !e.ratingKey.empty())
            e.key = "/library/metadata/" + e.ratingKey;
        out.push_back(std::move(e));
    }
    std::size_t dpos = 0;
    while ((dpos = xml.find("<Directory", dpos)) != std::string::npos) {
        const auto gt = xml.find('>', dpos);
        if (gt == std::string::npos)
            break;
        const std::string open = xml.substr(dpos, gt - dpos);
        dpos = gt + 1;
        const std::string type = markers::xmlAttr(open, "type");
        if (type != "season" && type != "episode")
            continue;
        NextEpisode e;
        e.key = markers::xmlUnescape(markers::xmlAttr(open, "key"));
        e.ratingKey = markers::xmlAttr(open, "ratingKey");
        e.title = markers::xmlUnescape(markers::xmlAttr(open, "title"));
        e.type = type;
        const std::string ix = markers::xmlAttr(open, "index");
        if (!ix.empty())
            e.index = static_cast<int>(std::strtol(ix.c_str(), nullptr, 10));
        if (e.key.empty() && !e.ratingKey.empty())
            e.key = "/library/metadata/" + e.ratingKey;
        out.push_back(std::move(e));
    }
    return out;
}

inline int containerTotal(const std::string& xml) {
    const auto openEnd = xml.find('>');
    const std::string head = openEnd == std::string::npos ? xml : xml.substr(0, openEnd);
    const std::string total = markers::xmlAttr(head, "totalSize");
    const std::string size = total.empty() ? markers::xmlAttr(head, "size") : total;
    if (size.empty())
        return -1;
    return static_cast<int>(std::strtol(size.c_str(), nullptr, 10));
}

// #340: no token parameter; the caller sends it as a header (plexMetadataHttpGet(url, token)).
inline std::string pagedGetUrl(const std::string& base, const std::string& key, int start) {
    std::string path = key;
    if (!path.empty() && path[0] != '/')
        path = "/" + path;
    if (path.size() < 9 || path.compare(path.size() - 9, 9, "/children") != 0)
        path += "/children";
    std::string url = base + path + "?X-Plex-Container-Start=" + std::to_string(start) +
                      "&X-Plex-Container-Size=50";
    return url;
}

inline markers::NextItem toNext(const NextEpisode& e) {
    markers::NextItem n;
    n.key = e.key;
    n.ratingKey = e.ratingKey;
    n.playQueueItemId = e.playQueueItemId;
    n.title = e.title;
    n.season = e.parentIndex;
    n.episode = e.index;
    return n;
}

inline const NextEpisode* afterCurrent(const std::vector<NextEpisode>& eps, const std::string& ratingKey,
                                       int index) {
    int at = -1;
    for (int i = 0; i < static_cast<int>(eps.size()); ++i) {
        if (!ratingKey.empty() && eps[static_cast<std::size_t>(i)].ratingKey == ratingKey) {
            at = i;
            break;
        }
    }
    if (at < 0 && index >= 0) {
        for (int i = 0; i < static_cast<int>(eps.size()); ++i) {
            if (eps[static_cast<std::size_t>(i)].index == index) {
                at = i;
                break;
            }
        }
    }
    if (at < 0)
        return nullptr;
    const int curIndex = eps[static_cast<std::size_t>(at)].index;
    const NextEpisode* best = nullptr;
    if (curIndex >= 0) {
        for (const NextEpisode& e : eps) {
            if (e.type == "season")
                continue;
            if (e.index > curIndex && (!best || e.index < best->index))
                best = &e;
        }
        if (best)
            return best;
    }
    if (at + 1 < static_cast<int>(eps.size())) {
        const NextEpisode& n = eps[static_cast<std::size_t>(at + 1)];
        if (n.type != "season")
            return &n;
    }
    return nullptr;
}

inline std::optional<markers::NextItem> resolveNextItem(const PlayRequest& cur, const ResolveResult& r,
                                                       const std::string& plexBase, const std::string& token) {
    if (r.type == "clip")
        return std::nullopt;
    const std::string base = normalizePlexBase(plexBase);
    if (base.empty())
        return std::nullopt;

    std::string qref = cur.containerKey;
    if (qref.empty() && !cur.playQueueId.empty())
        qref = "/playQueues/" + cur.playQueueId;
    if (!qref.empty() && qref.find("/playQueues/") != std::string::npos) {
        std::string raw = urlDecode(qref);
        auto qmark = raw.find('?');
        if (qmark != std::string::npos)
            raw = raw.substr(0, qmark);
        std::string id = raw;
        const auto slash = id.find_last_of('/');
        if (slash != std::string::npos)
            id = id.substr(slash + 1);
        while (!id.empty() && !std::isdigit(static_cast<unsigned char>(id.back())))
            id.pop_back();
        if (id.empty())
            return std::nullopt;
        std::string url = base + "/playQueues/" + id + "?own=1&X-Plex-Container-Size=50";
#ifdef MUT_NEXT_ITEM_WRITES
        (void)plexMetadataHttpGet(base + "/:/scrobble?identifier=com.plexapp.plugins.library");
#endif
        const std::string xml = plexMetadataHttpGet(url, token);
        const PlayQueue q = parsePlayQueueXml(xml, id, cur.key, cur.playQueueItemId);
        if (!q.ok || q.currentIndex < 0)
            return std::nullopt;
        const auto& curItem = q.items[static_cast<std::size_t>(q.currentIndex)];
        if (queueItemIsPreroll(curItem))
            return std::nullopt;
        const auto dest = queueStepDest(q.currentIndex, +1, q.items.size());
        if (!dest)
            return std::nullopt;
        const auto& item = q.items[static_cast<std::size_t>(*dest)];
        markers::NextItem n;
        n.key = item.key;
        n.ratingKey = item.ratingKey;
        n.playQueueItemId = item.playQueueItemId;
        n.title = item.title;
        const std::vector<NextEpisode> eps = episodesIn(xml);
        for (const NextEpisode& e : eps) {
            if ((!item.ratingKey.empty() && e.ratingKey == item.ratingKey) ||
                (!item.playQueueItemId.empty() && e.playQueueItemId == item.playQueueItemId)) {
                n.season = e.parentIndex;
                n.episode = e.index;
                if (n.title.empty())
                    n.title = e.title;
                break;
            }
        }
        return n;
    }

    if (r.type != "episode" && r.parentKey.empty())
        return std::nullopt;
    if (r.parentKey.empty())
        return std::nullopt;

    std::vector<NextEpisode> season;
    for (int start = 0, page = 0; page < 4; ++page, start += 50) {
        const std::string xml = plexMetadataHttpGet(pagedGetUrl(base, r.parentKey, start), token);
        if (xml.find("<MediaContainer") == std::string::npos)
            break;
        const std::vector<NextEpisode> pageEps = episodesIn(xml);
        if (const NextEpisode* n = afterCurrent(pageEps, r.ratingKey.empty() ? cur.ratingKey : r.ratingKey, r.index))
            return toNext(*n);
        season.insert(season.end(), pageEps.begin(), pageEps.end());
        const int total = containerTotal(xml);
        if (pageEps.size() < 50 || (total >= 0 && start + static_cast<int>(pageEps.size()) >= total))
            break;
    }
    if (afterCurrent(season, r.ratingKey.empty() ? cur.ratingKey : r.ratingKey, r.index))
        return toNext(*afterCurrent(season, r.ratingKey.empty() ? cur.ratingKey : r.ratingKey, r.index));

    if (r.grandparentKey.empty())
        return std::nullopt;
    const std::string shows = plexMetadataHttpGet(pagedGetUrl(base, r.grandparentKey, 0), token);
    const std::vector<NextEpisode> seasons = episodesIn(shows);
    const NextEpisode* nextSeason = nullptr;
    for (const NextEpisode& s : seasons) {
        if (s.type != "season")
            continue;
        if (s.index == 0)
            continue;
        if (r.parentIndex >= 0 && s.index <= r.parentIndex)
            continue;
        if (!nextSeason || s.index < nextSeason->index)
            nextSeason = &s;
    }
    if (!nextSeason || nextSeason->key.empty())
        return std::nullopt;
    const std::string kids = plexMetadataHttpGet(pagedGetUrl(base, nextSeason->key, 0), token);
    const std::vector<NextEpisode> eps = episodesIn(kids);
    const NextEpisode* first = nullptr;
    for (const NextEpisode& e : eps) {
        if (e.type == "season")
            continue;
        if (!first || (e.index >= 0 && (first->index < 0 || e.index < first->index)))
            first = &e;
    }
    if (!first)
        return std::nullopt;
    markers::NextItem n = toNext(*first);
    if (n.season < 0)
        n.season = nextSeason->index;
    return n;
}

} // namespace misterplex
