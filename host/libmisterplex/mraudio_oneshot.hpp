#pragma once
// GH#235: mix a one-shot PCM clip into the MrAudio playback buffer.
//
// The product pump is the only writer to /dev/MrAudio (s16le stereo @ 48 kHz).
// Theme music (#193) and UI sounds (#194) need a second source that does not
// steal that writer. Mix saturates; it does not wrap. One clip at a time; a
// new push replaces an unfinished one-shot (a click wins over a leftover blip).
// This header does not open /dev/MrAudio.

#include "libmisterplex/mraudio_status.hpp"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unistd.h>
#include <vector>

namespace misterplex {

inline int16_t mixS16Saturate(int32_t a, int32_t b) {
#ifdef MUT_ONESHOT_WRAP
    return static_cast<int16_t>(a + b);
#else
    const int32_t s = a + b;
    if (s > 32767)
        return 32767;
    if (s < -32768)
        return -32768;
    return static_cast<int16_t>(s);
#endif
}

// Mix clip[clipOff..) into dst. Both buffers are s16le. Returns bytes consumed
// from the clip. Odd trailing bytes are ignored.
inline size_t mixOneShotInto(uint8_t* dst, size_t dstBytes, const uint8_t* clip, size_t clipBytes,
                             size_t clipOff) {
    if (!dst || !clip || dstBytes < 2 || clipOff >= clipBytes)
        return 0;
    dstBytes &= ~size_t{1};
    size_t off = clipOff & ~size_t{1};
    if (off >= clipBytes)
        return 0;
    size_t n = dstBytes;
    if (n > clipBytes - off)
        n = clipBytes - off;
    n &= ~size_t{1};
    const int16_t* src = reinterpret_cast<const int16_t*>(clip + off);
    int16_t* out = reinterpret_cast<int16_t*>(dst);
    const size_t samples = n / 2;
    for (size_t i = 0; i < samples; ++i)
        out[i] = mixS16Saturate(out[i], src[i]);
    return n;
}

struct MrAudioOneShot {
    std::vector<uint8_t> pcm; // s16le stereo 48 kHz
    size_t off = 0;

    bool empty() const { return off >= pcm.size(); }

    void push(std::vector<uint8_t> clip) {
        pcm = std::move(clip);
        off = 0;
    }

    void clear() {
        pcm.clear();
        off = 0;
    }

    size_t mixInto(uint8_t* dst, size_t dstBytes) {
        if (empty())
            return 0;
        const size_t used = mixOneShotInto(dst, dstBytes, pcm.data(), pcm.size(), off);
        off += used;
        return used;
    }
};

// audioPump calls this on the chunk it is about to write. Empty queue is a no-op.
#ifdef MUT_ONESHOT_PUMP_SKIP
inline void audioPumpMixOneShot(uint8_t*, size_t, MrAudioOneShot&) {}
#else
inline void audioPumpMixOneShot(uint8_t* buf, size_t n, MrAudioOneShot& q) {
    if (!buf || n < 2 || q.empty())
        return;
    q.mixInto(buf, n);
}
#endif

enum class OneShotDelivery : uint8_t { PumpQueue, IdleFd };

inline OneShotDelivery oneshotDelivery(bool pumpActive, bool audioEnabled) {
    if (!audioEnabled)
        return OneShotDelivery::PumpQueue;
    return pumpActive ? OneShotDelivery::PumpQueue : OneShotDelivery::IdleFd;
}

// Idle/browse: write the clip to an already-open MrAudio (or test pipe) fd.
#ifdef MUT_ONESHOT_IDLE_SKIP
inline ssize_t idleWriteOneShotFd(int, const uint8_t*, size_t) { return 0; }
#else
inline ssize_t idleWriteOneShotFd(int fd, const uint8_t* pcm, size_t n) {
    if (fd < 0 || !pcm || n == 0)
        return 0;
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, pcm + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            break;
        off += static_cast<size_t>(w);
    }
    return static_cast<ssize_t>(off);
}
#endif

// 10 ms stereo 2 kHz click for browse Select (#194 / #235).
inline constexpr int kUiClickFrames = 480;

inline std::vector<uint8_t> uiClickPcm() {
#ifdef MUT_ONESHOT_CLICK_SILENT
    return std::vector<uint8_t>(static_cast<size_t>(kUiClickFrames) * 4, 0);
#else
    std::vector<uint8_t> out(static_cast<size_t>(kUiClickFrames) * 4);
    for (int i = 0; i < kUiClickFrames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kUiClickFrames);
        const float env = t < 0.2f ? t / 0.2f : (1.f - t);
        const float s =
            env * 0.25f *
            std::sin(2.f * 3.14159265f * 2000.f * static_cast<float>(i) / 48000.f);
        const auto v = static_cast<int16_t>(s * 32767.f);
        const auto lo = static_cast<uint8_t>(v & 0xff);
        const auto hi = static_cast<uint8_t>((v >> 8) & 0xff);
        const size_t o = static_cast<size_t>(i) * 4;
        out[o] = lo;
        out[o + 1] = hi;
        out[o + 2] = lo;
        out[o + 3] = hi;
    }
    return out;
#endif
}

} // namespace misterplex
