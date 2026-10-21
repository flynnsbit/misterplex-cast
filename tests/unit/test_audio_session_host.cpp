#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#define private public
#include "fpga_spi.hpp"
#undef private
#include <csignal>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ring = misterplex::ddr_bitstream_ring;
namespace audio = misterplex::audio_session;
namespace abi = mailbox_abi;
using Spi = misterplex::FpgaSpi;
using Control = Spi::AudioControl;

// Link-wrapped kernel status device: production FpgaSpi still performs the
// real open/read/close path; this test never touches a hardware device.
static constexpr int statusFd = 123456788;
static uint32_t producerPosition = 0;
static bool movingProducer = false;
enum class ProducerReadMode { Normal, OpenFailure, ReadFailure, MissingPointer };
static ProducerReadMode producerReadMode = ProducerReadMode::Normal;
extern "C" int __real_open(const char*, int, ...);
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" int __real_close(int);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (std::strcmp(path, "/dev/MrAudio") == 0) {
        if (producerReadMode == ProducerReadMode::OpenFailure) {
            errno = EIO;
            return -1;
        }
        if (movingProducer) producerPosition = (producerPosition + 4) % (512 * 1024);
        return statusFd;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    if (fd != statusFd) return __real_read(fd, buffer, size);
    if (producerReadMode == ProducerReadMode::ReadFailure) {
        errno = EIO;
        return -1;
    }
    char status[128];
    const int count = producerReadMode == ProducerReadMode::MissingPointer
        ? std::snprintf(status, sizeof(status), "rptr: 0, len: 0, comp: 0\n")
        : std::snprintf(status, sizeof(status),
            "rptr: 0, wptr: %u, len: %u, comp: 0\n", producerPosition, producerPosition);
    const size_t copied = size < static_cast<size_t>(count) ? size : static_cast<size_t>(count);
    std::memcpy(buffer, status, copied);
    return copied;
}
extern "C" int __wrap_close(int fd) {
    return fd == statusFd ? 0 : __real_close(fd);
}

static pid_t consumer = -1;
static void cleanup() {
    if (consumer > 0) {
        kill(consumer, SIGTERM);
        waitpid(consumer, nullptr, 0);
    }
}

static void expect(bool good, const char* message) {
    if (!good) {
        std::fprintf(stderr, "FAIL audio host: %s\n", message);
        std::exit(1);
    }
}

static void testDeviceByteCopy() {
    constexpr std::array<size_t, 34> lengths{{
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
        31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 8191, 8192,
        8193, 16384,
    }};
    size_t cases = 0;
    ring::copyToDeviceBytes(nullptr, nullptr, 0);
    for (size_t destinationOffset = 0; destinationOffset < 16; ++destinationOffset) {
        for (size_t sourceOffset = 0; sourceOffset < 16; ++sourceOffset) {
            for (size_t length : lengths) {
                std::vector<uint8_t> source(sourceOffset + length);
                for (size_t i = 0; i < source.size(); ++i)
                    source[i] = uint8_t(i * 37u + 11u);
                std::vector<uint8_t> destination(16 + destinationOffset + length + 16, 0xa5);
                auto expected = destination;
                for (size_t i = 0; i < length; ++i)
                    expected[16 + destinationOffset + i] = source[sourceOffset + i];
                ring::copyToDeviceBytes(destination.data() + 16 + destinationOffset,
                    length ? source.data() + sourceOffset : nullptr, length);
                expect(destination == expected, "device byte copy changed bytes or guards");
                ++cases;
            }
        }
    }
    std::printf("device byte-copy contents/guards: %zu cases (host RAM, not MMIO proof)\n", cases);
}

