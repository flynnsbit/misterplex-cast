#pragma once
// OWENB-P1B T6: what the ARM overlay draws (ARM-RENDERER-SPEC-1017 §2-§3) into a straight-alpha
// BGRA shadow of the band, and the copy of that band into an fb0 page.
//
// Pixels: 8888 = bytes B, G, R, A, straight alpha (h264ring.h:282-284; h264/ovl_plane.sv:458-466
// unpacks A [31:24] R [23:16] G [15:8] B [7:0] and premultiplies itself, ovl_plane.sv:467 +
// ovl_blend.sv:1-57). A zeroed byte is transparent. Glyphs are the daemon's 5x7 bitmap font
// (fpga_playback_overlay.hpp glyph(), playback_overlay.hpp) scaled sy lines tall and sy / par
// pixels wide (§2: par = aspect x H / W); the spec's TTF path (stb_truetype) is not in this
// change -- the bitmap font is the spec's own fallback (T6-OSD-SPEC-GAPS.md G1).
// 480i / 576i: every fill starts on an even line and covers an even count (strokes >= 2 lines,
// §2); the fabric's (1,2,1) anti-twitter stays on (fpga-bug-arm-osd-review-1017.md).

#include "ambient.hpp"
#include "browse_nav.hpp"
#include "fpga_playback_overlay.hpp"
#include "idle_screen.hpp"
#include "owenb_osd_layout.hpp"
#include "subtitle_cues.hpp"
#include "subtitle_position.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace misterplex::owenb_osd {

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

constexpr Rgba kPanel{22, 25, 28, 0xD0};      // fpga_overlay bg 0x16191c at 0xD0
constexpr Rgba kFg{0xef, 0xef, 0xf0, 0xFF};   // fpga_overlay fg 0xefeff0
constexpr Rgba kAccent{0xff, 0xb2, 0x20, 0xFF};
constexpr Rgba kTrack{70, 74, 80, 0xFF};
constexpr Rgba kBuffered{130, 134, 140, 0xFF};
constexpr Rgba kGhost{0xef, 0xef, 0xf0, 0x80};
constexpr Rgba kDim{150, 152, 156, 0xFF};
constexpr Rgba kPanelOnBlack{18, 20, 22, 0xFF};   // kPanel at 0xD0 over black, opaque
constexpr Rgba kErrorBg{10, 12, 14, 0xFF};    // error screens: opaque, the whole canvas

enum class Bar : uint8_t { Hidden, Playing, Paused, Buffering, Seeking };
// DaemonDown: "daemon not running", painted by tools/owenb_idle_card (no daemon is up to arm the Ready card).
enum class ErrorScreen : uint8_t { None, Refused, CoreNotLoaded, PmsUnreachable, StreamError, DaemonDown };

struct Menu {
    bool open = false;
    std::string title;
    std::vector<std::string> items;
    int selected = -1;
    std::vector<uint8_t> dim;   // 0010: 1 = an information row (dim, never selected)
};

// E9 calibration screen. Percents are the draft the couch is nudging (0-25).
// The store is updated only when the user saves.
struct OverscanView {
    bool open = false;
    int focus = 0;   // 0 left, 1 right, 2 top, 3 bottom
    int left = 0, right = 0, top = 0, bottom = 0;
};

// 0010: the library page (the daemon's browser, browse_controller.hpp / library_browser.hpp), a
// window of rows around the cursor: rows[0] is entry `offset` of `total`.
struct LibraryRow {
    std::string text;
    bool dir = false;          // a container: drawn with a trailing "/"
    bool selectable = true;    // false: dim (a header, "Loading...")
};
struct LibraryPage {
    bool open = false;
    std::string header;        // the breadcrumb: "Plex / Movies"
    std::string status;        // "Loading...", an error ("" none)
    std::vector<LibraryRow> rows;
    int cursor = -1;           // into rows
    int offset = 0, total = 0;
    bool operator==(const LibraryPage& o) const {
        if (open != o.open || header != o.header || status != o.status || cursor != o.cursor || offset != o.offset ||
            total != o.total || rows.size() != o.rows.size())
            return false;
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].text != o.rows[i].text || rows[i].dir != o.rows[i].dir ||
                rows[i].selectable != o.rows[i].selectable)
                return false;
        return true;
    }
    bool operator!=(const LibraryPage& o) const { return !(*this == o); }
};

// One resolved frame of UI (the timers already applied: UiModel::resolve, owenb_osd_renderer.hpp).
// OB port: a full-screen BGRA8888 frame drawn by another renderer (PlexUI), shown instead of the
// idle card while it is set. Opaque, tightly packed (stride w * 4), w x h <= the canvas.
struct FullSurface {
    unsigned w = 0, h = 0;
    uint64_t gen = 0;              // bumps with every new frame
    std::vector<uint8_t> bgra;
};

// Skip Intro / Skip Credits button, or the Up Next card. Empty unless MARKERS is on.
struct MarkerOverlay {
    bool card = false;
    int countdownS = -1;
    std::string label;
    std::string line1, line2, line3, line4;
    bool visible() const { return card || !label.empty(); }
    void clear() {
        card = false;
        countdownS = -1;
        label.clear();
        line1.clear();
        line2.clear();
        line3.clear();
        line4.clear();
    }
};

// One BIF frame, straight BGRA, shown above the bar while a seek is settling.
// Empty unless SCRUB_THUMBS is on and the part had an index.
struct ScrubThumb {
    int64_t atMs = -1;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bgra;
    bool visible() const {
        return atMs >= 0 && w > 0 && h > 0 && bgra.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    }
};

struct UiState {
    Bar bar = Bar::Hidden;
    std::string title;          // "" -> "MiSTerPlex"
    std::string transport;      // "+30s" feedback while it lasts: replaces the state word
    int64_t positionMs = 0, durationMs = 0;
    int64_t bufferedMs = -1;    // the buffered fill's end (-1 unknown)
    int bufferingPct = -1;
    std::string statusText;     // NET_RESUME: "RECONNECTING... 2/8" while the bar is Buffering
    int64_t seekTargetMs = -1;  // the ghost knob during the seek settle
    std::shared_ptr<const ScrubThumb> scrubThumb; // BIF preview while Seeking
    std::string chip;           // "Casting from <controller>": the controller ("" none)
    std::vector<std::string> toasts;   // at most 2, newest last
    ErrorScreen error = ErrorScreen::None;
    std::string errorDetail;    // refusal_name(), a server name, an error string
    Menu menu;
    // T7: the idle card, shown on the idle session (no play): "MISTERPLEX", the hint, the device's name
    bool idle = false;
    bool spelunkr = false;
    // PLAY-PENDING: play/cast accepted, first HDMI picture not up yet. The status card
    // and PlexUI are off; paint() draws the big chevron and, when set, pendingLabel
    // ("Loading YouTube..." / "Loading AirPlay..." / "Loading Plex..."). The 5x7 font
    // paints that line uppercased. buffering=true is not this cue. Empty label is the
    // chevron alone (a seek that never armed the cue).
    bool pending = false;
    std::string pendingLabel;
    bool spelunkrStatic = false;
    uint32_t staticFrame = 0;
    std::string idleHint;       // "" -> "Ready to cast"
    std::string idleName;       // the companion's device name ("" none)
    // AMBIENT, default off. Null unless the flag is on, idle, and a library
    // art card is current. The Ready card is painted over this field.
    std::shared_ptr<const misterplex::AmbientDeck> ambient;
    int ambientSlot = -1;
    LibraryPage library;        // 0010: over the idle card while open
    std::shared_ptr<const FullSurface> surface;   // OB port: PlexUI, replaces the whole page
    std::string caption1, caption2;
    bool captionArmed = false;  // text track on: reserve the caption+bar band
    int captionCue = -1;
    int64_t captionStreamId = 0;
    int64_t captionMediaMs = 0;
    MarkerOverlay marker;
    OverscanView overscan;
    // Spelunkr TV (daemon/OSD on the owenb core). Banner fades; guide is a channel list.
    struct SpelunkrBanner {
        bool on = false;
        uint8_t alpha = 255;
        std::string dial;
        std::string name;
        std::string title;
    } spelunkrBanner;
    struct SpelunkrGuide {
        bool open = false;
        int cursor = -1;
        std::vector<LibraryRow> rows;
    } spelunkrGuide;
    bool anyVisible() const {
        return bar != Bar::Hidden || !chip.empty() || !toasts.empty() || error != ErrorScreen::None || menu.open ||
               idle || pending || library.open || surface || captionArmed || !caption1.empty() || !caption2.empty() ||
               marker.visible() || overscan.open || spelunkrBanner.on || spelunkrGuide.open;
    }
};

