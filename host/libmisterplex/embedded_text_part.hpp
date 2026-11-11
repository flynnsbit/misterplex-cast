#pragma once
// #334: an EMBEDDED text subtitle (srt inside the mkv, no external sidecar) has no
// /library/streams/<id> body: PMS answers 501 Not Implemented. The #258 packet
// fallback demuxes the input the daemon plays, which on a universal transcode
// (start.mp4 ... subtitles=embedded) holds only video + audio, so it never finds
// the track either (glass lab lane 4, 2026-11-04 05:20 CT).
//
// Fix (option A, EVID-334-probes P3: Part GET honours Range, 206): read the text
// track from the original Part (/library/parts/<id>/<ts>/file.ext), token only in
// an X-Plex-Token request header, never in the URL, and give the overlay an SRT
// body built from the demuxed packets. The video play URL is unchanged.
// Pure pieces live here (host-testable); the libav demux is extractPartTextSrt in
// arm/misterplexd/owenb_av_source.cpp. Nothing here burns or PUTs.

#include "../../arm/misterplexd/log_redact.hpp"
#include "track_select.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace misterplex {

constexpr const char* kPartTextIndexParam = "mpxTextIndex=";
constexpr size_t kPartTextMaxBytes = 1u << 20;  // the caption island (osd_subtitle_sink.hpp)

inline bool partTextCodecSupported(const std::string& codec) {
    const std::string c = trackLower(codec);
    return c == "srt" || c == "subrip";
}

inline bool partKeyLooksLikePart(const std::string& partKey) {
    return partKey.rfind("/library/parts/", 0) == 0 && partKey.find('?') == std::string::npos &&
           !misterplex::hasPlexTokenKey(partKey);
}

// The path the overlay fetches for one soft-text pick (MediaPlayer::makeSubtitleSpec).
//  - external sidecar (location=external, or a streams key with no container index): its key.
//  - embedded srt with a container index and a known Part key: the Part, marked with
//    the container index ("...file.mkv?mpxTextIndex=2").
//  - otherwise the old /library/streams/<id> key (the #258 packet fallback still runs).
inline std::string softTextFetchPath(const MediaStream* s, int64_t streamId, const std::string& partKey) {
    const std::string streamsKey = "/library/streams/" + std::to_string(streamId);
    if (!s)
        return streamsKey;
    const bool hasStreamsKey = s->key.rfind("/library/streams/", 0) == 0;
    const std::string keyed = hasStreamsKey ? s->key : streamsKey;
    if (trackLower(s->location) == "external" || (hasStreamsKey && s->index < 0))
        return keyed;
#ifdef MUT_334_STREAMS_KEY_FOR_EMBEDDED
    (void)partKey;
#else
    if (s->index >= 0 && partTextCodecSupported(s->codec) && partKeyLooksLikePart(partKey))
        return partKey + "?" + kPartTextIndexParam + std::to_string(s->index);
#endif
    return keyed;
}

// Cache key is the Part path + index, not the scheme/host. Rearm after a WAN
// extract used http://IP/... while joinUrl later uses plex.direct (or the
// reverse) and must not cancel a Ready 24 KiB body for a 64-byte pending stub
// (GH#44 glass: ok=1 bytes=24491 then cues=1 bytes=64).
inline std::string partTextCacheKey(const std::string& partUrl, int index) {
    std::string p = partUrl;
    const auto lp = p.find("/library/parts/");
    if (lp != std::string::npos)
        p = p.substr(lp);
    const auto q = p.find('?');
    if (q != std::string::npos)
        p.resize(q);
    return p + "#" + std::to_string(index);
}

// Fetch side: url is "<base>/library/parts/...?mpxTextIndex=N[&encoding=utf-8]".
// partUrl gets the Part URL with no query at all (nothing can ride on it).
inline bool parsePartTextUrl(const std::string& url, std::string& partUrl, int& index) {
    const auto q = url.find('?');
    const auto lp = url.find("/library/parts/");
    if (q == std::string::npos || lp == std::string::npos || lp > q)
        return false;
    const auto m = url.find(kPartTextIndexParam, q);
    if (m == std::string::npos || (url[m - 1] != '?' && url[m - 1] != '&'))
        return false;
    size_t p = m + std::char_traits<char>::length(kPartTextIndexParam);
    if (p >= url.size() || url[p] < '0' || url[p] > '9')
        return false;
    long v = 0;
    while (p < url.size() && url[p] >= '0' && url[p] <= '9') {
        v = v * 10 + (url[p++] - '0');
        if (v > 9999)
            return false;
    }
    partUrl = url.substr(0, q);
    index = static_cast<int>(v);
    return true;
}

// The Part request carries the token only as a header line (empty token: none).
inline std::string partTextHeaders(const std::string& token) {
#ifdef MUT_334_NO_TOKEN_HEADER
    (void)token;
    return {};
#else
    return token.empty() ? std::string() : "X-Plex-Token: " + token + "\r\n";
#endif
}

