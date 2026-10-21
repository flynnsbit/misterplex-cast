#pragma once

#include "playback_overlay.hpp"
#include <array>
#include <limits>
#include <string>

namespace misterplex::fpga_overlay {

constexpr uint8_t kIoctlIndex = 6;
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderBytes = 64;
constexpr size_t kTextBytes = 512;
constexpr size_t kPacketBytes = kHeaderBytes + kTextBytes;
constexpr unsigned kTextWidth = 256;
constexpr unsigned kPanelHeight = 24;
constexpr unsigned kColumns = 42;
constexpr uint32_t kMagic = 0x314f504d; // MPO1, little endian.
constexpr uint32_t kCommitMagic = 0x544d434f; // OCMT.
enum class State : uint8_t { Hidden, Playing, Paused, Buffering, Error };
enum Flags : uint16_t { Visible = 1, Title = 2, Status = 4, Progress = 8, AutoFit = 16 };
using Packet = std::array<uint8_t, kPacketBytes>;

// Text and progress are presentation metadata, never writable video/DPB memory.
struct Model {
    State state = State::Hidden;
    std::string title;
    std::string transport; // Local input feedback, e.g. "<< 30S" or "SEEKING".
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
    // Default placement is measured by the leaf in native DE coordinates.
    uint16_t flags = Visible | Title | Status | Progress | AutoFit;
    unsigned x = 8;
    unsigned y = 204;
    unsigned scale = 1;
    unsigned width = kTextWidth;
    uint32_t foreground = 0xefeff0;
    uint32_t background = 0x16191c;
    uint32_t accent = 0xffb220;
};

// Coordinates describe the visible viewport in native beam pixels, not padded
// I420 storage. This changes only UI geometry; source crop/DAR remain untouched.
inline bool fitToViewport(Model& model, unsigned width, unsigned height,
                          unsigned origin_x = 0, unsigned origin_y = 0) {
    model.flags &= ~AutoFit;
    if (origin_x > 2047 || origin_y > 2047) {
        model.flags &= ~Visible;
        return false;
    }
    width = std::min(width, 2048u - origin_x);
    height = std::min(height, 2048u - origin_y);
    if (width < 6 || height < kPanelHeight) {
        model.flags &= ~Visible;
        return false;
    }
    const unsigned margin_x = std::min(8u, width / 16);
    const unsigned margin_y = std::min({8u, height / 16, height - kPanelHeight});
    model.scale = 1;
    for (unsigned scale : {2u, 4u})
        if (width - 2 * margin_x >= kTextWidth * scale &&
            height - margin_y >= kPanelHeight * scale)
            model.scale = scale;
    model.width = std::min(kTextWidth, (width - 2 * margin_x) / model.scale);
    model.x = origin_x + (width - model.width * model.scale) / 2;
    model.y = origin_y + height - margin_y - kPanelHeight * model.scale;
    return true;
}

inline void put16(uint8_t* p, uint16_t n) {
    p[0] = uint8_t(n); p[1] = uint8_t(n >> 8);
}
inline void put32(uint8_t* p, uint32_t n) {
    for (unsigned i = 0; i != 4; ++i) p[i] = uint8_t(n >> (8 * i));
}
inline void put64(uint8_t* p, uint64_t n) {
    for (unsigned i = 0; i != 8; ++i) p[i] = uint8_t(n >> (8 * i));
}
inline uint16_t get16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t get32(const uint8_t* p) {
    uint32_t n = 0;
    for (unsigned i = 0; i != 4; ++i) n |= uint32_t(p[i]) << (8 * i);
    return n;
}
inline uint64_t get64(const uint8_t* p) {
    uint64_t n = 0;
    for (unsigned i = 0; i != 8; ++i) n |= uint64_t(p[i]) << (8 * i);
    return n;
}

// One blank fallback per non-ASCII code point, including malformed sequences.
// ASCII lower case becomes upper case; no locale or external font dependency.
inline std::string displayText(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < kColumns;) {
        const uint8_t ch = uint8_t(text[i++]);
        if (ch < 0x80) {
            out += char(ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') :
                        ch >= 32 && ch <= 126 ? ch : ' ');
        } else {
            unsigned rest = ch >= 0xc2 && ch <= 0xdf ? 1 :
                            ch >= 0xe0 && ch <= 0xef ? 2 :
                            ch >= 0xf0 && ch <= 0xf4 ? 3 : 0;
            while (rest && i < text.size() && (uint8_t(text[i]) & 0xc0) == 0x80) {
                ++i; --rest;
            }
            out += ' ';
        }
    }
    return out;
}

