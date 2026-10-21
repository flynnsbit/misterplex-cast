#pragma once
// OWENB-P1B T3: identifying the owenb (PlexOB) core. Design: OWENB-P1B-CAST-DESIGN.md §c.2, §0b.
//
// Names (CONF_STR rename hook):
//   - The PlexOB fork's CONF_STR name is "PlexOB" (kOwenbCoreName). Main writes it to /tmp/CORENAME
//     and /tmp/RBFNAME; the RBF file is _Utility/PlexOB*.rbf.
//   - The rename itself is RTL work (vendor/h264/emu.sv:205 CONF_STR, T10). This tree only expects
//     the name.
//   - owenb's stock core is named "H264". It is always refused, because a stock H264.rbf the user
//     installed must never be driven by misterplexd.
// Ring identity (it replaces the scaffold's dead CAPS probe, which publishes nothing):
//   - "MiSTH264" magic, version (same major as ours, minor no older) and ring geometry, all read
//     from the control block at 0x31400000;
//   - a heartbeat (word 3) that advances while we watch, which rules out the stale words of a
//     previous load;
//   - the MPX BUILD_ID status word.
// MPX BUILD_ID word (layout fork, T10):
//   - word 0x58: [31:0] BUILD_ID, [63:32] the tag 'MPXB'. It is the first word past CORE3
//     (H264RING_STATUS_WORDS = 88), so it is a new word range, not CORE3 80-87.
//   - Without the tag, the build is "unstamped" and gets the id 0x0B000000 | ring version. That id
//     is lab-only (MPX_OWENB_ALLOW_UNSTAMPED=1), so the gate fails closed by default.
// Probing reads the control page only: a PROT_READ map of 4 KiB at 0x31400000. It never maps or
// writes the idle FB at 0x31600000.
// Self-contained (no h264ring.h), so core_gate.hpp can include it. test_owenb_core_gate
// static_asserts these constants against the vendored h264ring_layout.h.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace misterplex {

inline constexpr const char* kOwenbCoreName = "PlexOB";      // the PlexOB fork's CONF_STR name (hook)
inline constexpr const char* kOwenbRbfPrefix = "plexob";     // _Utility/PlexOB*.rbf, any case
inline constexpr const char* kOwenbUpstreamCoreName = "H264";  // owenb's stock CONF_STR: refused

namespace owenb_id {
inline constexpr uint64_t kMagic = 0x343632485453694DULL;   // "MiSTH264"
inline constexpr uint32_t kVersion = 0x101u;                // major << 8 | minor
inline constexpr uint32_t kCtrlPhys = 0x31400000u;
inline constexpr uint32_t kCtrlBytes = 0x1000u;
inline constexpr uint32_t kRingBytes = 0x800000u;
inline constexpr uint32_t kIdleFbPhys = 0x31600000u;        // ours (idle_fb_map.hpp): never touched here
inline constexpr unsigned kWordMagic = 0, kWordVersion = 1, kWordGeometry = 2, kWordHeartbeat = 3;
inline constexpr unsigned kMpxBuildIdWord = 0x58u;
inline constexpr uint32_t kMpxBuildIdTag = 0x4D505842u;     // 'MPXB'
inline constexpr uint32_t kBuildIdFamily = 0x0B000000u;     // OB ids: 0x0Bxxxxxx (legacy CAPS ids are 0x24/0x48...)
inline constexpr uint32_t kBuildIdFamilyMask = 0xFF000000u;
inline constexpr uint32_t unstampedBuildId(uint32_t version) { return kBuildIdFamily | (version & 0xFFFFu); }
}  // namespace owenb_id

inline std::string lowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The RBF base name: no directory, no ".rbf".
inline std::string rbfBaseName(const std::string& rbf) {
    const size_t slash = rbf.find_last_of('/');
    std::string base = rbf.substr(slash == std::string::npos ? 0 : slash + 1);
    if (base.size() > 4 && lowerAscii(base.substr(base.size() - 4)) == ".rbf")
        base.resize(base.size() - 4);
    return base;
}

inline bool coreNameIsPlexOwenb(const std::string& name) { return name == "PlexOB" || name == "PLEXOB"; }
inline bool coreNameIsRawH264(const std::string& name) { return lowerAscii(name) == "h264"; }
inline bool rbfNameIsPlexOwenb(const std::string& rbf) {
    return lowerAscii(rbfBaseName(rbf)).compare(0, 6, kOwenbRbfPrefix) == 0;
}
inline bool rbfNameIsRawH264(const std::string& rbf) {
    return lowerAscii(rbfBaseName(rbf)).compare(0, 4, "h264") == 0;
}