static void testRingRecordPublication() {
    const size_t bytes = ring::kRingBytes + 0x1000;
    auto* map = static_cast<uint8_t*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    expect(map != MAP_FAILED, "ring publication fixture allocation");
    Spi spi;
    spi.bitstreamMap_ = map;
    spi.bitstreamMapLen_ = bytes;
    spi.bitstreamMemFd_ = statusFd;
    spi.bitstreamProtocolVersion_ = abi::kFpgaVideoAbiVersion;
    auto word = [&](uint32_t address) -> volatile uint64_t& {
        return *reinterpret_cast<volatile uint64_t*>(map + address - ring::kDataPhys);
    };
    constexpr uint64_t session = 0x123456789abcdef0ULL;
    constexpr uint32_t sequence = 0x89abcdef;
    constexpr size_t maxPayload = ring::kRingBytes - ring::kControlReserveBytes -
        ring::kRecordHeaderBytes - ring::kAccessUnitMetadataBytes;
    std::vector<uint8_t> payload(maxPayload);
    std::array<uint8_t, ring::kAccessUnitMetadataBytes> metadata{};
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 53u + 19u);
    for (size_t i = 0; i < metadata.size(); ++i) metadata[i] = uint8_t(i * 7u + 3u);
    size_t cases = 0;
    auto run = [&](uint32_t start, size_t prefixLength, size_t payloadLength,
                   Spi::BitstreamPushResult result = Spi::BitstreamPushResult::Ok,
                   uint32_t occupied = 0) {
        std::memset(map, 0xa5, bytes);
        spi.bitstreamWriteCount_ = start;
        spi.bitstreamResetEpoch_ = (start & 1) != 0;
        word(ring::kReadPhys) = ring::kReadMagic |
            (uint64_t((start - occupied) & ring::kCountMask) << 32);
        word(ring::kErrPhys) = ring::kErrMagic;
        word(ring::kStat6Phys) = ring::kStat6Magic;
        const uint64_t oldCtrl = word(ring::kCtrlPhys);
        std::vector<uint8_t> expected(map, map + bytes);
        const uint32_t total = uint32_t(ring::kRecordHeaderBytes + prefixLength + payloadLength);
        const uint32_t next = (start + total) & ring::kCountMask;
        if (result == Spi::BitstreamPushResult::Ok) {
            std::array<uint8_t, ring::kRecordHeaderBytes> header{};
            ring::putLe32(header.data(), ring::kRecordMagic);
            header[4] = uint8_t(ring::Event::AccessUnit);
            header[6] = uint8_t(abi::kFpgaVideoAbiVersion);
            header[7] = uint8_t(abi::kFpgaVideoAbiVersion >> 8);
            ring::putLe64(header.data() + 8, session);
            ring::putLe32(header.data() + 16, sequence);
            ring::putLe32(header.data() + 20, uint32_t(prefixLength + payloadLength));
            uint32_t offset = start;
            auto append = [&](const uint8_t* source, size_t length) {
                for (size_t i = 0; i < length; ++i)
                    expected[(offset++) & (ring::kRingBytes - 1)] = source[i];
            };
            append(header.data(), header.size());
            append(metadata.data(), prefixLength);
            append(payload.data(), payloadLength);
            ring::putLe64(expected.data() + ring::kCtrlPhys - ring::kDataPhys,
                (uint64_t(spi.bitstreamResetEpoch_) << 63) |
                (uint64_t(next) << 32) | ring::kCtrlMagic);
        }
        expect(spi.writeBitstreamRecord(ring::Event::AccessUnit, session, sequence, 0,
                   payloadLength ? payload.data() : nullptr, payloadLength, 0,
                   prefixLength ? metadata.data() : nullptr, prefixLength) == result,
               "ring record returned unexpected status");
        expect(std::memcmp(map, expected.data(), bytes) == 0,
               "ring record changed bytes, wrap segments, control or untouched memory");
        expect(spi.bitstreamWriteCount_ ==
                   (result == Spi::BitstreamPushResult::Ok ? next : start),
               "ring record count was padded, rounded or advanced on refusal");
        if (result != Spi::BitstreamPushResult::Ok)
            expect(word(ring::kCtrlPhys) == oldCtrl, "refused record published CTRL");
        ++cases;
    };
    for (uint32_t base : {0u, ring::kCountMask - uint32_t(ring::kRingBytes) + 1u}) {
        for (uint32_t offset = 0; offset < 112; ++offset) {
            const uint32_t start = base + (offset < 16 ? offset :
                uint32_t(ring::kRingBytes) - (offset - 15));
            for (size_t prefix : {size_t(0), metadata.size()})
                for (size_t length : {0u, 1u, 7u, 8u, 31u, 32u, 33u, 127u, 8191u, 8192u})
                    run(start, prefix, length);
        }
    }
    for (uint32_t start : {0u, uint32_t(ring::kRingBytes - 1), ring::kCountMask}) {
        run(start, metadata.size(), maxPayload);
        run(start, metadata.size(), maxPayload + 1, Spi::BitstreamPushResult::Fatal);
        run(start, metadata.size(), maxPayload, Spi::BitstreamPushResult::Full, 1);
    }
    spi.bitstreamMemFd_ = -1;
    std::printf("production ring record contents/counts/wrap/refusal: %zu cases "
                "(host RAM, not MMIO proof)\n", cases);
}

