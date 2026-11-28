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

// MPX_FORCE_REMOTE_PATH=1 (env only, default OFF; glass/test aid, never set in a shipped conf).
// Makes the daemon treat EVERY Plex server as remote (location=wan): the remote-forced transcode
// rules (REMOTE_H264_TRANSCODE), the remote direct-play gate and the remote OwenB rung chooser
// (remoteObRung) then run against a LAN server, so glass can exercise the remote path without a
// real WAN server. plexLocationForBase() is the single switch; see docs/REMOTE-720P-RUNG.md.
// Mutants: MUT_FORCE_REMOTE_IGNORED (flag has no effect), MUT_FORCE_REMOTE_DEFAULT_ON (on when unset).
inline bool forceRemotePathEnabled() {
#if defined(MUT_FORCE_REMOTE_DEFAULT_ON)
    return true;
#elif defined(MUT_FORCE_REMOTE_IGNORED)
    return false;
#else
    return remoteH264EnvOn(std::getenv("MPX_FORCE_REMOTE_PATH"));
#endif
}

// MPX_FORCE_REMOTE_RETRY=1 (env only, default OFF; glass/test aid, never set in a shipped conf).
// Makes ONE remote PlexOB transcode request count as failed: ensureUniversalDecision reports a decision mismatch
// ("forced remote retry ...") so the daemon's real lowered-retry path runs (main.cpp: one lowerPmsLadder retry) and
// remoteObRung steps down to 640x480 at <= 2000k. Scope (FPGA Bug review of a335aebd, N1/N2/N4):
//  * only the HD first request: HDMI Native + /library key + remote + OwenB (tlsForcedRetryHdRung, set by
//    resolvePlayTarget from remoteObRung(...).hd). Analog/CRT output, Live TV and every other key are never forced;
//  * only ONCE PER DAEMON RUN: the first qualifying request claims the force (claimForcedRetry); a second play, a seek,
//    another server's first request or a re-resolve is never forced again (restart the daemon to re-arm);
//  * the lowered retry itself is never forced;
//  * if the forced mismatch cannot be retried (viaLadderProxy), main.cpp drops it (retryStepAfterResolve ->
//    DropForced) and resolves the same request normally instead of ending in "Server can't transcode this title".
//    viaLadderProxy is never true in the daemon today, so this is a defensive guard that cannot run in production
//    (test_remote_h264_transcode F34 fails if a setter is added; test_remote_rung_resolve R7b covers the real path).
// Combine with MPX_FORCE_REMOTE_PATH=1 to run it against a LAN server. See docs/REMOTE-720P-RUNG.md.
// Mutants: MUT_FORCE_RETRY_IGNORED, MUT_FORCE_RETRY_DEFAULT_ON, MUT_FORCE_RETRY_LAN, MUT_FORCE_RETRY_EVERY (also forces
// the lowered retry), MUT_FORCE_RETRY_ANY_SCOPE (not HD-rung scoped), MUT_FORCE_RETRY_EVERY_PLAY (never consumed),
// MUT_RETRY_FORCED_SKIP_ERRORS (a skipped forced retry is not dropped).
inline bool forceRemoteRetryEnabled() {
#if defined(MUT_FORCE_RETRY_DEFAULT_ON)
    return true;
#elif defined(MUT_FORCE_RETRY_IGNORED)
    return false;
#else
    return remoteH264EnvOn(std::getenv("MPX_FORCE_REMOTE_RETRY"));
#endif
}

constexpr const char* kForcedRetryMismatch = "forced remote retry (MPX_FORCE_REMOTE_RETRY=1)";

// Per-thread scope input: true only while resolvePlayTarget serves the HD first request (HDMI Native /library).
inline bool& tlsForcedRetryHdRung() {
    static thread_local bool v = false;
    return v;
}