inline uint64_t fnvMix(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline uint64_t fnvMix(uint64_t h, const std::string& s) {
    const uint64_t n = s.size();
    return fnvMix(fnvMix(h, &n, sizeof n), s.data(), s.size());
}

// Flag on, idle, and a decoded card. Menu, library, error, overscan, and
// PLAY-PENDING stay on the existing field. MUT_AMBIENT_PAINT_WHEN_OFF paints
// even when the flag is off.
inline bool ambientFieldWanted(const UiState& s) {
    if (!s.idle || s.pending || s.menu.open || s.library.open || s.overscan.open || s.surface ||
        s.error != ErrorScreen::None)
        return false;
    if (!s.ambient || s.ambientSlot < 0)
        return false;
    if (!misterplex::ambientArtAt(*s.ambient, s.ambientSlot))
        return false;
#ifdef MUT_AMBIENT_PAINT_WHEN_OFF
    return true;
#else
    return misterplex::ambientEnabled();
#endif
}

inline void mixAmbientKey(uint64_t& h, const UiState& s) {
    if (!ambientFieldWanted(s))
        return;
    const misterplex::AmbientArt* art = misterplex::ambientArtAt(*s.ambient, s.ambientSlot);
    if (!art)
        return;
    const int64_t av[] = {static_cast<int64_t>(s.ambientSlot), static_cast<int64_t>(art->fingerprint)};
    h = fnvMix(h, av, sizeof av);
    h = fnvMix(h, art->ratingKey);
}

// What is drawn, at the precision it is drawn (seconds for the clocks): equal keys = equal pixels.
inline uint64_t contentKey(const UiState& s, const Layout& L) {
    uint64_t h = 1469598103934665603ull;
    const int64_t geo[] = {L.W, L.H, L.bar.x, L.bar.y, L.bar.w, L.bar.h, L.safe.x, L.safe.y, L.safe.w,
                           L.safe.h, int64_t(L.par * 65536.0), L.evenLines, int64_t(L.mode)};
    h = fnvMix(h, geo, sizeof geo);
    const int64_t v[] = {int64_t(s.bar),
                         s.positionMs / 1000,
                         s.durationMs / 1000,
                         s.bufferedMs < 0 ? -1 : s.bufferedMs / 1000,
                         s.bufferingPct,
                         s.seekTargetMs < 0 ? -1 : s.seekTargetMs / 1000,
                         int64_t(s.error),
                         s.menu.open,
                         s.menu.selected,
                         int64_t(s.toasts.size()),
                         int64_t(s.menu.items.size()),
                         s.idle,
                         int64_t(s.spelunkr),
                         int64_t(s.pending),
                         int64_t(s.spelunkrStatic),
                         s.staticFrame};
    h = fnvMix(h, v, sizeof v);
    for (const std::string* t : {&s.title, &s.transport, &s.chip, &s.errorDetail, &s.menu.title, &s.idleHint,
                                 &s.idleName, &s.caption1, &s.caption2, &s.statusText, &s.pendingLabel})
        h = fnvMix(h, *t);
    const uint8_t capArm = s.captionArmed ? 1 : 0;
    h = fnvMix(h, &capArm, 1);
    const int subPos = static_cast<int>(misterplex::subtitlePosition());
    h = fnvMix(h, &subPos, sizeof subPos);
    const int64_t ov[] = {s.overscan.open ? int64_t{1} : int64_t{0}, s.overscan.focus, s.overscan.left,
                          s.overscan.right, s.overscan.top, s.overscan.bottom};
    h = fnvMix(h, ov, sizeof ov);
    if (s.marker.visible()) {
        const int64_t mv[] = {s.marker.card ? int64_t{1} : int64_t{0}, s.marker.countdownS};
        h = fnvMix(h, mv, sizeof mv);
        for (const std::string* t : {&s.marker.label, &s.marker.line1, &s.marker.line2, &s.marker.line3,
                                     &s.marker.line4})
            h = fnvMix(h, *t);
    }
    if (s.scrubThumb && s.scrubThumb->visible()) {
        const int64_t tv[] = {s.scrubThumb->atMs, s.scrubThumb->w, s.scrubThumb->h, s.seekTargetMs};
        h = fnvMix(h, tv, sizeof tv);
        h = fnvMix(h, s.scrubThumb->bgra.data(), s.scrubThumb->bgra.size());
    }
    for (const auto& t : s.toasts)
        h = fnvMix(h, t);
    for (const auto& t : s.menu.items)
        h = fnvMix(h, t);
    h = fnvMix(h, s.menu.dim.data(), s.menu.dim.size());
    const LibraryPage& lb = s.library;
    const int64_t lv[] = {lb.open, lb.cursor, lb.offset, lb.total, int64_t(lb.rows.size())};
    h = fnvMix(h, lv, sizeof lv);
    h = fnvMix(h, lb.header);
    h = fnvMix(h, lb.status);
    for (const LibraryRow& r : lb.rows) {
        h = fnvMix(h, r.text);
        const uint8_t fl = uint8_t(r.dir | r.selectable << 1);
        h = fnvMix(h, &fl, 1);
    }
    mixAmbientKey(h, s);
    if (s.spelunkrBanner.on) {
        h = fnvMix(h, &s.spelunkrBanner.alpha, 1);
        h = fnvMix(h, s.spelunkrBanner.dial);
        h = fnvMix(h, s.spelunkrBanner.name);
        h = fnvMix(h, s.spelunkrBanner.title);
    }
    if (s.spelunkrGuide.open) {
        h = fnvMix(h, &s.spelunkrGuide.cursor, sizeof s.spelunkrGuide.cursor);
        for (const auto& r : s.spelunkrGuide.rows)
            h = fnvMix(h, r.text);
    }
    return h;
}

// The screen, ignoring the clocks (position, buffer fill, seek ghost). A paused
// bar that only advances a second is the same screen, so an untaken flip keeps
// waiting. Idle, an error, the menu, the library page, or PlexUI is a new one.
inline uint64_t screenKey(const UiState& s, const Layout& L) {
    uint64_t h = 1469598103934665603ull;
    const int64_t geo[] = {L.W, L.H, int64_t(L.mode)};
    h = fnvMix(h, geo, sizeof geo);
    const int64_t v[] = {int64_t(s.bar), int64_t(s.error), s.menu.open, s.menu.selected,
                         int64_t(s.toasts.size()), int64_t(s.menu.items.size()), int64_t(s.idle),
                         int64_t(s.pending), int64_t(s.spelunkr), int64_t(s.library.open),
                         s.surface ? int64_t{1} : int64_t{0},
                         s.spelunkrBanner.on ? int64_t{1} : int64_t{0},
                         s.spelunkrBanner.on ? int64_t(s.spelunkrBanner.alpha) : int64_t{0},
                         s.spelunkrGuide.open ? int64_t{1} : int64_t{0}};
    h = fnvMix(h, v, sizeof v);
    for (const std::string* t : {&s.title, &s.transport, &s.chip, &s.errorDetail, &s.menu.title,
                                 &s.idleHint, &s.idleName, &s.caption1, &s.caption2, &s.statusText})
        h = fnvMix(h, *t);
    const uint8_t capArm = s.captionArmed ? 1 : 0;
    h = fnvMix(h, &capArm, 1);
    const int subPos = static_cast<int>(misterplex::subtitlePosition());
    h = fnvMix(h, &subPos, sizeof subPos);
    const int64_t ov[] = {s.overscan.open ? int64_t{1} : int64_t{0}, s.overscan.focus, s.overscan.left,
                          s.overscan.right, s.overscan.top, s.overscan.bottom};
    h = fnvMix(h, ov, sizeof ov);
    if (s.marker.visible()) {
        const uint8_t card = s.marker.card ? 1 : 0;
        h = fnvMix(h, &card, 1);
        h = fnvMix(h, s.marker.label);
        h = fnvMix(h, s.marker.line1);
    }
    if (s.surface)
        h = fnvMix(h, &s.surface->gen, sizeof s.surface->gen);
    mixAmbientKey(h, s);
    return h;
}

// GH#410: a hide the core never acked must retry, or the channel bar stays in fb0.
// pendingHide + empty chrome is not a "new screen" under the old anyVisible test.
inline bool overlayWaitForAck(bool pendingHide, bool anyVisible, bool screenKeyChanged, bool timeoutLogged) {
    if (pendingHide && timeoutLogged && !anyVisible)
        return false;
    const bool screenChanged = pendingHide ? anyVisible : screenKeyChanged;
    return !(timeoutLogged && screenChanged);
}

// H:MM:SS (§3)
inline std::string clockText(int64_t ms) {
    int64_t s = std::max<int64_t>(0, ms) / 1000;
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", static_cast<long long>(s / 3600),
                  static_cast<long long>(s / 60 % 60), static_cast<long long>(s % 60));
    return b;
}

// The font is upper case; anything outside ASCII prints as a space.
// U+00B7 MIDDLE DOT becomes the one-byte middot glyph (\x02).
inline std::string upper(const std::string& t) {
    std::string o;
    for (size_t i = 0; i < t.size();) {
        const uint8_t c = uint8_t(t[i++]);
        if (c == 0xC2 && i < t.size() && uint8_t(t[i]) == 0xB7) {
            ++i;
            o += '\x02';
            continue;
        }
        if (c < 0x80) {
            o += char(c >= 'a' && c <= 'z' ? c - 32 : (c >= 32 && c < 127) ? c : ' ');
            continue;
        }
        while (i < t.size() && (uint8_t(t[i]) & 0xc0) == 0x80)
            ++i;
        o += ' ';
    }
    return o;
}

// The 5x7 font: the daemon's glyphs plus the punctuation the OSD's strings use.
inline const uint8_t* osdGlyph(char ch) {
    static constexpr uint8_t extra[][7] = {
        {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00}, // +
        {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, // %
        {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08}, // ,
        {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, // !
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}, // ?
        {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, // (
        {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}, // )
        {0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d}, // &
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f}, // _
        {0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00}, // "
        {0x0a, 0x1f, 0x0a, 0x0a, 0x1f, 0x0a, 0x00}, // #
        {0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00}, // =
        {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x04, 0x08}, // ;
        {0x08, 0x0c, 0x0e, 0x0f, 0x0e, 0x0c, 0x08}, // > solid: the selection mark (\x01)
        {0x00, 0x00, 0x00, 0x0c, 0x0c, 0x00, 0x00}, // · middot (\x02), from U+00B7
    };
    static const char letters[] = "+%,!?()&_\"#=;\x01\x02";
    const char* f = ch ? std::strchr(letters, ch) : nullptr;
    return f ? extra[f - letters] : fpga_overlay::glyph(ch);
}

// The band's shadow: rows [y0, y0 + h) of a W-wide canvas, straight BGRA (zero = transparent).
struct Surface {
    unsigned W = 0;
    int y0 = 0, h = 0;
    bool evenLines = false;
    std::vector<uint8_t> px;

    void reset(unsigned w, int top, int rows, bool even) {
        W = w;
        y0 = top;
        h = std::max(0, rows);
        evenLines = even;
        px.assign(size_t(W) * size_t(h) * 4u, 0);
    }
    uint8_t* at(int x, int y) { return px.data() + (size_t(y - y0) * W + size_t(x)) * 4u; }
    const uint8_t* at(int x, int y) const { return px.data() + (size_t(y - y0) * W + size_t(x)) * 4u; }

    // straight-alpha "over": src (straight) over dst (straight), integer, deterministic
    static void over(uint8_t* d, Rgba s) {
        if (s.a == 0)
            return;
        if (s.a == 255 || d[3] == 0) {
            d[0] = s.b; d[1] = s.g; d[2] = s.r; d[3] = s.a;
            return;
        }
        const uint32_t sa = s.a, da = d[3];
        const uint32_t dw = da * (255u - sa);              // dst weight x 255
        const uint32_t oa255 = sa * 255u + dw;             // out alpha x 255
        const uint8_t sc[3] = {s.b, s.g, s.r};
        for (int i = 0; i < 3; ++i)
            d[i] = uint8_t((sc[i] * sa * 255u + d[i] * dw + oa255 / 2u) / oa255);
        d[3] = uint8_t((oa255 + 127u) / 255u);
    }

    void fill(Rect r, Rgba c, bool snap = true) {
        if (snap && evenLines) {   // even first line, even count (§2)
            const int b = r.bottom();
            r.y &= ~1;
            r.h = (b - r.y + 1) & ~1;
        }
        r = intersect(r, {0, y0, int(W), h});
        for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x)
                over(at(x, y), c);
    }
    // Straight alpha 0, so a later fill stores its colour exactly (the idle card over the chevron).
    void erase(Rect r) {
        if (evenLines) {
            const int b = r.bottom();
            r.y &= ~1;
            r.h = (b - r.y + 1) & ~1;
        }
        r = intersect(r, {0, y0, int(W), h});
        for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x) {
                uint8_t* d = at(x, y);
                d[0] = d[1] = d[2] = d[3] = 0;
            }
    }
};

