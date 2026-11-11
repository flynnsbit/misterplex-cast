#pragma once
// Empty subtitle seam for Cursor Box's soft-sub renderer.
//
// Production binds OsdSubtitleSink: sidecar SRT/ASS is fetched and painted on
// the overlay band. Image and embedded tracks are never passed to
// setSubtitleTrack; they clear a soft track the sink was showing. Fetch/parse
// failure toasts and stays Direct Play unless SUBTITLES=burn.
//
// No token is stored on SubtitleTrackSpec. The sink asks setTokenProvider when
// it needs one.
//
// url is the sidecar stream key path, for example /library/streams/12. It has
// no token and no query. The renderer adds ?encoding=utf-8 and the token at
// fetch time. TextSrt and TextAss are sidecar streams only. Embedded text
// (mov_text, webvtt, ttml, and the other container text codecs) is
// Kind::EmbeddedText: GET /library/streams/<id> returns 501, so this player
// burns those in and never calls setSubtitleTrack with them. Image tracks are
// the same. canAutoSync is the PMS Stream attribute canAutoSync="1". The
// renderer sends autoAdjustSubtitle=1 only when that is true.
//
// Timing seam: cue timing belongs to OwenbPlaySession::mediaMs() (landing +
// STC). Do not sample the play thread's bar / hold position for a soft cue.

#include "soft_subs.hpp"
#include "../../arm/misterplexd/log_redact.hpp"

#include <cstdio>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace mpx {

struct SubtitleTrackSpec {
    uint64_t playEpoch = 0;
    int64_t streamId = 0;
    // PacketText: SOFT_SUBS on, embedded text decoded from container packets.
    // Not fetched from /library/streams (that URL is 501 for embedded). Flag off
    // never produces this kind; offer then burns, matching EmbeddedText.
    enum class Kind { None, TextSrt, TextAss, EmbeddedText, Image, PacketText } kind = Kind::None;
    // Sidecar key path only: /library/streams/<id>. No token, no query.
    std::string url;
    std::string codec;
    std::string language;
    bool forced = false;
    // PMS Stream canAutoSync="1". The renderer may send autoAdjustSubtitle=1 only then.
    bool canAutoSync = false;
};

class SubtitleSink {
public:
    virtual ~SubtitleSink() = default;
    virtual void setSubtitleTrack(const SubtitleTrackSpec&) = 0;
    virtual void clearSubtitleTrack(uint64_t playEpoch) = 0;
    void setTokenProvider(std::function<std::string()> provider) { tokenProvider_ = std::move(provider); }
    // The sink reports that it cannot show this stream. The player burns in
    // when playEpoch is the live one and drops the call when it is stale.
    std::function<void(uint64_t playEpoch, int64_t streamId, std::string reason)> onSubtitleUnavailable;

protected:
    std::function<std::string()> tokenProvider_;
};

// Default no-op. Production binds OsdSubtitleSink so text returns Soft.
class NullSubtitleSink : public SubtitleSink {
public:
    void setSubtitleTrack(const SubtitleTrackSpec&) override {}
    void clearSubtitleTrack(uint64_t) override {}
};

// What the player should do after offering a track to the sink.
enum class SubtitleRoute : uint8_t { Off, Soft, Burn };

// Drop X-Plex-Token from a URL before it is handed to a sink. The token is not a field.
inline std::string stripPlexTokenParam(std::string url) {
    const auto hash = url.find('#');
    const auto end = hash == std::string::npos ? url.size() : hash;
    const auto q = url.find('?');
    if (q == std::string::npos || q > end)
        return url;
    const std::string frag = hash == std::string::npos ? std::string() : url.substr(hash);
    const std::string query = url.substr(q + 1, end - (q + 1));
    std::string kept;
    size_t i = 0;
    while (i < query.size()) {
        auto amp = query.find('&', i);
        if (amp == std::string::npos)
            amp = query.size();
        const std::string part = query.substr(i, amp - i);
        if (!misterplex::hasPlexTokenKey(part.substr(0, part.find('='))) && !part.empty()) {
            if (!kept.empty())
                kept.push_back('&');
            kept += part;
        }
        i = amp + 1;
    }
    if (kept.empty())
        return url.substr(0, q) + frag;
    return url.substr(0, q) + "?" + kept + frag;
}