// True: report this request's decision as a mismatch so the lowered retry is taken.
inline bool remoteForcedRetryDue(bool flagOn, bool wan, bool owenb, bool lowered, bool hdRung) {
#if defined(MUT_FORCE_RETRY_LAN)
    (void)wan; (void)hdRung;
    return flagOn && owenb && !lowered;
#elif defined(MUT_FORCE_RETRY_EVERY)
    (void)lowered; (void)hdRung;
    return flagOn && wan && owenb;
#elif defined(MUT_FORCE_RETRY_ANY_SCOPE)
    (void)hdRung;
    return flagOn && wan && owenb && !lowered;
#else
    return flagOn && wan && owenb && !lowered && hdRung;
#endif
}

// Once per daemon run: the first caller wins, later ones never.
inline std::atomic<bool>& forcedRetryConsumed() {
    static std::atomic<bool> c{false};
    return c;
}
inline bool claimForcedRetry() {
#if defined(MUT_FORCE_RETRY_EVERY_PLAY)
    return true;
#else
    return !forcedRetryConsumed().exchange(true);
#endif
}
inline void resetForcedRetryForTest() { forcedRetryConsumed().store(false); }

inline bool isForcedRetryMismatch(const std::string& mismatch) {
    return mismatch.rfind("forced remote retry", 0) == 0;
}

