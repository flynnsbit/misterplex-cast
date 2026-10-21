#pragma once

#include "companion.hpp"
#include "pms_timeline.hpp"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace misterplex {

struct TransportGenerations {
    std::atomic<uint64_t>& play;
    std::atomic<uint64_t>& seek;

    TransportRequest accept(TransportCommand command, uint64_t epoch) {
        TransportRequest request;
        if (command == TransportCommand::Seek || command == TransportCommand::Previous ||
            command == TransportCommand::Stop)
            request.seekGeneration = ++seek;
        else
            request.seekGeneration = seek.load();
        request.generation = command == TransportCommand::Seek || command == TransportCommand::Stop
            ? ++play : play.load();
        request.epoch = epoch;
        return request;
    }

    bool current(const TransportRequest& request) const {
        return request.generation == play.load() && request.seekGeneration == seek.load();
    }
};

// R1 C1 (review 08:17 (1)): a STOP ends the PLAY requests before its OWN generation, never
// before the live one: a PLAY and a seek after the STOP keep their PLXH en=1 hold.
inline uint64_t stopClearGeneration(const TransportRequest& request, const TransportGenerations& g) {
#ifdef MUT_R1_STOP_LIVE_GEN   // test-only mutant: the live playGen
    (void)request;
    return g.play.load();
#else
    (void)g;
    return request.generation;
#endif
}

// Caller owns the serialized player/reporter handoff. stop() returns its
// post-join snapshot, not the position/duration it subsequently clears.
template<class Player>
void retirePlayback(Player& player, PmsTimelineReporter& reporter) {
    const auto final = player.stop();
    reporter.endSession(final.timeMs, final.durationMs);
}

// OWENB-P1B T5: a player with an OB play (MediaPlayer::owenbPlayActive) seeks inside the running play (held); a
// player without that member (the tests' fakes) never does.
template<class P>
auto owenbPlayActiveOf(P& p, int) -> decltype(static_cast<bool>(p.owenbPlayActive())) {
    return p.owenbPlayActive();
}
template<class P>
bool owenbPlayActiveOf(P&, long) {
    return false;
}

template<class Player, class Start, class Clear, class Post>
void wirePlaybackControls(Companion& companion, Player& player, PmsTimelineReporter& reporter,
                          TransportGenerations generations, std::atomic<uint64_t>& activePlay,
                          std::mutex& handoff, std::mutex& seekMutex,
                          Start start, Clear clear, Post post) {
    companion.setTransportQueued([generations, &player](TransportCommand command) mutable {
        return generations.accept(command, player.playbackEpoch());
    });
    auto pauseResume = [generations, &player, &handoff](const TransportRequest& request, bool pause) {
        std::lock_guard<std::mutex> lock(handoff);
        if (!generations.current(request) || player.playbackEpoch() != request.epoch ||
            !player.playing())
            return;
        if (pause)
            player.pause();
        else
            player.resume();
    };
    companion.setPause([pauseResume](const TransportRequest& request) {
        pauseResume(request, true);
    });
    companion.setResume([pauseResume](const TransportRequest& request) {
        pauseResume(request, false);
    });
    companion.setStop([generations, &player, &reporter, &handoff, clear]
                      (const TransportRequest& request) {
        std::lock_guard<std::mutex> lock(handoff);
        if (!generations.current(request))
            return;
        retirePlayback(player, reporter);
        clear(stopClearGeneration(request, generations));
    });
    companion.setSeek([generations, &player, &activePlay, &handoff, &seekMutex, start, post]
                      (const TransportRequest& request) {
        post([generations, &player, &activePlay, &handoff, &seekMutex, start, request] {
            std::lock_guard<std::mutex> seekLock(seekMutex);
            if (!generations.current(request))
                return;
            try {
                const auto& media = request.media;
                {
                    // OWENB-P1B T5: the OB play seeks in place (held picture, the same ring session) instead of
                    // a re-resolve; the PMS offset is rewritten on the current URL (MediaPlayer::owenbSeek).
                    std::lock_guard<std::mutex> lock(handoff);
                    if (!generations.current(request))
                        return;
                    if (owenbPlayActiveOf(player, 0) && request.epoch == player.playbackEpoch()) {
                        player.seekMs(media.offsetMs, [&activePlay, request] {
                            activePlay.store(request.generation);
                        }, request.generation);
                        return;
                    }
                }
                if (media.key.rfind("/library", 0) == 0 ||
                    media.key.find("library/metadata") != std::string::npos) {
                    start(media);
                } else {
                    {
                        std::lock_guard<std::mutex> lock(handoff);
                        if (!generations.current(request))
                            return;
                        const auto binding = player.startedPlayback();
                        if (binding.generation != 0 &&
                            binding.generation == request.originGeneration &&
                            binding.epoch == request.epoch &&
                            binding.epoch == player.playbackEpoch()) {
                            player.seekMs(media.offsetMs, [&activePlay, request] {
                                activePlay.store(request.generation);
                            }, request.generation);
                            return;
                        }
                    }
                    // Loading/unknown bindings belong to the captured request,
                    // not the previous currentUrl. start() reacquires handoff.
                    if (generations.current(request))
                        start(media);
                }
            } catch (...) {
                std::fputs("misterplexd: seek exception\n", stderr);
            }
        });
    });
}

} // namespace misterplex
