#pragma once
// Custom AVIO over HttpRangeReader for a direct-path PMS Part. libav sees a
// continuous byte stream; a short Content-Length close is a Range reopen,
// not EOF. Ineligible URLs (https, transcode, AirPlay, ladder, YouTube)
// leave pb unset so avformat_open_input keeps today's HTTP.

#include "libmisterplex/http_range_reader.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace misterplex {
namespace httprange {

struct AvioSession {
    std::unique_ptr<HttpRangeReader> reader;
    AVIOContext* pb = nullptr;

    AvioSession() = default;
    AvioSession(const AvioSession&) = delete;
    AvioSession& operator=(const AvioSession&) = delete;

    ~AvioSession() { reset(); }

    void reset() {
        if (pb) {
            av_freep(&pb->buffer);
            avio_context_free(&pb);
            pb = nullptr;
        }
        reader.reset();
    }

    bool attached() const { return pb != nullptr && reader != nullptr; }

    // True when custom IO is on fmt. False: the caller opens the URL itself.
    bool attach(AVFormatContext* fmt, const std::string& url, const std::string& headers,
                std::function<bool()> cancel) {
        reset();
        if (!fmt || !shouldAttach(url))
            return false;
        auto r = std::make_unique<HttpRangeReader>();
        r->cancelled = std::move(cancel);
        if (!r->open(url, headers))
            return false;
        constexpr int kBuf = 32768;
        auto* buf = static_cast<unsigned char*>(av_malloc(kBuf));
        if (!buf)
            return false;
        pb = avio_alloc_context(buf, kBuf, 0, r.get(), &AvioSession::readPacket, nullptr,
                                &AvioSession::seekPacket);
        if (!pb) {
            av_free(buf);
            return false;
        }
        reader = std::move(r);
        fmt->pb = pb;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        return true;
    }

    static int readPacket(void* opaque, uint8_t* buf, int bufSize) {
        auto* r = static_cast<HttpRangeReader*>(opaque);
        const int n = r->read(buf, bufSize);
        if (n > 0)
            return n;
        if (r->aborted())
            return AVERROR_EXIT;
        if (n == 0)
            return AVERROR_EOF;
        return AVERROR(EIO);
    }

    static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
        auto* r = static_cast<HttpRangeReader*>(opaque);
        if (whence == AVSEEK_SIZE)
            return r->size();
        int64_t pos = offset;
        if (whence == SEEK_CUR)
            pos = r->tell() + offset;
        else if (whence == SEEK_END) {
            const int64_t sz = r->size();
            if (sz < 0)
                return AVERROR(ESPIPE);
            pos = sz + offset;
        } else if (whence != SEEK_SET) {
            return AVERROR(EINVAL);
        }
        if (!r->seek(pos))
            return AVERROR(EIO);
        return r->tell();
    }
};

}  // namespace httprange
}  // namespace misterplex