// main.cpp's step after the first resolve: the real one lowered retry, drop a forced mismatch that cannot retry
// (resolve the same request normally; the force is already consumed), or nothing.
enum class RetryStep { None, LoweredRetry, DropForced };
inline RetryStep retryStepAfterResolve(const std::string& mismatch, bool lowered, bool viaLadderProxy, bool genCurrent) {
    if (!mismatch.empty() && !lowered && !viaLadderProxy && genCurrent)
        return RetryStep::LoweredRetry;
#if !defined(MUT_RETRY_FORCED_SKIP_ERRORS)
    if (isForcedRetryMismatch(mismatch) && genCurrent)
        return RetryStep::DropForced;
#endif
    return RetryStep::None;
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

// Audio codecs compiled into static libavcodec (scripts/check_arm_ffmpeg_static.py EXPECTED_DECODERS).
// Any non-empty codec outside this allowlist (e.g. dca/dts, truehd, mlp, mp2, pcm_bluray, wmapro)
// cannot be decoded on-device and must be transcoded to stereo AAC by PMS rather than direct-played.
inline bool audioCodecSupportedForDirectPlay(const std::string& codec) {
    size_t b = 0;
    size_t e = codec.size();
    while (b < e && (codec[b] == ' ' || codec[b] == '\t')) ++b;
    while (e > b && (codec[e - 1] == ' ' || codec[e - 1] == '\t')) --e;
    if (b == e)
        return true;
    const std::string c = remoteDirectPmsLower(codec.substr(b, e - b));
    return c == "aac" || c == "aac_latm" || c == "mp4a" ||
           c == "ac3" || c == "eac3" || c == "ec-3" || c == "ec3" ||
           c == "mp3" || c == "mp3float" ||
           c == "flac" || c == "alac" || c == "opus" || c == "vorbis" ||
           c == "pcm_s16le" || c == "pcm_s24le" || c == "pcm_s32le" || c == "pcm_f32le";
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

// 720p titles publish native-sd (glass nc-s7 check 3): a remote PlexOB transcode used to ALWAYS ask the
// 640x480 box, so a 2.35:1 720p title decoded 640x272 and the core published a 640-wide raster
// (about 640x359, "native-sd"). The core's raster is picture-wide, display-shaped (raster_geom), so
// only a ~1280-wide decode reaches native-hd.
// Rung chooser for a remote (non-LAN) OwenB play:
//   * /library key, resolved Video Output = HDMI Native, first request -> 1280x720 at <= 4000k.
//   * lowered retry, CRT/VGA/interlaced output (raster is the fixed 720x480), unknown output, or any
//     non-/library key (Live TV) -> the unchanged 640x480 rung: <= 2000k with REMOTE_H264_TRANSCODE
//     (remoteUsesFpga640), else <= 4000k (#276 applyRemoteOwenbCap).
// kRemoteObNativeHdMaxKbps is the OB CAVLC rewrite ceiling (kOwenbCavlcMaxKbps = 4000): a first
// request above it would hand the ARM a stream it refuses.
// Mutants: MUT_RUNG_ALWAYS_480 MUT_RUNG_IGNORES_NATIVE MUT_RUNG_NO_LOWER_STEP MUT_RUNG_KBPS_UNCAPPED
// MUT_RUNG_ANY_KEY.
constexpr int kRemoteObNativeHdMaxKbps = 4000;
constexpr int kRemoteObVoutNative = 3;   // h264ring.h H264RING_VOUT_NATIVE (1 interlaced, 2 progressive)
struct RemoteObRung {
    std::string res;
    int kbps = 0;
    bool hd = false;   // true: the 1280x720 native-hd rung
};
inline RemoteObRung remoteObRung(bool flagOn, bool nativeHd, bool lowered, bool libraryKey, int curKbps) {
    (void)nativeHd; (void)lowered; (void)libraryKey;   // each is unused under one of the MUT_RUNG_* mutants
    RemoteObRung r;
#if defined(MUT_RUNG_ALWAYS_480)
    const bool hd = false;
#elif defined(MUT_RUNG_IGNORES_NATIVE)
    const bool hd = !lowered && libraryKey;
#elif defined(MUT_RUNG_NO_LOWER_STEP)
    const bool hd = nativeHd && libraryKey;
#elif defined(MUT_RUNG_ANY_KEY)
    const bool hd = nativeHd && !lowered;
#else
    const bool hd = nativeHd && !lowered && libraryKey;
#endif
    if (hd) {
        r.res = "1280x720";
        r.hd = true;
#if defined(MUT_RUNG_KBPS_UNCAPPED)
        r.kbps = curKbps > 0 ? curKbps : kRemoteObNativeHdMaxKbps;
#else
        r.kbps = (curKbps > 0 && curKbps < kRemoteObNativeHdMaxKbps) ? curKbps : kRemoteObNativeHdMaxKbps;
#endif
        return r;
    }
    r.res = "1280x720";
    r.kbps = curKbps;
    if (remoteUsesFpga640(flagOn, true)) {
        r.res = "640x480";
        r.kbps = remoteFpgaKbps(curKbps);
    } else {
        applyRemoteOwenbCap(r.res, r.kbps, true, true);
    }
    return r;
}

// HDMI Native 720p request floor: PMS maps maxVideoBitrate to a quality tier before it fits the box, and its
// 1.5 Mbps tier is the 720x480 box, so a 1280x720 request at the OB per-server memory floor (1500k) comes back
// 720x405 for a 16:9 title. 2000k is PMS's lowest 1280x720 tier.
// MPX_NATIVE_720P_MIN_KBPS: unset/empty/unparsable -> 2000, <= 0 -> off, else that floor.
// Mutant: MUT_PLEX_NATIVE_REQ_SD (returns kbps unchanged).
constexpr int kNativeHd720pMinKbps = 2000;
inline int nativeHd720pMinKbpsFromEnv() {
    const char* e = std::getenv("MPX_NATIVE_720P_MIN_KBPS");
    if (!e || !e[0])
        return kNativeHd720pMinKbps;
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || *end != 0)
        return kNativeHd720pMinKbps;
    if (v <= 0)
        return 0;
    return v > 1000000 ? 1000000 : static_cast<int>(v);
}
// kbps raised to floorKbps for an OwenB 1280x720 request on HDMI Native below the floor; otherwise unchanged
// (CRT/VGA/interlaced or unknown output, FPGA/legacy ladders, 640x480 rungs, unset kbps).
inline int nativeHd720pRequestKbps(bool owenb, int vout, const std::string& videoResolution, int kbps,
                                   int floorKbps) {
#ifdef MUT_PLEX_NATIVE_REQ_SD
    (void)owenb; (void)vout; (void)videoResolution; (void)floorKbps;
    return kbps;
#else
    if (owenb && vout == kRemoteObVoutNative && videoResolution == "1280x720" && floorKbps > 0 && kbps > 0 &&
        kbps < floorKbps)
        return floorKbps;
    return kbps;
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