enum class OwenbIdVerdict : uint8_t { NoMap, NoMagic, BadVersion, BadGeometry, Stale, Alive };

inline const char* toString(OwenbIdVerdict v) {
    switch (v) {
    case OwenbIdVerdict::NoMap: return "no control-block map";
    case OwenbIdVerdict::NoMagic: return "no MiSTH264 magic";
    case OwenbIdVerdict::BadVersion: return "ring version not compatible";
    case OwenbIdVerdict::BadGeometry: return "ring geometry differs";
    case OwenbIdVerdict::Stale: return "heartbeat frozen (stale words)";
    case OwenbIdVerdict::Alive: return "alive";
    }
    return "?";
}

struct OwenbIdentity {
    OwenbIdVerdict verdict = OwenbIdVerdict::NoMap;
    uint64_t magic = 0;
    uint32_t version = 0;
    uint32_t ringSize = 0;
    uint32_t heartbeat0 = 0, heartbeat1 = 0;
    bool stamped = false;     // the MPX BUILD_ID word carries its tag
    uint32_t buildId = 0;     // stamped: the word's id; else unstampedBuildId(version)
    std::string describe() const {
        char b[160];
        std::snprintf(b, sizeof b, "ring v%u.%u, %s, BUILD_ID 0x%08X%s, heartbeat %u -> %u",
                      (version >> 8) & 0xFFu, version & 0xFFu, toString(verdict), buildId,
                      stamped ? "" : " (unstamped)", heartbeat0, heartbeat1);
        return b;
    }
};

// A reader over the control block. read(word) returns one 64-bit word. sleepUs and nowUs are
// the clock, so a test advances a fake core on them.
struct OwenbCtrlReader {
    std::function<uint64_t(unsigned)> read;
    std::function<void(unsigned)> sleepUs;
    std::function<int64_t()> nowUs;
};

inline bool owenbVersionOk(uint32_t core) {
    return (core >> 8) == (owenb_id::kVersion >> 8) && (core & 0xFFu) >= (owenb_id::kVersion & 0xFFu);
}

// Read-only. Watches the heartbeat for up to heartbeatMs.
inline OwenbIdentity readOwenbIdentity(const OwenbCtrlReader& r, unsigned heartbeatMs) {
    using namespace owenb_id;
    OwenbIdentity id;
    if (!r.read)
        return id;
    id.magic = r.read(kWordMagic);
    id.version = static_cast<uint32_t>(r.read(kWordVersion) & 0xFFFFu);
    id.ringSize = static_cast<uint32_t>(r.read(kWordGeometry) >> 32);
    id.heartbeat0 = id.heartbeat1 = static_cast<uint32_t>(r.read(kWordHeartbeat));
    id.buildId = unstampedBuildId(id.version);
    if (id.magic != kMagic) {
        id.verdict = OwenbIdVerdict::NoMagic;
        return id;
    }
    if (!owenbVersionOk(id.version)) {
        id.verdict = OwenbIdVerdict::BadVersion;
        return id;
    }
    if (id.ringSize != kRingBytes) {
        id.verdict = OwenbIdVerdict::BadGeometry;
        return id;
    }
#ifndef MUT_OWENB_ID_SKIP_HEARTBEAT
    const int64_t t0 = r.nowUs();
    for (;;) {
        r.sleepUs(2000);
        id.heartbeat1 = static_cast<uint32_t>(r.read(kWordHeartbeat));
        if (id.heartbeat1 != id.heartbeat0)
            break;
        if (r.nowUs() - t0 >= static_cast<int64_t>(heartbeatMs) * 1000) {
            id.verdict = OwenbIdVerdict::Stale;
            return id;
        }
    }
#else
    (void)heartbeatMs;
#endif
    const uint64_t w = r.read(kMpxBuildIdWord);
    id.stamped = static_cast<uint32_t>(w >> 32) == kMpxBuildIdTag && static_cast<uint32_t>(w) != 0;
    if (id.stamped)
        id.buildId = static_cast<uint32_t>(w);
    id.verdict = OwenbIdVerdict::Alive;
    return id;
}

