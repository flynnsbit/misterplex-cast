#pragma once
// P2 header (moov) cache AVIO glue. The Spelunkr archive.org path is https + a hostname, so
// it cannot use http_range_avio.hpp (HttpRangeReader is http:// + numeric-IP only — see
// host/libmisterplex/http_range_reader.hpp shouldAttach). This wraps the same custom-AVIO
// technique around libav's own http protocol instead of a raw socket: a cache hit serves
// moov/ftyp bytes from the on-disk header_cache.hpp store with zero network I/O, opening a
// live inner AVIOContext (avio_open2, the same TLS/http libav already uses for everything
// else) lazily and only for a read that misses the cache — which in practice is the single
// keyframe range read after a warm open. A cache miss records every byte actually read
// before finalizeHeader() is called (right after avformat_find_stream_info) and persists it,
// so the next open of the same (url, size, etag) is a hit.

#include "spelunkr/header_cache.hpp"

#include "av_open_opts.hpp"
#include "ca_bundle.hpp"
#include "gcast/yt_open_budget.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace misterplex {
namespace spelunkr {

class HeaderCacheAvio {
public:
    struct Stats {
        bool cacheHit = false;
        int64_t cachedBytesServed = 0;
        int64_t liveBytesRead = 0;
        int64_t moovOffset = -1;
        int64_t moovSize = -1;
    };

    HeaderCacheAvio() = default;
    HeaderCacheAvio(const HeaderCacheAvio&) = delete;
    HeaderCacheAvio& operator=(const HeaderCacheAvio&) = delete;
    ~HeaderCacheAvio() { reset(); }

    void reset() {
        if (pb_) {
            av_freep(&pb_->buffer);
            avio_context_free(&pb_);
            pb_ = nullptr;
        }
        if (live_)
            avio_closep(&live_);
        ranges_.clear();
        newRanges_.clear();
        store_ = nullptr;
        pos_ = 0;
    }

    bool attached() const { return pb_ != nullptr; }

    // key.url empty: the caller should not attach at all (not a cacheable Spelunkr open).
    bool attach(AVFormatContext* fmt, const HeaderCacheKey& key, const std::string& headers,
               HeaderCacheStore& store, std::function<bool()> cancel) {
        reset();
        if (!fmt || key.url.empty())
            return false;
        key_ = key;
        headers_ = headers;
        cancel_ = std::move(cancel);
        store_ = &store;
        knownSize_ = key.size;
        HeaderCacheData cached;
        stats_.cacheHit = store_->load(key_, cached);
        if (stats_.cacheHit) {
            ranges_ = std::move(cached.ranges);
            stats_.moovOffset = cached.moovOffset;
            stats_.moovSize = cached.moovSize;
        }
        constexpr int kBuf = 32768;
        auto* buf = static_cast<unsigned char*>(av_malloc(kBuf));
        if (!buf)
            return false;
        pb_ = avio_alloc_context(buf, kBuf, 0, this, &HeaderCacheAvio::readPacket, nullptr,
                                 &HeaderCacheAvio::seekPacket);
        if (!pb_) {
            av_free(buf);
            return false;
        }
        fmt->pb = pb_;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        return true;
    }

    // Call once, right after avformat_find_stream_info returns, before any keyframe seek.
    // success: find_stream_info's result (true on success). Persists newly-touched bytes (a
    // miss, or a hit with a gap the cache did not cover) so the next open of this
    // (url, size, etag) is a full hit; a hit with no new bytes is a no-op write. A failed
    // find_stream_info (success=false) never persists: those bytes do not represent a good
    // header, and writing them would let a transient failure poison the next open's cache hit.
    void finalizeHeader(bool success = true) {
        if (!store_)
            return;
        if (success && (!newRanges_.empty() || !stats_.cacheHit)) {
            HeaderCacheData d;
            d.key = key_;
            d.ranges = mergeRanges(ranges_, newRanges_);
            d.moovOffset = scanMoovOffset(d.ranges);
            d.moovSize = -1;
            if (store_->store(d))
                ranges_ = std::move(d.ranges);
            stats_.moovOffset = d.moovOffset;
        }
        if (!success && stats_.cacheHit) {
            // P2-2: a bad or stale cache entry made this hit fail. Delete it so the next open
            // is a clean miss instead of failing on this (url, size, etag) forever.
            store_->remove(key_);
        }
        newRanges_.clear();
    }

    const Stats& stats() const { return stats_; }

private:
    static constexpr int64_t kMaxRecordBytes = 4ll * 1024 * 1024;

    AVIOContext* pb_ = nullptr;
    AVIOContext* live_ = nullptr;
    int64_t pos_ = 0;
    int64_t knownSize_ = -1;
    HeaderCacheKey key_;
    std::string headers_;
    std::function<bool()> cancel_;
    HeaderCacheStore* store_ = nullptr;
    std::vector<HeaderRange> ranges_;     // loaded from disk at attach() (empty on a miss)
    std::vector<HeaderRange> newRanges_;  // bytes actually read live during this open
    Stats stats_;

