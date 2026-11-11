#pragma once
// MIX-GLASS-MAX: present bank is glass-capped content, never Display.
// true480 never 1280×720. L4 never shrinks below 1280×720 except 960 bank.

#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/mailbox_abi_spec.hpp"
#include "libmisterplex/osd_menu.hpp"
#include "libmisterplex/plx_page.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MPX_STICK_I420
#define MPX_STICK_I420 0
#endif

namespace misterplex {

enum class LiveGlass { True480, L4 };

inline const char* liveGlassLabel(LiveGlass glass) {
    return glass == LiveGlass::L4 ? "L4" : "true480";
}

inline LiveGlass classifyLiveGlass(uint32_t plxjWord) {
    return plxjWord == mailbox_abi::kPlxjMagic ? LiveGlass::L4 : LiveGlass::True480;
}

// Leftover L4 PLXJ in HPS DRAM (survives reboot) must not pin glass=L4 when
// the live RBF is true480 (PLX page of its build row, 0x300FF000 on every
// recognised build). Cast then writes 720p doorbells the fabric never polls —
// chevron-only, Web still playing.
inline LiveGlass decideLiveGlassFromProbes(bool l4PlxjMagic, bool l4Changed,
                                           bool t480PlxiMagic, bool t480Changed) {
    if (l4Changed)
        return LiveGlass::L4;
    if (t480Changed)
        return LiveGlass::True480;
    // Leftover true480 PLXI in HPS DRAM is sticky after an L4 load. Static L4
    // PLXJ (magic present, not changing in the 40 ms sample) must still win
    // over that leftover — otherwise 28cb5a75 pins glass=true480 and
    // GLASS-MAX caps DECODE=1280x720 → bank=640x480. Live 480p still wins
    // via t480Changed (scanout ticks PLXD).
    if (l4PlxjMagic)
        return LiveGlass::L4;
    if (t480PlxiMagic)
        return LiveGlass::True480;
    return LiveGlass::True480;
}

struct LiveGlassPin {
    std::string prefix8;
    std::string core;
    std::string rbfname;
};

inline std::string trimPinText(std::string s) {
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

inline std::string readTrimPinFile(const char* path) {
    if (!path || !path[0])
        return {};
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    char buf[256];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return {};
    buf[n] = 0;
    return trimPinText(std::string(buf));
}

inline std::string rbfPinBasename(std::string s) {
    s = trimPinText(std::move(s));
    const auto slash = s.find_last_of('/');
    if (slash != std::string::npos)
        s = s.substr(slash + 1);
    if (s.size() >= 4) {
        const std::string ext = s.substr(s.size() - 4);
        if (ext == ".rbf" || ext == ".RBF")
            s.resize(s.size() - 4);
    }
    return s;
}

// RFC 1321 MD5, in-process. The pin check used to popen("md5sum \"<path>\""), which handed the
// path to a shell (#66). Reading the file here needs no shell and no md5sum binary.
struct Md5Ctx {
    uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u;
    uint64_t len = 0;
    unsigned char buf[64];
    size_t fill = 0;

    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16)      { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = B ^ C ^ D;          g = (3 * i + 5) & 15; }
            else             { f = C ^ (B | ~D);       g = (7 * i) & 15; }
            const uint32_t t = D;
            D = C;
            C = B;
            B = B + rol(A + f + K[i] + w[g], R[i]);
            A = t;
        }
        a += A; b += B; c += C; d += D;
    }

    void update(const unsigned char* p, size_t n) {
        len += n;
        while (n) {
            const size_t take = (64 - fill < n) ? 64 - fill : n;
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }

    std::string hex() {
        const uint64_t bits = len * 8;
        unsigned char pad[72] = {0x80};
        const size_t padn = (fill < 56) ? 56 - fill : 120 - fill;
        for (int i = 0; i < 8; ++i)
            pad[padn + i] = static_cast<unsigned char>(bits >> (8 * i));
        update(pad, padn + 8);
        static const char H[] = "0123456789abcdef";
        std::string out;
        const uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                const unsigned char byte = static_cast<unsigned char>(v[i] >> (8 * j));
                out.push_back(H[byte >> 4]);
                out.push_back(H[byte & 15]);
            }
        return out;
    }
};

inline std::string md5HexOfBytes(const void* p, size_t n) {
    Md5Ctx c;
    c.update(static_cast<const unsigned char*>(p), n);
    return c.hex();
}

