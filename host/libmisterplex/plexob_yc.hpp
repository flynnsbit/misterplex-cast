#pragma once
// PlexOB Y/C (S-Video / composite) on the owenb-combo-s9 tree.
//
// Status-bit map (CONF_STR v,1 in vendor/h264/emu.sv). Existing bits are unchanged.
//
//   bits      name            values
//   O[6:5]    Video Output    Auto, Interlaced, Progressive, Native (HDMI)
//   O[21:20]  Analog Output   0 RGB, 1 S-Video, 2 Composite
//   O[23:22]  NTSC Color      0 NTSC, 1 PAL60, 2 PAL-M
//   O[24]     reserved        later "15 kHz Raster, 480i/240p"; not implemented here
//
// Ring word 87 (shared with the later 240p PR; this build fills only [3:0]):
//   [3:0]   yc_mode = status[23:20]
//   [7:4]   reserved (tied 0)
//   [8]     p240 requested (status[24]); tied 0
//   [9]     p240 active; tied 0
//   [31:10] reserved (tied 0)
//   [63:32] 0
// Word 82 is owenb rumble and is not used. Word 88 remains the MPXB BUILD_ID stamp.
// The daemon decodes yc_mode from [3:0] only. A missing ring read is unknown: no ini
// write and no reload. The read runs on the OB tick through OwenbCtrlReader while
// CoreGate::owenbOpen() is true. CoreGate::open() stays false on PlexOB, so the
// legacy OSD poll is not the Y/C path.
//
// Saved status lives in /media/fat/config/PlexOB_v1.CFG. Main names that file from
// the CONF_STR version ("v,1;" → core name + "_v1" + ".CFG"). It is a little-endian
// image of the core status word. Bits 20-23 are the Y/C nibble. The daemon acts only when
// that nibble equals word 87 [3:0], the OSD is closed (control word 4, live bit 5,
// H264RING_L_OSD_OPEN), and that pair has been unchanged for a few seconds. An
// unsaved menu flip therefore cannot reload the core out from under Save settings.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <sys/stat.h>