// Text: 5x7 cells, 6 columns an advance, sy lines a glyph row, sx256 / 256 pixels a column.
struct Font {
    int sy = 3;
    int sx256 = 768;
    int colX(int c) const { return (c * sx256 + 128) >> 8; }
    int width(const std::string& t) const { return t.empty() ? 0 : colX(int(t.size()) * 6 - 1); }
    int height() const { return 7 * sy; }
    static Font forLayout(const Layout& L) {
        Font f;
        f.sy = (L.mode == OutMode::NativeHd || L.evenLines) ? 4 : 3;   // >= 20 px (§2); even at 480i
        const double sx = double(f.sy) / (L.par > 0.1 ? L.par : 1.0);
        f.sx256 = std::max(256, int(sx * 256.0 + 0.5));
        return f;
    }
    // the longest prefix that fits in w pixels, "..." marking a cut
    std::string fit(const std::string& t, int w) const {
        if (width(t) <= w)
            return t;
        std::string o = t;
        while (!o.empty() && width(o + "...") > w)
            o.pop_back();
        return o.empty() ? std::string() : o + "...";
    }
    void draw(Surface &s, int x, int y, const std::string &t, Rgba c, int maxW = -1) const {
        for (size_t i = 0; i < t.size(); ++i) {
            if (maxW >= 0 && colX(int(i) * 6) >= maxW)
                break;
            const uint8_t* rows = osdGlyph(t[i]);
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (rows[r] & (0x10 >> col)) {
                        const int g = int(i) * 6 + col;
                        Rect ink{x + colX(g), y + r * sy, colX(g + 1) - colX(g), sy};
                        if (maxW >= 0)
                            ink = intersect(ink, {x, y, maxW, height()});
                        s.fill(ink, c, false);
                    }
        }
    }
};

// PR #393 overlay font. Columns are
// 4/5 of Font::forLayout (~20% narrower). Rows are 4/5 too, then even on
// interlaced output so sy 3 does not twitter under (1,2,1).
inline Font inPlayFont(const Font& menu, bool evenLines) {
    Font f = menu;
    f.sx256 = std::max(256, (menu.sx256 * 4) / 5);
    int sy = std::max(2, (menu.sy * 4) / 5);
    if (evenLines && (sy & 1))
        --sy;
    if (sy < 2)
        sy = 2;
    f.sy = sy;
    return f;
}

inline Font spelunkrBannerFont(const Font &menu, bool evenLines) {
    // GH#410: one #393 4/5 pass (~20% narrower). A second pass is ~36%.
    return inPlayFont(menu, evenLines);
}

// GH#410/#412: pending and playing layouts share this so odd safe.y cannot drift.
inline Rect spelunkrBannerRect(const Layout& L, int pad, int fontH) {
    int h = 2 * fontH + 2 * pad;
    int y = L.safe.y;
    if (L.evenLines) {
        h = (h + 1) & ~1;
        y &= ~1;
    }
    return {L.safe.x, y, L.safe.w, h};
}

// Where each element of one frame goes (canvas coordinates), and the band they need.
struct Frame {
    Rect bar, chip, menu, screen, card, lib, caption, reserve, markerBtn, markerCard, scrubThumb, spelunkrBanner,
        spelunkrGuide;
    Font markerFont;
    Rect toast[2];
    int toasts = 0;
    int pad = 6;
    Font font;
    Font cardFont;   // bug 5: the idle card's text, narrowed (columns only) until its longest line fits
    std::vector<std::string> idleHintRows;     // wrapped status rows (accent)
    std::vector<std::string> idleFooterRows;   // wrapped quiet footer (browse keys + box name)
    bool idleField = false;   // bug 4: the whole raster, so the core's idle test video stays covered
    bool any() const {
        return !bar.empty() || !chip.empty() || !menu.empty() || !screen.empty() || !card.empty() || !lib.empty() ||
               toasts || !caption.empty() || !reserve.empty() || !markerBtn.empty() || !markerCard.empty() ||
               !scrubThumb.empty() || !spelunkrBanner.empty() || !spelunkrGuide.empty();
    }
    // the band: the lines of every element (even-aligned at 480i), -1 / -1 when nothing shows
    void band(const Layout& L, int& first, int& last) const {
        if (idleField) {
            first = 0;
            last = std::max(0, int(L.H) - 1);
            return;
        }
        first = INT32_MAX;
        last = -1;
        auto add = [&](const Rect& r) {
            if (r.empty())
                return;
            first = std::min(first, r.y);
            last = std::max(last, r.bottom() - 1);
        };
        add(bar); add(chip); add(menu); add(screen); add(card); add(lib); add(caption); add(reserve);
        add(markerBtn); add(markerCard); add(scrubThumb); add(spelunkrBanner); add(spelunkrGuide);
        for (int i = 0; i < toasts; ++i)
            add(toast[i]);
        if (last < 0) {
            first = last = -1;
            return;
        }
        if (L.evenLines) {
            first &= ~1;
            last |= 1;
        }
        first = std::max(first, 0);
        last = std::min(last, int(L.H) - 1);
    }
};

inline int evenUp(int v, bool even) { return even ? (v + 1) & ~1 : v; }

// Two outlined lines above the playbar, inside safe ∩ picture. Even y/h at 480i.
// Center the cue in the band. A left-side scaler Info OSD (~22% of the grab)
// covers left-aligned text; the band itself stays the full safe ∩ picture width.
inline int captionTextX(const Rect& caption, int textW, int pad) {
    const int room = caption.w - 2 * pad;
    if (room <= 0)
        return caption.x + pad;
    if (textW >= room)
        return caption.x + pad;
    return caption.x + pad + (room - textW) / 2;
}

inline int captionPad(const Layout& L) { return L.evenLines ? 4 : (L.H <= 240 ? 2 : 6); }

inline int captionOutline(const Layout& L) { return L.evenLines ? 2 : 1; }

// Two outlined lines. 240p bumps the 5×7 so it stays readable. Even height at 480i.
inline int captionBlockHeight(const Layout& L) {
    Font f = Font::forLayout(L);
    if (L.H <= 240 && !L.evenLines)
        f.sy = std::max(2, f.sy);
    return evenUp(2 * f.height() + captionPad(L) + 2 * captionOutline(L), L.evenLines);
}

// Today's glass: bottom of (picture ∩ safe), just above the transport bar.
inline Rect captionRectOverPicture(const Layout& L) {
    const int pad = captionPad(L);
    const int h = captionBlockHeight(L);
    Rect area = intersect(L.pic, L.safe);
    if (area.empty())
        area = L.safe;
    int y = L.bar.y - pad - h;
    if (y < area.y)
        y = area.y;
    if (L.evenLines)
        y &= ~1;
    if (y + h > area.bottom()) {
        y = area.bottom() - h;
        if (L.evenLines)
            y &= ~1;
        if (y < area.y)
            y = area.y;
    }
    int hh = h;
    if (y + hh > area.bottom())
        hh = evenUp(std::max(0, area.bottom() - y), L.evenLines);
    return {area.x, y, area.w, hh};
}

// Bottom black bar: rows [pic.bottom(), H). Empty unless letterbox left a real gap.
inline bool letterboxBottomBar(const Layout& L, int& top, int& bot) {
    top = L.pic.bottom();
    bot = static_cast<int>(L.H);
    return L.letterbox && !L.pic.empty() && top > 0 && top < bot;
}

// Park the block in the bottom bar. Prefer the title-safe bottom edge when the
// block still fits under the picture; otherwise sit in the bar with a small
// inset, flush under the picture if the bar is short. Never above pic.bottom().
inline Rect captionRectInBottomBar(const Layout& L, int blockH) {
    int barTop = 0, barBot = 0;
    if (!letterboxBottomBar(L, barTop, barBot) || blockH <= 0)
        return {};
    const int barH = barBot - barTop;
    int hh = blockH;
    if (L.evenLines)
        hh &= ~1;
    if (hh > barH) {
        hh = barH;
        if (L.evenLines)
            hh &= ~1;
    }
    if (hh <= 0)
        return {};

    auto place = [&](int y) -> Rect {
        if (L.evenLines && (y & 1))
            ++y;
        if (y < barTop) {
            y = barTop;
            if (L.evenLines && (y & 1))
                ++y;
        }
        int h = hh;
        if (y >= barBot)
            return {};
        if (y + h > barBot) {
            h = barBot - y;
            if (L.evenLines)
                h &= ~1;
        }
        if (h <= 0 || y < barTop || y + h > barBot)
            return {};
        int x = L.safe.x;
        int w = L.safe.w;
        if (x < 0) {
            w += x;
            x = 0;
        }
        if (x + w > static_cast<int>(L.W))
            w = static_cast<int>(L.W) - x;
        if (w <= 0)
            return {};
        return {x, y, w, h};
    };

    const int safeBot = std::min(L.safe.bottom(), barBot);
    const Rect fitted = place(safeBot - hh);
    if (!fitted.empty() && fitted.y >= barTop && fitted.bottom() <= safeBot)
        return fitted;

    const int pad = captionPad(L);
    const int inset = L.evenLines ? ((pad + 1) & ~1) : pad;
    if (barH >= hh + inset)
        return place(barBot - inset - hh);
    return place(barTop);
}

// pos is explicit so tests can pin each mode. The one-arg form reads conf.
inline Rect captionRectFor(const Layout& L, misterplex::SubtitlePosition pos) {
#ifdef MUT_SUBPOS_SAFE_OVER_PICTURE
    pos = misterplex::SubtitlePosition::Picture;
#endif
#ifdef MUT_SUBPOS_PICTURE_IN_BAR
    if (pos == misterplex::SubtitlePosition::Picture)
        pos = misterplex::SubtitlePosition::Safe;
#endif
    bool lb = L.letterbox;
#ifdef MUT_SUBPOS_NOLB_IN_BAR
    lb = true;
#endif
    if (pos == misterplex::SubtitlePosition::Safe && lb) {
        Layout probe = L;
        probe.letterbox = true;
        int top = 0, bot = 0;
        if (letterboxBottomBar(probe, top, bot))
            return captionRectInBottomBar(probe, captionBlockHeight(L));
    }
    return captionRectOverPicture(L);
}

inline Rect captionRectFor(const Layout& L) {
    return captionRectFor(L, misterplex::subtitlePosition());
}

inline std::string stateWord(const UiState& s) {
    if (!s.transport.empty())
        return upper(s.transport);
    switch (s.bar) {
    case Bar::Paused: return "PAUSED";
    case Bar::Buffering:
        if (!s.statusText.empty())
            return upper(s.statusText);
        return s.bufferingPct >= 0 ? "BUFFERING... " + std::to_string(std::min(100, s.bufferingPct)) + "%"
                                   : "BUFFERING...";
    case Bar::Seeking: return "SEEKING " + clockText(s.seekTargetMs);
    case Bar::Playing: return "PLAYING";
    default: return "";
    }
}

