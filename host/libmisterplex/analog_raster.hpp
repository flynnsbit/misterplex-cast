// ANALOG_RASTER=480i|240p|auto: lets the daemon select the PlexOB core's "Analog Raster" (status[24]).
// Spec rules kept: exact NTSC 240p is the core's own raster (262 lines, 450,450 dots, 59.94006 Hz); the daemon
// only picks the mode bit. It never retimes, never remaps the source fps, never loads an RBF itself.
//
//  * Unset: no behaviour change at all (no CFG write, no reload). "auto" is the same as unset: no existing doc
//    defines an auto rule for the raster (the auto-picture-mode helper is advise-only and default off), so
//    auto == default. Only an explicit 480i or 240p acts.
//  * Acts only on a core that supports 240p: exactly BUILD_ID 0x0B2403A7 (the dual-build-id table). Every other
//    id (0x0B59C11A, 0x0BFCE8C5, unknown) is left alone: on those, status[24] is reserved and word 87 [9:8] are 0.
//  * Mechanism: set bit 24 (byte 3 bit 0, little endian) in the EXISTING saved status image PlexOB_v1.CFG, then
//    one core reload through the Y/C reload worker. The CFG is never created (a missing or short file would zero
//    every other saved OSD value) and every other byte is preserved. No live status write over SPI.
//  * Word 87: [8] = p240 requested (status[24]); [9] = p240 active. Both are read back into the status file.
#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex { namespace plexob {

enum class RasterWant { Unset, R480i, R240p, Auto };

inline RasterWant parseRasterWant(const std::string& raw, bool* valid = nullptr) {
    std::string v;
    for (char c : raw) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') v += c;
    if (valid) *valid = true;
    if (v.empty()) return RasterWant::Unset;
    if (v == "480i") return RasterWant::R480i;
    if (v == "240p") return RasterWant::R240p;
    if (v == "auto") return RasterWant::Auto;
    if (valid) *valid = false;     // anything else is ignored: default behaviour
    return RasterWant::Unset;
}
inline const char* rasterWantName(RasterWant w) {
    switch (w) {
    case RasterWant::R480i: return "480i";
    case RasterWant::R240p: return "240p";
    case RasterWant::Auto: return "auto";
    default: return "unset";
    }
}

inline constexpr const char* kRasterStatusPath = "/tmp/mpx_analog_raster";   // tmpfs, read-only status text
inline constexpr uint32_t kBuild240p = 0x0B2403A7u;
inline constexpr uint32_t kBuild240p1101 = 0x0B1101C5u;
inline constexpr unsigned kW87P240Requested = 8u, kW87P240Active = 9u;
inline bool coreSupports240p(uint32_t buildId) {
#ifdef MUT_RASTER_ANY_CORE
    return buildId != 0;
#elif defined(MUT_RASTER_NO_1101)
    return buildId == kBuild240p;
#else
    return buildId == kBuild240p || buildId == kBuild240p1101;
#endif
}
inline bool w87Requested(uint64_t w87) { return ((w87 >> kW87P240Requested) & 1u) != 0; }
inline bool w87Active(uint64_t w87) {
#ifdef MUT_RASTER_ACTIVE_IS_REQUESTED
    return ((w87 >> kW87P240Requested) & 1u) != 0;
#endif
    return ((w87 >> kW87P240Active) & 1u) != 0;
}

// ---- the saved status image: bit 24 = byte 3, bit 0 ----
enum class CfgSet { Ok, Same, Missing, Short, TooBig, IoError };
constexpr size_t kCfgMax = 4096;
inline bool cfgBit24(const std::string& img, bool& bit) {
    if (img.size() < 4) return false;
    bit = (static_cast<unsigned char>(img[3]) & 1u) != 0;
    return true;
}
// Returns the new image with only bit 24 changed.
inline std::string cfgWithBit24(std::string img, bool v) {
#ifdef MUT_RASTER_CFG_WRONG_BIT
    img[3] = static_cast<char>(v ? (img[3] | 2) : (img[3] & ~2));
    return img;
#endif
    img[3] = static_cast<char>(v ? (static_cast<unsigned char>(img[3]) | 1u)
                                 : (static_cast<unsigned char>(img[3]) & ~1u));
    return img;
}

