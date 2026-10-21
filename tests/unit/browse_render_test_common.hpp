#pragma once

#include "libmisterplex/browse_render.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace brtest {

using misterplex::browse::BrowseState;
using misterplex::browse::RenderModel;
using misterplex::browse::Row;
using misterplex::browse::RowKind;

inline constexpr int W = misterplex::browse::kCanvasW;
inline constexpr int H = misterplex::browse::kCanvasH;
inline constexpr int Stride = misterplex::browse::kStrideRgb24;

inline void require(bool cond, const std::string& msg) {
    if (!cond)
        throw std::runtime_error(msg);
}

inline std::vector<std::uint8_t> render(const RenderModel& model) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(H) * Stride);
    misterplex::browse::renderBrowseRgb24(model, rgb.data());
    return rgb;
}

inline std::uint32_t crc32Bytes(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline std::uint32_t adler32Bytes(const std::uint8_t* data, std::size_t size) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

inline std::string hex32(const std::array<std::uint8_t, 16>& bytes) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(32);
        for (std::uint8_t b : bytes) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0x0F]);
        }
        return out;
}

inline std::array<std::uint8_t, 16> md5Bytes(const std::uint8_t* data, std::size_t size) {
        static constexpr std::uint32_t kShift[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        static constexpr std::uint32_t kTable[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
            0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
            0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
            0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
            0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        auto rotl = [](std::uint32_t v, std::uint32_t n) {
            return (v << n) | (v >> (32 - n));
        };

        std::vector<std::uint8_t> msg(data, data + size);
        const std::uint64_t bitLen = static_cast<std::uint64_t>(size) * 8u;
        msg.push_back(0x80);
        while ((msg.size() % 64u) != 56u)
            msg.push_back(0);
        for (int i = 0; i < 8; ++i)
            msg.push_back(static_cast<std::uint8_t>((bitLen >> (8 * i)) & 0xFFu));

        std::uint32_t a0 = 0x67452301;
        std::uint32_t b0 = 0xefcdab89;
        std::uint32_t c0 = 0x98badcfe;
        std::uint32_t d0 = 0x10325476;
        for (std::size_t off = 0; off < msg.size(); off += 64) {
            std::uint32_t m[16]{};
            for (int i = 0; i < 16; ++i) {
                const std::size_t p = off + static_cast<std::size_t>(i) * 4u;
                m[i] = static_cast<std::uint32_t>(msg[p]) |
                       (static_cast<std::uint32_t>(msg[p + 1]) << 8) |
                       (static_cast<std::uint32_t>(msg[p + 2]) << 16) |
                       (static_cast<std::uint32_t>(msg[p + 3]) << 24);
            }
            std::uint32_t a = a0, b = b0, c = c0, d = d0;
            for (int i = 0; i < 64; ++i) {
                std::uint32_t f = 0, g = 0;
                if (i < 16) {
                    f = (b & c) | ((~b) & d);
                    g = static_cast<std::uint32_t>(i);
                } else if (i < 32) {
                    f = (d & b) | ((~d) & c);
                    g = static_cast<std::uint32_t>((5 * i + 1) % 16);
                } else if (i < 48) {
                    f = b ^ c ^ d;
                    g = static_cast<std::uint32_t>((3 * i + 5) % 16);
                } else {
                    f = c ^ (b | (~d));
                    g = static_cast<std::uint32_t>((7 * i) % 16);
                }
                const std::uint32_t tmp = d;
                d = c;
                c = b;
                b = b + rotl(a + f + kTable[i] + m[g], kShift[i]);
                a = tmp;
            }
            a0 += a;
            b0 += b;
            c0 += c;
            d0 += d;
        }
        std::array<std::uint8_t, 16> digest{};
        const std::uint32_t words[4] = {a0, b0, c0, d0};
        for (int i = 0; i < 4; ++i) {
            digest[static_cast<std::size_t>(i) * 4u + 0u] = static_cast<std::uint8_t>(words[i] & 0xFFu);
            digest[static_cast<std::size_t>(i) * 4u + 1u] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFFu);
            digest[static_cast<std::size_t>(i) * 4u + 2u] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFFu);
            digest[static_cast<std::size_t>(i) * 4u + 3u] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFFu);
        }
        return digest;
}

inline std::string md5Hex(const std::vector<std::uint8_t>& bytes) {
    return hex32(md5Bytes(bytes.data(), bytes.size()));
}

inline void appendBe32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

inline void appendChunk(std::vector<std::uint8_t>& png, const char type[4],
                        const std::vector<std::uint8_t>& payload) {
    appendBe32(png, static_cast<std::uint32_t>(payload.size()));
    const std::size_t typeOff = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    const std::uint32_t crc = crc32Bytes(png.data() + typeOff, png.size() - typeOff);
    appendBe32(png, crc);
}

