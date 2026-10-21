#pragma once
// TASKS-NIGHT-1005 #22 T-SKIPNEXT-RKFALLBACK: the play request for a play-queue step (skipNext, skipPrevious,
// auto-next), built from the PMS queue item at the destination index.
// A ratingKey is not a playQueueItemID (f176804c; main.cpp "inventing one makes the controller index a nonexistent
// queue row"). An item without playQueueItemID gives a request without one, and the controller matches the item by
// key instead.
#include "companion.hpp"
#include "plex_keys.hpp"
#include "plex_resolve.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace misterplex {

inline int selectQueueIndex(const std::vector<QueueItem>& items, const std::string& currentKey,
                            const std::string& playQueueItemId, const char** by = nullptr) {
    if (by)
        *by = "none";
    if (!playQueueItemId.empty()) {
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            if (item.playQueueItemId == playQueueItemId
#ifdef MUT_PQ_ID_RK
                || item.ratingKey == playQueueItemId
#endif
#ifdef MUT_PQ_ID_SUB
                || item.key.find(playQueueItemId) != std::string::npos
#endif
            ) {
                if (by)
                    *by = "id";
                return static_cast<int>(i);
            }
        }
    }
    if (!currentKey.empty()) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].key == currentKey) {
                if (by)
                    *by = "key";
                return static_cast<int>(i);
            }
        }
    }
    const std::string rk = ratingKeyFromKey(currentKey);
    if (!rk.empty()) {
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            if (!item.ratingKey.empty() &&
                (item.ratingKey == rk
#ifdef MUT_PQ_KEY_SUB
                 || currentKey.find(item.ratingKey) != std::string::npos
#endif
                 )) {
                if (by)
                    *by = "rk";
                return static_cast<int>(i);
            }
        }
    }
#ifdef MUT_PQ_IDX0
    return 0;
#else
    return -1;
#endif
}

inline bool firstResolveBinds(const QueueItem& item, const std::string& reqKey,
                              const std::string& resolvedRatingKey) {
    if (item.key == reqKey)
        return true;
    if (item.ratingKey.empty())
        return false;
    if (!resolvedRatingKey.empty() && item.ratingKey == resolvedRatingKey)
        return true;
    const std::string rk = ratingKeyFromKey(reqKey);
    return (!rk.empty() && item.ratingKey == rk)
#ifdef MUT_PQ_BIND_SUB
           || reqKey.find(item.ratingKey) != std::string::npos
#endif
        ;
}

inline std::optional<int> queueStepDest(int currentIndex, int delta, size_t size) {
#ifndef MUT_PQ_STEP_NEG
    if (currentIndex < 0)
        return std::nullopt;
#endif
    const int dest = currentIndex + delta;
    if (dest < 0 || dest >= static_cast<int>(size))
        return std::nullopt;
    return dest;
}

inline PlayRequest queueStepRequest(const PlayRequest& cur, const PlayQueue& q, const QueueItem& item,
                                    const std::string& token, uint64_t generation) {
    PlayRequest n = cur;
    n.key = item.key;
    n.ratingKey = item.ratingKey;
#ifndef MUT_SKIPNEXT_RK_FALLBACK   // test-only mutant: the pre-#22 ratingKey fallback
    n.playQueueItemId = item.playQueueItemId;
#else
    n.playQueueItemId = !item.playQueueItemId.empty() ? item.playQueueItemId : item.ratingKey;
#endif
    n.playQueueId = !q.playQueueId.empty() ? q.playQueueId : cur.playQueueId;
    n.playQueueVersion = !q.playQueueVersion.empty() ? q.playQueueVersion : cur.playQueueVersion;
    n.containerKey = !q.containerKey.empty() ? q.containerKey + "?own=1" : cur.containerKey;
    n.offsetMs = 0;
    n.offsetPresent = true; // do not apply continue-watching on queue step
    n.token = token;
    n.dispatchGeneration = generation + 1;
    // SPS-gate L3: a different item starts clean (no inherited ladder force or used fallback).
    n.forceLadder = false;
    n.fallbackUsed = false;
    n.gateDetail.clear();
    return n;
}


// BUG2-PREROLL (owner 10-17): a Plex Web play queue starts with the PMS's pre-roll (cinema trailer / THX
// clip, type="clip") before the title the user picked. AUTO_NEXT gates next-episode advance only: when a
// pre-roll ends naturally, always step to the next queue item, else the cast stops before the movie starts.
inline bool queueItemIsPreroll(const QueueItem& item) {
#ifdef MUT_PREROLL_ANY_TYPE
    return true;
#else
    return item.type == "clip";
#endif
}

inline bool naturalEofAdvanceAllowed(bool autoNextConf, const PlayQueue& q) {
    if (autoNextConf)
        return true;
#ifdef MUT_PREROLL_IGNORED
    return false;
#else
    return q.currentIndex >= 0 && static_cast<size_t>(q.currentIndex) + 1 < q.items.size() &&
           queueItemIsPreroll(q.items[static_cast<size_t>(q.currentIndex)]);
#endif
}

} // namespace misterplex