inline bool readSmallFile(const std::string& path, std::string& out, bool& tooBig) {
    tooBig = false;
    struct stat sb;
    if (::stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
    if (static_cast<size_t>(sb.st_size) > kCfgMax) { tooBig = true; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
inline bool writeFileAtomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".mpxtmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0;
    ok = (::fsync(::fileno(f)) == 0) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}
// Set status[24] in an existing CFG. Never creates the file; touches only that one bit.
inline CfgSet setCfgBit24(const std::string& path, bool v) {
    std::string img; bool big = false;
#ifdef MUT_RASTER_CREATE_CFG
    if (!readSmallFile(path, img, big) && !big) { img.assign(4, '\0'); }
    else
#endif
    if (!readSmallFile(path, img, big)) return big ? CfgSet::TooBig : CfgSet::Missing;
    bool cur = false;
    if (!cfgBit24(img, cur)) return CfgSet::Short;
    if (cur == v) return CfgSet::Same;
#ifdef MUT_RASTER_TRUNCATE_CFG
    img.resize(4);
#endif
    return writeFileAtomic(path, cfgWithBit24(img, v)) ? CfgSet::Ok : CfgSet::IoError;
}
// -1 unknown (missing/short), else the saved bit.
inline int readCfgBit24(const std::string& path) {
    std::string img; bool big = false, b = false;
    if (!readSmallFile(path, img, big) || !cfgBit24(img, b)) return -1;
    return b ? 1 : 0;
}

struct RasterState {
    bool window = false; bool windowReq = false; int64_t windowSinceUs = 0;
    bool needReload = false, inFlight = false, capLogged = false;
    unsigned attempts = 0; int64_t nextAttemptUs = 0;
};
constexpr int64_t kRasterStableUs = 3000000;
constexpr unsigned kRasterAttemptCap = 3;

struct RasterIn {
    RasterWant want = RasterWant::Unset; uint32_t build = 0; uint64_t w87 = 0;
    bool osdOpen = false, playing = false; int64_t nowUs = 0; const char* cfgPath = "";
};
struct RasterOut {
    bool wantReload = false, gaveUp = false, wroteCfg = false;
    bool supports = false, req = false, act = false; int cfgBit = -1;
    const char* note = "idle";
};

// One OB tick. Default (Unset/Auto) and every non-240p core return before reading or writing any file.
inline RasterOut pollAnalogRaster(RasterState& st, const RasterIn& in) {
    RasterOut o;
    o.supports = coreSupports240p(in.build);
    o.req = w87Requested(in.w87); o.act = w87Active(in.w87);
#ifndef MUT_RASTER_UNSET_ACTS
#ifdef MUT_RASTER_AUTO_ACTS
    if (in.want == RasterWant::Unset) { o.note = "default"; return o; }
#else
    if (in.want == RasterWant::Unset || in.want == RasterWant::Auto) { o.note = "default"; return o; }
#endif
#endif
    if (!o.supports) { o.note = "core-lacks-240p"; return o; }
#ifndef MUT_RASTER_NO_INFLIGHT
    if (st.inFlight) { o.note = "reloading"; return o; }
#endif
#ifdef MUT_RASTER_AUTO_ACTS
    const bool target = in.want != RasterWant::R480i;
#else
    const bool target = in.want == RasterWant::R240p;
#endif
    o.cfgBit = readCfgBit24(in.cfgPath);
    if (o.req == target) { st.window = false; st.needReload = false; st.attempts = 0; o.note = "converged"; return o; }
#ifdef MUT_RASTER_IGNORE_OSD
    if (in.playing) {
#elif defined(MUT_RASTER_IGNORE_PLAYING)
    if (in.osdOpen) {
#else
    if (in.osdOpen || in.playing) {
#endif
        st.window = false; o.note = in.playing ? "playing" : "osd-open"; return o; }
    if (!st.needReload) {
#ifndef MUT_RASTER_CREATE_CFG
        if (o.cfgBit < 0) { o.note = "needs-saved-cfg"; return o; }          // never create the CFG
#endif
#ifndef MUT_RASTER_FIGHT_OSD
        if (o.cfgBit >= 0 && (o.cfgBit == 1) == target) { o.note = "osd-differs-not-fighting"; return o; }
#endif
    }
    if (!st.window || st.windowReq != o.req) { st.window = true; st.windowReq = o.req; st.windowSinceUs = in.nowUs; o.note = "settling"; return o; }
#ifndef MUT_RASTER_NO_SETTLE
    if (in.nowUs - st.windowSinceUs < kRasterStableUs) { o.note = "settling"; return o; }
#endif
#ifndef MUT_RASTER_NO_CAP
    if (st.attempts >= kRasterAttemptCap) { if (!st.capLogged) o.gaveUp = true; st.capLogged = true; st.needReload = false; o.note = "gave-up"; return o; }
#endif
    if (in.nowUs < st.nextAttemptUs) return o;
    if (!st.needReload) {
        const CfgSet r = setCfgBit24(in.cfgPath, target);
        if (r != CfgSet::Ok && r != CfgSet::Same) { o.note = "cfg-write-failed"; return o; }
        o.wroteCfg = r == CfgSet::Ok; st.needReload = true;
    }
    ++st.attempts; st.inFlight = true; st.nextAttemptUs = in.nowUs + kRasterStableUs;
    o.wantReload = true; o.note = "write-cfg-and-reload";
    return o;
}
inline void rasterNoteReloadFinished(RasterState& st, bool ok, int64_t nowUs) {
    st.inFlight = false;
    if (ok) { st.needReload = false; st.attempts = 0; st.window = false; } else st.nextAttemptUs = nowUs + kRasterStableUs;
}

// Fixed-schema status text (tmpfs file). Same keys, same order, every time.
inline std::string renderRasterStatus(RasterWant want, uint32_t build, uint64_t w87, int cfgBit, const char* note) {
    char b[320];
    std::snprintf(b, sizeof b,
        "analog_raster_conf=%s\ncore_build=%08X\ncore_240p=%d\nw87=%08X\np240_requested=%d\np240_active=%d\ncfg_status24=%s\nnote=%s\n",
        rasterWantName(want), static_cast<unsigned>(build), coreSupports240p(build) ? 1 : 0,
        static_cast<unsigned>(static_cast<uint32_t>(w87)), w87Requested(w87) ? 1 : 0, w87Active(w87) ? 1 : 0,
        cfgBit < 0 ? "unknown" : (cfgBit ? "1" : "0"), note ? note : "");
    return b;
}

}}  // namespace misterplex::plexob