    bool ensureLive() {
        if (live_)
            return true;
        AVDictionary* opts = nullptr;
        std::string h = headers_;
        if (!h.empty() && (h.size() < 2 || h.compare(h.size() - 2, 2, "\r\n") != 0))
            h += "\r\n";
        if (!h.empty())
            av_dict_set(&opts, "headers", h.c_str(), 0);
        // P1-1: this inner protocol is reached only through avio_open2, never through
        // avformat_open_input's own option dict (fmt->pb is custom IO, so applyOpenOpts in
        // owenb_av_source.cpp never gets here). Apply the same dict keys that path sets, or
        // libav's default tls_verify=0 lets a self-signed/MITM https peer through silently.
        av_dict_set(&opts, "tls_verify", "1", 0);
        const std::string ca = mpx::caBundlePath();
        if (!ca.empty())
            av_dict_set(&opts, "ca_file", ca.c_str(), 0);
        av_dict_set(&opts, "protocol_whitelist", misterplex::kAvProtocolWhitelist, 0);
        av_dict_set(&opts, "rw_timeout", std::to_string(misterplex::gcast::openRwTimeoutUs(key_.url)).c_str(), 0);
        // P1-2: doRead only polls cancel_() between reads, so a blocked avio_open2/avio_read
        // never returns on its own. A real AVIOInterruptCB lets the http/tls protocol itself
        // poll cancel_() during connect and read, so a stop/zap/prefetch-timeout cancel can
        // break a stalled open or a stalled mid-play read (this AVIO stays attached for the
        // whole play, not just the header fetch).
        AVIOInterruptCB intCb;
        intCb.callback = &HeaderCacheAvio::interruptCb;
        intCb.opaque = this;
        // Range GETs come from avio_seek(); the http protocol auto-detects seekability from
        // Accept-Ranges/Content-Length (an explicit "seekable" option is rejected by some
        // other protocols, e.g. "file", so it is left unset here).
        const int rc = avio_open2(&live_, key_.url.c_str(), AVIO_FLAG_READ, &intCb, &opts);
        av_dict_free(&opts);
        return rc >= 0 && live_ != nullptr;
    }

    static int interruptCb(void* opaque) {
        auto* self = static_cast<HeaderCacheAvio*>(opaque);
        return (self && self->cancel_ && self->cancel_()) ? 1 : 0;
    }

    // Serves [offset, offset+n) from a single covering cached range, from either the disk-loaded
    // set or bytes already read live this session. False: no single range covers the start.
    bool serveFromCache(int64_t offset, uint8_t* buf, int n, int& served) const {
        for (const auto* set : {&ranges_, &newRanges_}) {
            for (const auto& r : *set) {
                if (offset >= r.offset && offset < r.offset + static_cast<int64_t>(r.data.size())) {
                    const int64_t within = offset - r.offset;
                    const int64_t avail = static_cast<int64_t>(r.data.size()) - within;
                    served = static_cast<int>(std::min<int64_t>(avail, n));
                    std::memcpy(buf, r.data.data() + within, static_cast<size_t>(served));
                    return true;
                }
            }
        }
        return false;
    }

    void recordRead(int64_t offset, const uint8_t* buf, int n) {
        int64_t total = 0;
        for (const auto& r : ranges_)
            total += static_cast<int64_t>(r.data.size());
        for (const auto& r : newRanges_)
            total += static_cast<int64_t>(r.data.size());
        if (total + n > kMaxRecordBytes)
            return;  // header material only; a pathological read pattern is simply not cached
        newRanges_.push_back({offset, std::string(reinterpret_cast<const char*>(buf), static_cast<size_t>(n))});
    }

    int doRead(uint8_t* buf, int n) {
        if (cancel_ && cancel_())
            return AVERROR_EXIT;
        int served = 0;
        if (serveFromCache(pos_, buf, n, served) && served > 0) {
            pos_ += served;
            stats_.cachedBytesServed += served;
            return served;
        }
        if (!ensureLive())
            return AVERROR(EIO);
        if (avio_seek(live_, pos_, SEEK_SET) < 0)
            return AVERROR(EIO);
        const int r = avio_read(live_, buf, n);
        if (r > 0) {
            recordRead(pos_, buf, r);
            pos_ += r;
            stats_.liveBytesRead += r;
        }
        return r;
    }

    int64_t doSeek(int64_t offset, int whence) {
        if (whence == AVSEEK_SIZE) {
            if (knownSize_ >= 0)
                return knownSize_;
            if (!ensureLive())
                return AVERROR(EIO);
            const int64_t sz = avio_size(live_);
            if (sz >= 0)
                knownSize_ = sz;
            return sz;
        }
        int64_t target = offset;
        if (whence == SEEK_CUR) {
            target = pos_ + offset;
        } else if (whence == SEEK_END) {
            int64_t sz = knownSize_;
            if (sz < 0) {
                if (!ensureLive())
                    return AVERROR(EIO);
                sz = avio_size(live_);
            }
            if (sz < 0)
                return AVERROR(EIO);
            target = sz + offset;
        } else if (whence != SEEK_SET) {
            return AVERROR(EINVAL);
        }
        pos_ = target;
        return pos_;
    }

    static int readPacket(void* opaque, uint8_t* buf, int n) {
        return static_cast<HeaderCacheAvio*>(opaque)->doRead(buf, n);
    }
    static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
        return static_cast<HeaderCacheAvio*>(opaque)->doSeek(offset, whence);
    }

    // Logging-only: scan the recorded header bytes for a top-level 'moov' box signature.
    static int64_t scanMoovOffset(const std::vector<HeaderRange>& ranges) {
        for (const auto& r : ranges) {
            if (r.data.size() < 8)
                continue;
            for (size_t i = 4; i + 4 <= r.data.size(); ++i) {
                if (std::memcmp(r.data.data() + i, "moov", 4) == 0)
                    return r.offset + static_cast<int64_t>(i) - 4;
            }
        }
        return -1;
    }

    static std::vector<HeaderRange> mergeRanges(const std::vector<HeaderRange>& a, const std::vector<HeaderRange>& b) {
        std::vector<HeaderRange> out = a;
        out.insert(out.end(), b.begin(), b.end());
        std::sort(out.begin(), out.end(), [](const HeaderRange& x, const HeaderRange& y) { return x.offset < y.offset; });
        return out;
    }
};

}  // namespace spelunkr
}  // namespace misterplex