inline std::vector<std::uint8_t> pngEncodeRgb(const std::vector<std::uint8_t>& rgb) {
    require(rgb.size() == static_cast<std::size_t>(W) * H * 3u, "bad RGB size");
    std::vector<std::uint8_t> filtered;
    filtered.reserve(static_cast<std::size_t>(H) * (1u + W * 3u));
    for (int y = 0; y < H; ++y) {
        filtered.push_back(0);
        const auto* row = rgb.data() + static_cast<std::size_t>(y) * Stride;
        filtered.insert(filtered.end(), row, row + Stride);
    }

    uLongf zLen = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<std::uint8_t> z(static_cast<std::size_t>(zLen));
    require(compress2(z.data(), &zLen, filtered.data(), static_cast<uLong>(filtered.size()), Z_BEST_COMPRESSION) == Z_OK,
            "zlib compress failed");
    z.resize(static_cast<std::size_t>(zLen));

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    appendBe32(ihdr, W);
    appendBe32(ihdr, H);
    ihdr.push_back(8);
    ihdr.push_back(2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", z);
    appendChunk(png, "IEND", {});
    return png;
}

inline std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    require(static_cast<bool>(f), "cannot open " + path);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

inline void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    require(static_cast<bool>(f), "cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline std::uint32_t readBe32(const std::vector<std::uint8_t>& b, std::size_t off) {
    require(off + 4 <= b.size(), "truncated be32");
    return (static_cast<std::uint32_t>(b[off]) << 24) |
           (static_cast<std::uint32_t>(b[off + 1]) << 16) |
           (static_cast<std::uint32_t>(b[off + 2]) << 8) |
           static_cast<std::uint32_t>(b[off + 3]);
}

inline int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

inline std::vector<std::uint8_t> pngDecodeRgb(const std::string& path) {
    const auto png = readFile(path);
    const std::vector<std::uint8_t> sig = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    require(png.size() > sig.size() && std::equal(sig.begin(), sig.end(), png.begin()), "bad PNG signature");
    std::size_t off = 8;
    std::vector<std::uint8_t> idat;
    int width = 0, height = 0;
    while (off + 12 <= png.size()) {
        const std::uint32_t len = readBe32(png, off);
        off += 4;
        require(off + 4 + len + 4 <= png.size(), "truncated chunk");
        const std::string type(reinterpret_cast<const char*>(png.data() + off), 4);
        off += 4;
        if (type == "IHDR") {
            width = static_cast<int>(readBe32(png, off));
            height = static_cast<int>(readBe32(png, off + 4));
            require(png[off + 8] == 8 && png[off + 9] == 2 && png[off + 12] == 0, "unsupported PNG");
        } else if (type == "IDAT") {
            idat.insert(idat.end(), png.begin() + static_cast<std::ptrdiff_t>(off),
                        png.begin() + static_cast<std::ptrdiff_t>(off + len));
        } else if (type == "IEND") {
            break;
        }
        off += len + 4;
    }
    require(width == W && height == H, "unexpected PNG dimensions");
    std::vector<std::uint8_t> filtered(static_cast<std::size_t>(H) * (1u + W * 3u));
    uLongf outLen = static_cast<uLongf>(filtered.size());
    require(uncompress(filtered.data(), &outLen, idat.data(), static_cast<uLong>(idat.size())) == Z_OK,
            "zlib inflate failed");
    require(outLen == filtered.size(), "unexpected inflate length");
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(H) * Stride);
    const int bpp = 3;
    for (int y = 0; y < H; ++y) {
        const auto* src = filtered.data() + static_cast<std::size_t>(y) * (1u + Stride);
        const int filter = src[0];
        ++src;
        auto* dst = rgb.data() + static_cast<std::size_t>(y) * Stride;
        const auto* prev = y ? dst - Stride : nullptr;
        for (int x = 0; x < Stride; ++x) {
            const int left = x >= bpp ? dst[x - bpp] : 0;
            const int up = prev ? prev[x] : 0;
            const int upLeft = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int add = 0;
            if (filter == 1)
                add = left;
            else if (filter == 2)
                add = up;
            else if (filter == 3)
                add = (left + up) / 2;
            else if (filter == 4)
                add = paeth(left, up, upLeft);
            else
                require(filter == 0, "unknown PNG filter");
            dst[x] = static_cast<std::uint8_t>((src[x] + add) & 0xFF);
        }
    }
    return rgb;
}

inline RenderModel s2Servers() {
    RenderModel m;
    m.state = BrowseState::S2Servers;
    m.headerTitle = "Servers";
    m.pageIndicator = "1-4 / 4";
    m.footerText = "OK open    Back close";
    m.rows = {
        {RowKind::Directory, "MENU1  Owned  Local"},
        {RowKind::Directory, "Office  Owned  Relay"},
        {RowKind::Directory, "Friend A  Shared  Remote"},
        {RowKind::Info, "Friend B  Shared  Offline"},
    };
    m.cursorIndex = 0;
    return m;
}

