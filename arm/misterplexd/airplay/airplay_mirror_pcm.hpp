// fr-airplay-audio-1025 (c): audio during an AirPlay screen-mirror session. Decoded RAOP PCM (48 kHz s16le stereo)
// is bridged into the same record ring as the mirror video (IAvRecordProducer::pushPcm). Pure: no libav, no I/O.
// Rules: PCM records are a non-zero multiple of 4 bytes (a split frame is carried to the next call, never sent
// half); nothing is sent unless a mirror session is active and holds the session id; one record is bounded
// (kMirrorPcmMaxChunk); the live audio never blocks on a full ring: it is dropped and counted.
#pragma once
#include "airplay/ns.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
#include "libmisterplex/h264_bitstream_transport.hpp"

namespace misterplex {
namespace airplay {

inline constexpr size_t kMirrorPcmMaxChunk = 16384;   // bytes per ring record (multiple of 4)

class MirrorPcmBridge {
public:
    // Returns the bytes the ring accepted.
    size_t push(h264stream::IAvRecordProducer* prod, bool sessionActive, uint64_t sessionId, const uint8_t* pcm,
                size_t len) {
        if (!prod || !sessionActive || !pcm || len == 0) {
#ifndef MUT_MPCM_KEEP_CARRY_INACTIVE
            carry_.clear();   // a stale half frame must never join the next session
#endif
            if (len)
                ++droppedInactive_;
            return 0;
        }
        std::vector<uint8_t> buf;
        buf.reserve(carry_.size() + len);
        buf.insert(buf.end(), carry_.begin(), carry_.end());
        buf.insert(buf.end(), pcm, pcm + len);
#ifdef MUT_MPCM_NO_ALIGN
        size_t usable = buf.size();
#else
        size_t usable = buf.size() & ~static_cast<size_t>(3);
#endif
        carry_.assign(buf.begin() + usable, buf.end());
        size_t sent = 0;
        while (sent < usable) {
#ifdef MUT_MPCM_NO_BOUND
            const size_t n = usable - sent;
#else
            const size_t n = usable - sent < kMirrorPcmMaxChunk ? usable - sent : kMirrorPcmMaxChunk;
#endif
            h264stream::PcmView v;
#ifdef MUT_MPCM_WRONG_SESSION
            v.session_id = sessionId + 1;
#else
            v.session_id = sessionId;
#endif
            v.s16le = buf.data() + sent;
            v.len = n;
            v.pts90k = h264stream::kNoPts90k;
            const h264stream::PushResult r = prod->pushPcm(v);
            if (r == h264stream::PushResult::Ok) {
                sent += n;
                accepted_ += n;
                continue;
            }
            ++droppedRing_;   // Full / Desync / Fatal: live audio, drop the rest of this call
            break;
        }
        return sent;
    }
    void reset() { carry_.clear(); }
    size_t carried() const { return carry_.size(); }
    uint64_t accepted() const { return accepted_; }
    uint64_t droppedInactive() const { return droppedInactive_; }
    uint64_t droppedRing() const { return droppedRing_; }

private:
    std::vector<uint8_t> carry_;
    uint64_t accepted_ = 0, droppedInactive_ = 0, droppedRing_ = 0;
};

}  // namespace airplay
}  // namespace misterplex