inline const uint8_t* glyph(char ch) {
    // Complete the existing transport font only for this new title plane.
    static constexpr uint8_t extra[][7] = {
        {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, // B
        {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, // C
        {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, // F
        {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, // H
        {0x07,0x02,0x02,0x02,0x12,0x12,0x0c}, // J
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
        {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, // M
        {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, // Q
        {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, // R
        {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}, // V
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, // W
        {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, // X
        {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, // Z
        {0,0x01,0x02,0x04,0x08,0x10,0},       // /
        {0,0,0,0,0,0x0c,0x0c},               // .
        {0x04,0x04,0x04,0,0,0,0},            // '
    };
    const char* letters = "BCFHJKMQRVWXZ/.'";
    const char* found = std::strchr(letters, ch);
    return ch && found ? extra[found - letters] : PlaybackOverlay::fontGlyph(ch);
}

inline void drawLine(Packet& packet, unsigned line, const std::string& raw,
                     unsigned width = kTextWidth) {
    const std::string text = displayText(raw);
    for (size_t col = 0; col < text.size() && col < width / 6; ++col) {
        const uint8_t* rows = glyph(text[col]);
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 5; ++x)
                if (rows[y] & (1u << (4 - x))) {
                    const unsigned px = unsigned(col) * 6 + x;
                    packet[kHeaderBytes + (line * 8 + y) * 32 + px / 8] |=
                        uint8_t(1u << (7 - (px & 7)));
                }
    }
}

inline const char* stateText(State state) {
    switch (state) {
    case State::Playing: return "PLAYING";
    case State::Paused: return "PAUSED";
    case State::Buffering: return "BUFFERING";
    case State::Error: return "ERROR";
    default: return "";
    }
}

// Overflow-safe, bounded progress; int64 timestamps may span far beyond a movie.
inline uint16_t progressPixels(int64_t position, int64_t duration,
                               unsigned width = kTextWidth) {
    width = std::min(width, kTextWidth);
    if (position <= 0 || duration <= 0) return 0;
    if (position >= duration) return uint16_t(width);
    uint64_t rem = 0, den = uint64_t(duration);
    unsigned result = 0;
    for (int bit = 8; bit >= 0; --bit) {
        rem *= 2;
        result <<= 1;
        if (rem >= den) { rem -= den; result |= 1; }
        if (width & (1u << bit)) {
            rem += uint64_t(position);
            if (rem >= den) { rem -= den; ++result; }
        }
    }
    return uint16_t(result);
}

inline Packet encode(uint64_t epoch, uint64_t nonce, uint32_t sequence,
                     const Model& model) {
    Packet packet{};
    put32(packet.data(), kMagic);
    put16(packet.data() + 4, kVersion);
    put16(packet.data() + 6, kPacketBytes);
    put64(packet.data() + 8, epoch);
    put64(packet.data() + 16, nonce);
    put32(packet.data() + 24, sequence);
    const bool valid_state = model.state >= State::Playing && model.state <= State::Error;
    const unsigned flags = valid_state ? (model.flags & 31) : 0;
    put16(packet.data() + 28, uint16_t(flags));
    packet[30] = valid_state ? uint8_t(model.state) : 0;
    // Only powers of two need wiring, not a pixel-rate divider.
    packet[31] = model.scale >= 4 ? 4 : model.scale >= 2 ? 2 : 1;
    // AutoFit progress is Q8 over the complete strip, so the pixel leaf can
    // rescale it to a measured narrow viewport with a multiply/shift, no divider.
    const unsigned width = (flags & AutoFit) ? kTextWidth :
                           std::max(1u, std::min(model.width, kTextWidth));
    put16(packet.data() + 32, uint16_t(std::min(model.x, 2047u)));
    put16(packet.data() + 34, uint16_t(std::min(model.y, 2047u)));
    put16(packet.data() + 36, progressPixels(model.position_ms, model.duration_ms, width));
    put32(packet.data() + 40, model.foreground & 0xffffff);
    put32(packet.data() + 44, model.background & 0xffffff);
    put32(packet.data() + 48, model.accent & 0xffffff);
    put16(packet.data() + 52, uint16_t(width));
    put16(packet.data() + 54, kPanelHeight);
    put32(packet.data() + 56, kCommitMagic);
    put32(packet.data() + 60, ~sequence);
    drawLine(packet, 0, model.title, width);
    char position[32], duration[32];
    PlaybackOverlay::formatTimestamp(model.position_ms, position);
    PlaybackOverlay::formatTimestamp(model.duration_ms, duration);
    std::string status = stateText(model.state);
    if (!model.transport.empty()) { status += " "; status += model.transport; }
    status += " "; status += position; status += "/"; status += duration;
    drawLine(packet, 1, status, width);
    return packet;
}

inline bool validate(const uint8_t* p, size_t length, uint64_t epoch, uint64_t nonce,
                     uint32_t last_sequence) {
    if (!p || length != kPacketBytes || !epoch || !nonce ||
        get32(p) != kMagic || get16(p + 4) != kVersion ||
        get16(p + 6) != kPacketBytes || get64(p + 8) != epoch ||
        get64(p + 16) != nonce || !get32(p + 24) ||
        get32(p + 24) <= last_sequence || (get16(p + 28) & ~31) || p[30] > 4 ||
        (p[31] != 1 && p[31] != 2 && p[31] != 4) ||
        get16(p + 32) > 2047 || get16(p + 34) > 2047 ||
        get16(p + 36) > get16(p + 52) || get16(p + 38) || p[43] || p[47] || p[51] ||
        !get16(p + 52) || get16(p + 52) > kTextWidth ||
        ((get16(p + 28) & AutoFit) && get16(p + 52) != kTextWidth) ||
        get16(p + 54) != kPanelHeight ||
        get32(p + 56) != kCommitMagic || get32(p + 60) != ~get32(p + 24))
        return false;
    return true;
}

// Sender must expose the actual FpgaSpi::sendFileTx API. Sequence advances only
// on transport success. Periodic publication retries updates dropped while the
// receiver is waiting for a pixel frame boundary (SPI success is not UI ACK).
template<class Sender>
bool send(Sender& sender, uint64_t epoch, uint64_t nonce, uint32_t& sequence,
          const Model& model) {
    if (!epoch || !nonce || sequence == std::numeric_limits<uint32_t>::max())
        return false;
    const Packet packet = encode(epoch, nonce, sequence + 1, model);
    if (!sender.sendFileTx(packet.data(), packet.size(), kIoctlIndex)) return false;
    ++sequence;
    return true;
}

} // namespace misterplex::fpga_overlay