inline bool kindIsSoftText(SubtitleTrackSpec::Kind kind) {
    return kind == SubtitleTrackSpec::Kind::TextSrt || kind == SubtitleTrackSpec::Kind::TextAss;
}

// On-device OpenSubtitles cache (or other local sidecar). streamId stays 0 on the wire.
inline bool isLocalSoftSidecarUrl(const std::string& url) {
    return !url.empty() && (url.rfind("/media/fat/", 0) == 0 || url.rfind("file:", 0) == 0);
}

inline bool isLocalSoftSidecarSpec(const SubtitleTrackSpec& spec) {
    return kindIsSoftText(spec.kind) && isLocalSoftSidecarUrl(spec.url);
}

// Soft overlay (sidecar SRT/ASS) always paints. SOFT_SUBS off still drops
// PacketText except mov_text / "text" (#260 overlay). When
// softOverlayAllowed is false (MUT_SOFTSUBS_OFF_OVERLAY), sidecars are dropped
// too — that mutant must fail the host test.
inline SubtitleTrackSpec refuseSoftOverlayWhenOff(SubtitleTrackSpec spec) {
    if (!misterplex::softOverlayAllowed()) {
        if (kindIsSoftText(spec.kind) || spec.kind == SubtitleTrackSpec::Kind::PacketText) {
            spec.kind = SubtitleTrackSpec::Kind::None;
            spec.streamId = 0;
            spec.url.clear();
        }
        return spec;
    }
    if (!misterplex::softSubsEnabled() && spec.kind == SubtitleTrackSpec::Kind::PacketText &&
        !misterplex::packetTextAlwaysOverlay(spec.codec)) {
        spec.kind = SubtitleTrackSpec::Kind::None;
        spec.streamId = 0;
        spec.url.clear();
    }
    return spec;
}

// #334: a relative Part text path from softTextFetchPath survives paintSoft.
inline bool partTextPathKept(const std::string& url) {
#ifdef MUT_334_PAINT_REWRITES_PART
    (void)url;
    return false;
#else
    // Same shape misterplex::parsePartTextUrl accepts (not included here: header cycle).
    const auto q = url.find('?');
    const auto m = url.find("mpxTextIndex=");
    return url.rfind("/library/parts/", 0) == 0 && q != std::string::npos && m == q + 1 &&
           m + 13 < url.size() && url[m + 13] >= '0' && url[m + 13] <= '9' &&
           !misterplex::hasPlexTokenKey(url);
#endif
}

// Sidecar key the renderer fetches. No scheme, no query, no token.
inline std::string sidecarStreamKey(int64_t streamId) {
    return "/library/streams/" + std::to_string(streamId);
}

// srt/ass sidecars are offered to the sink. Embedded text and image codecs are
// burn-only: they are never passed to setSubtitleTrack, and a painted soft
// track is cleared so the sink does not keep text the player is burning.
inline SubtitleTrackSpec::Kind kindForCodec(const std::string& codec) {
    std::string c;
    c.reserve(codec.size());
    for (unsigned char ch : codec)
        c.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    if (c == "srt" || c == "subrip")
        return SubtitleTrackSpec::Kind::TextSrt;
    if (c == "ass" || c == "ssa")
        return SubtitleTrackSpec::Kind::TextAss;
    if (c == "mov_text" || c == "text" || c == "webvtt" || c == "vtt" || c == "ttml" || c == "subt" || c == "sub" ||
        c == "smi")
        return SubtitleTrackSpec::Kind::EmbeddedText;
    if (c == "pgs" || c == "pgssub" || c == "hdmv_pgs_subtitle" || c == "vobsub" || c == "dvd_subtitle" ||
        c == "dvdsub" || c == "dvb_subtitle" || c == "dvbsub" || c == "xsub")
        return SubtitleTrackSpec::Kind::Image;
    return SubtitleTrackSpec::Kind::None;
}

