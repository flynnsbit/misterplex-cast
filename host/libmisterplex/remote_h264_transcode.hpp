#pragma once
#include <algorithm>
#include <vector>
// REMOTE_H264_TRANSCODE (misterplex.conf) / MPX_REMOTE_H264_TRANSCODE (env wins). Default on.
// On: every remote (location=wan) Plex play goes through a PMS universal H.264 transcode that fits
// the decoder. The WAN decision always pins videoCodec/audioCodec/videoProfile/videoLevel, the
// resolver never takes the source Part on a remote server (HEVC refused; out-of-spec H.264 gave
// black screen + audio only, #207), and a failed remote decision errors instead of "trying Part".
// A remote H.264 that already fits the OwenB core still direct-plays (reviewer-DIRECT-1030).
// Off (0): tip behavior. LAN plays are never changed.
#include <cctype>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace misterplex {

inline bool remoteH264EnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return !std::strcmp(buf, "1") || !std::strcmp(buf, "true") || !std::strcmp(buf, "yes") ||
           !std::strcmp(buf, "on");
}

inline bool remoteH264TranscodeEnabled() {
#ifdef MUT_RH264_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_REMOTE_H264_TRANSCODE"); e && e[0])
        return remoteH264EnvOn(e);
    if (const char* e = std::getenv("REMOTE_H264_TRANSCODE"); e && e[0])
        return remoteH264EnvOn(e);
    return true;
#endif
}

// The forced rule applies only to a remote play with the flag on.
inline bool remoteH264Forced(bool flagOn, bool wan) {
#if defined(MUT_RH264_OFF_BINDS)
    (void)flagOn;
    return wan;
#elif defined(MUT_RH264_IGNORED)
    (void)flagOn; (void)wan;
    return false;
#elif defined(MUT_RH264_WRONG_CONTEXT)
    (void)wan;
    return flagOn;
#else
    return flagOn && wan;
#endif
}

// WAN decision: pin codec/profile/level? legacyPin = tip rule (highCabac || FPGA baseline).
inline bool remoteDecisionPinsCodec(bool flagOn, bool wan, bool legacyPin) {
    return legacyPin || remoteH264Forced(flagOn, wan);
}

// May the resolver play the source Part after the decision? legacyTake = tip takePart.
inline bool remoteMayTakePart(bool flagOn, bool wan, bool legacyTake) {
    return legacyTake && !remoteH264Forced(flagOn, wan);
}

// reviewer-DIRECT-1030: with the flag on, a remote source that already fits the OB core direct-plays
// (no PMS transcode). OB caps (third_party/owenb-h264ring/include/h264ring.h struct h264ring_caps,
// h264ring_layout.h word 81): max 3,600 MBs = 1280x720 (L3.1 MaxFS), 108,000 MB/s (720p30 / 480p60),
// 8-bit 4:2:0. Bitrate: kObDirectMaxKbps 8000 (BISECT-BLACK: 11.7M black); Baseline/CAVLC sources
// are rewritten on the ARM and guarded at OB_CAVLC_MAX_KBPS 4000 (owenb_session.hpp).
// Anything else (HEVC, 10-bit, > 720p, > 30 fps at 720p, high bitrate, unknown) is transcoded.
constexpr int kRemoteDirectMaxKbps = 8000;
constexpr int kRemoteDirectCavlcMaxKbps = 4000;
constexpr int kRemoteDirectMaxMbs = 3600;
constexpr int kRemoteDirectMaxMbps = 108000;
// Review 156a264 nit 2: a known bit depth must be exactly 8. An unknown one (<= 0, Stream@bitDepth
// absent) passes only when the profile name itself is an 8-bit-only H.264 profile (baseline,
// constrained baseline, main, high); "high 10", "high 4:2:2" etc. fail closed. Review agreed.
inline bool remoteBitDepthOk(int bitDepth, const std::string& profile) {
    if (bitDepth > 0)
        return bitDepth == 8;
#ifdef MUT_RDIRECT_BD_UNKNOWN
    (void)profile; return true;
#else
    return profile == "baseline" || profile == "constrained baseline" || profile == "main" ||
           profile == "high";
#endif
}

