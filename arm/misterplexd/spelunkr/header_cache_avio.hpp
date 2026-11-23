#pragma once
#include <cstdio>
// P2 header (moov) cache AVIO glue. The Spelunkr archive.org path is https + a hostname, so
// it cannot use http_range_avio.hpp (HttpRangeReader is http:// + numeric-IP only — see
// host/libmisterplex/http_range_reader.hpp shouldAttach). This wraps the same custom-AVIO
// technique around libav's own http protocol instead of a raw socket: a cache hit serves
// moov/ftyp bytes from the on-disk header_cache.hpp store with zero network I/O, opening a
// live inner AVIOContext (avio_open2, the same TLS/http libav already uses for everything
// else) lazily and only for a read that misses the cache — which in practice is the single
// keyframe range read after a warm open. A cache miss records every byte actually read
// before finalizeHeader() is called (right after avformat_find_stream_info) and persists it,
// so the next open of the same (url, size, etag) is a hit. browse-lane P2-A: a cancel/prefetch-timeout
// mid-open persists those bytes as a resumable *partial* instead (on-path only; never a
// neighbour fetch), so the next attach() of this (url, size, etag) resumes from it rather than
// re-fetching the whole header cold -- see header_cache.hpp storePartial()/loadPartial().

#include "spelunkr/header_cache.hpp"