inline const char* errorHeadline(ErrorScreen e) {
    switch (e) {
    case ErrorScreen::Refused: return "THIS STREAM CAN'T PLAY DIRECTLY";
    case ErrorScreen::CoreNotLoaded: return "MISTERPLEX CORE NOT LOADED";
    case ErrorScreen::PmsUnreachable: return "PLEX SERVER UNREACHABLE";
    case ErrorScreen::StreamError: return "PLAYBACK ERROR";
    case ErrorScreen::DaemonDown: return "MISTERPLEX DAEMON NOT RUNNING";   // "MISTERPLEX " + upper(kIdleDaemonDownText), pinned by test_daemon_down_card
    default: return "";
    }
}

inline std::string idleHintText(const UiState& s) {
    if (!s.idleHint.empty())
        return s.idleHint;
    return s.spelunkr ? "Open the Guide" : "Ready to cast";  // GH#411: never Ready-to-cast in TV mode
}

// A leftover browse-key row ("Press X, F1, Start or Program", or the pre-fix wording).
// The idle card does not paint it.
inline bool idleBrowseHintLine(const std::string& line) {
    if (line.compare(0, 6, "Press ") != 0)
        return false;
    return line.find("Start") != std::string::npos || line.find("Program") != std::string::npos ||
           line.find("F1") != std::string::npos;
}

// Ready-card hint may be two rows (cast types, then plex.tv/link). width()/fit() count
// every byte, so a '\n' must not stay in the string used for sizing.
inline std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    std::size_t i = 0;
    for (;;) {
        const std::size_t n = t.find('\n', i);
        if (n == std::string::npos) {
            out.push_back(t.substr(i));
            break;
        }
        out.push_back(t.substr(i, n - i));
        i = n + 1;
    }
    return out;
}

// Title is painted separately ("MISTERPLEX" + the small card chevron).
// status: the ready row, then "plex.tv/link CODE" when that row is present.
// footer: the box name only. Browse-key rows are dropped, never the footer.
struct IdleCopy {
    std::vector<std::string> status;
    std::string footer;
};

inline IdleCopy idleCardCopy(const UiState& s) {
    IdleCopy c;
    for (const std::string& line : splitLines(idleHintText(s))) {
        if (line.empty() || idleBrowseHintLine(line))
            continue;
        c.status.push_back(line);
    }
    if (c.status.empty())
        c.status.push_back(s.spelunkr ? "Open the Guide" : "Ready to cast");
    c.footer = s.idleName;
    return c;
}

// Status rows, then the quiet footer when there is one. Callers that only need the
// raw hint (including its newline) use idleHintText / splitLines.
inline std::vector<std::string> idleHintLines(const UiState& s) {
    const IdleCopy c = idleCardCopy(s);
    std::vector<std::string> rows = c.status;
    if (!c.footer.empty())
        rows.push_back(c.footer);
    return rows;
}

// Word-wrap one overlay line so it stays uncut at the idle card's minimum
// column width (sx256 384). Breaks on space; hard-splits a single long token.
inline std::vector<std::string> wrapLineToWidth(const std::string& line, const Font& f, int maxPx) {
    if (maxPx <= 0 || line.empty() || f.width(upper(line)) <= maxPx)
        return {line};
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        while (!cur.empty() && cur.front() == ' ')
            cur.erase(cur.begin());
        if (!cur.empty())
            out.push_back(cur);
        cur.clear();
    };
    std::size_t i = 0;
    while (i < line.size()) {
        std::size_t n = line.find(' ', i);
        if (n == std::string::npos)
            n = line.size();
        const std::string word = line.substr(i, n - i);
        const std::string next = cur.empty() ? word : cur + " " + word;
        if (!word.empty() && f.width(upper(next)) <= maxPx) {
            cur = next;
        } else {
            flush();
            if (f.width(upper(word)) <= maxPx) {
                cur = word;
            } else {
                std::string tok = word;
                while (!tok.empty()) {
                    std::size_t take = tok.size();
                    while (take > 1 && f.width(upper(tok.substr(0, take))) > maxPx)
                        --take;
                    out.push_back(tok.substr(0, take));
                    tok = tok.substr(take);
                }
            }
        }
        i = n == line.size() ? n : n + 1;
    }
    flush();
    return out.empty() ? std::vector<std::string>{line} : out;
}

inline void placeMarker(Frame& f, const UiState& s, const Layout& L);

// Low-res preview above the bar (and above a caption band). Even box on 480i.
// Centered on the seek target, clamped into title-safe.
inline Rect scrubThumbBox(const Layout& L, const Rect& bar, int64_t seekMs, int64_t durationMs, int anchorY) {
    int tw = (L.evenLines || L.H <= 480) ? 96 : 160;
    int th = (L.evenLines || L.H <= 480) ? 54 : 90;
    if (L.evenLines) {
        tw &= ~1;
        th &= ~1;
    }
    if (tw < 8 || th < 8 || bar.empty() || anchorY <= 0)
        return {};
    const Rect area = L.safe.empty() ? L.canvas : L.safe;
    int cx = bar.x + bar.w / 2;
    if (durationMs > 0 && seekMs >= 0) {
        const int64_t c = std::clamp(seekMs, int64_t{0}, durationMs);
        cx = bar.x + static_cast<int>(c * bar.w / durationMs);
    }
    const int gap = L.evenLines ? 2 : 0;
    int x = cx - tw / 2;
    int y = anchorY - gap - th;
    if (x < area.x)
        x = area.x;
    if (x + tw > area.right())
        x = std::max(area.x, area.right() - tw);
    if (L.evenLines)
        y &= ~1;
    if (y < area.y)
        y = L.evenLines ? (area.y + (area.y & 1)) : area.y;
    if (y + th > anchorY && anchorY - y > th)
        y = anchorY - gap - th;
    if (L.evenLines)
        y &= ~1;
    if (x < 0 || y < 0 || tw <= 0 || th <= 0)
        return {};
    if (x + tw > static_cast<int>(L.W) || y + th > static_cast<int>(L.H))
        return {};
    return {x, y, tw, th};
}

