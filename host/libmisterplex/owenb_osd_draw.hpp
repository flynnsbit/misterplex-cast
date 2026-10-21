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
// §2); the fabric's (1,2,1) anti-twitter stays on (review-arm-osd-review-1017.md).

#include "fpga_playback_overlay.hpp"
#include "idle_screen.hpp"
#include "owenb_osd_layout.hpp"

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
enum class ErrorScreen : uint8_t { None, Refused, CoreNotLoaded, PmsUnreachable, StreamError };

struct Menu {
    bool open = false;
    std::string title;
    std::vector<std::string> items;
    int selected = -1;
    std::vector<uint8_t> dim;   // 0010: 1 = an information row (dim, never selected)
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

struct UiState {
    Bar bar = Bar::Hidden;
    std::string title;          // "" -> "MiSTerPlex"
    std::string transport;      // "+30s" feedback while it lasts: replaces the state word
    int64_t positionMs = 0, durationMs = 0;
    int64_t bufferedMs = -1;    // the buffered fill's end (-1 unknown)
    int bufferingPct = -1;
    int64_t seekTargetMs = -1;  // the ghost knob during the seek settle
    std::string chip;           // "Casting from <controller>": the controller ("" none)
    std::vector<std::string> toasts;   // at most 2, newest last
    ErrorScreen error = ErrorScreen::None;
    std::string errorDetail;    // refusal_name(), a server name, an error string
    Menu menu;
    // T7: the idle card, shown on the idle session (no play): "MISTERPLEX", the hint, the device's name
    bool idle = false;
    std::string idleHint;       // "" -> "Ready to cast"
    std::string idleName;       // the companion's device name ("" none)
    LibraryPage library;        // 0010: over the idle card while open
    std::shared_ptr<const FullSurface> surface;   // OB port: PlexUI, replaces the whole page
    bool anyVisible() const {
        return bar != Bar::Hidden || !chip.empty() || !toasts.empty() || error != ErrorScreen::None || menu.open ||
               idle || library.open || surface;
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

// What is drawn, at the precision it is drawn (seconds for the clocks): equal keys = equal pixels.
inline uint64_t contentKey(const UiState& s, const Layout& L) {
    uint64_t h = 1469598103934665603ull;
    const int64_t geo[] = {L.W, L.H, L.bar.x, L.bar.y, L.bar.w, L.bar.h, L.safe.x, L.safe.y, L.safe.w,
                           L.safe.h, int64_t(L.par * 65536.0), L.evenLines, int64_t(L.mode)};
    h = fnvMix(h, geo, sizeof geo);
    const int64_t v[] = {int64_t(s.bar), s.positionMs / 1000, s.durationMs / 1000,
                         s.bufferedMs < 0 ? -1 : s.bufferedMs / 1000, s.bufferingPct,
                         s.seekTargetMs < 0 ? -1 : s.seekTargetMs / 1000, int64_t(s.error), s.menu.open,
                         s.menu.selected, int64_t(s.toasts.size()), int64_t(s.menu.items.size()), s.idle};
    h = fnvMix(h, v, sizeof v);
    for (const std::string* t : {&s.title, &s.transport, &s.chip, &s.errorDetail, &s.menu.title, &s.idleHint, &s.idleName})
        h = fnvMix(h, *t);
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
                         int64_t(s.library.open), s.surface ? int64_t{1} : int64_t{0}};
    h = fnvMix(h, v, sizeof v);
    for (const std::string* t : {&s.title, &s.transport, &s.chip, &s.errorDetail, &s.menu.title,
                                 &s.idleHint, &s.idleName})
        h = fnvMix(h, *t);
    if (s.surface)
        h = fnvMix(h, &s.surface->gen, sizeof s.surface->gen);
    return h;
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
inline std::string upper(const std::string& t) {
    std::string o;
    for (size_t i = 0; i < t.size();) {
        const uint8_t c = uint8_t(t[i++]);
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
    };
    static const char letters[] = "+%,!?()&_\"#=;\x01";
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
    void draw(Surface& s, int x, int y, const std::string& t, Rgba c) const {
        for (size_t i = 0; i < t.size(); ++i) {
            const uint8_t* rows = osdGlyph(t[i]);
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (rows[r] & (0x10 >> col)) {
                        const int g = int(i) * 6 + col;
                        s.fill({x + colX(g), y + r * sy, colX(g + 1) - colX(g), sy}, c, false);
                    }
        }
    }
};

// Where each element of one frame goes (canvas coordinates), and the band they need.
struct Frame {
    Rect bar, chip, menu, screen, card, lib;
    Rect toast[2];
    int toasts = 0;
    int pad = 6;
    Font font;
    Font cardFont;   // bug 5: the idle card's text, narrowed (columns only) until its longest line fits
    bool idleField = false;   // bug 4: the whole raster, so the core's idle test video stays covered
    bool any() const {
        return !bar.empty() || !chip.empty() || !menu.empty() || !screen.empty() || !card.empty() || !lib.empty() || toasts;
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
        add(bar); add(chip); add(menu); add(screen); add(card); add(lib);
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

inline std::string stateWord(const UiState& s) {
    if (!s.transport.empty())
        return upper(s.transport);
    switch (s.bar) {
    case Bar::Paused: return "PAUSED";
    case Bar::Buffering:
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
    default: return "";
    }
}

inline std::string idleHintText(const UiState& s) { return s.idleHint.empty() ? "Ready to cast" : s.idleHint; }

inline Frame planFrame(const UiState& s, const Layout& L) {
    Frame f;
    f.font = Font::forLayout(L);
    f.pad = L.evenLines ? 4 : 6;
    const int fh = f.font.height(), p = f.pad;
    if (s.error != ErrorScreen::None) {   // full canvas, no video (§3)
        f.screen = L.canvas;
        return f;
    }
    if (s.bar != Bar::Hidden)
        f.bar = L.bar;
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
        // T7: the idle card, centred in title-safe (three rows beside a two-row chevron)
        const int chev = f.font.colX(12);
        // bug 5 (V2-BUGS-1017 #5/5a): at 480i / 576i the glyphs are a third larger (sy 4, even lines) and the
        // card filled all of title-safe, so "MISTERPLEX-192.0.2.110" ran into a CRT's overscan (Auto =
        // Interlaced on an analog TV). The card now keeps to 70 % of the canvas there, and its text is narrowed
        // (columns only, never the rows: strokes stay whole line pairs) until the longest line fits.
        int maxW = L.safe.w;
#ifndef MUT_OSD_CARD_NO_SHRINK   // mutant: the pre-fix card (all of title-safe, the text cut with "...")
        if (L.mode == OutMode::Interlaced)
            maxW = std::min(maxW, int(L.W) * 7 / 10);
#endif
        std::string longest = "MISTERPLEX";
        for (const std::string& t : {upper(idleHintText(s)), upper(s.idleName)})
            if (t.size() > longest.size())
                longest = t;
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
        int w = std::max(f.cardFont.width("MISTERPLEX"), f.cardFont.width(upper(idleHintText(s))));
        if (!s.idleName.empty())
            w = std::max(w, f.cardFont.width(upper(s.idleName)));
        w = std::min(maxW, w + chev + 4 * p);
        const int h = evenUp(3 * (fh + p) + 3 * p, L.evenLines);
#ifdef MUT_F5_CARD_CENTRED
        int top = L.safe.y + (L.safe.h - h) / 2;   // mutant: centred, over the chevron
#else
        // FIELD-1018 F5: bottom centre of title-safe, so the centred chevron behind it shows. Title-safe already
        // keeps the overscan / interlace margins (bug 5); one pad above its bottom edge.
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
    return f;
}


inline void paintBar(Surface& S, const UiState& s, const Layout& L, const Frame& f) {
    const Font& F = f.font;
    const int p = f.pad, fh = F.height(), trackH = 8;
    const Rect b = f.bar;
    S.fill(b, kPanel);
    const int content = 2 * fh + trackH + 2 * p;
    int y = b.y + std::max(0, (b.h - content) / 2);
    y = evenUp(y, L.evenLines);
    const int x0 = b.x + 2 * p, x1 = b.right() - 2 * p;
    // line 1: the title, the state word (or the transport feedback) on the right
    const std::string word = stateWord(s);
    const int ww = F.width(word);
    const Rgba wc = s.transport.empty() ? kFg : kAccent;
    if (ww)
        F.draw(S, x1 - ww, y, word, wc);
    const std::string title = upper(s.title.empty() ? "MiSTerPlex" : s.title);
    F.draw(S, x0, y, F.fit(title, std::max(0, x1 - x0 - ww - F.colX(12))), kFg);
    // the track: the buffered fill, the played fill, the knob, the seek's ghost knob
    const int ty = y + fh + p;
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
    // line 2: elapsed left, duration right
    const int y2 = ty + trackH + p;
    F.draw(S, x0, y2, clockText(s.positionMs), kFg);
    if (s.durationMs > 0) {
        const std::string d = clockText(s.durationMs);
        F.draw(S, x1 - F.width(d), y2, d, kDim);
    }
}

inline void paintBox(Surface& S, const Frame& f, const Rect& r, const std::string& text, Rgba c) {
    if (r.empty())
        return;
    S.fill(r, kPanel);
    const std::string t = f.font.fit(upper(text), r.w - 2 * f.pad);
    f.font.draw(S, r.x + f.pad, evenUp(r.y + f.pad, S.evenLines), t, c);
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

inline void paintChevronField(Surface& S, const Layout& L) {
    const misterplex::IdleRenderState st =
        misterplex::idleRenderState(int(L.W), int(L.H), misterplex::IdleMode::Logo, 0);
    for (int y = S.y0; y < S.y0 + S.h; ++y) {
        for (unsigned x = 0; x < S.W; ++x) {
            uint8_t r = 0, g = 0, b = 0;
#ifdef MUT_F3_IDLE_BG_NOT_BLACK
            misterplex::idlePixelRgb(int(x), y, st, r, g, b);   // mutant: the old grey field
#else
            // FIELD-1018 F3 (owner 11:09): solid black behind the status screen, the chevron blended over black;
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

// T7: the idle card -- a chevron (two strokes, whole line pairs at 480i/576i) and three rows of text.
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
    y = evenUp(y + rowH, L.evenLines);
    T.draw(S, tx, y, T.fit(upper(idleHintText(s)), tw), kAccent);
    if (!s.idleName.empty()) {
        y = evenUp(y + rowH, L.evenLines);
        T.draw(S, tx, y, T.fit(upper(s.idleName), tw), kDim);
    }
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
    if (lb.total > 0 && lb.cursor >= 0) {
        const std::string pos = std::to_string(lb.offset + lb.cursor + 1) + " / " + std::to_string(lb.total);
        const int yb = evenUp(r.bottom() - p - rowH, L.evenLines);
        F.draw(S, r.right() - 2 * p - F.width(pos), yb, pos, kDim);
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
        if (!f.lib.empty()) {
            // Idle library: opaque black over the band. The core's SMPTE bars stay hidden.
            S.fill({0, S.y0, int(S.W), S.h}, Rgba{0, 0, 0, 0xFF}, false);
        } else {
            paintChevronField(S, L);
        }
    }
    if (!f.screen.empty()) {
        paintScreen(S, s, L, f);
        return;
    }
    if (!f.card.empty())
        paintCard(S, s, L, f);
    if (!f.lib.empty())
        paintLibrary(S, s, L, f);
    if (!f.bar.empty())
        paintBar(S, s, L, f);
    paintBox(S, f, f.chip, "Casting from " + s.chip, kFg);
    for (int i = 0; i < f.toasts; ++i) {
        const size_t k = s.toasts.size() - size_t(f.toasts) + size_t(i);
        paintBox(S, f, f.toast[i], s.toasts[k], kFg);
    }
    if (!f.menu.empty())
        paintMenu(S, s, f);
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