// Review d8726ba nit 1: without a level gate, the reference count must fit the OB DPB. owenb's
// store: H264RING_DPB_STORES 0x14 = 20 stores of H264RING_DPB_STORE 2 MiB (h264ring_layout.h:59-61;
// a 1280x720 4:2:0 picture is 1.38 MB, one store). Budget: refs + the picture being decoded + the
// one on scan-out + one held for output order (kObDpbReserve 3) <= 20. Spec max_num_ref_frames is
// <= 16, so refFrames must be 1..16. Unknown (Stream@refFrames absent) fails closed.
constexpr int kObDpbStores = 20;
constexpr int kObDpbReserve = 3;
inline bool remoteDirectRefsFit(int refFrames) {
#ifdef MUT_RDIRECT_REFS_UNKNOWN
    if (refFrames <= 0) return true;
#endif
    if (refFrames <= 0)
        return false;
#ifndef MUT_RDIRECT_REFS_CAP
    // the binding cap: min(spec 16, stores - reserve 17) = 16
    const int cap = kObDpbStores - kObDpbReserve < 16 ? kObDpbStores - kObDpbReserve : 16;
    if (refFrames > cap)
        return false;
#endif
    return true;
}

// Frame rate for remoteDirectFits: Stream@frameRate ("59.940", "23.976") rounded up, else
// Media@videoFrameRate by exact name (24p, 25p/pal, 30p/ntsc, 50p, 60p, 48p). Anything else is 0.
inline int remoteDirectFpsHint(const std::string& streamFrameRate, const std::string& mediaVfr) {
    if (!streamFrameRate.empty()) {
        char* end = nullptr;
        const double f = std::strtod(streamFrameRate.c_str(), &end);
        if (end != streamFrameRate.c_str() && f > 0 && f < 1000)
            return static_cast<int>(f + 0.999);
    }
    if (mediaVfr == "24p") return 24;
    if (mediaVfr == "25p" || mediaVfr == "pal") return 25;
    if (mediaVfr == "30p" || mediaVfr == "ntsc") return 30;
    if (mediaVfr == "48p") return 48;
    if (mediaVfr == "50p") return 50;
    if (mediaVfr == "60p") return 60;
    return 0;
}

// fps: required. <= 0 (frame rate unknown) fails closed (review 156a264 nit 1).
inline bool remoteDirectFits(const std::string& codec, const std::string& profile, int bitDepth,
                             int w, int h, int level, int kbps, int fps) {
    if (codec != "h264" && codec != "avc" && codec != "avc1")
        return false;
    if (profile != "baseline" && profile != "constrained baseline" && profile != "main" &&
        profile != "high")
        return false;
#ifndef MUT_RDIRECT_10BIT
    if (!remoteBitDepthOk(bitDepth, profile))
        return false;
#endif
#ifndef MUT_RDIRECT_HD
    if (w <= 0 || h <= 0 || w > 1280 || h > 720)
        return false;
#endif
    const int mbs = ((w + 15) / 16) * ((h + 15) / 16);
    if (mbs > kRemoteDirectMaxMbs)
        return false;
#ifndef MUT_RDIRECT_FPS0
    if (fps <= 0)
        return false;
#endif
#ifndef MUT_RDIRECT_FPS
    if (mbs * fps > kRemoteDirectMaxMbps)
        return false;
#endif
    // Glass 156a264 (#Lucky Number 712x400 L42 rejected, played on 0eafa2a2): the level number is
    // not an OB limit. The core caps frame size (3600 MBs) and MB rate (108k MB/s), checked above;
    // a level >= 4.2 that fits those and the bitrate cap direct-plays. level is logged only.
#ifdef MUT_RDIRECT_LEVEL_CAP
    if (level <= 0 || level > 41)
        return false;
#else
    (void)level;
#endif
    if (kbps <= 0 || kbps > kRemoteDirectMaxKbps)
        return false;
    if (profile.find("baseline") != std::string::npos && kbps > kRemoteDirectCavlcMaxKbps)
        return false;
    return true;
}

// --- PMS metadata for remoteDirectFits (unit-tested; keep in sync with parseObSourceEntropy) ---
inline std::string remoteDirectPmsAttrIn(const std::string& slice, const char* name) {
    const std::string key = std::string(name) + "=\"";
    auto p = slice.find(key);
    if (p == std::string::npos)
        return {};
    p += key.size();
    auto e = slice.find('"', p);
    if (e == std::string::npos)
        return {};
    return slice.substr(p, e - p);
}