inline Frame planFrame(const UiState& s, const Layout& L) {
    Frame f;
    f.font = Font::forLayout(L);
    if (uiSafePrefs().size == UiSize::Large) {
        f.font.sy = std::min(f.font.sy + 1, 6);
        f.font.sx256 = f.font.sx256 * 5 / 4;
    }
    f.pad = L.evenLines ? 4 : 6;
    // E9: the calibration field covers the raster so the tube's edges are visible.
    if (s.overscan.open) {
        f.idleField = true;
        return f;
    }
    const int fh = f.font.height(), p = f.pad;
    if (s.error != ErrorScreen::None) {   // full canvas, no video (§3)
        f.screen = L.canvas;
        return f;
    }
    // PLAY-PENDING: full-field chevron, no status card, no BUFFERING bar. IDLE (pending
    // false) still falls through and lays out the B2 card. buffering alone is not this path.
    if (s.pending || (s.spelunkr && s.idle && !s.menu.open && !s.library.open)) {
        f.bar = {};
#ifndef MUT_PENDING_KEEPS_CARD
        f.card = {};
#endif
        if (s.spelunkrGuide.open) {
            f.idleField = true;
            f.spelunkrGuide = L.safe;
        } else if (s.spelunkrBanner.on) {
            const int bfh = spelunkrBannerFont(f.font, L.evenLines).height();
            f.spelunkrBanner = spelunkrBannerRect(L, p, bfh);
            // GH#410: after first picture, static is off. Banner-only; no landing field.
            f.idleField = s.idle || s.spelunkrStatic;
        } else {
            f.idleField = true;
        }
#ifndef MUT_PENDING_KEEPS_CARD
        return f;
#else
        // mutant: the status card stays up during PLAY-PENDING
#endif
    }
    if (s.bar != Bar::Hidden)
        f.bar = L.bar;
    if (s.captionArmed || !s.caption1.empty() || !s.caption2.empty()) {
        f.caption = captionRectFor(L);
        // Keep the bar band reserved so a 4 s playbar fade does not replan fb0.
        f.reserve = L.bar;
        if (s.bar == Bar::Hidden)
            f.bar = {};
    }
    if (s.library.open && !s.menu.open) {  // 0010: the page takes title-safe (the idle card waits; the panel
        f.lib = L.safe;                    // hides it: two translucent panels overlapping are unreadable)
        if (s.idle)
            f.idleField = true;            // cover the core's SMPTE bars before the page is drawn
    }
#ifdef MUT_OSD_CARD_UNDER_PAGE
    if (s.idle) {   // mutant: the idle card drawn under the page and the panel (their alpha shows it through)
#else
    if (s.idle && !s.library.open && !s.menu.open) {   // 0010: not under the panel or the page
#endif
        // T7: the idle card, bottom centre of title-safe. The small chevron sits in the card.
        const int chev = f.font.colX(12);
        // bug 5 (V2-BUGS-1017 #5/5a): at 480i / 576i the glyphs are a third larger (sy 4, even lines) and the
        // card filled all of title-safe, so "MISTERPLEX-203.0.113.40" ran into a CRT's overscan (Auto =
        // Interlaced on an analog TV). The card now keeps to 70 % of the canvas there, and its text is narrowed
        // (columns only, never the rows: strokes stay whole line pairs) until the longest line fits.
        int maxW = L.safe.w;
#ifndef MUT_OSD_CARD_NO_SHRINK   // mutant: the pre-fix card (all of title-safe, the text cut with "...")
        if (L.mode == OutMode::Interlaced)
            maxW = std::min(maxW, int(L.W) * 7 / 10);
#endif
        Font wrapFont = f.font;
        wrapFont.sx256 = 384;   // wrap at the shrink floor so fit() never paints "..."
        const int wrapRoom = maxW - chev - 4 * p;
        const IdleCopy copy = idleCardCopy(s);
        auto wrapAll = [&](const std::vector<std::string>& in) {
            std::vector<std::string> out;
            for (const std::string& raw : in) {
#ifdef MUT_BH_NO_WRAP
                out.push_back(raw);   // mutant: no wrap, fit() cuts the long row with "..."
#else
                for (const std::string& wline : wrapLineToWidth(raw, wrapFont, wrapRoom))
                    out.push_back(wline);
#endif
            }
            return out;
        };
        f.idleHintRows = wrapAll(copy.status);
        f.idleFooterRows = copy.footer.empty() ? std::vector<std::string>{} : wrapAll({copy.footer});
        std::string longest = "MISTERPLEX";
        auto consider = [&](const std::string& t) {
            const std::string u = upper(t);
            if (u.size() > longest.size())
                longest = u;
        };
        for (const std::string& t : f.idleHintRows)
            consider(t);
        for (const std::string& t : f.idleFooterRows)
            consider(t);
        if (!s.idleName.empty())
            consider(s.idleName);
        f.cardFont = f.font;
#ifndef MUT_OSD_CARD_NO_SHRINK
        const int room = maxW - chev - 4 * p;   // the text's width inside the card (tw below, less one pad)
        if (room > 0 && f.cardFont.width(longest) > room) {
            const int cols = int(longest.size()) * 6 - 1;
            f.cardFont.sx256 = std::max(384, (room * 256 - 128) / std::max(1, cols));   // >= 1.5 px a column
            while (f.cardFont.sx256 > 384 && f.cardFont.width(longest) > room)
                --f.cardFont.sx256;
        }
#endif
        int w = f.cardFont.width("MISTERPLEX");
        for (const std::string& t : f.idleHintRows)
            w = std::max(w, f.cardFont.width(upper(t)));
        for (const std::string& t : f.idleFooterRows)
            w = std::max(w, f.cardFont.width(upper(t)));
        if (!s.idleName.empty())
            w = std::max(w, f.cardFont.width(upper(s.idleName)));
        w = std::min(maxW, w + chev + 4 * p);
        // Title + status + footer. The two-row chevron needs three rows so its even strokes
        // stay inside the card; extra wrapped rows grow it. Fewer rows do not add a fourth.
        const int textRows = 1 + int(f.idleHintRows.size()) + int(f.idleFooterRows.size());
        const int rows = std::max(3, textRows);
#ifdef MUT_BH_CARD_NO_GROW
        const int rowsUsed = 3;   // mutant: the card keeps its 3-row height, rows paint below it
#else
        const int rowsUsed = rows;
#endif
        const int h = evenUp(rowsUsed * (fh + p) + 3 * p, L.evenLines);
#ifdef MUT_F5_CARD_CENTRED
        int top = L.safe.y + (L.safe.h - h) / 2;   // mutant: centred, over the chevron
#else
        // FIELD-1018 F5: bottom centre of title-safe. Title-safe already keeps the overscan /
        // interlace margins (bug 5); one pad above its bottom edge. The field behind the card is black.
        int top = L.safe.y + L.safe.h - h - p;
#endif
        if (L.evenLines)
            top &= ~1;
        f.idleField = true;
        f.card = {L.safe.x + (L.safe.w - w) / 2, top, w, h};
    }
    const int boxH = evenUp(fh + 2 * p, L.evenLines);
    int y = L.safe.y;   // toasts: top-right, newest last, stacked down
    for (size_t i = s.toasts.size() > 2 ? s.toasts.size() - 2 : 0; i < s.toasts.size(); ++i) {
        const int w = std::min(L.safe.w, f.font.width(upper(s.toasts[i])) + 2 * p);
        f.toast[f.toasts++] = {L.safe.right() - w, y, w, boxH};
        y += boxH + p;
    }
    if (!s.chip.empty()) {   // the chip: top-left, or under the toasts when they would overlap it
        const int w = std::min(L.safe.w, f.font.width(upper("Casting from " + s.chip)) + 2 * p);
        f.chip = {L.safe.x, L.safe.y, w, boxH};
        for (int i = 0; i < f.toasts; ++i)
            if (!intersect(f.chip, {f.toast[i].x - p, f.toast[i].y, f.toast[i].w + p, f.toast[i].h}).empty()) {
                f.chip.y = evenUp(y, L.evenLines);
                break;
            }
    }
    if (s.menu.open) {
        int w = f.font.width(upper(s.menu.title));
        for (const auto& it : s.menu.items)
            w = std::max(w, f.font.width(upper(it)) + f.font.colX(12));
        w = std::min(L.safe.w, w + 4 * p);
        const int rowH = fh + p;
        // 0010: above the bar when it shows (the panel over a play keeps the bar readable)
        const int areaH = f.bar.empty() ? L.safe.h : std::max(rowH * 3, f.bar.y - p - L.safe.y);
        const int room = std::max(1, (areaH - 2 * p) / rowH - 1);
        const int rows = 1 + std::min<int>(room, int(s.menu.items.size()));
        const int h = rows * rowH + 2 * p;
        int top = L.safe.y + std::max(0, (areaH - h) / 2);
        if (L.evenLines)
            top &= ~1;
        f.menu = {L.safe.x + (L.safe.w - w) / 2, top, w, h};
    }
    placeMarker(f, s, L);
    if (s.spelunkrGuide.open) {
        f.spelunkrGuide = L.safe;
        f.idleField = true;
    } else if (s.spelunkrBanner.on) {
        const int fh = spelunkrBannerFont(f.font, L.evenLines).height();
        f.spelunkrBanner = spelunkrBannerRect(L, f.pad, fh);
    }
    if (s.bar == Bar::Seeking && !f.bar.empty()) {
        const int anchor = f.caption.empty() ? f.bar.y : std::min(f.bar.y, f.caption.y);
#ifdef MUT_SCRUB_THUMBS_BOX_WITHOUT_FRAME
        f.scrubThumb = scrubThumbBox(L, f.bar, s.seekTargetMs, s.durationMs, anchor);
#else
        if (s.scrubThumb && s.scrubThumb->visible())
            f.scrubThumb = scrubThumbBox(L, f.bar, s.seekTargetMs, s.durationMs, anchor);
#endif
    }
    return f;
}

// Skip Intro button (right) or Up Next card (left), bottom-anchored above the bar,
// or in the bar's slot when the bar is hidden. Soft subs: stack above the caption band.
inline void placeMarker(Frame& f, const UiState& s, const Layout& L) {
    if (!s.marker.visible() || s.idle || s.pending || s.menu.open || s.library.open || s.surface)
        return;
    const bool hd = L.mode == OutMode::NativeHd && L.H > 240;
    const bool even = L.evenLines || L.H <= 240;
    Font mf;
    mf.sy = hd ? 3 : 2;
    if (mf.sy < 2)
        mf.sy = 2;
    const double par = L.par > 0.1 ? L.par : 1.0;
    mf.sx256 = std::max(256, int(double(mf.sy) / par * 256.0 + 0.5));
    f.markerFont = mf;
    const int pad = hd ? 6 : 4;
    const int fh = mf.height();
    int anchor = f.bar.empty() ? L.bar.bottom() : f.bar.y;
    if (!f.caption.empty())
        anchor = std::min(anchor, f.caption.y);
    auto fit = [&](Rect r) {
        if (r.h > L.safe.h)
            r.h = L.safe.h;
        if (r.w > L.safe.w)
            r.w = L.safe.w;
        if (r.x < L.safe.x)
            r.x = L.safe.x;
        if (r.right() > L.safe.right())
            r.x = std::max(L.safe.x, L.safe.right() - r.w);
        if (r.bottom() > anchor && r.h < anchor - L.safe.y)
            r.y = anchor - r.h;
        if (r.y < L.safe.y)
            r.y = L.safe.y;
        if (r.bottom() > L.safe.bottom())
            r.h = std::max(0, L.safe.bottom() - r.y);
        if (even && r.h > 0) {
            if (r.y & 1)
                --r.y;
            if (r.y < L.safe.y)
                r.y = L.safe.y + (L.safe.y & 1);
            if (r.h & 1)
                ++r.h;
            if (r.bottom() > L.safe.bottom() && r.h > 2)
                r.h -= 2;
        }
        return r;
    };
    if (s.marker.card) {
        const int capH = hd ? 96 : 72;
        int h = std::min(capH, 4 * fh + 2 * pad);
        int w = std::min(L.safe.w, hd ? 520 : 360);
        f.markerCard = fit({L.safe.x, anchor - h, w, h});
        return;
    }
    const int capH = hd ? 44 : 32;
    const std::string label = upper(s.marker.label.empty() ? "SKIP INTRO" : s.marker.label);
    int w = std::min(L.safe.w, mf.width(label) + 2 * pad + mf.colX(2));
    int h = std::min(capH, fh + 2 * pad);
    f.markerBtn = fit({L.safe.right() - w, anchor - h, w, h});
}


inline void drawOutlinedText(Surface& S, const Font& F, int x, int y, const std::string& t, bool even) {
    const int o = even ? 2 : 1;
    const Rgba ink{0, 0, 0, 0xFF};
    for (int dy = -o; dy <= o; dy += o ? o : 1) {
        for (int dx = -o; dx <= o; dx += o ? o : 1) {
            if (dx == 0 && dy == 0)
                continue;
            F.draw(S, x + dx, y + dy, t, ink);
        }
    }
    F.draw(S, x, y, t, kFg);
}

inline void paintCaption(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    if (f.caption.empty())
        return;
    const Font& F = f.font;
    const int pad = f.pad;
    const int outline = L.evenLines ? 2 : 1;
    int y = evenUp(f.caption.y + outline, L.evenLines);
    const int maxW = std::max(0, f.caption.w - 2 * pad);
    auto paintLine = [&](const std::string& raw) {
        const std::string t = F.fit(upper(raw), maxW);
        drawOutlinedText(S, F, captionTextX(f.caption, F.width(t), pad), y, t, L.evenLines);
    };
    if (!s.caption1.empty()) {
        paintLine(s.caption1);
        y = evenUp(y + F.height() + (L.evenLines ? 2 : 1), L.evenLines);
    }
    if (!s.caption2.empty())
        paintLine(s.caption2);
    // FPGA overlay band only. HDMI scaler "Unknown 2200x513p" is not consulted.
    publishCaptionDump(captionDebugLine(s.captionStreamId, s.captionCue, s.captionMediaMs, s.captionArmed,
                                        s.caption1, s.caption2, f.caption.x, f.caption.y, f.caption.w,
                                        f.caption.h, static_cast<int>(L.W), static_cast<int>(L.H)));
}

inline void paintScrubThumb(Surface& S, const UiState& s, const Frame& f) {
    if (f.scrubThumb.empty())
        return;
    const Rect box = f.scrubThumb;
    const int b = 2;
    S.fill(box, kFg);
    const Rect inner{box.x + b, box.y + b, box.w - 2 * b, box.h - 2 * b};
    if (inner.w <= 0 || inner.h <= 0)
        return;
    if (!s.scrubThumb || !s.scrubThumb->visible()) {
#ifdef MUT_SCRUB_THUMBS_BOX_WITHOUT_FRAME
        S.fill(inner, kAccent);
#endif
        return;
    }
    const int sw = s.scrubThumb->w;
    const int sh = s.scrubThumb->h;
    const uint8_t* src = s.scrubThumb->bgra.data();
    const Rect clip = intersect(inner, {0, S.y0, int(S.W), S.h});
    for (int y = clip.y; y < clip.bottom(); ++y) {
        const int sy = std::min(sh - 1, (y - inner.y) * sh / std::max(1, inner.h));
        for (int x = clip.x; x < clip.right(); ++x) {
            const int sx = std::min(sw - 1, (x - inner.x) * sw / std::max(1, inner.w));
            const uint8_t* p = src + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) +
                                      static_cast<std::size_t>(sx)) *
                                         4u;
            Surface::over(S.at(x, y), Rgba{p[2], p[1], p[0], p[3]});
        }
    }
}

