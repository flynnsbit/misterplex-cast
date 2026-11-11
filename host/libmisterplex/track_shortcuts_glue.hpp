#pragma once
// Glue between the OB input loop and the pure chord detector (track_shortcuts.hpp): the pad word in, the navs the
// router may still see, and (on a chord) the SetStreamsRequest that goes through the same applyUserStreams path as
// Plex Web's setStreams. Host-testable: no pad read, no PMS call, no OSD paint here.
#include "owenb_input.hpp"
#include "track_shortcuts.hpp"

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace misterplex {
namespace trackkeys {

inline uint32_t padBitByName(std::string n) {
    for (char& c : n)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "a") return H264RING_PAD_A;
    if (n == "b") return H264RING_PAD_B;
    if (n == "x") return H264RING_PAD_X;
    if (n == "y") return H264RING_PAD_Y;
    if (n == "lb") return H264RING_PAD_LB;
    if (n == "rb") return H264RING_PAD_RB;
    if (n == "back") return H264RING_PAD_BACK;
    if (n == "start") return H264RING_PAD_START;
    return 0;
}

// conf controller_track_shortcuts = "<modifier>,<audio>,<subtitle>" (pad names). Anything unclear (empty, on/off,
// an unknown name, a wrong count, overlapping buttons) yields a disabled Binding. There is no default chord.
inline Binding parseBinding(const std::string& s) {
    std::vector<std::string> p;
    size_t from = 0;
    while (true) {
        const size_t c = s.find(',', from);
        std::string t = s.substr(from, c == std::string::npos ? std::string::npos : c - from);
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        p.push_back(t);
        if (c == std::string::npos) break;
        from = c + 1;
    }
    Binding b;
    if (p.size() != 3)
        return b;
    b.modifier = padBitByName(p[0]);
    b.audio = padBitByName(p[1]);
    b.subtitle = padBitByName(p[2]);
#ifndef MUT_TKG_PARSE_LAX
    if (!bindingValid(b))
        return Binding{};
#endif
    return b;
}

struct Tracks {   // a snapshot of the live play's streams (taken only when a chord fires)
    StreamCatalog catalog;
    int partId = 0, audioId = 0, subtitleId = 0;
    bool burnIn = false;
};

struct Step {
    std::vector<owenb::ObNav> navs;   // what the router may still see
    Action action = Action::None;
    bool hasRequest = false;
    SetStreamsRequest req;
    std::string osd;
};

class Shortcuts {
public:
    Step step(uint32_t pads, int64_t nowMs, std::vector<owenb::ObNav> navs, const Binding& b, const Gate& g,
              const std::function<Tracks()>& tracks) {
        Step s;
        s.action = chord_.feed(pads, nowMs, b, g);
#ifdef MUT_TKG_NAVS_NOT_DROPPED
        s.navs = std::move(navs);
#else
        if (!chord_.consumes(pads, b, g))
            s.navs = std::move(navs);
#endif
        if (s.action == Action::None)
            return s;
        const Tracks t = tracks();
        const CycleResult r = s.action == Action::CycleAudio
                                  ? cycleAudio(t.catalog, t.partId, t.audioId)
                                  : cycleSubtitle(t.catalog, t.partId, t.subtitleId, t.burnIn);
        s.osd = r.osd;
#ifdef MUT_TKG_REQUEST_WHEN_UNCHANGED
        const bool want = r.total > 0;
#else
        const bool want = r.changed;
#endif
        if (!want)
            return s;
        s.hasRequest = true;
        if (s.action == Action::CycleAudio) {
            s.req.hasAudio = true;
            s.req.audioStreamId = r.streamId;
#ifdef MUT_TKG_AUDIO_REQ_HAS_SUB
            s.req.hasSubtitle = true;
            s.req.subtitleStreamId = 0;
#endif
        } else {
            s.req.hasSubtitle = true;
            s.req.subtitleStreamId = r.streamId;
        }
        return s;
    }

private:
    Chord chord_;
};

}  // namespace trackkeys
}  // namespace misterplex
