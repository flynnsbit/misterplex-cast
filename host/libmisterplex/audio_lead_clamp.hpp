#pragma once

#include <cstdint>

namespace misterplex {

// Shared 240p/480p audio-lead helpers (review-366 + review-370). 480i 720x480
// must not take the 240p 30 ms DMA ceiling.
constexpr int kAudioLeadClamp480pMs = 80;
constexpr int64_t kMrAudioBytesPerSecLead = 48000 * 4;
constexpr int64_t kAudioLeadClamp240pDmaBytes = (kMrAudioBytesPerSecLead * 3) / 100;  // 5760 = 30 ms
constexpr int64_t kAudioLeadClampDefaultDmaBytes = kMrAudioBytesPerSecLead / 5;       // 38400 = 200 ms

// Delay in ms to bring audio PTS lead down to boundMs. 0 = already in bound.
inline int audioLeadClamp(int64_t audioPtsUs, int64_t videoPtsUs, int boundMs) {
    if (boundMs <= 0)
        return 0;
    const int64_t leadUs = audioPtsUs - videoPtsUs;
    const int64_t boundUs = static_cast<int64_t>(boundMs) * 1000;
    if (leadUs <= boundUs)
        return 0;
    const int64_t delayUs = leadUs - boundUs;
    return static_cast<int>((delayUs + 999) / 1000);
}

inline int audioLeadClamp480p(int64_t audioPtsUs, int64_t videoPtsUs, int boundMs) {
    return audioLeadClamp(audioPtsUs, videoPtsUs, boundMs);
}

// DMA queue ceiling in bytes. (void)queued: never rewrite occupancy.
inline int64_t audioLeadClamp240p(int64_t /*queued*/, int outW, int outH) {
    if (outW == 320 && outH == 240)
        return kAudioLeadClamp240pDmaBytes;
    return kAudioLeadClampDefaultDmaBytes;
}

}  // namespace misterplex