#include "av_open_opts.hpp"
#include "ca_bundle.hpp"
#include "gcast/yt_open_budget.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>
#include <memory>

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
        int64_t liveOpenMs = -1;
        int64_t liveSeekMs = 0;
        int64_t cacheReadMs = 0;
        int liveOpens = 0;
        int liveSeeks = 0;
        int liveReconnects = 0;
        bool recordCapHit = false;  // P3-3: recording stopped at kMaxRecordBytes
        bool resumedFromPartial = false;  // browse-lane P2-A: attach() seeded rangesHold_ from a partial
    };

    HeaderCacheAvio() = default;
    HeaderCacheAvio(const HeaderCacheAvio&) = delete;
    HeaderCacheAvio& operator=(const HeaderCacheAvio&) = delete;
    ~HeaderCacheAvio() { reset(); }

    void reset() {
        if (store_)
            store_->waitIdle(kHeaderCacheIdleTimeoutMs);
        if (pb_) {
            av_freep(&pb_->buffer);
            avio_context_free(&pb_);
            pb_ = nullptr;
        }
        if (live_)
            avio_closep(&live_);
        liveAt_ = -1;
        // Drop our ref; an in-flight storeAsync may still hold rangesHold_ (P3-7 / P3-8).
        rangesHold_ = std::make_shared<std::vector<HeaderRange>>();
        newRanges_.clear();
        store_ = nullptr;
        pos_ = 0;
        recording_ = true;
        recordCapHit_ = false;
        abortOnCapHit_ = false;
        touchGlobalCapHit_ = true;
        neighborStore_ = false;
        stats_.recordCapHit = false;
        partialRefs_.clear();
        partialBytesOnDisk_ = 0;
    }

    bool attached() const { return pb_ != nullptr; }

    // key.url empty: the caller should not attach at all (not a cacheable Spelunkr open).
    // neighbor: true only for the standalone neighbour-prefetch path (prefetchHeaderToStore).
    // A neighbour attach never consults or creates a partial (kept on the 12 MiB cap /
    // over_cache_cap skip -- browse-lane P2-A must not start warming 35 MB neighbours).
    bool attach(AVFormatContext* fmt, const HeaderCacheKey& key, const std::string& headers,
               HeaderCacheStore& store, std::function<bool()> cancel, bool neighbor = false) {
        reset();
        if (!fmt || key.url.empty())
            return false;
        key_ = key;
        headers_ = headers;
        cancel_ = std::move(cancel);
        store_ = &store;
        neighborStore_ = neighbor;
        // key.size==0 is the Back When direct-play cache-key sentinel ("unknown archive byte
        // length"), not the media file size. Feeding it to AVSEEK_SIZE made ffmpeg think the
        // file was empty (bytes_read=0, moov not found).
#ifndef MUT_HDR_KNOWN_ZERO_AS_SIZE
        knownSize_ = key.size > 0 ? key.size : -1;
#else
        knownSize_ = key.size;
#endif
        HeaderCacheData cached;
        stats_.cacheHit = store_->load(key_, cached);
        if (stats_.cacheHit) {
            rangesHold_ = std::make_shared<std::vector<HeaderRange>>(std::move(cached.ranges));
            stats_.moovOffset = cached.moovOffset;
            stats_.moovSize = cached.moovSize;
        } else if (!neighborStore_) {
            // browse-lane P2-A dial-500: resume from a partial index on SD without loading payloads into
            // RAM (browse-lane #663: dial-500 partial must not raise VmHWM with a 48 MiB vector).
            int64_t moov = -1;
            if (store_->loadPartialIndex(key_, partialRefs_, partialBytesOnDisk_, moov) && !partialRefs_.empty()) {
                stats_.resumedFromPartial = true;
                if (moov >= 0)
                    stats_.moovOffset = moov;
            }
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

    void setAbortOnCapHit(bool v) { abortOnCapHit_ = v; }
    void setTouchGlobalCapHit(bool v) { touchGlobalCapHit_ = v; }
    void setNeighborStore(bool v) { neighborStore_ = v; }

    // Call once, right after avformat_find_stream_info returns, before any keyframe seek, or
    // from an open-phase failure (owenb_av_source.cpp's avformat_open_input rc<0 branch).
    // success: find_stream_info's (or the open's) result (true on success). Persists newly-
    // touched bytes (a miss, or a hit with a gap the cache did not cover) so the next open of
    // this (url, size, etag) is a full hit; a hit with no new bytes is a no-op write. A failed
    // find_stream_info (success=false) never persists a complete entry: those bytes do not
    // represent a good header, and writing them would let a transient failure poison the next
    // open's cache hit. cancelledOrTimedOut (browse-lane P2-A): true only when a failure was a
    // cancel/zap-window/prefetch-slice timeout (owenb_av_source.cpp's openStopped_/
    // openCapped_), not a genuine protocol error; persists a resumable partial in that case.
    void finalizeHeader(bool success = true, bool cancelledOrTimedOut = false) {
        // browse-lane #647 P2-1: stop recording so later playback reads are not held in newRanges_.
        recording_ = false;
        if (!store_)
            return;
        // browse-lane #663 follow-up: after a spill, a successful open may have most of the recording
        // on disk (partialBytesOnDisk_) and only the last slice still in RAM. The complete .hdr
        // format holds every byte in one in-RAM vector, so a storeAsync() here could only ever
        // cover the in-RAM tail -- writing that as "complete" and then removePartial()-ing the
        // spilled prefix would leave hasEntry() pointing at an incomplete tail-only header.
        // keptPartialOnSuccess tracks the case where the recording must stay a partial instead.
        bool keptPartialOnSuccess = false;
        if (success && (!newRanges_.empty() || !stats_.cacheHit)) {
            *rangesHold_ = mergeRanges(std::move(*rangesHold_), std::move(newRanges_));
            const int64_t moov = scanMoovOffset(*rangesHold_);
            stats_.moovOffset = moov;
            int64_t ramBytes = 0;
            for (const auto& r : *rangesHold_)
                ramBytes += static_cast<int64_t>(r.data.size());
            // Total recorded is the spilled disk prefix plus whatever is still in RAM -- not
            // just the in-RAM ranges (browse-lane #663 follow-up).
            const int64_t recorded = partialBytesOnDisk_ + ramBytes;
            stats_.recordCapHit = recordCapHit_;
            if (recordCapHit_ && touchGlobalCapHit_)
                lastHeaderRecordCapHit().store(true, std::memory_order_release);
            // A spilled prefix already on disk can never be folded into a complete .hdr (that
            // format has no notion of a disk-resident prefix); only persist a complete entry
            // when nothing has spilled and the recording is otherwise eligible.
            const bool noSpilledPrefix = partialBytesOnDisk_ == 0;
            // P3-3: do not persist a cap-hit / oversize head. hasEntry would report
            // warm and clear a dead verdict, then every later tune would still go live.
            std::fprintf(stderr,
                         "misterplexd: spelunkr: header record bytes=%lld ram=%lld disk=%lld cap_hit=%d "
                         "persist=%d moov_off=%lld\n",
                         static_cast<long long>(recorded), static_cast<long long>(ramBytes),
                         static_cast<long long>(partialBytesOnDisk_), recordCapHit_ ? 1 : 0,
                         (noSpilledPrefix && shouldPersistRecordedRanges(recorded, recordCapHit_)) ? 1 : 0,
                         static_cast<long long>(moov));
            if (noSpilledPrefix && shouldPersistRecordedRanges(recorded, recordCapHit_)) {
                store_->storeAsync(key_, moov, rangesHold_, neighborStore_);
            } else if (!neighborStore_) {
                // Over the 12 MiB complete cap, cap-hit, or a spilled prefix already on disk:
                // keep the resumable partial current instead of dropping the recorded bytes
                // (browse-lane #663 follow-up) so the next attach() can still resume rather than
                // starting cold.
                spillRamToPartial();
                keptPartialOnSuccess = true;
            }
        }
        if (!success && stats_.cacheHit) {
            // P2-2: a bad or stale cache entry made this hit fail. Delete it so the next open
            // is a clean miss instead of failing on this (url, size, etag) forever.
            store_->remove(key_);
        }
        // browse-lane P2-A dial-500: a cancel/timeout mid-open must not drop the bytes already read of
        // a large moov. Persist them as a resumable partial (never a neighbour fetch -- those
        // stay on the 12 MiB cap / over_cache_cap skip). On a genuine protocol failure (not a
        // cancel/timeout) cancelledOrTimedOut is false and no partial is written.
        if (!success && cancelledOrTimedOut && !neighborStore_)
            persistPartial();
        // Once the header is fully resolved with a complete entry that covers every recorded
        // byte, a stale partial from an earlier attempt no longer has any use -- drop it rather
        // than leaving a large resumable recording on disk/at the next attach() forever (browse-lane
        // P2-A). A success that could only keep the recording as a partial
        // (keptPartialOnSuccess) must leave it in place so the next visit resumes instead of
        // starting cold (browse-lane #663 follow-up).
        if (success && !neighborStore_ && !keptPartialOnSuccess)
            store_->removePartial(key_);
        newRanges_.clear();
    }

    const Stats& stats() const { return stats_; }

private:
    static constexpr int64_t kMaxRecordBytes = kMaxHeaderCacheBytes;

    AVIOContext* pb_ = nullptr;
    AVIOContext* live_ = nullptr;
    int64_t pos_ = 0;
    int64_t liveAt_ = -1;  // byte offset of live_, or -1 when closed
    int64_t knownSize_ = -1;
    HeaderCacheKey key_;
    std::string headers_;
    std::function<bool()> cancel_;
    HeaderCacheStore* store_ = nullptr;
    std::shared_ptr<std::vector<HeaderRange>> rangesHold_ =
        std::make_shared<std::vector<HeaderRange>>();  // loaded at attach(); shared with storeAsync
    std::vector<HeaderRange> newRanges_;  // bytes actually read live during this open
    bool recording_ = true;               // false after finalizeHeader(); playback is not cached
    bool recordCapHit_ = false;           // P3-3: a live read was dropped because of kMaxRecordBytes
    bool abortOnCapHit_ = false;          // neighbour prefetch: stop libav parse at the cap
    bool touchGlobalCapHit_ = true;       // false for neighbour: do not poison the on-path flag
    bool neighborStore_ = false;          // use the neighbour pending-store cap
    std::vector<PartialRangeRef> partialRefs_;  // on-disk partial index (no payloads in RAM)
    int64_t partialBytesOnDisk_ = 0;
    Stats stats_;

    // Open (or reuse) the live handle. If an offset open fails because the server
    // ignored Range (libav: Unexpected offset / EIO), retry once at byte 0 and let
    // avio_seek skip forward (browse-lane #647 P2-3).
    bool ensureLive() {
        if (live_)
            return true;
        if (openLive(/*allowOffset=*/true))
            return true;
        if (pos_ > 0)
            return openLive(/*allowOffset=*/false);
        return false;
    }

    bool openLive(bool allowOffset) {
        if (live_) {
            avio_closep(&live_);
            liveAt_ = -1;
        }
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
        // Open at the needed offset and keep the connection for later seeks (keyframe /
        // playback). Without this, ffmpeg's http protocol GETs at byte 0, then reconnects
        // for the seek and again for the IDR (~0.9 s per setup on lab).
        av_dict_set(&opts, "multiple_requests", "1", 0);
        const bool useOffset = allowOffset && pos_ > 0;
        if (useOffset)
            av_dict_set(&opts, "offset", std::to_string(pos_).c_str(), 0);
        // P1-2: doRead only polls cancel_() between reads, so a blocked avio_open2/avio_read
        // never returns on its own. A real AVIOInterruptCB lets the http/tls protocol itself
        // poll cancel_() during connect and read, so a stop/zap/prefetch-timeout cancel can
        // break a stalled open or a stalled mid-play read (this AVIO stays attached for the
        // whole play, not just the header fetch).
        AVIOInterruptCB intCb;
        intCb.callback = &HeaderCacheAvio::interruptCb;
        intCb.opaque = this;
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = avio_open2(&live_, key_.url.c_str(), AVIO_FLAG_READ, &intCb, &opts);
        av_dict_free(&opts);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (stats_.liveOpenMs < 0)
            stats_.liveOpenMs = ms;
        else
            stats_.liveOpenMs += ms;
        ++stats_.liveOpens;
        if (rc >= 0 && live_ != nullptr) {
            liveAt_ = useOffset ? pos_ : 0;
            return true;
        }
        live_ = nullptr;
        liveAt_ = -1;
        return false;
    }

    bool seekLive(int64_t target) {
        if (live_ && liveAt_ == target)
            return true;
        if (!ensureLive())
            return false;
        if (liveAt_ == target)
            return true;
        const auto t0 = std::chrono::steady_clock::now();
        const int64_t s = avio_seek(live_, target, SEEK_SET);
        stats_.liveSeekMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        ++stats_.liveSeeks;
        if (s >= 0) {
            liveAt_ = target;
            return true;
        }
        // browse-lane #647 P3-5: a cancel must not start a new connect/DNS.
        if (cancel_ && cancel_())
            return false;
        // Keep-alive or offset open failed; fall back to a fresh open at the needed offset,
        // then at 0 if the server ignores Range.
        avio_closep(&live_);
        liveAt_ = -1;
        ++stats_.liveReconnects;
        const int64_t saved = pos_;
        pos_ = target;
        bool opened = openLive(/*allowOffset=*/true);
        if (!opened && target > 0)
            opened = openLive(/*allowOffset=*/false);
        pos_ = saved;
        if (!opened)
            return false;
        if (liveAt_ == target)
            return true;
        const auto t1 = std::chrono::steady_clock::now();
        const int64_t s2 = avio_seek(live_, target, SEEK_SET);
        stats_.liveSeekMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t1)
                                 .count();
        ++stats_.liveSeeks;
        if (s2 < 0)
            return false;
        liveAt_ = target;
        return true;
    }

    static int interruptCb(void* opaque) {
        auto* self = static_cast<HeaderCacheAvio*>(opaque);
        return (self && self->cancel_ && self->cancel_()) ? 1 : 0;
    }

    // Serves [offset, offset+n) from a single covering cached range, from either the disk-loaded
    // set or bytes already read live this session. False: no single range covers the start.
    bool serveFromCache(int64_t offset, uint8_t* buf, int n, int& served) const {
        const std::vector<HeaderRange>* const sets[] = {rangesHold_.get(), &newRanges_};
        for (const auto* set : sets) {
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
        if (store_ && !partialRefs_.empty()) {
            int disk = 0;
            if (store_->readPartialSpan(key_, partialRefs_, offset, buf, n, disk) && disk > 0) {
                served = disk;
                return true;
            }
        }
        return false;
    }

    int64_t inRamRecordedBytes() const {
        int64_t total = 0;
        for (const auto& r : *rangesHold_)
            total += static_cast<int64_t>(r.data.size());
        for (const auto& r : newRanges_)
            total += static_cast<int64_t>(r.data.size());
        return total;
    }

    // browse-lane #663: merge in-RAM ranges to `.hdr.partial`, drop payloads from RAM (12 MiB cap).
    bool spillRamToPartial() {
        if (!store_ || neighborStore_)
            return false;
        *rangesHold_ = mergeRanges(std::move(*rangesHold_), std::move(newRanges_));
        newRanges_.clear();
        if (rangesHold_->empty())
            return partialBytesOnDisk_ > 0;
        std::vector<HeaderRange> spill = std::move(*rangesHold_);
        rangesHold_ = std::make_shared<std::vector<HeaderRange>>();
        int64_t total = 0;
        if (!store_->spillPartialRanges(key_, spill, total)) {
            // browse-lane #663 follow-up (P2-1): spillPartialRanges can fail for reasons that never
            // touch disk (disk full, under kMinHeaderCacheFreeBytes, an open backoff window,
            // a rewrite tmp-file fopen failure). Nothing was persisted, so the up-to-12 MiB
            // in-RAM prefix this call just emptied rangesHold_ of must come back, or a
            // subsequent recordRead/persistPartial sees it as gone and the recording silently
            // loses bytes it never actually spilled.
            *rangesHold_ = std::move(spill);
            return false;
        }
        partialBytesOnDisk_ = total;
        int64_t moov = -1;
        if (!store_->loadPartialIndex(key_, partialRefs_, partialBytesOnDisk_, moov))
            return false;
        if (moov >= 0)
            stats_.moovOffset = moov;
        return true;
    }

    void recordRead(int64_t offset, const uint8_t* buf, int n) {
        if (!recording_)
            return;
        // browse-lane P2-A: up to 48 MiB may reside on SD as a resumable partial; in-RAM recording stays
        // at kMaxPartialRecordRamBytes (12 MiB) via spillRamToPartial() (browse-lane #663).
        // effectivePartialRamCapBytes() is kMaxRecordBytes(==kMaxHeaderCacheBytes) unless the
        // MPX_SPELUNKR_FORCE_PARTIAL_HEADER glass fixture hook (browse-lane #663 follow-up) lowers it;
        // the neighbour cap follows the same (possibly lowered) value so a forced-small on-path
        // regime cannot leave neighbour warms quietly using the unlowered 12 MiB cap.
        const int64_t effectiveCap = effectivePartialRamCapBytes();
        const int64_t totalCap = neighborStore_ ? effectiveCap : kMaxPartialHeaderBytes;
        const int64_t ramCap = effectiveCap;
        int64_t ramTotal = inRamRecordedBytes();
        const int64_t grandTotal = partialBytesOnDisk_ + ramTotal;
        if (grandTotal + n > totalCap) {
            recordCapHit_ = true;
            stats_.recordCapHit = true;
            return;
        }
        if (!neighborStore_ && ramTotal + n > ramCap) {
            if (!spillRamToPartial()) {
                recordCapHit_ = true;
                stats_.recordCapHit = true;
                return;
            }
            ramTotal = inRamRecordedBytes();
            if (ramTotal + n > ramCap) {
                recordCapHit_ = true;
                stats_.recordCapHit = true;
                return;
            }
        } else if (neighborStore_ && ramTotal + n > ramCap) {
            recordCapHit_ = true;
            stats_.recordCapHit = true;
            return;
        }
        newRanges_.push_back({offset, std::string(reinterpret_cast<const char*>(buf), static_cast<size_t>(n))});
    }

    // browse-lane P2-A: merge rangesHold_ + newRanges_ (same merge finalizeHeader's success path uses)
    // and, if the result is a usable (non-cap-hit, under kMaxPartialHeaderBytes) recording,
    // persist it so the next attach() of this (url, size, etag) resumes instead of starting
    // cold. Never called for a neighbour fetch (finalizeHeader() guards the call site).
    void persistPartial() {
        if (!rangesHold_->empty() || !newRanges_.empty()) {
            if (!spillRamToPartial() && !recordCapHit_)
                recordCapHit_ = true;
        }
        const int64_t recorded = partialBytesOnDisk_ + inRamRecordedBytes();
        const bool persist = shouldPersistPartialRecordedRanges(recorded, recordCapHit_);
        std::fprintf(stderr,
                     "misterplexd: spelunkr: header partial bytes=%lld ram=%lld disk=%lld cap_hit=%d persist=%d\n",
                     static_cast<long long>(recorded), static_cast<long long>(inRamRecordedBytes()),
                     static_cast<long long>(partialBytesOnDisk_), recordCapHit_ ? 1 : 0, persist ? 1 : 0);
        // Spill paths already wrote `.hdr.partial` synchronously for hasPartialEntry().
    }

    int doRead(uint8_t* buf, int n) {
        // browse-lane #647 P3-E: recordRead allocates a std::string inside libav's C
        // callback. A throw would cross C frames; return ENOMEM instead.
        try {
            if (cancel_ && cancel_())
                return AVERROR_EXIT;
            int served = 0;
            if (serveFromCache(pos_, buf, n, served) && served > 0) {
                const auto t0 = std::chrono::steady_clock::now();
                pos_ += served;
                stats_.cachedBytesServed += served;
                stats_.cacheReadMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count();
                // browse-lane #663 follow-up: a warm-resumed partial being served back counts as
                // progress too -- cancelled()'s in-place zap-window restart treats it the
                // same as a live read (see lastHeaderFetchProgressBytes()).
                if (touchGlobalCapHit_)
                    lastHeaderFetchProgressBytes().fetch_add(served, std::memory_order_relaxed);
                return served;
            }
            if (!seekLive(pos_))
                return AVERROR(EIO);
            const int r = avio_read(live_, buf, n);
            if (r > 0) {
                recordRead(pos_, buf, r);
                pos_ += r;
                liveAt_ = pos_;
                stats_.liveBytesRead += r;
                if (touchGlobalCapHit_)
                    lastHeaderFetchProgressBytes().fetch_add(r, std::memory_order_relaxed);
                if (abortOnCapHit_ && recordCapHit_)
                    return AVERROR_EXIT;
            }
            return r;
        } catch (const std::bad_alloc&) {
            return AVERROR(ENOMEM);
        }
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

    static std::vector<HeaderRange> mergeRanges(std::vector<HeaderRange> a, std::vector<HeaderRange> b) {
        a.reserve(a.size() + b.size());
        for (auto& r : b)
            a.push_back(std::move(r));
        std::sort(a.begin(), a.end(), [](const HeaderRange& x, const HeaderRange& y) { return x.offset < y.offset; });
        return a;
    }
};


// Standalone header fetch for neighbour prefetch. Caps at kMaxHeaderCacheBytes
// (12 MiB) via HeaderCacheAvio. Does not touch the in-play MediaPlayer.
struct PrefetchHeaderOpts {
    bool abortOnCapHit = false;
    bool touchGlobalCapHit = true;
    bool neighborStore = false;
    bool* capHitOut = nullptr;
};

inline bool prefetchHeaderToStore(const HeaderCacheKey& key, const std::string& headers,
                                  HeaderCacheStore& store, const std::function<bool()>& cancel,
                                  const PrefetchHeaderOpts& prefetchOpts = {}) {
    if (key.url.empty())
        return false;
    if (store.hasEntry(key))
        return true;
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt)
        return false;
    HeaderCacheAvio avio;
    if (!avio.attach(fmt, key, headers, store, cancel, /*neighbor=*/prefetchOpts.neighborStore)) {
        avformat_free_context(fmt);
        return false;
    }
    avio.setAbortOnCapHit(prefetchOpts.abortOnCapHit);
    avio.setTouchGlobalCapHit(prefetchOpts.touchGlobalCapHit);
    avio.setNeighborStore(prefetchOpts.neighborStore);
    if (cancel && cancel()) {
        avformat_free_context(fmt);
        return false;
    }
    AVDictionary* opts = nullptr;
    int err = avformat_open_input(&fmt, key.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (err >= 0 && !(cancel && cancel()))
        err = avformat_find_stream_info(fmt, nullptr);
    const bool ok = err >= 0 && !(cancel && cancel());
    avio.finalizeHeader(ok);
    if (prefetchOpts.capHitOut)
        *prefetchOpts.capHitOut = avio.stats().recordCapHit;
    if (fmt)
        avformat_close_input(&fmt);
    // storeAsync is detached; hasEntry would miss a still-pending persist
    // and the daemon would log "neighbor prefetch end" for a successful
    // store (browse-lane #647 P3-B).
    store.waitIdle(kHeaderCacheIdleTimeoutMs);
    return ok && store.hasEntry(key);
}

}  // namespace spelunkr
}  // namespace misterplex