// In-play bar only. Menus, the idle card, and captions keep Font::forLayout.
// Columns are 4/5 of the menu advance (~20% narrower), which is what lets a
// title show more than about six characters on a 15 kHz raster. Rows are 4/5
// too, then snapped to an even stroke when the output is interlaced: an odd
// sy twitters under the fabric's (1,2,1), and 80% of sy 4 is 3, so 480i / 576i
// drop to sy 2. That shorter row is what leaves room for the clock above the
// CRT overscan inside the existing 80 px bar.
#ifdef MUT_CRT_INSET_0
constexpr int kCrtInterlacedTimeInset = 0;
#else
constexpr int kCrtInterlacedTimeInset = 24; // frame lines above safe.bottom()
#endif

// Playing / Paused with no transport feedback: a mark, not the word.
// BUFFERING, SEEKING, and transport text still use stateWord().
inline bool inPlayStateIcon(const UiState& s) {
    return s.transport.empty() && (s.bar == Bar::Playing || s.bar == Bar::Paused);
}

struct InPlayBarGeom {
    Font font;
    int pad = 4;
    int trackH = 8;
    int limit = 0; // exclusive bottom for the clock
    int titleY = 0;
    int trackY = 0;
    int timeY = 0;
    int x0 = 0;
    int x1 = 0;
    bool icon = false;
    int iconX = 0, iconY = 0, iconW = 0, iconH = 0;
    int titleRoom = 0;
    std::string word;
    int wordW = 0;
};

inline int inPlayContentH(const Font& font, int pad, int trackH) {
    return 2 * font.height() + trackH + 2 * pad;
}

inline int inPlayContentLimit(const Layout& L, const Rect& bar) {
    int limit = std::min(bar.bottom(), L.safe.bottom());
    if (!L.evenLines)
        return limit;
    // 15 kHz interlaced: a CRT that overscans past the 10% title-safe margin
    // clips the clock. Keep the last clock pixel at least this many frame
    // lines above safe.bottom(). A letterbox whose picture already ends above
    // that line is not lifted again.
    const int crt = (L.safe.bottom() - kCrtInterlacedTimeInset) & ~1;
    if (crt < limit)
        limit = std::max(bar.y, crt);
    return limit;
}

inline void inPlayIconSize(int textH, bool evenLines, bool paused, int& w, int& h) {
    int markH = std::max(8, textH * 3 / 5);
    if (evenLines && (markH & 1))
        ++markH;
    if (markH > textH)
        markH = evenLines ? (textH & ~1) : textH;
    if (markH < 2)
        markH = 2;
    if (evenLines && (markH & 1))
        markH = std::max(2, markH - 1);
    h = markH;
    if (paused)
        w = 3 * std::max(2, markH / 4); // bar, gap, bar
    else
        w = std::max(4, markH * 2 / 3);
}

inline InPlayBarGeom layoutInPlayBar(const UiState& s, const Layout& L, const Frame& f) {
    InPlayBarGeom g;
    const Rect bar = f.bar.empty() ? L.bar : f.bar;
    g.font = inPlayFont(f.font, L.evenLines);
    g.pad = f.pad > 0 ? f.pad : (L.evenLines ? 4 : 6);
    g.trackH = 8;
    g.limit = inPlayContentLimit(L, bar);
    const int roomH = std::max(0, g.limit - bar.y);
    auto contentFits = [&]() { return inPlayContentH(g.font, g.pad, g.trackH) <= roomH; };
    while (!contentFits() && g.pad > 2) {
        const int step = L.evenLines ? 2 : 1;
        g.pad = std::max(2, g.pad - step);
    }
    while (!contentFits() && g.font.sy > 2) {
        const int step = L.evenLines ? 2 : 1;
        g.font.sy = std::max(2, g.font.sy - step);
    }
    const int content = inPlayContentH(g.font, g.pad, g.trackH);
    int y = bar.y;
    if (roomH > content)
        y = bar.y + (roomH - content) / 2;
    y = evenUp(y, L.evenLines);
    if (y + content > g.limit) {
        y = g.limit - content;
        if (L.evenLines)
            y &= ~1;
        if (y < bar.y)
            y = evenUp(bar.y, L.evenLines);
    }
    g.titleY = y;
    g.trackY = g.titleY + g.font.height() + g.pad;
    g.timeY = g.trackY + g.trackH + g.pad;
    if (g.timeY + g.font.height() > g.limit) {
        int drop = g.timeY + g.font.height() - g.limit;
        if (L.evenLines && (drop & 1))
            ++drop;
        g.timeY -= drop;
        if (L.evenLines)
            g.timeY &= ~1;
        if (g.timeY < g.titleY)
            g.timeY = g.titleY;
    }
    g.x0 = bar.x + 2 * g.pad;
    g.x1 = bar.right() - 2 * g.pad;
    if (g.x1 < g.x0)
        g.x1 = g.x0;
    g.icon = inPlayStateIcon(s);
    const int fh = g.font.height();
    if (g.icon) {
        inPlayIconSize(fh, L.evenLines, s.bar == Bar::Paused, g.iconW, g.iconH);
        g.iconX = g.x1 - g.iconW;
        int iy = g.titleY + std::max(0, (fh - g.iconH) / 2);
        iy = evenUp(iy, L.evenLines);
        if (iy + g.iconH > g.titleY + fh) {
            iy = g.titleY + fh - g.iconH;
            if (L.evenLines)
                iy &= ~1;
        }
        if (iy < g.titleY)
            iy = evenUp(g.titleY, L.evenLines);
        g.iconY = iy;
        g.titleRoom = std::max(0, g.iconX - g.x0 - g.font.colX(6));
    } else {
        g.word = stateWord(s);
        g.wordW = g.font.width(g.word);
        g.titleRoom = std::max(0, g.x1 - g.x0 - g.wordW - g.font.colX(12));
    }
    return g;
}

// || : two even-height bars. snap is off; the caller already even-aligns y/h
// so a snap cannot grow the mark onto the next line pair.
inline void drawPauseMark(Surface& S, int x, int y, int w, int h) {
    if (w < 4 || h <= 0)
        return;
    const int t = std::max(2, w / 3);
    S.fill({x, y, t, h}, kFg, false);
    S.fill({x + w - t, y, t, h}, kFg, false);
}

// > : a right-pointing triangle in even-height spans (both fields of a 480i pair).
inline void drawPlayMark(Surface& S, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    const int step = (S.evenLines || (h % 2) == 0) ? 2 : 1;
    const int half = std::max(1, h / 2);
    for (int row = 0; row < h; row += step) {
        const int band = std::min(step, h - row);
        const int mid = std::min(h - 1, row + (band > 1 ? 1 : 0));
        const int dist = mid < half ? mid + 1 : h - mid;
        int ww = dist * w / half;
        if (ww < 2)
            ww = std::min(w, 2);
        if (ww > w)
            ww = w;
        S.fill({x, y + row, ww, band}, kFg, false);
    }
}

inline void paintBar(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Rect b = f.bar;
    S.fill(b, kPanel);
    const InPlayBarGeom g = layoutInPlayBar(s, L, f);
    const Font& F = g.font;
    const int x0 = g.x0, x1 = g.x1;
    // line 1: the title, and a small play/pause mark or the state word
    // (BUFFERING / SEEKING / transport). Transport still replaces the mark.
    if (g.icon) {
        if (s.bar == Bar::Paused)
            drawPauseMark(S, g.iconX, g.iconY, g.iconW, g.iconH);
        else
            drawPlayMark(S, g.iconX, g.iconY, g.iconW, g.iconH);
    } else if (g.wordW) {
        const Rgba wc = s.transport.empty() ? kFg : kAccent;
        F.draw(S, x1 - g.wordW, g.titleY, g.word, wc);
    }
    const std::string title = upper(s.title.empty() ? "MiSTerPlex" : s.title);
    F.draw(S, x0, g.titleY, F.fit(title, g.titleRoom), kFg);
    // the track: the buffered fill, the played fill, the knob, the seek's ghost knob
    const int ty = g.trackY;
    const int trackH = g.trackH;
    const Rect track{x0, ty, std::max(0, x1 - x0), trackH};
    S.fill(track, kTrack);
    auto px = [&](int64_t ms) {
        if (s.durationMs <= 0)
            return 0;
        const int64_t c = std::clamp<int64_t>(ms, 0, s.durationMs);
        return int(c * track.w / s.durationMs);
    };
    if (s.bufferedMs >= 0)
        S.fill({track.x, ty, px(s.bufferedMs), trackH}, kBuffered);
    const int played = px(s.positionMs);
    S.fill({track.x, ty, played, trackH}, kAccent);
    const int knobW = std::max(4, F.colX(3));
    const Rect knob{track.x + played - knobW / 2, ty - 2, knobW, trackH + 4};
    if (s.durationMs > 0)
        S.fill(intersect(knob, b), kFg);
    if (s.seekTargetMs >= 0 && s.durationMs > 0)
        S.fill(intersect({track.x + px(s.seekTargetMs) - knobW / 2, ty - 2, knobW, trackH + 4}, b), kGhost);
    // line 2: elapsed left, duration right, above the CRT overscan on 15 kHz
    const int y2 = g.timeY;
    F.draw(S, x0, y2, clockText(s.positionMs), kFg);
    if (s.durationMs > 0) {
        const std::string d = clockText(s.durationMs);
        F.draw(S, x1 - F.width(d), y2, d, kDim);
    }
}

inline void paintMarker(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Font& F = f.markerFont.sy > 0 ? f.markerFont : f.font;
    const bool even = L.evenLines || L.H <= 240;
    const int pad = f.pad;
    if (!f.markerBtn.empty()) {
        S.fill(f.markerBtn, kPanel);
        const std::string t = F.fit(upper(s.marker.label), std::max(0, f.markerBtn.w - 2 * pad));
        F.draw(S, f.markerBtn.x + pad, evenUp(f.markerBtn.y + pad, even), t, kFg);
    }
    if (f.markerCard.empty())
        return;
    S.fill(f.markerCard, kPanel);
    const int maxW = std::max(0, f.markerCard.w - 2 * pad);
    const char* lines[4] = {s.marker.line1.c_str(), s.marker.line2.c_str(), s.marker.line3.c_str(),
                            s.marker.line4.c_str()};
    int y = evenUp(f.markerCard.y + pad, even);
    const int row = F.height();
    for (const char* raw : lines) {
        if (y + row > f.markerCard.bottom())
            break;
        if (raw && raw[0])
            F.draw(S, f.markerCard.x + pad, y, F.fit(upper(raw), maxW), kFg);
        y = evenUp(y + row, even);
    }
}