// Player-side owner of one sink. Image and embedded tracks return Burn and do
// not call setSubtitleTrack; they clear a soft track that was already painted.
// Text calls setSubtitleTrack. NullSubtitleSink then returns Burn. OsdSubtitleSink
// (production) returns Soft: the player must not also put subtitles=burn on the
// universal URL. onSubtitleUnavailable burns only when SUBTITLES=burn and the
// epoch is live.
//
// mu_ guards epoch_, live_, streamId_, ticket_, latest_, burn_, and positionMs_.
// It is a leaf lock: setSubtitleTrack, clearSubtitleTrack, and the burn callback
// run outside it. Holding it across those calls deadlocks: the sink may call
// onSubtitleUnavailable, and burn restarts playback which re-enters this object.
// sinkMu_ serializes those sink calls so two offers cannot interleave. It is
// recursive because a sink may re-enter offer or endSoft from setSubtitleTrack.
// endSoft drops live_ and bumps ticket_ under mu_ before clearSubtitleTrack.
// If this offer lost the ticket, it re-applies the latest soft spec instead of
// clearing the winner. A stop (not live) still clears.
class SubtitleControl {
public:
    explicit SubtitleControl(SubtitleSink& sink) : sink_(&sink) { bind(); }

    SubtitleControl(const SubtitleControl&) = delete;
    SubtitleControl& operator=(const SubtitleControl&) = delete;

    ~SubtitleControl() {
        if (sink_)
            sink_->onSubtitleUnavailable = nullptr;
    }

    // Point the controller at another sink. Clears the previous callback first.
    // Call before hooks run; a hook already installed stays on the old sink.
    void rebind(SubtitleSink& sink) {
        std::lock_guard<std::recursive_mutex> sinkLock(sinkMu_);
        if (sink_)
            sink_->onSubtitleUnavailable = nullptr;
        sink_ = &sink;
        bind();
    }

    void setBurn(std::function<void(int64_t positionMs, int64_t streamId)> burn) {
        std::lock_guard<std::mutex> lock(mu_);
        burn_ = std::move(burn);
    }
    void setPositionMs(std::function<int64_t()> positionMs) {
        std::lock_guard<std::mutex> lock(mu_);
        positionMs_ = std::move(positionMs);
    }
    // #258: the sink could not fetch a soft-text track (PMS 501 for an embedded
    // SRT). The hook gets the first try; true means it took the track (demux
    // fallback onto the same overlay) and nothing is burned. Called outside mu_.
    // True when the user opted into burn as the last resort (SUBTITLE_MODE=burn).
    void setSoftTextBurnOptIn(std::function<bool()> optIn) {
        std::lock_guard<std::mutex> lock(mu_);
        softTextBurnOptIn_ = std::move(optIn);
    }

    void setTextFallback(std::function<bool(uint64_t playEpoch, int64_t streamId)> fallback) {
        std::lock_guard<std::mutex> lock(mu_);
        textFallback_ = std::move(fallback);
    }

    SubtitleRoute playStarted(const SubtitleTrackSpec& spec) { return offer(spec); }
    SubtitleRoute trackChanged(const SubtitleTrackSpec& spec) { return offer(spec); }

    void subtitlesOff(uint64_t playEpoch) { endSoft(playEpoch); }
    void stopped(uint64_t playEpoch) { endSoft(playEpoch); }
    // A play session ended on its own thread (EOF, stream error, cast teardown) rather than through MediaPlayer::stop().
    // Only the live session may clear: a stale end (a newer play already owns the sink) reports false and touches
    // nothing. The caller then clears the caption model it publishes (UiModel::clearCaptions).
    bool sessionEnded(uint64_t endedEpoch, uint64_t liveEpoch) {
        if (endedEpoch != liveEpoch)
            return false;
        endSoft(endedEpoch);
        return true;
    }