inline std::string remoteDirectPmsTagAttr(const std::string& xml, const char* tag, const char* name) {
    const std::string open = std::string("<") + tag;
    size_t tpos = 0;
    while ((tpos = xml.find(open, tpos)) != std::string::npos) {
        const size_t boundary = tpos + open.size();
        if (boundary >= xml.size() ||
            std::isspace(static_cast<unsigned char>(xml[boundary])) ||
            xml[boundary] == '>' || xml[boundary] == '/')
            break;
        tpos = boundary;
    }
    if (tpos == std::string::npos)
        return {};
    auto end = xml.find('>', tpos);
    if (end == std::string::npos)
        return {};
    return remoteDirectPmsAttrIn(xml.substr(tpos, end - tpos), name);
}

inline std::string remoteDirectPmsLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// First video Stream (streamType="1" or type="video"). Do not use a bare streamType="1" substring
// search: attribute order varies and some payloads use type="video" only.
inline std::string remoteDirectPmsVideoStreamSlice(const std::string& xml) {
    size_t sp = 0;
    while ((sp = xml.find("<Stream", sp)) != std::string::npos) {
        const auto end = xml.find('>', sp);
        if (end == std::string::npos)
            break;
        const std::string slice = xml.substr(sp, end - sp);
        if (slice.find("streamType=\"1\"") != std::string::npos ||
            slice.find("type=\"video\"") != std::string::npos)
            return slice;
        sp = end + 1;
    }
    return {};
}

struct RemoteDirectPmsCheck {
    std::string codec;
    std::string profile;
    int bitDepth = 0;
    int mw = 0;
    int mh = 0;
    int level = 0;
    int bitrateKbps = 0;
    int fpsHint = 0;
    int refFrames = 0;
    bool fits = false;
};

// Remote-server metadata -> direct-play gate. Stream@refFrames is often missing on shared/remote
// PMS XML; there is no safe substitute without the bitstream SPS, so absent refFrames fails closed.
inline RemoteDirectPmsCheck remoteDirectCheckFromPmsXml(const std::string& xml) {
    RemoteDirectPmsCheck r;
    const std::string vslice = remoteDirectPmsVideoStreamSlice(xml);
    r.codec = remoteDirectPmsLower(remoteDirectPmsAttrIn(vslice, "codec"));
    r.profile = remoteDirectPmsLower(remoteDirectPmsAttrIn(vslice, "profile"));
    const std::string br = remoteDirectPmsAttrIn(vslice, "bitrate");
    if (!br.empty())
        r.bitrateKbps = static_cast<int>(std::strtol(br.c_str(), nullptr, 10));
    if (r.profile.empty())
        r.profile = remoteDirectPmsLower(remoteDirectPmsTagAttr(xml, "Media", "videoProfile"));
    if (r.codec.empty())
        r.codec = remoteDirectPmsLower(remoteDirectPmsTagAttr(xml, "Media", "videoCodec"));
    if (r.bitrateKbps <= 0) {
        const std::string mbr = remoteDirectPmsTagAttr(xml, "Media", "bitrate");
        r.bitrateKbps = mbr.empty() ? 0 : static_cast<int>(std::strtol(mbr.c_str(), nullptr, 10));
    }
    r.level = std::atoi(remoteDirectPmsAttrIn(vslice, "level").c_str());
    const std::string bd = remoteDirectPmsAttrIn(vslice, "bitDepth");
    r.bitDepth = bd.empty() ? 0 : std::atoi(bd.c_str());
    r.mw = std::atoi(remoteDirectPmsTagAttr(xml, "Media", "width").c_str());
    r.mh = std::atoi(remoteDirectPmsTagAttr(xml, "Media", "height").c_str());
    r.fpsHint = remoteDirectFpsHint(remoteDirectPmsAttrIn(vslice, "frameRate"),
                                    remoteDirectPmsLower(remoteDirectPmsTagAttr(xml, "Media", "videoFrameRate")));
    const std::string rf = remoteDirectPmsAttrIn(vslice, "refFrames");
    r.refFrames = rf.empty() ? 0 : std::atoi(rf.c_str());
    r.fits = remoteDirectFits(r.codec, r.profile, r.bitDepth, r.mw, r.mh, r.level, r.bitrateKbps, r.fpsHint) &&
             remoteDirectRefsFit(r.refFrames);
    return r;
}