inline void paintBox(Surface &S, const Frame &f, const Rect &r, const std::string &text, Rgba c,
                     bool clipText = false) {
    if (r.empty())
        return;
    S.fill(r, kPanel);
    const int maxW = std::max(0, r.w - 2 * f.pad);
    const std::string t = clipText ? upper(text) : f.font.fit(upper(text), maxW);
    f.font.draw(S, r.x + f.pad, evenUp(r.y + f.pad, S.evenLines), t, c, clipText ? maxW : -1);
}

inline void paintMenu(Surface& S, const UiState& s, const Frame& f) {
    const Font& F = f.font;
    const int p = f.pad, rowH = F.height() + p;
    const Rect m = f.menu;
    S.fill(m, kPanel);
    int y = evenUp(m.y + p, S.evenLines);
    F.draw(S, m.x + 2 * p, y, F.fit(upper(s.menu.title), m.w - 4 * p), kAccent);
    const int rows = (m.h - 2 * p) / rowH - 1;
    const int n = int(s.menu.items.size());
    const int first = std::clamp(s.menu.selected - rows + 1, 0, std::max(0, n - rows));
    for (int i = first; i < n && i < first + rows; ++i) {
        y += rowH;
        const bool sel = i == s.menu.selected;
        if (sel)
            S.fill({m.x + p, y - p / 2, m.w - 2 * p, rowH}, kAccent);
        const bool dim = size_t(i) < s.menu.dim.size() && s.menu.dim[size_t(i)];
        const Rgba c = sel ? Rgba{kPanel.r, kPanel.g, kPanel.b, 0xFF} : dim ? kDim : kFg;
        F.draw(S, m.x + 2 * p, y, F.fit(upper(s.menu.items[size_t(i)]), m.w - 4 * p), c);
    }
}

inline void paintScreen(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Font& F = f.font;
    S.fill(f.screen, kErrorBg);
    std::vector<std::pair<std::string, Rgba>> lines;
    auto wrap = [&](const std::string& t, Rgba c) {   // at spaces, to the safe width
        std::string line, word;
        std::istringstream in(t);
        while (in >> word) {
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && F.width(next) > L.safe.w) {
                lines.push_back({line, c});
                line = word;
            } else {
                line = next;
            }
        }
        if (!line.empty())
            lines.push_back({line, c});
    };
    wrap(errorHeadline(s.error), kFg);
    if (!s.errorDetail.empty())
        wrap(upper(s.errorDetail), kDim);
    if (s.error == ErrorScreen::Refused)
        wrap("TRANSCODING INSTEAD", kAccent);
    const int rowH = F.height() + 2 * f.pad;
    int y = evenUp(int(L.H) / 2 - rowH * int(lines.size()) / 2, L.evenLines);
    for (const auto& [t, c] : lines) {
        const std::string fit = F.fit(t, L.safe.w);
        F.draw(S, L.safe.x + (L.safe.w - F.width(fit)) / 2, y, fit, c);
        y += rowH;
    }
}

// Source line under the big chevron. IDLE ready does not call this. An empty label
// leaves the chevron alone. MUT_PENDING_NO_LABEL keeps the chevron and drops the line.
inline void paintPendingCueLine(Surface& S, const UiState& s, const Layout& L) {
    if (!s.pending || s.pendingLabel.empty())
        return;
#ifdef MUT_PENDING_NO_LABEL
    (void)S;
    (void)L;
    return;
#else
    Font F = Font::forLayout(L);
    if (uiSafePrefs().size == UiSize::Large) {
        F.sy = std::min(F.sy + 1, 6);
        F.sx256 = F.sx256 * 5 / 4;
    }
    const std::string line = F.fit(upper(s.pendingLabel), std::max(8, int(L.W) - 16));
    if (line.empty())
        return;
    const misterplex::IdleRenderState st =
        misterplex::idleRenderState(int(L.W), int(L.H), misterplex::IdleMode::Logo, 0);
    const int gap = L.evenLines ? 8 : 12;
    const int fh = F.height();
    int y = evenUp(st.oy + st.size + gap, L.evenLines);
    const int limit = std::max(0, int(L.H) - fh);
    if (y > limit)
        y = evenUp(limit, L.evenLines);
    const int x = std::max(0, (int(L.W) - F.width(line)) / 2);
    F.draw(S, x, y, line, kAccent);
#endif
}

// Big chevron on black. IDLE ready does not call this (the field is solid black and the
// status card is on). tipA-play-pending-cue paints this with the status card off.
inline void paintChevronField(Surface& S, const Layout& L) {
    const misterplex::IdleRenderState st =
        misterplex::idleRenderState(int(L.W), int(L.H), misterplex::IdleMode::Logo, 0);
    for (int y = S.y0; y < S.y0 + S.h; ++y) {
        for (unsigned x = 0; x < S.W; ++x) {
            uint8_t r = 0, g = 0, b = 0;
#ifdef MUT_F3_IDLE_BG_NOT_BLACK
            misterplex::idlePixelRgb(int(x), y, st, r, g, b);   // mutant: the old grey field
#else
            // FIELD-1018 F3 (Shawn 11:09): solid black behind the status screen, the chevron blended over black;
            // never the last frame or the core's pattern
            const int cov = st.blank ? 0 : misterplex::idleChevronCover256(int(x), y, st.ox, st.oy, st.size);
            if (cov > 0) {
                const int c = cov >= 256 ? 256 : cov;
                r = uint8_t((misterplex::kIdleFgR * c + 128) >> 8);
                g = uint8_t((misterplex::kIdleFgG * c + 128) >> 8);
                b = uint8_t((misterplex::kIdleFgB * c + 128) >> 8);
            }
#endif
            uint8_t* d = S.at(int(x), y);
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 0xFF;
        }
    }
}

// T7: the idle card -- a small chevron (two strokes, whole line pairs at 480i/576i),
// the title, the ready row, an optional plex.tv/link row, and the box name.
inline void paintCard(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Font& F = f.font;
    const int p = f.pad, fh = F.height(), rowH = fh + p;
    const Rect c = f.card;
    // FIELD-1018 F3: on the idle field the panel is opaque (kPanel pre-blended over black), so nothing behind shows
#ifdef MUT_F3_CARD_SEE_THROUGH
    S.fill(c, kPanel);   // mutant: the old translucent card: the last frame / colorbars / stripes show through
#else
    S.fill(c, f.idleField ? kPanelOnBlack : kPanel);
#endif
    const int step = L.evenLines ? 2 : 1;
    const int ch = 2 * rowH;                        // the chevron's height
    const int cw = F.colX(6), thick = std::max(2, F.colX(2));
    const int cx = c.x + 2 * p, cy = evenUp(c.y + 2 * p + rowH / 2, L.evenLines);
    for (int y = 0; y < ch; y += step) {
        const int half = ch / 2;
        const int dx = (half - std::abs(y - half)) * cw / std::max(1, half);
        S.fill(intersect({cx + dx, cy + y, thick, step}, c), kAccent);
    }
    const Font& T = f.cardFont;   // bug 5: the text's (maybe narrowed) font; the chevron keeps F's columns
    const int tx = c.x + 2 * p + F.colX(12), tw = c.right() - p - tx;
    int y = evenUp(c.y + 2 * p, L.evenLines);
    T.draw(S, tx, y, T.fit("MISTERPLEX", tw), kFg);
    std::vector<std::string> hintRows = f.idleHintRows;
    std::vector<std::string> footerRows = f.idleFooterRows;
    if (hintRows.empty() && footerRows.empty()) {
        const IdleCopy copy = idleCardCopy(s);
        hintRows = copy.status;
        if (!copy.footer.empty())
            footerRows.push_back(copy.footer);
    }
    for (const std::string& line : hintRows) {
        y = evenUp(y + rowH, L.evenLines);
        T.draw(S, tx, y, T.fit(upper(line), tw), kAccent);
#ifdef MUT_BH_PAINT_ONE_ROW
        break;   // mutant: only the first status row is painted; the footer never shows
#endif
    }
#ifndef MUT_BH_PAINT_ONE_ROW
    for (const std::string& line : footerRows) {
        y = evenUp(y + rowH, L.evenLines);
        T.draw(S, tx, y, T.fit(upper(line), tw), kDim);
    }
#endif
}

// 0010: the library page: the breadcrumb, a status line, the rows around the cursor, "N / TOTAL".
inline int libraryRows(const Layout& L, const Frame& f) {   // rows of entries the page shows
    const int rowH = f.font.height() + f.pad;
    return std::max(1, (L.safe.h - 2 * f.pad) / rowH - 3);
}

inline void paintLibrary(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Font& F = f.font;
    const LibraryPage& lb = s.library;
    const int p = f.pad, rowH = F.height() + p;
    const Rect r = f.lib;
    S.fill(r, f.idleField ? kPanelOnBlack : kPanel);
    const int x = r.x + 2 * p, w = r.w - 4 * p;
    int y = evenUp(r.y + p, L.evenLines);
    F.draw(S, x, y, F.fit(upper(lb.header.empty() ? "Library" : lb.header), w), kAccent);
    y = evenUp(y + rowH, L.evenLines);
    if (!lb.status.empty())
        F.draw(S, x, y, F.fit(upper(lb.status), w), kDim);
    const int rows = libraryRows(L, f);
    const int n = int(lb.rows.size());
    const int first = std::clamp(lb.cursor - rows / 2, 0, std::max(0, n - rows));
    for (int i = first; i < n && i < first + rows; ++i) {
        y = evenUp(y + rowH, L.evenLines);
        const LibraryRow& row = lb.rows[size_t(i)];
        const bool sel = i == lb.cursor && row.selectable;
        if (sel)
            S.fill({r.x + p, y - p / 2, r.w - 2 * p, rowH}, kAccent);
        const Rgba c = sel ? Rgba{kPanel.r, kPanel.g, kPanel.b, 0xFF} : row.selectable ? kFg : kDim;
        F.draw(S, x, y, F.fit(upper(row.text + (row.dir ? " /" : "")), w), c);
    }
    const int yb = evenUp(r.bottom() - p - rowH, L.evenLines);
    int hintBudget = w;
    if (lb.total > 0 && lb.cursor >= 0) {
        const std::string pos = std::to_string(lb.offset + lb.cursor + 1) + " / " + std::to_string(lb.total);
        hintBudget = std::max(0, w - F.width(pos) - F.colX(2));
        F.draw(S, r.right() - 2 * p - F.width(pos), yb, pos, kDim);
    }
    const std::string hint = upper(std::string(misterplex::browseHintLine()));
    if (!hint.empty())
        F.draw(S, x, yb, F.fit(hint, hintBudget), kDim);
}

