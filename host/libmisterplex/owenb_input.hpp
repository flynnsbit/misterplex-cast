#pragma once
// OWENB-P1B 0010: navigation from what owenb exposes. While a core runs Main grabs every input device
// (EVIOCGRAB, h264ring.h:383); the core mirrors Main's pads, keys and OSD into the input region, words
// 48-71 (h264ring.h:382-394), read with h264ring_input (a seqlocked copy, no session needed, beside
// the writer without its mutex: h264ring.h:36-37) and h264ring_input_events (h264ring.h:402-409).
//   - keys: EV_KEY, PS/2 set-2 codes ([9] pressed, [8] E0; h264ring_layout.h:38-39,125);
//   - pads: PADn_JOY snapshots (h264ring_layout.h:769-816), the J line's bits (h264ring.h:416-421):
//     edges are taken here, with auto-repeat for a held d-pad;
//   - Main's OSD: EV_OSD opened/closed (h264ring_layout.h:127): while it is open Main's menu has the
//     input, so we act on nothing (keys and pads are released);
//   - the OSD's actions (EV_ACTION, h264ring_layout.h:128; h264ring.h:390-394): 0 "Send Home to the
//     program" -> Home (our library), 1 "Program menu" -> Menu (our menu panel). They arrive BEFORE
//     the OSD's closing: kept, delivered after the close.
// A lost queue (read more than H264RING_IN_Q_LEN events late) releases every key and button.

#include "h264ring.h"

#include <cstdint>
#include <vector>

namespace misterplex::owenb {

enum class ObNav : uint8_t { Up, Down, Left, Right, Select, Back, PageUp, PageDown, Info, Menu, Home, PlayPause, Aux };

inline const char* obNavName(ObNav n) {
    static const char* const k[] = {"up", "down", "left", "right", "select", "back",
                                    "pageup", "pagedown", "info", "menu", "home", "playpause", "aux"};
    return unsigned(n) < sizeof k / sizeof k[0] ? k[unsigned(n)] : "?";
}

// PS/2 set-2 make codes (ext = the E0 prefix) -> a navigation, or false.
inline bool obNavForKey(unsigned code, bool ext, ObNav& n) {
    struct K {
        unsigned code;
        bool ext;
        ObNav nav;
    };
    static const K k[] = {
        {0x75, true, ObNav::Up},       {0x72, true, ObNav::Down},     {0x6B, true, ObNav::Left},
        {0x74, true, ObNav::Right},    {0x7D, true, ObNav::PageUp},   {0x7A, true, ObNav::PageDown},
        {0x5A, true, ObNav::Select},   {0x6C, true, ObNav::Home},     {0x5A, false, ObNav::Select},
        {0x76, false, ObNav::Back},    {0x66, false, ObNav::Back},    {0x29, false, ObNav::PlayPause},
        {0x43, false, ObNav::Info},    {0x3A, false, ObNav::Menu},    {0x0D, false, ObNav::Menu},
    };   // arrows, PgUp/PgDn, Enter, Home; Esc/Backspace back, Space, I(nfo), M(enu)/Tab
    for (const K& x : k)
        if (x.code == code && x.ext == ext) {
            n = x.nav;
            return true;
        }
    return false;
}

// one pad button -> a navigation (A select, B back, X info, Y / Start menu, LB/RB page, Back home)
struct ObPadMap {
    uint32_t bit;
    ObNav nav;
    bool repeat;
};
inline const std::vector<ObPadMap>& obPadMap() {
    static const std::vector<ObPadMap> m = {
        {H264RING_PAD_UP, ObNav::Up, true},       {H264RING_PAD_DOWN, ObNav::Down, true},
        {H264RING_PAD_LEFT, ObNav::Left, true},   {H264RING_PAD_RIGHT, ObNav::Right, true},
        {H264RING_PAD_A, ObNav::Select, false},   {H264RING_PAD_B, ObNav::Back, false},
        {H264RING_PAD_X, ObNav::Info, false},     {H264RING_PAD_Y, ObNav::Aux, false},   // bug 3: bit 7 (Skip Back while playing)
        {H264RING_PAD_START, ObNav::Menu, false}, {H264RING_PAD_BACK, ObNav::Home, false},
        {H264RING_PAD_LB, ObNav::PageUp, true},   {H264RING_PAD_RB, ObNav::PageDown, true},
    };
    return m;
}

struct ObInputStats {
    unsigned events = 0, keys = 0, actions = 0, osdOpens = 0, lost = 0, navs = 0;
    // bug 3: what the RBF sends that no mapping takes (logged by the daemon, so glass shows the real ids)
    unsigned unknownActions = 0, unmappedKeys = 0;
    int lastUnknownAction = -1, lastUnmappedKey = -1;   // ACTION number; KEY code | 0x100 for E0
};

// Feed it each copy of the region (h264ring_input), at the poll's rate (the daemon: ~15 ms).
class ObInputDecoder {
public:
    static constexpr int64_t kRepeatDelayMs = 400, kRepeatMs = 120;