    bool live() const {
        std::lock_guard<std::mutex> lock(mu_);
        return live_;
    }
    uint64_t epoch() const {
        std::lock_guard<std::mutex> lock(mu_);
        return epoch_;
    }

private:
    void bind() {
        sink_->onSubtitleUnavailable = [this](uint64_t playEpoch, int64_t streamId, std::string reason) {
            unavailable(playEpoch, streamId, std::move(reason));
        };
    }

    void endSoft(uint64_t playEpoch) {
        std::lock_guard<std::recursive_mutex> sinkLock(sinkMu_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++ticket_;
            if (playEpoch == epoch_)
                live_ = false;
        }
        sink_->clearSubtitleTrack(playEpoch);
    }

    void paintSoft(const SubtitleTrackSpec& spec) {
        SubtitleTrackSpec clean = spec;
        // Prefer a PMS stream key or a local OpenSubtitles cache path already on
        // the spec. Fall back to /library/streams/<id>.
        // #334: keep the embedded-srt Part path ("/library/parts/...?mpxTextIndex=N");
        // rewriting it to /library/streams/<id> is the PMS 501 the fix exists to avoid.
        if (clean.url.empty() || clean.url.find("://") != std::string::npos ||
            (clean.url.rfind("/library/streams/", 0) != 0 && clean.url.rfind("/media/fat/", 0) != 0 &&
             clean.url.rfind("file:", 0) != 0 && !partTextPathKept(clean.url)))
            clean.url = sidecarStreamKey(spec.streamId);
        clean.url = stripPlexTokenParam(clean.url);
        sink_->setSubtitleTrack(clean);
    }

    // Called with sinkMu_ held and mu_ not held.
    void settleStale(uint64_t ticket, uint64_t playEpoch) {
        bool stale = false;
        bool live = false;
        SubtitleTrackSpec latest;
        {
            std::lock_guard<std::mutex> lock(mu_);
            stale = ticket_ != ticket || !live_;
            live = live_;
            latest = latest_;
        }
        if (!stale)
            return;
        // The winner is a newer soft track. Clearing here would wipe it.
        if (live && kindIsSoftText(latest.kind) &&
            (latest.streamId > 0 || isLocalSoftSidecarSpec(latest))) {
            paintSoft(latest);
            return;
        }
        sink_->clearSubtitleTrack(playEpoch);
    }