int main() {
    std::atexit(cleanup);
    testDeviceByteCopy();
    testRingRecordPublication();
    const size_t bytes = ring::kRingBytes + 0x1000;
    auto* map = static_cast<uint8_t*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    expect(map != MAP_FAILED, "anonymous fixture allocation");
    auto word = [&](uint32_t addr) -> volatile uint64_t& {
        return *reinterpret_cast<volatile uint64_t*>(map + addr - ring::kDataPhys);
    };
    constexpr uint32_t modeAddr = ring::kCtrlPhys + 0xf00;
    constexpr uint32_t resetSeenAddr = modeAddr + 8;
    constexpr uint32_t quitAddr = modeAddr + 16;
    constexpr uint32_t coreResetAddr = modeAddr + 24;
    constexpr uint32_t coreResetDoneAddr = modeAddr + 32;
    // mode bit0 drops audio ACK; bit1 keeps decoder reset pending;
    // bit2 publishes a stale nonce; bit3 enables the fixture's video Begin ACK.
    // Video End/Flush ACKs are never fabricated.
    word(modeAddr) = 0;
    word(quitAddr) = 0;
    const pid_t child = fork();
    expect(child >= 0, "fork fixture consumer");
    if (child == 0) {
        uint64_t previousCommit = 0, lastCtrl = 0;
        uint64_t epoch = 0, nonce = 0, consumed = 0;
        uint32_t publication = 0;
        uint8_t flags = audio::kSupported | audio::kPaused;
        bool reset_waiting = false;
        uint64_t core_reset_seen = 0;
        auto invalidateAudio = [&] {
            const auto published = uint32_t(word(abi::kAudioStatusCommitAddr) >> 32);
            if (publication < published) publication = published;
            word(abi::kAudioStatusCommitAddr) = 0;
            __sync_synchronize();
        };
        auto commitAudio = [&] {
            __sync_synchronize();
            word(abi::kAudioStatusCommitAddr) = abi::kAudioStatusCommitMagic |
                (uint64_t(++publication) << 32);
        };
        while (!word(quitAddr)) {
            if (word(coreResetAddr) != core_reset_seen) {
                core_reset_seen = word(coreResetAddr);
                flags = audio::kSupported | audio::kPaused;
                consumed = 0;
                invalidateAudio();
                word(abi::kAudioStatusAddr) =
                    (word(abi::kAudioStatusAddr) & ~(uint64_t(0xff) << 40)) |
                    (uint64_t(flags) << 40);
                word(abi::kAudioStatusAddr + 32) = 0;
                commitAudio();
                // A core-local reset loses video identity, not the retained
                // consumer epoch/nonce or the HPS CTRL word. No reset ACK yet.
                word(ring::kReadPhys) = 0;
                word(ring::kErrPhys) = 0;
                word(ring::kStat3Phys) = ring::kStat3Magic;
                word(ring::kStat4Phys) = ring::kStat4Magic;
                word(ring::kStat6Phys) = ring::kStat6Magic;
                word(abi::kVideoPresentationCommitAddr) = 0;
                reset_waiting = false;
                lastCtrl = 0;
                word(coreResetDoneAddr) = core_reset_seen;
            }
            uint64_t ctrl = word(ring::kCtrlPhys);
            if (ctrl != lastCtrl && uint32_t(ctrl) == ring::kCtrlMagic) {
                lastCtrl = ctrl;
                if ((ctrl >> 32 & ring::kCountMask) == 0) {
                    reset_waiting = true;
                    word(resetSeenAddr) = ctrl;
                } else if ((word(modeAddr) & 8) &&
                           uint8_t(word(ring::kDataPhys) >> 32) == uint8_t(ring::Event::Begin)) {
                    const auto videoEpoch = word(ring::kDataPhys + 8);
                    word(ring::kStat3Phys) = ring::kStat3Magic | (uint64_t(uint32_t(videoEpoch)) << 32);
                    word(ring::kStat4Phys) = ring::kStat4Magic | (videoEpoch & 0xffffffff00000000ULL);
                    word(ring::kStat6Phys) = ring::kStat6Magic |
                        (uint64_t(1) << (32 + ring::kStat6ActiveFlagBit));
                    word(ring::kErrPhys) = ring::kErrMagic |
                        (uint64_t(1) << ring::kErrActiveBit) |
                        (((ctrl >> 63) & 1) << ring::kErrResetEpochBit);
                    word(ring::kReadPhys) = ring::kReadMagic |
                        (uint64_t(uint32_t(ctrl >> 32) & ring::kCountMask) << 32);
                }
            }
            if (reset_waiting && !(word(modeAddr) & 2)) {
                word(ring::kReadPhys) = ring::kReadMagic;
                word(ring::kErrPhys) = ring::kErrMagic |
                    (((ctrl >> 63) & 1) << ring::kErrResetEpochBit);
                word(ring::kStat6Phys) = ring::kStat6Magic;
                reset_waiting = false;
            }
            const uint64_t commit = word(abi::kAudioControlCommitAddr);
            if (uint32_t(commit) == abi::kAudioControlCommitMagic &&
                commit != previousCommit && !(word(modeAddr) & 1)) {
                const auto op = Control(uint8_t(word(abi::kAudioControlAddr) >> 48));
                const uint64_t reqEpoch = word(abi::kAudioControlAddr + 8);
                const uint64_t reqNonce = word(abi::kAudioControlAddr + 16);
                const uint64_t token = word(abi::kAudioControlAddr + 24);
                previousCommit = commit;
                if (op == Control::Begin || op == Control::Reset) {
                    epoch = reqEpoch; nonce = reqNonce; consumed = 0;
                    flags = audio::kSupported | audio::kPaused |
                            (op == Control::Begin ? audio::kActive : 0);
                } else if (op == Control::Pause) flags |= audio::kPaused;
                else if (op == Control::Resume) flags &= ~audio::kPaused;
                invalidateAudio();
                word(abi::kAudioStatusAddr) = abi::kAudioStatusMagic |
                    (uint64_t(abi::kAudioSessionAbiVersion) << 32) |
                    (uint64_t(flags) << 40) | (uint64_t(op) << 48);
                word(abi::kAudioStatusAddr + 8) = epoch;
                word(abi::kAudioStatusAddr + 16) = nonce;
                word(abi::kAudioStatusAddr + 24) = token;
                word(abi::kAudioStatusAddr + 32) = consumed;
                word(abi::kAudioStatusAddr + 40) = reqEpoch;
                word(abi::kAudioStatusAddr + 48) = reqNonce - ((word(modeAddr) & 4) ? 1 : 0);
                commitAudio();
            }
            usleep(100);
        }
        _exit(0);
    }
    consumer = child;
    auto coreReset = [&] {
        const auto request = word(coreResetAddr) + 1;
        word(coreResetAddr) = request;
        for (unsigned wait = 0; wait < 1000 && word(coreResetDoneAddr) != request; ++wait)
            usleep(100);
        expect(word(coreResetDoneAddr) == request, "fixture core-local reset timed out");
    };
    Spi spi;
    spi.bitstreamMap_ = map;
    spi.bitstreamMapLen_ = bytes;
    spi.bitstreamMemFd_ = 123456789; // Fixture mapping only; never opened or used.
    spi.bitstreamSessionActive_ = true;
    spi.bitstreamSessionId_ = 0x123456789abcdef0ULL;
    spi.bitstreamProtocolVersion_ = abi::kFpgaVideoAbiVersion;
    spi.videoCapabilities_.nonce = 0x1020304050607080ULL;
    spi.videoCapabilities_.features =
        ring::ConsumedAudioClock | ring::AudioSessionControl | ring::FencedReset;
    const auto epoch = spi.bitstreamSessionId_;
    word(ring::kStat3Phys) = ring::kStat3Magic | (uint64_t(uint32_t(epoch)) << 32);
    word(ring::kStat4Phys) = ring::kStat4Magic | (epoch & 0xffffffff00000000ULL);
    expect(!spi.controlAudioSession(epoch - 1, Control::Begin, 5), "accepted wrong epoch");
    producerPosition = 3;
    expect(!spi.controlAudioSession(epoch, Control::Begin, 5), "accepted a non-stereo DMA pointer");
    producerPosition = 0;
    movingProducer = true;
    expect(!spi.controlAudioSession(epoch, Control::Begin, 5), "accepted a moving producer");
    movingProducer = false;
    producerPosition = 512 * 1024 - 4;
    expect(spi.controlAudioSession(epoch, Control::Begin, 100), "real Begin ACK");
    expect(word(abi::kAudioControlAddr + 32) == producerPosition,
           "Begin failed to publish the exact half-qword kernel pointer");
    for (auto mode : {ProducerReadMode::OpenFailure, ProducerReadMode::ReadFailure,
                      ProducerReadMode::MissingPointer}) {
        producerReadMode = mode;
        const auto previousCommit = word(abi::kAudioControlCommitAddr);
        const auto previousToken = spi.audioCommandToken_;
        expect(!spi.controlAudioSession(epoch, Control::Reset, 5),
               "unknown producer pointer silently became zero");
        expect(spi.audioSessionOwned_ && spi.audioCommandToken_ == previousToken &&
               word(abi::kAudioControlCommitAddr) == previousCommit,
               "unknown producer pointer published a command or lost ownership");
    }
    producerReadMode = ProducerReadMode::Normal;
    uint32_t presentationPublication = 0;
    auto publishPresentation = [&](uint64_t frozenSamples, bool hasClock) {
        const uint32_t flags = ring::kVideoActive | ring::kVideoHasFrame |
                               (hasClock ? ring::kVideoAudioClock : 0);
        const std::array<uint64_t, 9> frame{{
            abi::kVideoPresentationMagic | (uint64_t(flags) << 32),
            epoch, 90000, (uint64_t(90000) << 32) | 1,
            (uint64_t(3) << 32) | 17, frozenSamples, spi.videoCapabilities_.nonce, 0,
            abi::kVideoPresentationCommitMagic | (uint64_t(++presentationPublication) << 32),
        }};
        word(abi::kVideoPresentationCommitAddr) = 0;
        __sync_synchronize();
        for (size_t i = 0; i + 1 < frame.size(); ++i)
            word(abi::kVideoPresentationAddr + uint32_t(i * 8)) = frame[i];
        __sync_synchronize();
        word(abi::kVideoPresentationCommitAddr) = frame.back();
    };
    for (const auto& samples : {std::array<uint64_t, 2>{0, 456},
                               std::array<uint64_t, 2>{7, 11},
                               std::array<uint64_t, 2>{123, 579}}) {
        const uint64_t frozenSamples = samples[0], liveSamples = samples[1];
        const auto nextAudioCommit = word(abi::kAudioStatusCommitAddr) + (uint64_t(1) << 32);
        word(abi::kAudioStatusCommitAddr) = 0;
        __sync_synchronize();
        word(abi::kAudioStatusAddr + 32) = liveSamples;
        __sync_synchronize();
        word(abi::kAudioStatusCommitAddr) = nextAudioCommit;
        publishPresentation(frozenSamples, true);
        Spi::VideoPresentation frame;
        Spi::AudioSessionStatus current;
        expect(spi.readVideoPresentation(epoch, frame) && spi.readAudioSessionStatus(epoch, current),
               "paired presentation/live audio snapshots were unavailable");
        expect(frame.has_audio_clock && frame.audio_samples_consumed == frozenSamples &&
               current.samples_consumed == liveSamples &&
               frame.seq == 17 && frame.pts == 90000 && frame.presentation_count == 3,
               "live MAST overwrote the frozen frame/audio pairing");
    }
    publishPresentation(0, false);
    Spi::VideoPresentation frame;
    expect(spi.readVideoPresentation(epoch, frame) && !frame.has_audio_clock &&
           frame.audio_samples_consumed == 0, "live MAST invented a missing frame audio clock");
    publishPresentation(123, true);
    const auto audioCommit = word(abi::kAudioStatusCommitAddr);
    word(abi::kAudioStatusCommitAddr) = 0;
    Spi::AudioSessionStatus current;
    expect(spi.readVideoPresentation(epoch, frame) && frame.has_audio_clock &&
           frame.audio_samples_consumed == 123 && !spi.readAudioSessionStatus(epoch, current),
           "an unavailable live clock invalidated committed frame/audio feedback");
    word(abi::kAudioStatusCommitAddr) = audioCommit;
    expect(!spi.endBitstreamSession(epoch, 5), "End dropped owned audio");
    expect(spi.controlAudioSession(epoch, Control::Resume, 100), "real Resume ACK");
    expect(spi.controlAudioSession(epoch, Control::Pause, 100), "real Pause ACK");
    word(modeAddr) = 4;
    expect(!spi.controlAudioSession(epoch, Control::Resume, 10), "accepted stale ACK nonce");
    word(modeAddr) = 0;
    expect(spi.controlAudioSession(epoch, Control::Resume, 100), "fresh retry Resume ACK");
    word(modeAddr) = 1;
    expect(!spi.controlAudioSession(epoch, Control::Pause, 10), "accepted stale token/op ACK");
    expect(!spi.abortFpgaVideoSession(epoch, 10), "abort cleared ownership without audio ACK");
    expect(spi.bitstreamSessionActive_ && spi.audioSessionOwned_ &&
           spi.bitstreamAbortPending_, "audio timeout lost recovery ownership");
    const auto failedAudioToken = spi.audioCommandToken_;
    coreReset();
    Spi::AudioSessionStatus retainedAudio;
    expect(spi.readAudioSessionStatus(epoch, retainedAudio) && !retainedAudio.active &&
           retainedAudio.session_id == epoch && retainedAudio.nonce == spi.videoCapabilities_.nonce &&
           spi.audioSessionOwned_ && !spi.bitstreamResetPending_ &&
           (word(ring::kStat3Phys) >> 32) == 0 && (word(ring::kStat4Phys) >> 32) == 0,
           "manual core reset lost old audio ownership or fabricated its Reset ACK");
    word(modeAddr) = 2;
    expect(!spi.abortFpgaVideoSession(epoch, 20), "abort cleared ownership before decoder ACK");
    expect(spi.bitstreamSessionActive_ && !spi.audioSessionOwned_ &&
           !spi.bitstreamResetPending_ && spi.videoCapabilities_.nonce != 0,
           "decoder timeout discarded retained epoch/capability");
    expect(spi.readAudioSessionStatus(epoch, retainedAudio) &&
           retainedAudio.command == Control::Reset && retainedAudio.token > failedAudioToken &&
           audio::completed(retainedAudio, Control::Reset),
           "recovery reused an old Audio Reset ACK after local reset");
    auto pendingCtrl = word(resetSeenAddr);
    expect(pendingCtrl != 0, "fresh CTRL reset not issued");
    const auto completedAudioToken = spi.audioCommandToken_;
    coreReset();
    expect(!spi.abortFpgaVideoSession(epoch, 10), "pending decoder reset incorrectly succeeded");
    expect(word(ring::kCtrlPhys) != pendingCtrl, "retry did not toggle a fresh fence after the timeout");
    expect(spi.audioCommandToken_ == completedAudioToken && !spi.audioSessionOwned_,
           "manual reset forgot the already completed audio fence");
    expect(!spi.beginBitstreamSession(epoch + 1, 5), "begin stole failed-abort ownership");
    expect(!spi.pauseBitstreamSession(epoch, 5), "normal control escaped pending abort");
    expect(!spi.drainBitstreamSession(epoch, 5), "Drain escaped pending abort");
    expect(!spi.controlAudioSession(epoch, Control::Resume, 5), "Resume escaped pending abort");
    word(modeAddr) = 0;
    expect(spi.abortFpgaVideoSession(epoch, 100), "fresh decoder reset completion");
    expect(!spi.bitstreamSessionActive_ && !spi.bitstreamResetPending_ &&
           !spi.bitstreamAbortPending_ && spi.bitstreamSessionId_ == 0,
           "successful fenced reset did not release ownership");
    for (bool flush : {false, true}) {
        spi.bitstreamSessionActive_ = true;
        spi.bitstreamSessionId_ = epoch;
        spi.bitstreamProtocolVersion_ = abi::kFpgaVideoAbiVersion;
        spi.videoCapabilities_.nonce = 0x1020304050607080ULL;
        spi.videoCapabilities_.features = ring::FencedReset;
        expect(!(flush ? spi.flushBitstreamSession(epoch, 10) :
                         spi.endBitstreamSession(epoch, 10)), "missing End/Flush ACK succeeded");
        expect(spi.bitstreamSessionActive_, "End/Flush timeout lost ownership");
        expect(spi.abortFpgaVideoSession(epoch, 100), "failed End/Flush wedged recovery");
        expect(!spi.bitstreamSessionActive_, "End/Flush abort retained completed ownership");
    }
    word(modeAddr) = 8;
    expect(spi.beginBitstreamSession(epoch + 3, 100) &&
           spi.bitstreamSessionId_ == epoch + 3 && spi.bitstreamSessionActive_,
           "completed recovery permanently wedged the next Begin");
    word(quitAddr) = 1;
    int status = 0;
    waitpid(child, &status, 0);
    consumer = -1;
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fixture consumer failed");
    spi.bitstreamMemFd_ = -1;
    std::puts("audio host ACK identity, frozen/live clock separation, timeout retention, "
              "and bounded fenced recovery passed");
}