// Flag on + remote + fits: take the Part directly. Flag off: never (tip path unchanged).
inline bool remoteDirectPlay(bool flagOn, bool wan, bool fits, bool noAudioSubWork) {
#ifdef MUT_RDIRECT_OFF_TAKES
    (void)flagOn; return wan && fits && noAudioSubWork;
#else
    return flagOn && wan && fits && noAudioSubWork;
#endif
}

// Review 156a264 nit 3: a remote direct play that shows no decoded picture within
// kRemoteDirectFirstPicMs falls back ONCE to the forced 480p transcode of the same key.
// alreadyFellBack (this key was already forced) stops a loop; current = same play generation.
constexpr int kRemoteDirectFirstPicMs = 15000;
inline bool remoteDirectPicFallback(bool remoteDirect, bool decodedPicture, bool alreadyFellBack,
                                    bool current) {
#if defined(MUT_RDIRECT_FB_LOOP)
    (void)alreadyFellBack; return remoteDirect && !decodedPicture && current;
#elif defined(MUT_RDIRECT_FB_NOPIC)
    (void)decodedPicture; return remoteDirect && !alreadyFellBack && current;
#else
    return remoteDirect && !decodedPicture && !alreadyFellBack && current;
#endif
}
// Review d8726ba nit 2: a seek re-plays through doPlay, so the watch re-arms on every remote direct
// resolve, seek replays included (a fresh window from the seek). seekReplay must not suppress it.
inline bool remoteDirectArmWatch(bool transcoded, const std::string& detail, bool seekReplay) {
#ifdef MUT_RDIRECT_SEEK_NOARM
    if (seekReplay) return false;
#else
    (void)seekReplay;
#endif
    return !transcoded && detail.rfind("remote direct Part", 0) == 0;
}
// Review d8726ba nit 3: the replay claims the next generation only if playGen is still the watched
// one (compare-and-swap); a user PLAY/STOP/seek that landed first wins. Returns the new gen or 0.
inline uint64_t remoteDirectClaimGen(std::atomic<uint64_t>& playGen, uint64_t watched) {
#ifdef MUT_RDIRECT_GEN_RACE
    (void)watched; return ++playGen;
#else
    uint64_t expected = watched;
    return playGen.compare_exchange_strong(expected, watched + 1) ? watched + 1 : 0;
#endif
}

// The key forced to transcode after a fallback. Sticky until another key resolves, so a seek or
// replay of the fallen-back title never direct-plays (and never re-arms) again.
inline std::mutex& remoteDirectForcedMu() { static std::mutex m; return m; }
inline std::string& remoteDirectForcedKeyRef() { static std::string k; return k; }
inline void setRemoteDirectForcedKey(const std::string& k) {
    std::lock_guard<std::mutex> g(remoteDirectForcedMu()); remoteDirectForcedKeyRef() = k;
}
// The resolver's key of the last remote direct play (what a fallback forces).
inline std::string& remoteDirectLastKeyRef() { static std::string k; return k; }
inline void setRemoteDirectLastKey(const std::string& k) {
    std::lock_guard<std::mutex> g(remoteDirectForcedMu()); remoteDirectLastKeyRef() = k;
}
inline std::string remoteDirectLastKey() {
    std::lock_guard<std::mutex> g(remoteDirectForcedMu()); return remoteDirectLastKeyRef();
}
// True when key is forced. A different non-empty key clears the force.
inline bool remoteDirectForced(const std::string& key) {
    std::lock_guard<std::mutex> g(remoteDirectForcedMu());
    std::string& f = remoteDirectForcedKeyRef();
    if (!f.empty() && f == key)
        return true;
    if (!key.empty())
        f.clear();
    return false;
}

// After a failed decision, may the resolver fall back to the source Part?
inline bool remoteFailedDecisionTriesPart(bool flagOn, bool wan) {
    return !remoteH264Forced(flagOn, wan);
}