    static uint32_t padsOf(const struct ::h264ring_input& in) {
        uint32_t p = 0;   // any pad drives the menus (pad n is Main's player n + 1, h264ring.h:261)
        for (unsigned i = 0; i < 6; ++i)
            p |= uint32_t(in.w[H264RING_PAD0_JOY_W - H264RING_IN_WORD + i]) & 0xFFFFu;
        return p;
    }

    std::vector<ObNav> feed(const struct ::h264ring_input& in, int64_t nowMs) {
        std::vector<ObNav> out;
        const uint32_t pads = padsOf(in);
        if (!started_) {   // events before we began are not ours; a button already held is not a press
            started_ = true;
            count_ = uint32_t(H264RING_IN(&in, EV_COUNT));
            pads_ = pads;
            return out;
        }
        uint16_t ev[H264RING_IN_Q_LEN];
        uint32_t lost = 0;
        const unsigned n = h264ring_input_events(&in, &count_, ev, H264RING_IN_Q_LEN, &lost);
        if (lost) {   // h264ring.h:405-407: treat every key and button as unknown
            st_.lost += lost;
            pads_ = pads;
            pending_.clear();
        }
        for (unsigned i = 0; i < n; ++i)
            event(ev[i], pads, out);
        if (osd_) {
            pads_ = pads;
#ifndef MUT_INPUT_ACT_IN_OSD
            st_.navs += unsigned(out.size());
            return out;
#endif
        }
        padEdges(pads, nowMs, out);
        pads_ = pads;
        st_.navs += unsigned(out.size());
        return out;
    }
    bool osdOpen() const { return osd_; }
    const ObInputStats& stats() const { return st_; }

private:
    void event(uint16_t e, uint32_t pads, std::vector<ObNav>& out) {
        ++st_.events;
        const unsigned type = H264RING_EV_TYPE(e);
        if (type == H264RING_EV_KEY) {
            ObNav nv;
            if (H264RING_EV_PRESSED(e) && !obNavForKey(H264RING_EV_CODE(e), H264RING_EV_EXT(e) != 0, nv)) {
                ++st_.unmappedKeys;
                st_.lastUnmappedKey = int(H264RING_EV_CODE(e) | (H264RING_EV_EXT(e) ? 0x100u : 0u));
            } else if (H264RING_EV_PRESSED(e)) {
                ++st_.keys;
#ifndef MUT_INPUT_ACT_IN_OSD
                if (!osd_)
#endif
                    out.push_back(nv);
            }
        } else if (type == H264RING_EV_OSD) {
            osd_ = H264RING_EV_OPEN(e) != 0;
            if (osd_) {
                ++st_.osdOpens;
            } else {   // the close: the action chosen in it now, the pad's state as Main left it
                out.insert(out.end(), pending_.begin(), pending_.end());
                pending_.clear();
                pads_ = pads;
            }
        } else if (type == H264RING_EV_ACTION) {
            ++st_.actions;
            const unsigned a = H264RING_EV_CODE(e);
            if (a > 1) {   // h264ring.h IN_ACTIONS is 2; a fork's extra entry is counted, never acted on blind
                ++st_.unknownActions;
                st_.lastUnknownAction = int(a);
                return;
            }
            const ObNav nv = a == 0 ? ObNav::Home : ObNav::Menu;
            if (osd_)
                pending_.push_back(nv);   // "take it while the OSD is open, act on it after the close"
            else
                out.push_back(nv);
        }   // mouse buttons: not used
    }
    void padEdges(uint32_t pads, int64_t nowMs, std::vector<ObNav>& out) {
        for (const ObPadMap& m : obPadMap()) {
            const unsigned b = bitIndex(m.bit);
            const bool now = pads & m.bit;
#ifdef MUT_INPUT_LEVEL
            const bool before = false;   // mutant: level-triggered -- a held button fires every poll
#else
            const bool before = pads_ & m.bit;
#endif
            if (now && !before) {
                out.push_back(m.nav);
                nextRepeat_[b] = nowMs + kRepeatDelayMs;
            } else if (now && m.repeat && nowMs >= nextRepeat_[b]) {
                out.push_back(m.nav);
                nextRepeat_[b] = nowMs + kRepeatMs;
            }
        }
    }
    static unsigned bitIndex(uint32_t bit) {
        unsigned i = 0;
        while (i < 15 && !(bit >> i & 1u))
            ++i;
        return i;
    }
    bool started_ = false, osd_ = false;
    uint32_t count_ = 0, pads_ = 0;
    int64_t nextRepeat_[16] = {};
    std::vector<ObNav> pending_;
    ObInputStats st_;
};

} // namespace misterplex::owenb
