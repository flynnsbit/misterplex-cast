#pragma once
// Controller shortcuts for audio / subtitle switching. Pure: pad masks in, an action out, plus the
// cycle choice over a StreamCatalog (track_select.hpp). Nothing here reads a pad, calls PMS or paints an OSD.
// While playing, LB/RB already step the play queue (media_player.cpp PageUp/PageDown -> postQueueStep), so the
// shortcut is a CHORD: hold the modifier pad button, then press the audio or subtitle button. The pad bits are
// a caller-supplied Binding. A binding with no modifier, no button, or overlapping masks is disabled.
// Folded from the #72 helper. The OB input loop is the only caller (track_shortcuts_glue.hpp).
#include "track_select.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace misterplex {
namespace trackkeys {

enum class Action { None, CycleAudio, CycleSubtitle };

struct Binding {
    uint32_t modifier = 0, audio = 0, subtitle = 0;
};
struct Gate {
    bool enabled = false;      // conf controller_track_shortcuts named a valid binding
    bool playing = false;
    bool browseVisible = false;
};

constexpr int64_t kMinGapMs = 250;

inline bool bindingValid(const Binding& b) {
    return b.modifier && b.audio && b.subtitle && !(b.modifier & b.audio) && !(b.modifier & b.subtitle) &&
           !(b.audio & b.subtitle);
}
inline bool gateOpen(const Gate& g) {
#ifdef MUT_TK_ENABLED_IGNORED
    return g.playing && !g.browseVisible;
#elif defined(MUT_TK_IGNORE_PLAYING)
    return g.enabled && !g.browseVisible;
#else
    return g.enabled && g.playing && !g.browseVisible;
#endif
}

class Chord {
public:
    Action feed(uint32_t pads, int64_t nowMs, const Binding& b, const Gate& g) {
        const uint32_t prev = prev_;
        prev_ = pads;
        if (!started_) {   // a button already held at start is not a press
            started_ = true;
            return Action::None;
        }
        if (!bindingValid(b) || !gateOpen(g))
            return Action::None;
#ifdef MUT_TK_CHORD_NO_MOD
        const bool held = true;
#else
        const bool held = (pads & b.modifier) == b.modifier;
#endif
#ifdef MUT_TK_REPEAT_ON_HOLD
        const uint32_t rising = pads;
#else
        const uint32_t rising = pads & ~prev;
#endif
        Action a = Action::None;
        if (held && (rising & b.audio))
            a = Action::CycleAudio;
        else if (held && (rising & b.subtitle))
            a = Action::CycleSubtitle;
        if (a == Action::None)
            return a;
#ifndef MUT_TK_NO_DEBOUNCE
        if (nowMs - lastMs_ < kMinGapMs)
            return Action::None;
#endif
        lastMs_ = nowMs;
        return a;
    }
    // While the chord is armed the caller drops the pad navs (LB/RB queue skip) so a chord never also skips.
    bool consumes(uint32_t pads, const Binding& b, const Gate& g) const {
        return bindingValid(b) && gateOpen(g) && (pads & b.modifier) == b.modifier;
    }

private:
    uint32_t prev_ = 0;
    int64_t lastMs_ = -1000000;
    bool started_ = false;
};

struct CycleResult {
    bool changed = false;
    int streamId = 0;          // the stream to select; for subtitles 0 = off
    int pos = 0, total = 0;    // 1-based position among the choices (subtitles: Off is the last choice)
    std::string osd;           // one short line, printable ASCII, at most 24 chars
};

inline std::string osdLine(const char* what, int pos, int total, const MediaStream* s) {
    std::string o = std::string(what) + " " + std::to_string(pos) + "/" + std::to_string(total);
    if (s) {
        o += " " + (s->language.empty() ? std::string("und") : s->language);
        if (!s->codec.empty())
            o += " " + trackLower(s->codec);
    }
    std::string r;
    for (const char c : o) {
        if (r.size() >= 24)
            break;
        const unsigned char u = static_cast<unsigned char>(c);
        r += (u >= 0x20 && u <= 0x7e) ? c : '?';
    }
    return r;
}

inline bool inPart(const MediaStream& s, int partId) {
#ifdef MUT_TK_OTHER_PART
    (void)s; (void)partId;
    return true;
#else
    return track_detail::streamInPart(s, partId);
#endif
}

// Audio: the PMS stream order of this part, wrapping. One stream or none changes nothing.
// A subtitle id is never an audio choice.
inline CycleResult cycleAudio(const StreamCatalog& cat, int partId, int currentId) {
    std::vector<const MediaStream*> a;
    for (const MediaStream& s : cat.streams) {
#ifdef MUT_TK_AUDIO_TAKES_SUB
        if ((s.kind == StreamKind::Audio || s.kind == StreamKind::Subtitle) && inPart(s, partId))
#else
        if (s.kind == StreamKind::Audio && inPart(s, partId))
#endif
            a.push_back(&s);
    }
    CycleResult r;
    if (a.empty()) { r.osd = "No audio tracks"; return r; }
    size_t cur = a.size();   // unknown current = before the first
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i]->id == currentId)
            cur = i;
    size_t nx = cur + 1;
#ifdef MUT_TK_NO_WRAP
    if (nx >= a.size()) nx = a.size() - 1;
#else
    if (nx >= a.size()) nx = 0;
#endif
    r.total = static_cast<int>(a.size());
    r.pos = static_cast<int>(nx) + 1;
    r.streamId = a[nx]->id;
    r.changed = a[nx]->id != currentId;
    r.osd = osdLine("Audio", r.pos, r.total, a[nx]);
    return r;
}

// Subtitles: text tracks (image tracks only when burn-in is supported), then Off, then wrap to the first.
inline CycleResult cycleSubtitle(const StreamCatalog& cat, int partId, int currentId, bool burnInSupported) {
    std::vector<const MediaStream*> v;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || !inPart(s, partId))
            continue;
#ifndef MUT_TK_IMAGE_NOT_SKIPPED
        if (subtitleCodecIsImage(s.codec) && !burnInSupported)
            continue;
#endif
        v.push_back(&s);
    }
    CycleResult r;
    if (v.empty()) { r.osd = "No subtitles"; return r; }
    const size_t offIdx = v.size();   // the last choice is Off
    size_t cur = offIdx;              // 0 or an unknown id counts as Off
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i]->id == currentId && currentId != 0)
            cur = i;
    size_t nx = cur + 1;
#ifdef MUT_TK_SUB_NO_OFF
    if (nx >= offIdx) nx = 0;
#else
    if (nx > offIdx) nx = 0;
#endif
    r.total = static_cast<int>(v.size()) + 1;
    r.pos = static_cast<int>(nx) + 1;
    r.streamId = nx == offIdx ? 0 : v[nx]->id;
    r.changed = r.streamId != (cur == offIdx ? 0 : currentId);
    r.osd = nx == offIdx ? "Sub off " + std::to_string(r.pos) + "/" + std::to_string(r.total)
                         : osdLine("Sub", r.pos, r.total, v[nx]);
    return r;
}

}  // namespace trackkeys
}  // namespace misterplex