// #242 (glass dce1d26 step 4): a key with no metadata (404 / no MediaContainer) is a missing title, not a
// transcode failure. The forced-remote "no original Part" stop (and its error OSD) only fires when the item
// exists; a missing item falls through to "no such item on PMS" and the idle card.
inline bool remoteTranscodeStopApplies(bool flagOn, bool wanNoHelper, bool metaFound) {
#ifdef MUT_RMISS_AS_XCODE
    (void)metaFound;
    return !remoteFailedDecisionTriesPart(flagOn, wanNoHelper);
#else
    return metaFound && !remoteFailedDecisionTriesPart(flagOn, wanNoHelper);
#endif
}

// Round 3 (glass 347e48b: start 400 after decision OK). The start must repeat the decision:
// directPlay=0&directStream=0 on a forced remote play (the decision already asked for a transcode).
inline bool remoteStartForcesTranscode(bool flagOn, bool wan, bool legacyForce) {
#ifdef MUT_RSTART_DIRECT
    (void)flagOn; (void)wan;
    return legacyForce;
#else
    return legacyForce || remoteH264Forced(flagOn, wan);
#endif
}

// A forced remote PlexOB play asks for the FPGA 640x480 profile, not the 1280x720/6000k session ladder.
inline bool remoteUsesFpga640(bool flagOn, bool wan) {
#ifdef MUT_RFPGA_OFF
    (void)flagOn; (void)wan;
    return false;
#else
    return remoteH264Forced(flagOn, wan);
#endif
}
constexpr int kRemoteFpgaMaxKbps = 2000;
inline int remoteFpgaKbps(int cur) {
    return (cur > 0 && cur < kRemoteFpgaMaxKbps) ? cur : kRemoteFpgaMaxKbps;
}

// #276: any remote OwenB play (flag off too) asks 640x480 at <= 4000k so reviewer
// 720p/6.5M does not abort. REMOTE_H264_TRANSCODE=1 still uses 2000k via F7.
constexpr int kRemoteOwenbMaxKbps = 4000;
inline void applyRemoteOwenbCap(std::string& videoResolution, int& maxVideoBitrateKbps, bool owenb,
                                bool wan) {
#ifdef MUT_276_NO_REMOTE_CAP
    (void)videoResolution;
    (void)maxVideoBitrateKbps;
    (void)owenb;
    (void)wan;
#else
    if (!owenb || !wan)
        return;
    videoResolution = "640x480";
    if (maxVideoBitrateKbps <= 0 || maxVideoBitrateKbps > kRemoteOwenbMaxKbps)
        maxVideoBitrateKbps = kRemoteOwenbMaxKbps;
#endif
}

constexpr const char* kRemoteTranscodeFailOsd = "Server can't transcode this title";
// #226: idle-card / error-screen detail when a forced remote play is abandoned after a dead start.
inline std::string remoteTranscodeFailOsdDetail(int httpStatus) {
    if (httpStatus >= 400 && httpStatus < 600)
        return std::string("Server could not transcode (HTTP ") + std::to_string(httpStatus) + ")";
    return kRemoteTranscodeFailOsd;
}
// A forced remote resolve failure (all such details end with the flag tag) shows the error OSD.
inline bool remoteTranscodeFailed(const std::string& detail) {
#ifdef MUT_ROSD_MISS
    (void)detail;
    return false;
#else
    return detail.find("(REMOTE_H264_TRANSCODE=1)") != std::string::npos;
#endif
}

// tipA-local (glass b9): the X-Plex-Client-Profile-Extra hdr line was cut at 512 bytes ("re...").
// Split an already-redacted value into <= maxChunk pieces so each log line stays whole.
inline std::vector<std::string> remoteHdrLogChunks(const std::string& v, std::size_t maxChunk = 400) {
    std::vector<std::string> out;
    if (maxChunk == 0)
        maxChunk = 400;
#ifdef MUT_RHDR_TRUNC
    out.push_back(v.substr(0, std::min<std::size_t>(v.size(), 512)));
    return out;
#else
    for (std::size_t i = 0; i < v.size(); i += maxChunk)
        out.push_back(v.substr(i, maxChunk));
    if (out.empty())
        out.emplace_back();
    return out;
#endif
}

} // namespace misterplex