// The OB build table. A row opens the gate; labOnly rows open it only with
// MPX_OWENB_ALLOW_UNSTAMPED=1. T10 adds the stamped PlexOB rows when the layout fork assigns
// their BUILD_IDs. Every id must be in the 0x0B family.
struct OwenbBuildInfo {
    uint32_t id;
    const char* label;
    bool labOnly;
    const char* lineage;
};

inline constexpr OwenbBuildInfo kOwenbBuilds[] = {
    {owenb_id::unstampedBuildId(owenb_id::kVersion), "PlexOB ring v1.1, unstamped", true,
     "pre-T10 PlexOB fork: CONF_STR PlexOB, no MPX BUILD_ID word yet; lab only"},
#ifndef MUT_OWENB_GATE_DROP_T10
    {0x0B593F92u, "PlexOB T10 MPXB stamped", false,
     "lane/owenb-t10-plexob; RBF md5 <pending closed R1 fit>"},
    {0x0B59C11Au, "PlexOB Y/C word 87", false,
     "rel/yc-plexob-1019; vendor/H264.qsf MPX_PLEXOB_BUILD_ID 32'h0B59C11A; "
     "ring word 87 [3:0]=status[23:20], [9:4] tied 0; O[24] reserved for 240p; "
     "ring_in restored; dead fit 0x0B59C119 is not this id; shipped PlexOB RBF md5 c8b34604"},
#endif
};

inline const OwenbBuildInfo* recognisedOwenbBuild(uint32_t id) {
    for (const OwenbBuildInfo& b : kOwenbBuilds)
        if (b.id == id)
            return &b;
    return nullptr;
}

inline bool owenbAllowUnstampedFromEnv() {
    const char* v = std::getenv("MPX_OWENB_ALLOW_UNSTAMPED");
    return v && std::strcmp(v, "1") == 0;
}

// Empty if the gate may open on this identity; otherwise the reason it may not.
inline std::string owenbIdentityRefusal(const OwenbIdentity& id, bool allowUnstamped) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", id.buildId);
    if (id.verdict != OwenbIdVerdict::Alive)
        return std::string("ring identity: ") + toString(id.verdict);
    if ((id.buildId & owenb_id::kBuildIdFamilyMask) != owenb_id::kBuildIdFamily)
        return std::string("BUILD_ID ") + hex + " is not an OB (0x0B) build id";
    const OwenbBuildInfo* b = recognisedOwenbBuild(id.buildId);
    if (!b)
        return std::string("BUILD_ID ") + hex + " is not in the OB build table";
    if (b->labOnly && !allowUnstamped)
        return std::string("BUILD_ID ") + hex + " (" + b->label +
               ") is lab-only: set MPX_OWENB_ALLOW_UNSTAMPED=1 to drive it";
    return {};
}

// A read-only map of the control page (4 KiB at 0x31400000) through path (/dev/mem, or a test's
// sparse file). It never maps the ring, and never maps the idle FB at 0x31600000.
class OwenbCtrlPageMap {
public:
    OwenbCtrlPageMap() = default;
    OwenbCtrlPageMap(const OwenbCtrlPageMap&) = delete;
    OwenbCtrlPageMap& operator=(const OwenbCtrlPageMap&) = delete;
    ~OwenbCtrlPageMap() { close(); }
    int open(const char* path) {
        close();
        fd_ = ::open(path, O_RDONLY | O_SYNC | O_CLOEXEC);
        if (fd_ < 0)
            return -errno;
        void* p = ::mmap(nullptr, owenb_id::kCtrlBytes, PROT_READ, MAP_SHARED, fd_,
                         static_cast<off_t>(owenb_id::kCtrlPhys));
        if (p == MAP_FAILED) {
            const int rc = -errno;
            close();
            return rc;
        }
        page_ = static_cast<const volatile uint64_t*>(p);
        return 0;
    }
    void close() {
        if (page_)
            ::munmap(const_cast<uint64_t*>(page_), owenb_id::kCtrlBytes);
        page_ = nullptr;
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    bool mapped() const { return page_ != nullptr; }
    OwenbCtrlReader reader() const {
        OwenbCtrlReader r;
        if (!page_)
            return r;
        const volatile uint64_t* p = page_;
        r.read = [p](unsigned w) { return w < owenb_id::kCtrlBytes / 8 ? p[w] : 0; };
        r.sleepUs = [](unsigned us) { ::usleep(us); };
        r.nowUs = [] {
            timespec ts{};
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
        };
        return r;
    }

private:
    const volatile uint64_t* page_ = nullptr;
    int fd_ = -1;
};

}  // namespace misterplex