namespace misterplex::plexob {

// vendor/H264.qsf MPX_PLEXOB_BUILD_ID. Distinct from T10 0x0B593F92, from the
// word-82 lane 0x0B59C101, and from the dead input-tied-off fit 0x0B59C119.
inline constexpr uint32_t kPlexObYcBuildId = 0x0B59C11Au;
inline constexpr unsigned kRingWordYcMode = 87u;
inline constexpr uint32_t kYcRingLoMask = 0xFu;
// h264ring_layout.h: H264RING_LIVE_W 4, LIVE_LSB 16, L_OSD_OPEN (1<<5).
inline constexpr unsigned kRingWordLive = 4u;
inline constexpr unsigned kYcOsdOpenBit = 16u + 5u;
inline constexpr int64_t kYcStableUs = 3000000;
inline constexpr unsigned kYcReloadAttemptCap = 3u;

inline bool isPlexObYcBuild(uint32_t buildId) { return buildId == kPlexObYcBuildId; }

inline unsigned analogFromRingLo(uint32_t lo) { return lo & 3u; }
inline unsigned ntscFromRingLo(uint32_t lo) { return (lo >> 2) & 3u; }
inline uint32_t packYcValue(unsigned analog, unsigned ntsc) {
    return ((ntsc & 3u) << 2) | (analog & 3u);
}

inline const char* ycVgaModeFromAnalogOutput(unsigned mode) {
    switch (mode & 3u) {
    case 1:
        return "svideo";
    case 2:
        return "cvbs";
    default:
        return "rgb";
    }
}

inline const char* ycNtscModeFromColor(unsigned mode) {
    switch (mode & 3u) {
    case 1:
        return "1";
    case 2:
        return "2";
    default:
        return "0";
    }
}

inline bool vgaModeValid(const std::string& mode) {
    return mode == "rgb" || mode == "svideo" || mode == "cvbs";
}

inline bool ntscModeValid(const std::string& mode) {
    return mode == "0" || mode == "1" || mode == "2";
}

inline bool ycModeWantsScalerOff(const std::string& vgaMode) {
    return vgaMode == "svideo" || vgaMode == "cvbs";
}

struct PlexObIniYc {
    std::string vgaMode;
    std::string ntscMode;
    std::string vgaScaler;
    bool foundSection = false;
    bool hasVga = false;
    bool hasNtsc = false;
    bool hasScaler = false;
};

struct YcReconcileRequest {
    bool fieldRead = false;
    bool plexObBuild = false;
    bool playing = false;
    bool iniMatches = false;
    bool markerMatches = false;
};

struct YcReconcilePlan {
    bool writeIni = false;
    bool reload = false;
    bool persistMarker = false;
};

// Unread field, a non-Y/C build, or a live play: do nothing (no RGB default).
// ini already equal to the OSD: no write, no reload.
// ini differs: write once; reload once per (BUILD_ID, packed value). A marker
// from a previous process suppresses the reload.
inline YcReconcilePlan planYcReconcile(const YcReconcileRequest& in) {
    YcReconcilePlan p;
    if (!in.fieldRead || !in.plexObBuild || in.playing || in.iniMatches)
        return p;
    p.writeIni = true;
    if (!in.markerMatches) {
        p.reload = true;
        p.persistMarker = true;
    }
    return p;
}

// RGB inherits the global [MiSTer] vga_mode. Keys present under [PlexOB] would
// override that, including a global vga_mode=ypbpr, so analog 0 matches only when
// vga_mode, ntsc_mode and vga_scaler are all absent. S-Video and composite also
// require vga_scaler=0 so the encoder sees the core raster, not the HDMI scaler.
inline bool iniInheritsAnalog(const PlexObIniYc& ini) {
    return !ini.hasVga && !ini.hasNtsc && !ini.hasScaler;
}

inline bool iniMatchesOsdYc(const PlexObIniYc& ini, unsigned analogOutput, unsigned ntscColor) {
    if ((analogOutput & 3u) == 0)
        return iniInheritsAnalog(ini);
    const std::string wantVga = ycVgaModeFromAnalogOutput(analogOutput);
    const std::string wantNtsc = ycNtscModeFromColor(ntscColor);
    if (!ini.foundSection || !ini.hasVga || !ini.hasNtsc)
        return false;
    if (ini.vgaMode != wantVga || ini.ntscMode != wantNtsc)
        return false;
    if (ycModeWantsScalerOff(wantVga))
        return ini.hasScaler && ini.vgaScaler == "0";
    return true;
}

struct YcReloadMarker {
    uint32_t buildId = 0;
    uint32_t value = 0;
    bool present = false;
};

inline bool markerCovers(const YcReloadMarker& m, uint32_t buildId, uint32_t value) {
    return m.present && m.buildId == buildId && m.value == value;
}

// One line: "<buildId hex> <packed value hex>". Sits next to ob_pms_traits.
inline YcReloadMarker readYcReloadMarker(const std::string& path) {
    YcReloadMarker m;
    std::ifstream in(path);
    if (!in)
        return m;
    std::string a, b;
    if (!(in >> a >> b))
        return m;
    char* end = nullptr;
    const unsigned long id = std::strtoul(a.c_str(), &end, 16);
    if (!end || *end)
        return m;
    end = nullptr;
    const unsigned long value = std::strtoul(b.c_str(), &end, 16);
    if (!end || *end)
        return m;
    m.buildId = static_cast<uint32_t>(id);
    m.value = static_cast<uint32_t>(value);
    m.present = true;
    return m;
}

inline bool writeYcReloadMarker(const std::string& path, uint32_t buildId, uint32_t value) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        char line[64];
        std::snprintf(line, sizeof line, "%08X %X\n", buildId, value & kYcRingLoMask);
        out << line;
        if (!out)
            return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// Default paths. Tests pass their own; the daemon uses these.
inline constexpr const char* kMisterIniPath = "/media/fat/MiSTer.ini";
inline constexpr const char* kYcReloadMarkerPath = "/media/fat/misterplex/ob_yc_reload";
// CONF_STR "v,1;" — Main's Save settings name is PlexOB_v<N>.CFG.
inline constexpr int kPlexObConfVersion = 1;

inline std::string plexObCfgFileName(int version) {
    return "PlexOB_v" + std::to_string(version) + ".CFG";
}

inline std::string plexObCfgPathForVersion(int version) {
    return std::string("/media/fat/config/") + plexObCfgFileName(version);
}

inline std::string plexObCfgPath() { return plexObCfgPathForVersion(kPlexObConfVersion); }

// Word 88 after a Y/C reload: tag MPXB and this tree's BUILD_ID. Anything else
// (T10 0x0B593F92, unstamped, unreadable) is a different bitstream.
inline bool ycReloadStampOk(uint64_t word88) {
    const uint32_t tag = static_cast<uint32_t>(word88 >> 32);
    const uint32_t id = static_cast<uint32_t>(word88);
    return tag == 0x4D505842u && id == kPlexObYcBuildId;
}

// Little-endian status[31:0]. Bits 20-23 are the high nibble of byte 2.
struct YcCfgNibble {
    bool present = false;
    uint32_t nibble = 0;
};

inline YcCfgNibble readPlexObCfgBytes(const uint8_t* bytes, size_t n) {
    YcCfgNibble c;
    if (!bytes || n < 3)
        return c;
    const uint32_t b3 = n > 3 ? bytes[3] : 0;
    const uint32_t status = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                            (static_cast<uint32_t>(bytes[2]) << 16) | (b3 << 24);
    c.present = true;
    c.nibble = (status >> 20) & kYcRingLoMask;
    return c;
}

inline YcCfgNibble readPlexObCfgFile(const std::string& path) {
    YcCfgNibble c;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return c;
    uint8_t b[4] = {};
    in.read(reinterpret_cast<char*>(b), 4);
    return readPlexObCfgBytes(b, static_cast<size_t>(in.gcount()));
}

inline bool ycOsdOpenFromLiveWord(uint64_t liveWord) {
    return ((liveWord >> kYcOsdOpenBit) & 1ull) != 0;
}

// Stability and per-boot reload attempts. The poll owns this; it is not a
// stand-in for CoreGate or for the control-page reader.
struct YcTickState {
    bool window = false;
    uint32_t windowValue = 0;
    int64_t windowSinceUs = 0;
    bool decided = false;
    uint32_t decidedValue = 0;
    bool needReload = false;
    bool inFlight = false;
    unsigned attempts = 0;
    int64_t nextAttemptUs = 0;
    bool capLogged = false;
    bool cfgSeen = false;
    bool cfgStatOk = false;
    int64_t cfgMtimeNs = 0;
    int64_t cfgCtimeNs = 0;
    uint64_t cfgSize = 0;
    YcCfgNibble cfgCached{};
};

// stat every call. The four status bytes are read when mtime, ctime, or size
// changes, so a second Save inside one mtime tick is still seen when the
// inode's ctime or the length moves. A missing file stays not-present.
inline YcCfgNibble readPlexObCfgCached(YcTickState& st, const std::string& path) {
    struct stat sb {};
    const bool ok = ::stat(path.c_str(), &sb) == 0;
    int64_t mns = 0;
    int64_t cns = 0;
    uint64_t sz = 0;
    if (ok) {
        mns = static_cast<int64_t>(sb.st_mtim.tv_sec) * 1000000000LL +
              static_cast<int64_t>(sb.st_mtim.tv_nsec);
        cns = static_cast<int64_t>(sb.st_ctim.tv_sec) * 1000000000LL +
              static_cast<int64_t>(sb.st_ctim.tv_nsec);
        sz = static_cast<uint64_t>(sb.st_size);
    }
    if (st.cfgSeen && st.cfgStatOk == ok && st.cfgMtimeNs == mns && st.cfgCtimeNs == cns &&
        st.cfgSize == sz)
        return st.cfgCached;
    st.cfgSeen = true;
    st.cfgStatOk = ok;
    st.cfgMtimeNs = mns;
    st.cfgCtimeNs = cns;
    st.cfgSize = sz;
    st.cfgCached = ok ? readPlexObCfgFile(path) : YcCfgNibble{};
    return st.cfgCached;
}

struct YcPollFiles {
    std::string iniPath;
    std::string markerPath;
    std::string cfgPath;
};

struct YcPollResult {
    bool readWord = false;
    bool wroteIni = false;
    bool removedKeys = false;
    bool wantReload = false;
    bool gaveUp = false;
    unsigned analog = 0;
    unsigned ntsc = 0;
    uint32_t value = 0;
    uint32_t buildId = 0;
};

// The marker is written by the caller after the reload returns success.
inline void ycNoteReloadFinished(YcTickState& st, bool ok, int64_t nowUs) {
    st.inFlight = false;
    if (ok)
        st.needReload = false;
    else
        st.nextAttemptUs = nowUs + kYcStableUs;
}

}  // namespace misterplex::plexob
