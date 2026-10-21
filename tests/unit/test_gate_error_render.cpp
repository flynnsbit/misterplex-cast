// SPS-gate layer 3: unit rows for the on-screen gate error renderer (host/libmisterplex/gate_error_render.hpp).
// Wrap/clip rows; then for each raster geometry (240p 320x240, 480p coded 624x480 with its visible window, 480i
// 720x480, the R1 idle FB 640x480 RGB24): render into a buffer with 4 KiB guard bands on both sides, require the
// guards byte-identical, and DECODE the painted 8x16 cells back to text through the font table, which must equal the
// wrapped lines exactly. The same decoder reads the daemon's real targets in the L3 dry (T1n).
#include "libmisterplex/gate_error_render.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace misterplex;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL gate error render: %s\n", msg.c_str());
        ++fails;
    }
}

constexpr size_t kGuard = 4096;
constexpr uint8_t kGuardByte = 0xA5;

// Reads one 8x16 cell (lit = sample(x, y)) and returns the matching font char, ' ' for an empty cell, or '\0' when no
// glyph matches exactly.
template <class Lit>
char decodeCell(int x0, int y0, Lit lit) {
    std::array<uint8_t, 16> rows{};
    for (int gy = 0; gy < 16; ++gy)
        for (int gx = 0; gx < 8; ++gx)
            if (lit(x0 + gx, y0 + gy))
                rows[static_cast<size_t>(gy)] |= static_cast<uint8_t>(0x80u >> gx);
    for (int c = kFont8x16First; c <= kFont8x16Last; ++c)
        if (kFont8x16[static_cast<size_t>(c - kFont8x16First)] == rows)
            return static_cast<char>(c);
    return '\0';
}

// Decodes `nLines` text lines of up to `cols` cells from (originX, originY); trailing blanks trimmed.
template <class Lit>
std::vector<std::string> decodeLines(int originX, int originY, int cols, int nLines, Lit lit) {
    std::vector<std::string> out;
    for (int li = 0; li < nLines; ++li) {
        std::string s;
        for (int ci = 0; ci < cols; ++ci) {
            const char c = decodeCell(originX + ci * 8, originY + li * 16, lit);
            s += c ? c : '#';
        }
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        out.push_back(s);
    }
    while (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

const std::string kText =
    "Can't play here: max_num_ref_frames 3 (limit 1) (core 0x48005D52). No ladder helper configured.";

// Every lit pixel of a target must sit inside the visible 90% of its active rect (gate_error_render.hpp textEnd).
template <class Lit>
void checkVisible(const char* name, int w, int h, int ax, int aw, Lit lit) {
    int maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (lit(x, y)) {
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
    expect(maxX >= 0, std::string(name) + ": some text is painted");
    expect(maxX < ax + gate::errTextEnd(aw) && maxY < gate::errTextEnd(h),
           std::string(name) + ": the text ends inside the visible 90% (max x " + std::to_string(maxX) + " < " +
               std::to_string(ax + gate::errTextEnd(aw)) + ", max y " + std::to_string(maxY) + " < " +
               std::to_string(gate::errTextEnd(h)) + ")");
}

void checkRgb(const char* name, int w, int h, int ax, int aw, const std::string& text) {
    const size_t bytes = static_cast<size_t>(w) * h * 3;
    std::vector<uint8_t> mem(bytes + 2 * kGuard, kGuardByte);
    uint8_t* buf = mem.data() + kGuard;
    expect(gate::renderGateErrorRgb24(buf, w, h, ax, aw, text), std::string(name) + ": render returns true");
    bool guards = true;
    for (size_t i = 0; i < kGuard; ++i)
        guards = guards && mem[i] == kGuardByte && mem[kGuard + bytes + i] == kGuardByte;
    expect(guards, std::string(name) + ": the 4 KiB guard bands are byte-identical");
    const auto want = gate::wrapErrorText(text, aw, h);
    const int cols = (aw - 2 * gate::kErrMargin) / 8;
    auto lit = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h)
            return false;
        return buf[(static_cast<size_t>(y) * w + x) * 3] > 127;
    };
    const auto got = decodeLines(ax + gate::kErrMargin, gate::kErrMargin, cols, (h - 2 * gate::kErrMargin) / 16, lit);
    expect(got == want, std::string(name) + ": decoded lines equal the wrapped text (" + std::to_string(got.size()) +
                            " vs " + std::to_string(want.size()) + " lines)");
    checkVisible(name, w, h, ax, aw, lit);
    // Outside the active rect only background bytes.
    bool outside = true;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((x < ax || x >= ax + aw) && buf[(static_cast<size_t>(y) * w + x) * 3] != 0)
                outside = false;
    expect(outside, std::string(name) + ": no glyph pixel outside the active rect");
}

void checkI420(const char* name, int w, int h, int ax, int aw, const std::string& text) {
    const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
    std::vector<uint8_t> mem(bytes + 2 * kGuard, kGuardByte);
    uint8_t* buf = mem.data() + kGuard;
    expect(gate::renderGateErrorI420(buf, w, h, ax, aw, text), std::string(name) + ": render returns true");
    bool guards = true;
    for (size_t i = 0; i < kGuard; ++i)
        guards = guards && mem[i] == kGuardByte && mem[kGuard + bytes + i] == kGuardByte;
    expect(guards, std::string(name) + ": the 4 KiB guard bands are byte-identical");
    const uint8_t bg = idleRgbToY(0, 0, 0), fg = idleRgbToY(255, 255, 255);
    expect(fg > bg + 64, std::string(name) + ": text luma is well above the background");
    const auto want = gate::wrapErrorText(text, aw, h);
    const int cols = (aw - 2 * gate::kErrMargin) / 8;
    auto lit = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h)
            return false;
        return buf[static_cast<size_t>(y) * w + x] > (bg + fg) / 2;
    };
    const auto got = decodeLines(ax + gate::kErrMargin, gate::kErrMargin, cols, (h - 2 * gate::kErrMargin) / 16, lit);
    expect(got == want, std::string(name) + ": decoded lines equal the wrapped text");
    checkVisible(name, w, h, ax, aw, lit);
    bool chroma = true;
    for (size_t i = static_cast<size_t>(w) * h; i < bytes; ++i)
        chroma = chroma && (i < static_cast<size_t>(w) * h * 5 / 4 ? buf[i] == idleRgbToU(0, 0, 0)
                                                                     : buf[i] == idleRgbToV(0, 0, 0));
    expect(chroma, std::string(name) + ": chroma is the background's everywhere (achromatic text)");
}

} // namespace

