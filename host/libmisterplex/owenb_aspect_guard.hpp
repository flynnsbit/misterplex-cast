#pragma once
// OWENB-P1B 0006: the Analog Aspect default (spec Decision 1) without making it stick (review 10:08).
// OWENB-P1B 0008: the same for the HDMI Aspect Ratio 4:3 override of a VUI-less 4:3 stream (design d.3.5).
//
// At a session's start: the user's menu values (word 27 OSD_ANALOG_ASPECT / OSD_ASPECT,
// h264ring_layout.h:351-374) are saved -- in memory and in a small state file, before anything is
// asked -- and then, in one request (the request word holds one request, h264ring.c:999-1022):
//   - Analog Aspect = Letterbox at 31 kHz progressive (h264ring_video_out() == 2, h264ring.h:377-380)
//     with the menu on Auto; never VIDEO_OUTPUT (that resyncs the TV, h264ring.h:229);
//   - Aspect Ratio = 4:3 (h264ring.h:221 "0 Auto, 1 4:3, 2 16:9") when the stream's SPS carries no
//     aspect (no VUI aspect_ratio_info, which the core reads as 16:9, design d.3.5 / OB display.md:178-179)
//     and its container DAR is 4:3 (owenbForceAspect43), with the menu's Aspect Ratio on Auto.
// At the session's end (stop, end of stream, error, teardown): the saved values are requested back,
// the sequence awaited in word 30 SET_SEQ (h264ring_layout.h:414-416, result SET_ERR :372-374),
// and only then the request word cleared (h264ring_clear_settings, h264ring.h:245-248).
//   - The user changed an option during the session (the mirror no longer shows ours after our
//     request was consumed): their choice wins (h264ring.h:225-227); that option is not restored.
//   - The restore is not taken (no session: -ENOTCONN, an OSD Reset, a dead core): the request
//     word is left holding the user's values -- the core applies it again at the next start of this
//     handle's session (h264ring.h:245-247; a new handle starts with no word, h264ring.c:175-181) --
//     and the state file keeps them, so the next session (the idle session, the next play, or a
//     restarted daemon) requests them again before anything else.
// Thread: one caller at a time (the play thread, then stop() after the join); a mutex guards it.

#include "owenb_osd_renderer.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace misterplex::owenb_osd {

// The user's values while ours are on the menu (each option only if we asked for it).
struct AspectSaved {
    bool aa = false;   // Analog Aspect saved
    unsigned aaVal = 0;
    bool ar = false;   // (0008) Aspect Ratio saved
    unsigned arVal = 0;
    bool any() const { return aa || ar; }
};

class IAspectStore {
public:
    virtual ~IAspectStore() = default;
    virtual bool load(AspectSaved& s) = 0;
    virtual bool save(const AspectSaved& s) = 0;
    virtual void erase() = 0;
    // Analog Aspect only (0006 callers)
    bool load(unsigned& v) {
        AspectSaved s;
        if (!load(s) || !s.aa)
            return false;
        v = s.aaVal;
        return true;
    }
    bool save(unsigned v) {
        AspectSaved s;
        s.aa = true;
        s.aaVal = v;
        return save(s);
    }
};

class MemAspectStore final : public IAspectStore {
public:
    using IAspectStore::load;
    using IAspectStore::save;
    bool load(AspectSaved& s) override {
        if (!has && !hasAr)
            return false;
        s.aa = has;
        s.aaVal = val;
        s.ar = hasAr;
        s.arVal = ar;
        return true;
    }
    bool save(const AspectSaved& s) override {
        has = s.aa;
        val = s.aaVal;
        hasAr = s.ar;
        ar = s.arVal;
        ++saves;
        return true;
    }
    void erase() override { has = hasAr = false; }
    bool has = false, hasAr = false;
    unsigned val = 0, ar = 0, saves = 0;
};