    SubtitleRoute offer(const SubtitleTrackSpec& spec) {
        std::lock_guard<std::recursive_mutex> sinkLock(sinkMu_);
        if (spec.kind == SubtitleTrackSpec::Kind::PacketText) {
            uint64_t ticket = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ticket = ++ticket_;
                epoch_ = spec.playEpoch;
                streamId_ = spec.streamId;
                latest_ = spec;
                live_ = spec.streamId > 0;
            }
            // No sidecar fetch. The demuxer paints from decoded packets.
            sink_->clearSubtitleTrack(spec.playEpoch);
            const bool show = misterplex::softSubsEnabled() && spec.streamId > 0 &&
                              dynamic_cast<NullSubtitleSink*>(sink_) == nullptr;
            settleStale(ticket, spec.playEpoch);
            if (spec.streamId <= 0)
                return SubtitleRoute::Off;
            return show ? SubtitleRoute::Soft : SubtitleRoute::Burn;
        }
        const bool soft = kindIsSoftText(spec.kind);
#ifndef MUT_LOCAL_SUB_STREAMID_OFF
        const bool localSoft = isLocalSoftSidecarSpec(spec);
#else
        const bool localSoft = false;
#endif
        // Embedded text, image, and an unclassified id (Kind::None with an id)
        // burn in. None of them are passed to setSubtitleTrack. A painted soft
        // track is cleared. No id is off unless a local sidecar path is offered.
        const bool burnOnly = spec.streamId > 0 && !soft;
        const bool off = spec.streamId <= 0 && !localSoft;
        uint64_t ticket = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            ticket = ++ticket_;
            epoch_ = spec.playEpoch;
            streamId_ = spec.streamId;
            latest_ = spec;
            live_ = !off;
        }
        // Outside mu_. sinkMu_ is still held, so this clear cannot land after a
        // newer soft offer. live_ stays true: burn is the active choice.
        if (burnOnly) {
            sink_->clearSubtitleTrack(spec.playEpoch);
            return SubtitleRoute::Burn;
        }
        if (off) {
            sink_->clearSubtitleTrack(spec.playEpoch);
            return SubtitleRoute::Off;
        }
        if (localSoft)
            std::fprintf(stderr, "misterplexd: subtitle local srt soft path=%s\n", misterplex::logUrl(spec.url).c_str());
        paintSoft(spec);
        const SubtitleRoute route =
            dynamic_cast<NullSubtitleSink*>(sink_) != nullptr ? SubtitleRoute::Burn : SubtitleRoute::Soft;
        settleStale(ticket, spec.playEpoch);
        return route;
    }

    void unavailable(uint64_t playEpoch, int64_t streamId, std::string reason) {
        (void)reason;
        std::function<void(int64_t, int64_t)> burn;
        std::function<int64_t()> positionMs;
        std::function<bool(uint64_t, int64_t)> fallback;
        std::function<bool()> optIn;
        bool softText = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!live_ || playEpoch != epoch_)
                return;
            burn = burn_;
            positionMs = positionMs_;
            fallback = textFallback_;
            optIn = softTextBurnOptIn_;
            softText = kindIsSoftText(latest_.kind) && latest_.streamId == streamId;
        }
#ifndef MUT_258_NO_FALLBACK
        if (softText && fallback && fallback(playEpoch, streamId))
            return;
#endif
        // Spec #252: SRT/ASS go to the overlay. Burn is only the last resort the
        // user opted into (SUBTITLE_MODE=burn, reviewer 2026-11-01) when no overlay
        // path (sidecar or #258 demux) can serve the track.
#ifndef MUT_258_BURN_SRT
#ifdef MUT_258_OPTIN_IGNORED
        if (softText)
            return;
#else
        if (softText && !(optIn && optIn()))
            return;
#endif
#endif
        if (burn)
            burn(positionMs ? positionMs() : 0, streamId);
    }

    SubtitleSink* sink_ = nullptr;
    // Held across setSubtitleTrack and clearSubtitleTrack. A synchronous sink
    // calls onSubtitleUnavailable, and thus burn, before set returns, so this
    // lock is held across that re-entry (that is why it is recursive). mu_ is
    // not held across set, clear, or burn.
    std::recursive_mutex sinkMu_;
    mutable std::mutex mu_;
    uint64_t ticket_ = 0;
    uint64_t epoch_ = 0;
    int64_t streamId_ = 0;
    SubtitleTrackSpec latest_{};
    bool live_ = false;
    std::function<void(int64_t, int64_t)> burn_;
    std::function<int64_t()> positionMs_;
    std::function<bool(uint64_t, int64_t)> textFallback_;
    std::function<bool()> softTextBurnOptIn_;
};

// Sink plus the one-time hook install MediaPlayer uses. Production passes
// NullSubtitleSink. A test passes its own sink, or rebind() before hooks run.
class SubtitleWire {
public:
    explicit SubtitleWire(SubtitleSink& sink) : sink_(&sink), ctrl_(sink) {}

    void rebind(SubtitleSink& sink) {
        sink_ = &sink;
        ctrl_.rebind(sink);
    }

    SubtitleSink& sink() const { return *sink_; }
    SubtitleControl& control() { return ctrl_; }

    template <class F>
    void ensureHooks(F&& install) {
        std::call_once(hooksOnce_, static_cast<F&&>(install));
    }

private:
    SubtitleSink* sink_ = nullptr;
    SubtitleControl ctrl_;
    std::once_flag hooksOnce_;
};

} // namespace mpx