// 32 lowercase hex chars, or "" when the file cannot be opened or read to the end.
inline std::string md5HexOfFile(const char* path) {
    if (!path || !*path)
        return {};
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return {};
    Md5Ctx c;
    unsigned char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        c.update(chunk, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return bad ? std::string() : c.hex();
}

inline std::string md5Prefix8OfFile(const char* path) {
    const std::string hex = md5HexOfFile(path);
    return hex.size() < 8 ? std::string() : hex.substr(0, 8);
}

inline LiveGlassPin readLiveGlassPin(const char* plexRbfPath = "/media/fat/_Utility/Plex.rbf",
                                     const char* corePath = "/tmp/CORENAME",
                                     const char* rbfNamePath = "/tmp/RBFNAME") {
    LiveGlassPin p;
    p.prefix8 = md5Prefix8OfFile(plexRbfPath);
    p.core = readTrimPinFile(corePath);
    p.rbfname = rbfPinBasename(readTrimPinFile(rbfNamePath));
    return p;
}

inline bool envFlagWanted(const char* name) {
    const char* e = std::getenv(name);
    if (!e || !*e)
        return false;
    return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' ||
           std::strcmp(e, "on") == 0 || std::strcmp(e, "ON") == 0 ||
           std::strcmp(e, "YES") == 0 || std::strcmp(e, "yes") == 0;
}

inline bool budget960Wanted() { return envFlagWanted("MPX_BUDGET_960"); }

inline bool skipLastPresentedWanted() {
    const char* e = std::getenv("MPX_SKIP_LAST_PRESENTED");
    if (!e || !*e)
        return budget960Wanted();
    if (e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
        std::strcmp(e, "off") == 0 || std::strcmp(e, "OFF") == 0)
        return false;
    return envFlagWanted("MPX_SKIP_LAST_PRESENTED");
}

inline bool isPlex960BankSize(int w, int h) { return w == 960 && h == 540; }

inline bool isPlex960DdrFrameGeometry(const DdrFrameGeometry& g) {
    return g.presented_width == 960 && g.presented_height == 540;
}

// Default OFF. A second ffmpeg on the dual-A9 stole the decode core:
// isolated 1280x720 Farpoint decode 10s in 7.39s (~32 fps) but product
// split-A/V unique stuck at ~15. 480p gold is single-process. Opt-in
// MPX_SPLIT_AV=1 only.
// 720p CATCH swaps once per vblank. Unpaced BestEffort: pfps=23.8 hw_fps=22.2
// (lost kicks). Wait-THIS-swap then avDecide/overlay added ~2 ms/period
// (unique ~22.8). Kick-on-swap: wait the previous blank, doorbell at swap.
// WC copy of the current slot lands in the free bank after that swap.
// Default ON for 1280×720; MPX_BEAM_PACE=0 disables. 480p stays off.
inline bool beamPaceWanted(int bankW, int bankH) {
    const char* e = std::getenv("MPX_BEAM_PACE");
    if (e && *e) {
        const char c = e[0];
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F' || c == 'o' ||
            c == 'O')
            return false;
        return true;
    }
    return bankW == 1280 && bankH == 720;
}

inline bool plex720pClassSplitAv(int w, int h) {
    (void)w;
    (void)h;
    const char* e = std::getenv("MPX_SPLIT_AV");
    if (!e || !*e)
        return false;
    const char c = e[0];
    if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
        return false;
    return true;
}

inline bool rbfPrefix8IsKnown960Store(const std::string& /*prefix8*/) { return false; }

inline ContentResolution glassMax(int contentW, int contentH, LiveGlass glass,
                                  const std::string& prefix8 = std::string()) {
    if (glass == LiveGlass::L4) {
        if (budget960Wanted() || isPlex960BankSize(contentW, contentH) ||
            rbfPrefix8IsKnown960Store(prefix8))
            return {960, 540, "720p", 1500};
        return {1280, 720, "720p", 1500};
    }
    // true480 (07f54d9f) scans one store: 640×480 @ 0x300FF000. OSD 240p is a
    // PMS ladder only. 320×240 / 1280×720 doorbells on this bitstream are
    // chevron (no live PLXD at those pages). 720p24 needs RBF 03f1b95a.
    // AU32 240p (4ce24aa9) present store is 320×240 @ 0x3007F000.
    // R15 PLEX_PRESENT_480I: host must use 720×480 @ 1 MiB / 0x301FF000.
    if (prefix8 == "4ce24aa9" || (contentW == 320 && contentH == 240))
        return {320, 240, "240p", 1000};
    if (contentW == 720 && contentH == 480)
        return {720, 480, "480i", 2500};
    if (envFlagWanted("MPX_PRESENT_480I"))
        return {720, 480, "480i", 2500};
    return {640, 480, "480p", 2500};
}

