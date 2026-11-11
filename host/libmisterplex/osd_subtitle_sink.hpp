#pragma once
// Production SubtitleSink: fetch a text sidecar, parse cues, serve lines at mediaMs.
// Caption RAM is a 1 MiB island. It never touches ImageCache / 0012.

#include "embedded_text_part.hpp"
#include "subtitle_cues.hpp"
#include "subtitle_sink.hpp"

#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

namespace mpx {

class OsdSubtitleSink : public SubtitleSink {
public:
    using FetchFn = std::function<std::string(const std::string& url, const std::string& token, std::string& err)>;

    void setFetch(FetchFn f) { fetch_ = std::move(f); }
    void setPmsBaseProvider(std::function<std::string()> p) { baseProvider_ = std::move(p); }

    void setSubtitleTrack(const SubtitleTrackSpec& spec) override {
        const bool localSoft = isLocalSoftSidecarSpec(spec);
        if (!kindIsSoftText(spec.kind) || (spec.streamId <= 0 && !localSoft)) {
            clearLocked(spec.playEpoch);
            return;
        }
        std::string url;
        std::string token;
        if (localSoft) {
            url = stripPlexTokenParam(spec.url);
        } else {
            token = tokenProvider_ ? tokenProvider_() : std::string();
            const std::string base = baseProvider_ ? baseProvider_() : std::string();
            std::string path = spec.url.empty() ? sidecarStreamKey(spec.streamId) : spec.url;
            path = stripPlexTokenParam(path);
            url = joinUrl(base, path);
            if (url.find('?') == std::string::npos)
                url += "?encoding=utf-8";
            else if (url.find("encoding=") == std::string::npos)
                url += "&encoding=utf-8";
            url = stripPlexTokenParam(url);
        }

        std::string err;
        std::string body;
        if (fetch_)
            body = fetch_(url, token, err);
        else
            err = "no fetch";
        // Part-text loader still running: do not arm the 64-byte placeholder cue
        // (GH#44: caption dump text="" until ok=1 rearm).
        if (body == misterplex::kPartTextPendingSrt)
            return;
        if (!err.empty() || body.empty()) {
            if (err.empty())
                err = "empty sidecar";
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (spec.playEpoch < epoch_)
                    return;
                epoch_ = spec.playEpoch;
                streamId_ = spec.streamId;
                armed_ = false;
                cues_ = {};
                styled_ = false;
                overflow_ = false;
            }
            if (onSubtitleUnavailable)
                onSubtitleUnavailable(spec.playEpoch, spec.streamId, err);
            return;
        }

        // A non-empty body that carries no cue timing is an error page (e.g. PMS 501 for an embedded stream),
        // not an empty caption track: report it unavailable instead of arming silently.
        if (body.find("-->") == std::string::npos && body.find("[Events]") == std::string::npos) {
            std::string head = body.substr(0, 60);
            for (char& c : head)
                if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
                    c = '.';
            std::fprintf(stderr, "misterplexd: subtitle sidecar stream=%lld not subtitle text body_len=%zu head=%s\n",
                         static_cast<long long>(spec.streamId), body.size(), head.c_str());
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (spec.playEpoch < epoch_)
                    return;
                epoch_ = spec.playEpoch;
                streamId_ = spec.streamId;
                armed_ = false;
                cues_ = {};
                styled_ = false;
                overflow_ = false;
            }
            if (onSubtitleUnavailable)
                onSubtitleUnavailable(spec.playEpoch, spec.streamId, "not subtitle text");
            return;
        }

        misterplex::CueList cues = spec.kind == SubtitleTrackSpec::Kind::TextAss ? misterplex::parseAssCues(body)
                                    : misterplex::bodyLooksWebVtt(body)          ? misterplex::parseWebVttCues(body)
                                                                                 : misterplex::parseSrtCues(body);

        std::lock_guard<std::mutex> lock(mu_);
        if (spec.playEpoch < epoch_)
            return;
        epoch_ = spec.playEpoch;
        streamId_ = spec.streamId;
        cues_ = std::move(cues);
        armed_ = true;
        styled_ = cues_.hadOverrides;
        overflow_ = cues_.overflow;
        toastStyled_ = cues_.hadOverrides;
        toastOverflow_ = cues_.overflow;
        if (cues_.cues.empty()) {
            std::string head = body.substr(0, 80);
            for (char& c : head)
                if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
                    c = '.';
            std::fprintf(stderr, "misterplexd: subtitle sidecar empty parse body_len=%zu token=%d head=%s\n", body.size(),
                         token.empty() ? 0 : 1, head.c_str());
        }
        std::fprintf(stderr, "misterplexd: subtitle sidecar stream=%lld cues=%zu bytes=%zu\n",
                     static_cast<long long>(spec.streamId), cues_.cues.size(), cues_.bytes);
    }

    void clearSubtitleTrack(uint64_t playEpoch) override { clearLocked(playEpoch); }

    misterplex::CaptionLines linesAt(int64_t mediaMs) const {
        std::lock_guard<std::mutex> lock(mu_);
        misterplex::CaptionLines o = misterplex::captionAt(cues_, mediaMs);
        o.armed = armed_;
        return o;
    }

    bool armed() const {
        std::lock_guard<std::mutex> lock(mu_);
        return armed_;
    }
    uint64_t epoch() const {
        std::lock_guard<std::mutex> lock(mu_);
        return epoch_;
    }
    int64_t streamId() const {
        std::lock_guard<std::mutex> lock(mu_);
        return streamId_;
    }
    size_t cueBytes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cues_.bytes;
    }
    size_t cueCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cues_.cues.size();
    }
    bool takeStyledToast() {
        std::lock_guard<std::mutex> lock(mu_);
        const bool t = toastStyled_;
        toastStyled_ = false;
        return t;
    }
    bool takeOverflowToast() {
        std::lock_guard<std::mutex> lock(mu_);
        const bool t = toastOverflow_;
        toastOverflow_ = false;
        return t;
    }

    static std::string joinUrl(std::string base, const std::string& path) {
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
            return path;
        if (base.empty())
            return path;
        if (path.empty() || path[0] == '/')
            return base + path;
        return base + "/" + path;
    }

private:
    void clearLocked(uint64_t playEpoch) {
        std::lock_guard<std::mutex> lock(mu_);
        if (playEpoch < epoch_)
            return;
        epoch_ = playEpoch;
        streamId_ = 0;
        armed_ = false;
        cues_ = {};
        styled_ = false;
        overflow_ = false;
        toastStyled_ = false;
        toastOverflow_ = false;
    }

    FetchFn fetch_;
    std::function<std::string()> baseProvider_;
    mutable std::mutex mu_;
    uint64_t epoch_ = 0;
    int64_t streamId_ = 0;
    bool armed_ = false;
    misterplex::CueList cues_{};
    bool styled_ = false;
    bool overflow_ = false;
    bool toastStyled_ = false;
    bool toastOverflow_ = false;
};

} // namespace mpx