inline std::vector<Row> movieRows() {
    const std::vector<std::string> films = {
        "Alien (1979)", "Aliens (1986)", "Amadeus (1984)", "Back to the Future (1985)",
        "Blade Runner (1982)", "Brazil (1985)", "Casablanca (1942)", "Chinatown (1974)",
        "Close Encounters of the Third Kind (1977)", "Die Hard (1988)", "Dune (1984)",
        "E.T. the Extra-Terrestrial (1982)", "Fargo (1996)", "Ghostbusters (1984)",
        "Goodfellas (1990)", "Heat (1995)", "Jaws (1975)", "Labyrinth (1986)",
        "Mad Max 2: The Road Warrior (1981)", "Metropolis (1927)"};
    std::vector<Row> rows;
    rows.push_back({RowKind::PagePrev, "Prev"});
    for (std::size_t i = 0; i < films.size(); ++i) {
        Row r{RowKind::Directory, films[i]};
        if (i == 1 || i == 7)
            r.watched = true;
        if (i == 4)
            r.pct = 37;
        if (i == 10)
            r.pct = 82;
        rows.push_back(r);
    }
    rows.push_back({RowKind::PageNext, "Next"});
    return rows;
}

inline RenderModel s4Movies(bool scrolled) {
    RenderModel m;
    m.state = BrowseState::S4List;
    m.headerTitle = "Movies";
    m.pageIndicator = "21-40 / 812";
    m.footerText = "OK details    Back up    L/R page";
    m.rows = movieRows();
    m.cursorIndex = scrolled ? 5 : 1;
    m.viewportTop = scrolled ? 2 : 0;
    return m;
}

inline RenderModel s3Root() {
    RenderModel m;
    m.state = BrowseState::S3Root;
    m.headerTitle = "MENU1";
    m.pageIndicator = "1-6 / 6";
    m.footerText = "OK open    Back servers";
    m.rows = {
        {RowKind::Directory, "Movies"},
        {RowKind::Directory, "TV Shows"},
        {RowKind::Directory, "Music"},
        {RowKind::Directory, "Playlists"},
        {RowKind::Directory, "Recently Added"},
        {RowKind::Directory, "Continue Watching"},
    };
    m.cursorIndex = 1;
    return m;
}

inline RenderModel s5Detail() {
    RenderModel m;
    m.state = BrowseState::S5Detail;
    m.headerTitle = "Blade Runner";
    m.footerText = "OK play    Back list";
    m.rows = {
        {RowKind::Info, "Blade Runner (1982)"},
        {RowKind::Info, "1982    117m    R    4K -> 480p"},
        {RowKind::Info, "Watched 37%  (43:20 of 1:57:00)"},
        {RowKind::Info, ""},
        {RowKind::Item, "Resume from 43:20"},
        {RowKind::Item, "Play from start"},
    };
    m.cursorIndex = 4;
    return m;
}

inline RenderModel s6Starting() {
    RenderModel m;
    m.state = BrowseState::S6Starting;
    m.footerText = "Starting Blade Runner";
    return m;
}

inline RenderModel s8Error() {
    RenderModel m;
    m.state = BrowseState::S8Error;
    m.headerTitle = "Browse Error";
    m.footerText = "Network unavailable";
    m.error = true;
    m.rows = {{RowKind::Info, "Could not load Movies"}, {RowKind::Item, "Retry"}};
    m.cursorIndex = 1;
    return m;
}

inline std::vector<std::pair<std::string, RenderModel>> goldenModels() {
    return {
        {"rg1_s2_servers", s2Servers()},
        {"rg1_s3_root", s3Root()},
        {"rg1_s4_movies", s4Movies(false)},
        {"rg1_s4_movies_scrolled", s4Movies(true)},
        {"rg1_s5_detail", s5Detail()},
        {"rg1_s6_starting", s6Starting()},
        {"rg1_s8_error", s8Error()},
    };
}

inline bool isBackgroundAt(const std::vector<std::uint8_t>& rgb, int x, int y) {
    std::uint8_t bg[3];
    misterplex::browse::backgroundForLine(y, bg);
    const std::size_t off = static_cast<std::size_t>(y) * Stride + static_cast<std::size_t>(x) * 3u;
    return rgb[off] == bg[0] && rgb[off + 1] == bg[1] && rgb[off + 2] == bg[2];
}

inline bool allowedOutsideTitleSafe(int x, int y, const std::vector<std::uint8_t>& rgb) {
    if (x >= misterplex::browse::kTitleSafeX0 && x <= misterplex::browse::kTitleSafeX1 &&
        y >= misterplex::browse::kTitleSafeY0 && y <= misterplex::browse::kTitleSafeY1)
        return true;
    if (x >= misterplex::browse::kSelectionX0 && x <= misterplex::browse::kSelectionX1 &&
        y >= misterplex::browse::kListY0 &&
        y < misterplex::browse::kListY0 + misterplex::browse::kViewRows * misterplex::browse::kRowH) {
        const std::size_t off = static_cast<std::size_t>(y) * Stride + static_cast<std::size_t>(x) * 3u;
        return rgb[off] == misterplex::browse::kAmber[0] &&
               rgb[off + 1] == misterplex::browse::kAmber[1] &&
               rgb[off + 2] == misterplex::browse::kAmber[2];
    }
    return false;
}

} // namespace brtest