// OSD 720p on true480 must not request a 1280 PMS ladder. OSD 240p/480p stay
// as PMS quality. Display raster follows the live RBF, not F12 Display.
inline ContentResolution clampContentToLiveGlass(const ContentResolution& content,
                                                 LiveGlass glass) {
    if (glass == LiveGlass::L4)
        return {1280, 720, "720p", 1500};
    if (content.width >= 1280 || content.height >= 720)
        return {640, 480, "480p", 2500};
    return content;
}

inline ContentResolution clampDisplayToLiveGlass(LiveGlass glass,
                                                const ContentResolution& wanted = {}) {
    if (glass == LiveGlass::L4)
        return {1280, 720, "720p", 1500};
    // true480: present store is always 640×480. Display labels only pick
    // video_mode: 240p=15 kHz bob CRT, 480i=15 kHz weave (OSD Display 720p
    // remaps here — 720p HDMI is illegal on this RBF), 480p=SVGA 800×600.
    if (wanted.label && std::strcmp(wanted.label, "240p") == 0)
        return {320, 240, "240p", 1000};
    if (wanted.label && (std::strcmp(wanted.label, "480i") == 0 ||
                         std::strcmp(wanted.label, "720p") == 0))
        return {720, 480, "480i", 2500};
    return {640, 480, "480p", 2500};
}

inline uint32_t presentBankDoorbell(int bankW, int bankH) {
    const auto geom = ddrFrameGeometryForPresentedSize(bankW, bankH);
    const auto layout = makeDdrFrameLayout(geom, ddrFramePhysBaseForGeometry(geom));
    return layout.doorbell_phys;
}

// WC stick ingest writes the free present bank. Stub fabric_direct must not
// disable that (unique 23.5 memcpy path). New RBF fabric reader can set
// MPX_STICK_I420=0.
inline bool plex720pStickIngestOverridesFabric(bool stickWanted, bool fabricWanted) {
    (void)fabricWanted;
    return stickWanted;
}

inline bool stickI420Wanted() {
    const char* e = std::getenv("MPX_STICK_I420");
    if (e && *e) {
        const char c = e[0];
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F' || c == 'o' ||
            c == 'O')
            return false;
        return true;
    }
#if MPX_STICK_I420
    return true;
#else
    // Default OFF. Opt in via env/conf MPX_STICK_I420=1; 720p still
    // geometry-gated. 480i host-paint must stay plain DDR memcpy.
    return false;
#endif
}

inline bool stickI420SkipPresentMemcpy(bool wanted, uint32_t doorbellPhys, size_t /*bytes*/) {
    return wanted && doorbellPhys == kPlex720pYuv420pDoorbellPhys;
}

inline bool stickI420SkipPresentMemcpy(bool wanted, int bankW, int bankH) {
    const size_t bytes = yuv420pFrameBytes(bankW, bankH);
    return stickI420SkipPresentMemcpy(wanted, presentBankDoorbell(bankW, bankH), bytes);
}

// True480 pages the live-glass probe reads: the distinct non-L4 PLX pages of the
// recognised builds (fpga_build_ids.hpp). A page no row claims is never read.
struct PlxPageSet {
    uint32_t page[4] = {};
    size_t count = 0;
};

inline constexpr PlxPageSet true480PlxPagesFromRows() {
    PlxPageSet s;
    for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
        if (b.plxPage == PlxPageClass::Present720pL4)
            continue;
        const uint32_t p = plxPageForClass(b.plxPage);
        bool seen = false;
        for (size_t i = 0; i < s.count; ++i)
            seen = seen || s.page[i] == p;
        if (!seen && s.count < 4)
            s.page[s.count++] = p;
    }
    return s;
}

inline constexpr PlxPageSet kTrue480PlxPages = true480PlxPagesFromRows();

// One probe sample. On a live core a high word moves: PLXS [63:48] is the
// heartbeat seq (ddr_frame_store republishes PLXS at least every 2^18 clk_ddr,
// ~3 ms) and PLXD [63:48] counts bank swaps. PLXI/PLXJ low words are magics.
// Both pages read the heartbeat: an idle L4 core loaded before its first PLXJ
// commit has nothing else that moves (Bug 01:51 N2).
struct LiveGlassSample {
    uint32_t l4Plxj = 0;
    uint32_t l4PlxdHi = 0;
    uint32_t l4PlxsHi = 0;
    uint32_t t480Plxi[4] = {};
    uint32_t t480PlxdHi[4] = {};
    uint32_t t480PlxsHi[4] = {};
};