// "analog_aspect=N\n" and/or "aspect=M\n"; written to path.tmp then renamed. Default path (daemon):
// /media/fat/misterplex/ob_aspect_restore (MPX_OB_ASPECT_STATE), so a crash or a reboot still restores.
// A 0006 file (analog_aspect only) still loads.
class FileAspectStore final : public IAspectStore {
public:
    using IAspectStore::load;
    using IAspectStore::save;
    explicit FileAspectStore(std::string path) : path_(std::move(path)) {}
    bool load(AspectSaved& s) override {
        std::FILE* f = std::fopen(path_.c_str(), "r");
        if (!f)
            return false;
        AspectSaved r;
        char line[64];
        while (std::fgets(line, sizeof line, f)) {
            unsigned x = 0;
            if (std::sscanf(line, "analog_aspect=%u", &x) == 1 && x <= 3) {
                r.aa = true;
                r.aaVal = x;
            } else if (std::sscanf(line, "aspect=%u", &x) == 1 && x <= 2) {
                r.ar = true;
                r.arVal = x;
            }
        }
        std::fclose(f);
        if (!r.any())
            return false;
        s = r;
        return true;
    }
    bool save(const AspectSaved& s) override {
        const std::string tmp = path_ + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f)
            return false;
        bool ok = true;
        if (s.aa)
            ok = std::fprintf(f, "analog_aspect=%u\n", s.aaVal) > 0 && ok;
        if (s.ar)
            ok = std::fprintf(f, "aspect=%u\n", s.arVal) > 0 && ok;
        const bool closed = std::fclose(f) == 0;
        return ok && closed && std::rename(tmp.c_str(), path_.c_str()) == 0;
    }
    void erase() override { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

enum class AspectEnd { Nothing, Restored, UserChanged, Pending };

inline const char* aspectEndName(AspectEnd e) {
    switch (e) {
    case AspectEnd::Nothing: return "nothing";
    case AspectEnd::Restored: return "restored";
    case AspectEnd::UserChanged: return "user-changed";
    case AspectEnd::Pending: return "pending";
    }
    return "?";
}

struct AspectStats {
    unsigned letterboxRequests = 0, restores = 0, userChanged = 0, pending = 0, startRestores = 0;
    unsigned aspect43Requests = 0;   // 0008
};

// 0008: what the play's first video access unit said (owenb_play.cpp sniffs it, owenb_aspect.hpp decides).
struct StreamAspect {
    bool known = false;     // a video keyframe was seen
    bool force43 = false;   // VUI-less SPS and a 4:3 container DAR
    bool square43 = false;  // BUG6: the coded picture is itself 4:3 (square samples)
};

class AspectGuard {
public:
    using Sleep = std::function<void(unsigned)>;
    static constexpr unsigned kLetterbox = 2;
    static constexpr unsigned kAspect43 = 1;   // Aspect Ratio: 0 Auto, 1 4:3, 2 16:9 (h264ring.h:221)
    static constexpr unsigned kAckMs = 100;    // "within a poll (a few ms)", h264ring.h:227

    explicit AspectGuard(std::shared_ptr<IAspectStore> store = nullptr, std::function<void(const std::string&)> log = {})
        : store_(store ? std::move(store) : std::make_shared<MemAspectStore>()), log_(std::move(log)) {}

    void configure(bool enabled, std::shared_ptr<IAspectStore> store, std::function<void(const std::string&)> log) {
        std::lock_guard<std::mutex> g(mu_);
        enabled_ = enabled;
        if (store)
            store_ = std::move(store);
        log_ = std::move(log);
    }
    // A new play (or the idle session) begins: check once more when its session opens.
    // needStream (0008, a play): wait for the stream's aspect (setStream) before the one request.
    void newSession(bool needStream = false) {
        std::lock_guard<std::mutex> g(mu_);
        checked_ = false;
        needStream_ = needStream;
        stream_ = StreamAspect{};
    }
    void setStream(const StreamAspect& a) {
        std::lock_guard<std::mutex> g(mu_);
        if (!stream_.known)
            stream_ = a;
        stream_.known = true;
    }
    StreamAspect stream() const {
        std::lock_guard<std::mutex> g(mu_);
        return stream_;
    }
    bool wantsStatus() const {
        std::lock_guard<std::mutex> g(mu_);
        return enabled_ && !checked_ && (!needStream_ || stream_.known);
    }
    // Once a session, when it is open: a left-over restore first, then our defaults.
    // allowLetterbox = false (the idle session): only a left-over restore.
    void onStatus(IOverlayPort& p, const OvlStatus& s, const Sleep& sleepMs, bool allowLetterbox = true);
    // The session ends (every path). Blocks up to ackMs for the restore's sequence.
    AspectEnd endSession(IOverlayPort& p, const Sleep& sleepMs, unsigned ackMs = kAckMs);

    AspectStats stats() const {
        std::lock_guard<std::mutex> g(mu_);
        return st_;
    }
    bool requested() const {
        std::lock_guard<std::mutex> g(mu_);
        return reqAa_ || reqAr_;
    }

private:
    void logf(const std::string& s) {
        if (log_)
            log_(s);
    }
    static int request(IOverlayPort& p, bool aa, unsigned aaVal, bool ar, unsigned arVal) {
        unsigned vals[H264RING_OPT_COUNT] = {};
        unsigned mask = 0;
        if (aa) {
            mask |= 1u << H264RING_OPT_ANALOG_ASPECT;
            vals[H264RING_OPT_ANALOG_ASPECT] = aaVal;
        }
        if (ar) {
            mask |= 1u << H264RING_OPT_ASPECT;
            vals[H264RING_OPT_ASPECT] = arVal;
        }
        return p.requestSettings(mask, vals);
    }
    static int requestSaved(IOverlayPort& p, const AspectSaved& v) { return request(p, v.aa, v.aaVal, v.ar, v.arVal); }
    static std::string savedText(const AspectSaved& v) {
        std::string t;
        if (v.aa)
            t += "Analog Aspect " + std::to_string(v.aaVal);
        if (v.ar)
            t += std::string(t.empty() ? "" : ", ") + "Aspect Ratio " + std::to_string(v.arVal);
        return t;
    }
    // the core consumed request `seq` (SET_SEQ) and applied it (SET_ERR 0)
    static bool waitAck(IOverlayPort& p, int seq, const Sleep& sleepMs, unsigned ms) {
        for (unsigned t = 0;; t += 2) {
            const OvlStatus s = p.status();
            if (s.valid && s.session && s.setSeq == unsigned(seq) && s.setErr == 0)
                return true;
            if (t >= ms)
                return false;
            if (sleepMs)
                sleepMs(2);
        }
    }

    mutable std::mutex mu_;
    std::shared_ptr<IAspectStore> store_;
    std::function<void(const std::string&)> log_;
    bool enabled_ = true, checked_ = false, needStream_ = false;
    bool reqAa_ = false, reqAr_ = false;   // our value is on the request word
    StreamAspect stream_;
    int seq_ = 0;
    AspectStats st_;
};

inline void AspectGuard::onStatus(IOverlayPort& p, const OvlStatus& s, const Sleep& sleepMs, bool allowLetterbox) {
    std::lock_guard<std::mutex> g(mu_);
    if (!enabled_ || checked_ || !s.valid || !s.session || (needStream_ && !stream_.known))
        return;
    checked_ = true;   // once a session: the user's later choice wins (h264ring.h:225-227)
    AspectSaved sv;
    const bool pend = store_->load(sv);
    // a stuck value of ours is not the user's: a saved value wins over the mirror
    const unsigned userAa = pend && sv.aa ? sv.aaVal : s.osdAnalogAspect;
    const unsigned userAr = pend && sv.ar ? sv.arVal : s.osdAspect;
    const bool wantAa = allowLetterbox && s.raster.videoOut == H264RING_VOUT_PROGRESSIVE && userAa == 0;
#ifdef MUT_ASPECT43_IGNORE_USER
    const bool wantArShape = allowLetterbox && stream_.force43;   // mutant: over the user's 16:9 / 4:3 choice
#else
    const bool wantArShape = allowLetterbox && stream_.force43 && userAr == 0;
#endif
    // BUG6 (owner 22:07: the squish is ONLY in Video Output Native (HDMI)): Native shows the picture at its
    // own pixels inside the core's raster (a 960x720 picture pillarboxed in 1280x720), so a square-sample 4:3
    // picture is already right; Aspect 4:3 would squeeze the 16:9 raster. Non-native outputs scale the
    // picture to the fitted raster and still need the 4:3.
#ifndef MUT_ASPECT43_NATIVE_SQUARE
    const bool nativeSquare = s.raster.videoOut == H264RING_VOUT_NATIVE && stream_.square43;
    if (wantArShape && nativeSquare)
        logf("aspect: a square-sample 4:3 picture on Video Output Native (HDMI): shown at its own pixels, "
             "pillarboxed; no Aspect Ratio 4:3 request");
    const bool wantAr = wantArShape && !nativeSquare;
#else
    const bool wantAr = wantArShape;
#endif
    if (!wantAa && !wantAr) {
        if (!pend)
            return;
        // the way back from an earlier session that could not restore
        const int rc = requestSaved(p, sv);
        if (rc > 0 && waitAck(p, rc, sleepMs, kAckMs)) {
            p.clearSettings();
            store_->erase();
            ++st_.startRestores;
            logf("aspect: restored the user's " + savedText(sv) + " left from an earlier session");
        } else {
            logf("aspect: the left-over restore (" + savedText(sv) + ") was not taken (" + std::to_string(rc) +
                 "); kept for the next session");
        }
        return;
    }
    AspectSaved ns = pend ? sv : AspectSaved{};
    if (wantAa && !ns.aa) {
        ns.aa = true;
        ns.aaVal = userAa;
    }
    if (wantAr && !ns.ar) {
        ns.ar = true;
        ns.arVal = userAr;
    }
    if (!store_->save(ns))
        logf("aspect: the state file did not save; restoring from memory only");
    // one request: ours where wanted, a left-over user value back elsewhere (the word holds one request)
    const int rc = request(p, ns.aa, wantAa ? kLetterbox : ns.aaVal, ns.ar, wantAr ? kAspect43 : ns.arVal);
    if (rc > 0) {
        reqAa_ = wantAa;
        reqAr_ = wantAr;
        seq_ = rc;
        if (wantAa) {
            ++st_.letterboxRequests;
            logf("aspect: Analog Aspect Auto at 31 kHz progressive -> Letterbox (request " + std::to_string(rc) +
                 "), the user's " + std::to_string(userAa) + " saved");
        }
        if (wantAr) {
            ++st_.aspect43Requests;
            logf("aspect: a VUI-less 4:3 stream with Aspect Ratio Auto -> 4:3 (request " + std::to_string(rc) +
                 "), the user's " + std::to_string(userAr) + " saved");
        }
    } else {
        logf("aspect: the request failed (" + std::to_string(rc) + ")");
        if (pend)
            store_->save(sv);
        else
            store_->erase();
    }
}

inline AspectEnd AspectGuard::endSession(IOverlayPort& p, const Sleep& sleepMs, unsigned ackMs) {
    std::lock_guard<std::mutex> g(mu_);
    checked_ = false;
    AspectSaved sv;
    if (!store_->load(sv)) {
        reqAa_ = reqAr_ = false;
        return AspectEnd::Nothing;
    }
#ifdef MUT_ASPECT_NO_RESTORE
    (void)sleepMs;
    (void)ackMs;
    p.clearSettings();   // mutant: T6's behaviour -- drop the request, Letterbox sticks on the menu
    store_->erase();
    reqAa_ = reqAr_ = false;
    return AspectEnd::Restored;
#else
    const OvlStatus s = p.status();
    if ((reqAa_ || reqAr_) && s.valid && s.session && s.setSeq == unsigned(seq_)) {
        // our request was consumed: an option no longer showing ours is the user's choice
        if (reqAa_ && s.osdAnalogAspect != kLetterbox && sv.aa) {
            sv.aa = false;
            logf("aspect: the user chose Analog Aspect " + std::to_string(s.osdAnalogAspect) + " during the session: kept");
        }
        if (reqAr_ && s.osdAspect != kAspect43 && sv.ar) {
            sv.ar = false;
            logf("aspect: the user chose Aspect Ratio " + std::to_string(s.osdAspect) + " during the session: kept");
        }
        if (!sv.any()) {
            p.clearSettings();
            store_->erase();
            reqAa_ = reqAr_ = false;
            ++st_.userChanged;
            return AspectEnd::UserChanged;
        }
    }
#ifdef MUT_ASPECT43_NO_RESTORE
    sv.ar = false;   // mutant: the 4:3 override is dropped from the restore (it sticks on the menu)
    if (!sv.any()) {
        p.clearSettings();
        store_->erase();
        reqAa_ = reqAr_ = false;
        return AspectEnd::Restored;
    }
#endif
    const int rc = requestSaved(p, sv);
    const bool ok = rc > 0 && waitAck(p, rc, sleepMs, ackMs);
    reqAa_ = reqAr_ = false;
#ifndef MUT_ASPECT_CLEAR_UNACKED
    if (!ok) {
        store_->save(sv);   // without the options the user took back
        ++st_.pending;
        logf("aspect: the restore of " + savedText(sv) + " was not taken (" + std::to_string(rc) +
             "): the request stays for the next session's start, the state file keeps it");
        return AspectEnd::Pending;
    }
#endif
    p.clearSettings();
    store_->erase();
    ++st_.restores;
    logf("aspect: the user's " + savedText(sv) + " restored");
    return ok ? AspectEnd::Restored : AspectEnd::Pending;
#endif
}

} // namespace misterplex::owenb_osd