// Part@key for the Part the play uses (partId > 0: that Part; else the first one).
inline std::string partKeyFromXml(const std::string& xml, int partId) {
    size_t p = 0;
    while ((p = xml.find("<Part", p)) != std::string::npos) {
        const auto e = xml.find('>', p);
        if (e == std::string::npos)
            return {};
        const std::string tag = xml.substr(p, e - p);
        p = e;
        if (tag.size() > 5 && tag[5] != ' ' && tag[5] != '\t' && tag[5] != '\n')
            continue;  // <Parts ...> or similar
        if (partId > 0 && xmlAttrInt(tag, "id", 0) != partId)
            continue;
        const std::string key = xmlTagAttr(tag, "key");
        return partKeyLooksLikePart(key) ? key : std::string();
    }
    return {};
}

struct PartTextPacket {
    int64_t startMs = 0;
    int64_t durMs = 0;
    std::string text;
};

inline std::string srtTimestamp(int64_t ms) {
    if (ms < 0)
        ms = 0;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", static_cast<long long>(ms / 3600000),
                  static_cast<long long>((ms / 60000) % 60), static_cast<long long>((ms / 1000) % 60),
                  static_cast<long long>(ms % 1000));
    return buf;
}

// SRT body from demuxed subrip packets (start order kept). Empty text and
// non-positive durations are skipped. Stops before kPartTextMaxBytes.
inline std::string srtFromPartPackets(const std::vector<PartTextPacket>& pkts) {
    std::string out;
    int n = 0;
    for (const PartTextPacket& p : pkts) {
        std::string t = p.text;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == '\0'))
            t.pop_back();
        if (t.empty() || p.durMs <= 0)
            continue;
        std::string cue = std::to_string(n + 1) + "\n" + srtTimestamp(p.startMs) + " --> " +
                          srtTimestamp(p.startMs + p.durMs) + "\n" + t + "\n\n";
#ifndef MUT_334_NO_SIZE_CAP
        if (out.size() + cue.size() > kPartTextMaxBytes)
            break;
#endif
        out += cue;
        ++n;
    }
    return out;
}

// The sink fetch runs on the play-open path, and a Part read walks the whole file
// (mkv interleaves text), so the read never blocks the open: the first fetch for a
// (Part, index) starts one worker and arms an empty placeholder track; when the
// worker finishes, done() re-arms the sink, and that fetch gets the cached body (or
// the cached error, which then takes the normal unavailable path: toast, or burn
// only on the SUBTITLE_MODE=burn opt-in).
constexpr const char* kPartTextPendingSrt = "1\n00:00:00,000 --> 00:00:00,001\n \n\n";

class PartTextLoader {
public:
    using Extract = std::function<bool(const std::string& partUrl, const std::string& headers, int index,
                                       const std::function<bool()>& cancelled, std::string& srt,
                                       std::string& err)>;
    using Done = std::function<void(const std::string& partUrl, int index)>;

    PartTextLoader() = default;
    PartTextLoader(const PartTextLoader&) = delete;
    PartTextLoader& operator=(const PartTextLoader&) = delete;
    ~PartTextLoader() { stop(); }

    void setExtract(Extract e) { extract_ = std::move(e); }
    void setDone(Done d) { done_ = std::move(d); }

    std::string fetch(const std::string& partUrl, const std::string& headers, int index, std::string& err) {
        const std::string key = partTextCacheKey(partUrl, index);
        std::thread old;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (key == key_) {
                if (state_ == State::Ready)
                    return body_;
                if (state_ == State::Failed) {
                    err = err_;
                    return {};
                }
                return kPartTextPendingSrt;   // still loading
            }
            if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id()) {
                err = "part text busy";
                return {};
            }
            old = std::move(worker_);
        }
        cancel_.store(true);
        if (old.joinable())
            old.join();
        std::lock_guard<std::mutex> lock(mu_);
        key_ = key;
        state_ = State::Loading;
        body_.clear();
        err_.clear();
        cancel_.store(false);
        ++starts_;
        worker_ = std::thread([this, partUrl, headers, index, key] { run(partUrl, headers, index, key); });
        return kPartTextPendingSrt;
    }

    // Cancel and join the worker (the caller must not hold a lock done() takes).
    void stop() {
        std::thread old;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id())
                return;
            old = std::move(worker_);
            key_.clear();
            state_ = State::Idle;
        }
        cancel_.store(true);
        if (old.joinable())
            old.join();
    }

    int starts() const {
        std::lock_guard<std::mutex> lock(mu_);
        return starts_;
    }

private:
    enum class State { Idle, Loading, Ready, Failed };

    void run(const std::string& partUrl, const std::string& headers, int index, const std::string& key) {
        std::string srt;
        std::string err;
        bool ok = false;
        if (extract_)
            ok = extract_(partUrl, headers, index, [this] { return cancel_.load(); }, srt, err);
        else
            err = "no extractor";
        if (cancel_.load())
            return;   // superseded or stopped: no state, no done()
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (key != key_)
                return;
            if (ok && !srt.empty()) {
                state_ = State::Ready;
                body_ = std::move(srt);
            } else {
                state_ = State::Failed;
                err_ = err.empty() ? "part text failed" : err;
            }
        }
        if (done_)
            done_(partUrl, index);
    }

    mutable std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    Extract extract_;
    Done done_;
    std::string key_;
    State state_ = State::Idle;
    std::string body_;
    std::string err_;
    int starts_ = 0;
};

} // namespace misterplex