template <class ReadWord>
LiveGlassSample sampleLiveGlass(ReadWord&& readWord) {
    constexpr uint32_t l4 = plxPageForClass(PlxPageClass::Present720pL4);
    LiveGlassSample s;
    s.l4Plxj = readWord(plxWordAddr(l4, mailbox_abi::kPlxjOff));
    s.l4PlxdHi = readWord(plxWordAddr(l4, mailbox_abi::kPlxdOff, true));
    s.l4PlxsHi = readWord(plxWordAddr(l4, mailbox_abi::kPlxsOff, true));
    for (size_t i = 0; i < kTrue480PlxPages.count; ++i) {
        const uint32_t page = kTrue480PlxPages.page[i];
        s.t480Plxi[i] = readWord(plxWordAddr(page, mailbox_abi::kPlxiOff));
        s.t480PlxdHi[i] = readWord(plxWordAddr(page, mailbox_abi::kPlxdOff, true));
        s.t480PlxsHi[i] = readWord(plxWordAddr(page, mailbox_abi::kPlxsOff, true));
    }
    return s;
}

inline LiveGlass decideLiveGlassFromSamples(const LiveGlassSample& a, const LiveGlassSample& b) {
    const bool l4Changed = a.l4Plxj != b.l4Plxj || a.l4PlxdHi != b.l4PlxdHi ||
                           a.l4PlxsHi != b.l4PlxsHi;
    bool t480Changed = false;
    bool t480PlxiMagic = false;
    for (size_t i = 0; i < kTrue480PlxPages.count; ++i) {
        t480Changed = t480Changed || a.t480Plxi[i] != b.t480Plxi[i] ||
                      a.t480PlxdHi[i] != b.t480PlxdHi[i] || a.t480PlxsHi[i] != b.t480PlxsHi[i];
        t480PlxiMagic = t480PlxiMagic || b.t480Plxi[i] == mailbox_abi::kPlxiMagic;
    }
    return decideLiveGlassFromProbes(b.l4Plxj == mailbox_abi::kPlxjMagic, l4Changed,
                                     t480PlxiMagic, t480Changed);
}

template <class ReadWord, class Pause>
LiveGlass probeLiveGlassWith(ReadWord&& readWord, Pause&& pause) {
    const LiveGlassSample a = sampleLiveGlass(readWord);
    pause();
    const LiveGlassSample b = sampleLiveGlass(readWord);
    return decideLiveGlassFromSamples(a, b);
}

inline LiveGlass probeLiveGlass(const char* memPath = "/dev/mem") {
    // Watcher writes this from the live Plex.rbf md5. Leftover L4/true480
    // mailbox words in HPS DRAM must not override the bitstream on glass.
    const std::string pin = readTrimPinFile("/tmp/misterplex-live-glass");
    if (pin == "L4" || pin == "l4" || pin == "720p24")
        return LiveGlass::L4;
    if (pin == "true480" || pin == "480p")
        return LiveGlass::True480;
    if (!memPath || !memPath[0])
        return LiveGlass::True480;
    const int fd = ::open(memPath, O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0)
        return LiveGlass::True480;
    constexpr size_t kPage = 4096u;
    auto readWord = [&](uint32_t phys) -> uint32_t {
        const off_t page = static_cast<off_t>(phys & ~(kPage - 1u));
        void* map = ::mmap(nullptr, kPage, PROT_READ, MAP_SHARED, fd, page);
        if (map == MAP_FAILED)
            return 0;
        const auto off = phys & (kPage - 1u);
        uint32_t word = 0;
        std::memcpy(&word, static_cast<const uint8_t*>(map) + off, sizeof(word));
        ::munmap(map, kPage);
        return word;
    };
    const LiveGlass glass = probeLiveGlassWith(readWord, [] { ::usleep(40000); });
    ::close(fd);
    return glass;
}

inline LiveGlass corroborateLiveGlass(uint32_t plxjWord, const LiveGlassPin& pin) {
    const LiveGlass magic = classifyLiveGlass(plxjWord);
    if (magic == LiveGlass::L4)
        return LiveGlass::L4;
    if (pin.core == "Plex" && pin.rbfname.find("720") != std::string::npos)
        return LiveGlass::L4;
    return LiveGlass::True480;
}

} // namespace misterplex