// The draft safe rect (the marks the couch is moving). Independent of the saved store.
inline Rect overscanViewRect(const Layout& L, const OverscanView& v) {
    OverscanEdges e;
    e.left = v.left;
    e.right = v.right;
    e.top = v.top;
    e.bottom = v.bottom;
    const OverscanPx p = overscanPxFor(static_cast<int>(L.W), static_cast<int>(L.H), e, L.evenLines, 0, 0, 0, 0);
    return {p.left, p.top, std::max(0, static_cast<int>(L.W) - p.left - p.right),
            std::max(0, static_cast<int>(L.H) - p.top - p.bottom)};
}

inline void paintOverscan(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Rect box = overscanViewRect(L, s.overscan);
    if (box.empty())
        return;
    const int thick = L.evenLines ? 4 : 2;
    const int focus = s.overscan.focus;
    auto side = [&](int edge, Rect r) {
        S.fill(r, focus == edge ? kAccent : kFg, false);
    };
    side(2, {box.x, box.y, box.w, thick});
    side(3, {box.x, std::max(box.y, box.bottom() - thick), box.w, thick});
    side(0, {box.x, box.y, thick, box.h});
    side(1, {box.right() - thick, box.y, thick, box.h});

    const Font& F = f.font;
    const int vals[4] = {s.overscan.left, s.overscan.right, s.overscan.top, s.overscan.bottom};
    const char* names[4] = {"LEFT", "RIGHT", "TOP", "BOTTOM"};
    const int fi = focus >= 0 && focus < 4 ? focus : 0;
    char edgeLine[32];
    std::snprintf(edgeLine, sizeof edgeLine, "%s %d%%", names[fi], vals[fi]);
    const std::string lines[5] = {"SAFE AREA", edgeLine, "LEFT RIGHT ADJUST", "UP DOWN EDGE", "A SAVE  B CANCEL"};
    const int maxW = std::max(0, box.w - 2 * thick - 8);
    int y = box.y + thick + (L.evenLines ? 4 : 6);
    for (const std::string& raw : lines) {
        if (y + F.height() >= box.bottom() - thick)
            break;
        const std::string t = F.fit(raw, maxW);
        const int x = box.x + std::max(thick, (box.w - F.width(t)) / 2);
        F.draw(S, x, y, t, raw == lines[1] ? kAccent : kFg);
        y += F.height() + (L.evenLines ? 4 : 3);
        if (L.evenLines)
            y &= ~1;
    }
}

// Library art, letterboxed into the idle field. The Ready card is drawn later.
inline void paintAmbientField(Surface& S, const UiState& s) {
    const misterplex::AmbientArt* art =
        s.ambient ? misterplex::ambientArtAt(*s.ambient, s.ambientSlot) : nullptr;
    const int fieldW = std::max(1, int(S.W));
    const int fieldH = std::max(1, S.y0 + S.h);
    for (int y = S.y0; y < S.y0 + S.h; ++y) {
        for (unsigned x = 0; x < S.W; ++x) {
            uint8_t b = 0, g = 0, r = 0, a = 0xFF;
            if (art)
                misterplex::ambientSampleBgra(*art, int(x), y, fieldW, fieldH, b, g, r, a);
            uint8_t* d = S.at(int(x), y);
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = a;
        }
    }
}

// Draw one frame into S, which is reset to the frame's band (rows [first, last]).
inline void paint(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    int first, last;
    f.band(L, first, last);
    S.reset(L.W, std::max(0, first), last < 0 ? 0 : last - first + 1, L.evenLines);
    if (last < 0)
        return;
    if (f.idleField) {
#ifdef MUT_F3_IDLE_BG_NOT_BLACK
        if (f.lib.empty())
            paintChevronField(S, L);   // mutant: the old grey field, not FIELD-1018 black
        else
            S.fill({0, S.y0, int(S.W), S.h}, Rgba{0, 0, 0, 0xFF}, false);
#else
        if (s.spelunkr && !s.spelunkrStatic && (s.idle || (s.pending && !s.spelunkrBanner.on))) {
            S.fill({0, S.y0, int(S.W), S.h}, Rgba{0, 0, 0, 255}, false);
            const Font& F = f.font;
            const int y = evenUp(L.safe.y + L.safe.h / 2 - F.height(), L.evenLines);
            for (const auto& row : {std::make_pair("SPELUNKR", kFg),
                                    std::make_pair("OPEN THE GUIDE", kAccent)}) {
                const std::string text = F.fit(row.first, L.safe.w);
                F.draw(S, L.safe.x + (L.safe.w - F.width(text)) / 2,
                       row.first == std::string("SPELUNKR") ? y : evenUp(y + F.height() + f.pad, L.evenLines),
                       text, row.second);
            }
        } else if (s.pending) {
            if (s.spelunkrStatic) {
                uint32_t noise = s.staticFrame + 1;
                // Two-line cells keep analog interlace stable; redraw at the existing 8 Hz limit.
                for (int y = S.y0; y < S.y0 + S.h; y += 2)
                    for (unsigned x = 0; x < S.W; x += 4) {
                        noise ^= noise << 13;
                        noise ^= noise >> 17;
                        noise ^= noise << 5;
                        const uint8_t gray = uint8_t(noise >> 24);
                        S.fill({int(x), y, 4, 2}, Rgba{gray, gray, gray, 255}, false);
                    }
            } else {
                paintChevronField(S, L);
                paintPendingCueLine(S, s, L);
            }
        } else if (ambientFieldWanted(s)) {
            // AMBIENT: library art on the idle field. The Ready card is painted after this.
            paintAmbientField(S, s);
        } else {
            // IDLE ready skips paintChevronField. Solid black (FIELD-1018 F3): no large watermark,
            // last frame, colour bars, or grey field. The small chevron stays in the card.
            S.fill({0, S.y0, int(S.W), S.h}, Rgba{0, 0, 0, 0xFF}, false);
        }
#endif
    }
    if (s.overscan.open) {
        paintOverscan(S, s, L, f);
        return;
    }
    if (!f.screen.empty()) {
        paintScreen(S, s, L, f);
        return;
    }
    if (!f.card.empty())
        paintCard(S, s, L, f);
    if (!f.lib.empty())
        paintLibrary(S, s, L, f);
    if (!f.caption.empty())
        paintCaption(S, s, L, f);
    if (!f.scrubThumb.empty())
        paintScrubThumb(S, s, f);
    if (!f.bar.empty())
        paintBar(S, s, L, f);
    paintMarker(S, s, L, f);
    paintBox(S, f, f.chip, "Casting from " + s.chip, kFg);
    for (int i = 0; i < f.toasts; ++i) {
        const size_t k = s.toasts.size() - size_t(f.toasts) + size_t(i);
        paintBox(S, f, f.toast[i], s.toasts[k], kFg);
    }
    if (!f.menu.empty())
        paintMenu(S, s, f);
    if (!f.spelunkrGuide.empty()) {
        Frame gf = f;
        gf.font = inPlayFont(Font::forLayout(L), L.evenLines);
        Rgba bg{0x1c, 0x0a, 0x57, 0xE0};
        S.fill(f.spelunkrGuide, bg, L.evenLines);
        const int fh = gf.font.height() + gf.pad;
        int y = f.spelunkrGuide.y + f.pad;
        paintBox(S, gf, {f.spelunkrGuide.x + f.pad, y, f.spelunkrGuide.w - 2 * f.pad, fh}, "SPELUNKR GUIDE", kAccent,
                 true);
        y += fh + f.pad;
        const int room = std::max(1, (f.spelunkrGuide.bottom() - y - f.pad) / fh);
        int start = 0;
        if (s.spelunkrGuide.cursor >= room)
            start = s.spelunkrGuide.cursor - room + 1;
        for (int i = 0; i < room && start + i < int(s.spelunkrGuide.rows.size()); ++i) {
            const int idx = start + i;
            const LibraryRow& row = s.spelunkrGuide.rows[static_cast<size_t>(idx)];
            const bool sel = idx == s.spelunkrGuide.cursor;
            Rgba fg = sel ? Rgba{0xff, 0xff, 0x00, 0xFF} : kFg;
            paintBox(S, gf, {f.spelunkrGuide.x + f.pad, y, f.spelunkrGuide.w - 2 * f.pad, fh}, row.text, fg, true);
            y += fh;
        }
    } else if (!f.spelunkrBanner.empty() && s.spelunkrBanner.on) {
        Rgba bg{0x1c, 0x0a, 0x57, s.spelunkrBanner.alpha};
        S.fill(f.spelunkrBanner, bg, L.evenLines);
        Frame bf = f;
        bf.font = spelunkrBannerFont(f.font, L.evenLines);
        const int fh = bf.font.height();
        std::string line1 = s.spelunkrBanner.dial;
        if (!s.spelunkrBanner.name.empty()) {
            if (!line1.empty())
                line1 += "  ";
            line1 += s.spelunkrBanner.name;
        }
        std::string line2 = s.spelunkrBanner.title;
        Rgba fg{0xff, 0xff, 0xff, s.spelunkrBanner.alpha};
        // GH#410: inner panel must fade with the bar. kPanel at 0xD0 never faded, so the
        // box looked stuck until the whole overlay dropped.
        Rgba inner = kPanel;
        inner.a = uint8_t(int(kPanel.a) * int(s.spelunkrBanner.alpha) / 255);
        int innerY = f.spelunkrBanner.y + f.pad / 2;
        if (L.evenLines)
            innerY &= ~1;  // GH#410/#412: pad/2 is odd (pad=6); odd fill twitters
        int innerH = 2 * fh;
        if (L.evenLines)
            innerH = (innerH + 1) & ~1;
        S.fill({f.spelunkrBanner.x + f.pad, innerY, f.spelunkrBanner.w - 2 * f.pad, innerH}, inner);
        const int x = f.spelunkrBanner.x + 2 * f.pad;
        const int y = evenUp(innerY + f.pad, L.evenLines);
        const int maxW = std::max(0, f.spelunkrBanner.w - 4 * f.pad);
        bf.font.draw(S, x, y, upper(line1), fg, maxW);
        bf.font.draw(S, x, evenUp(y + fh, L.evenLines), upper(line2), fg, maxW);
    }
}

// Copy the band into a page: `page` is the page's mapping (its line 0), `stride` bytes a line.
// The pixels stay straight (the core premultiplies, ovl_plane.sv:467).
inline void writeBand(uint8_t* page, uint32_t stride, const Surface& S) {
    for (int y = S.y0; y < S.y0 + S.h; ++y) {
        uint8_t* d = page + size_t(y) * stride;
        const uint8_t* s = S.at(0, y);
#ifdef MUT_OSD_PREMULT
        // mutant: premultiply on the ARM (the core would then darken every edge twice)
        for (unsigned x = 0; x < S.W; ++x)
            for (int c = 0; c < 4; ++c)
                d[x * 4 + c] = c == 3 ? s[x * 4 + 3] : uint8_t((s[x * 4 + c] * s[x * 4 + 3] + 127) / 255);
#else
        std::memcpy(d, s, size_t(S.W) * 4u);
#endif
    }
}

} // namespace misterplex::owenb_osd