int main() {
    // --- wrap / clip (textEnd: min(e - 16, e * 9/10); 320x240 -> 34 cols, 12 rows) ---
    {
        const auto l = gate::wrapErrorText(kText, 320, 240);  // 34 cols, 12 rows
        expect(!l.empty(), "wrap 240p: some lines");
        bool fit = true;
        std::string joined;
        for (const auto& s : l) {
            fit = fit && s.size() <= 34;
            joined += (joined.empty() ? "" : " ") + s;
        }
        expect(fit, "wrap 240p: every line <= 34 chars");
        expect(joined == kText, "wrap 240p: the lines rejoin to the text (word wrap, nothing lost): " + joined);
    }
    {
        const auto l = gate::wrapErrorText(std::string(400, 'x'), 320, 240);  // hard split: 11 x 34 + 26
        bool full = l.size() == 12;
        for (size_t i = 0; full && i + 1 < l.size(); ++i)
            full = l[i].size() == 34;
        expect(full && l.back().size() == 26, "hard split: 400 x into 34-col lines, got " + std::to_string(l.size()) +
                                                  " lines");
    }
    {
        std::string longText;
        for (int i = 0; i < 60; ++i)
            longText += "word" + std::to_string(i) + " ";
        const auto l = gate::wrapErrorText(longText, 320, 80);  // 34 cols, 3 rows (the margin bounds activeH 80)
        expect(l.size() == 3, "clip: 3 rows at activeH 80, got " + std::to_string(l.size()));
        expect(!l.empty() && l.back().size() >= 3 && l.back().compare(l.back().size() - 3, 3, "...") == 0 &&
                   l.back().size() <= 34,
               "clip: the last line ends '...' within 34 cols: " + (l.empty() ? std::string() : l.back()));
    }
    expect(gate::wrapErrorText("x", 32, 240).empty(), "no room: activeW 32 gives 0 cols, no lines");
    expect(gate::wrapErrorText("a\tb\x01", 320, 240) == std::vector<std::string>{"a?b?"}, "non-ASCII renders '?'");

    // --- R7 (release 11:36, Track D 17:17): the 480p R1 idle FB is 640x480 and the glass capture of the 800x600 HDMI
    // output is 720x576, so only FB x < 720 * 640 / 800 = 576 and y < 576 * 480 / 600 = 460 are visible. ---
    {
        constexpr int kVisX = 720 * 640 / 800, kVisY = 576 * 480 / 600;
        auto extent = [](const std::vector<std::string>& lines) {  // {max x + 1, max y + 1} of the painted glyphs
            int mx = 0, my = 0;
            gate::forEachErrorGlyphPixel(lines, gate::kErrMargin, gate::kErrMargin, [&](int x, int y) {
                mx = std::max(mx, x + 1);
                my = std::max(my, y + 1);
            });
            return std::pair<int, int>(mx, my);
        };
        const auto now = gate::wrapErrorText(kText, 640, 480);
        expect(gate::errTextEnd(640) == kVisX && (gate::errTextEnd(640) - gate::kErrMargin) / 8 == 70,
               "R7: 640 wide wraps at 70 cols, ending at the visible x 576");
        const auto e = extent(now);
        expect(e.first <= kVisX && e.second <= kVisY,
               "R7: the gate error ends inside the 720x576 capture's view of the 640x480 FB (x " +
                   std::to_string(e.first) + " <= 576, y " + std::to_string(e.second) + " <= 460)");
        std::string joined;
        for (const auto& s : now)
            joined += (joined.empty() ? "" : " ") + s;
        expect(joined == kText, "R7: nothing is lost by the narrower wrap: " + joined);
        // NEG: the old (activeW - 2*margin)/8 = 76-col wrap ran past the capture's right edge (its first line ends at
        // x 624), so the tooth above fails the old layout.
        const auto old =
            gate::wrapErrorTextCols(kText, (640 - 2 * gate::kErrMargin) / 8, (480 - 2 * gate::kErrMargin) / 16);
        expect(extent(old).first > kVisX, "R7 NEG: the old 76-col layout paints past x 576 (got x " +
                                              std::to_string(extent(old).first) + ")");
        // A full screen of text stays inside the view vertically too (26 rows end at y 432).
        std::string longText;
        for (int i = 0; i < 400; ++i)
            longText += "word" + std::to_string(i) + " ";
        const auto full = gate::wrapErrorText(longText, 640, 480);
        expect(full.size() == 26 && extent(full).second <= kVisY,
               "R7: a full screen is 26 rows inside y 460 (got " + std::to_string(full.size()) + " rows, y " +
                   std::to_string(extent(full).second) + ")");
    }

    // --- each raster's targets ---
    checkRgb("240p fb0 RGB24 320x240", 320, 240, 0, 320, kText);
    checkI420("240p bank I420 320x240", 320, 240, 0, 320, kText);
    checkI420("480p bank I420 624x480", 624, 480, 0, 624, kText);
    checkI420("480p bank I420 640x480 window 8+624", 640, 480, 8, 624, kText);
    checkI420("480i bank I420 720x480", 720, 480, 0, 720, kText);
    checkRgb("480i fb0 RGB24 720x480", 720, 480, 0, 720, kText + " " + kText);
    checkRgb("R1 idle FB RGB24 640x480", 640, 480, 0, 640, kText);
    expect(!gate::renderGateErrorI420(nullptr, 320, 240, 0, 320, kText), "null buffer refused");
    {
        std::vector<uint8_t> b(320 * 240 * 3 / 2);
        expect(!gate::renderGateErrorI420(b.data(), 320, 240, 8, 320, kText), "active rect past the buffer refused");
    }

    if (fails) {
        std::fprintf(stderr, "test_gate_error_render: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_gate_error_render: OK (wrap/clip, R7 visible area + NEG, 7 targets decoded with guards)\n");
    return 0;
}
