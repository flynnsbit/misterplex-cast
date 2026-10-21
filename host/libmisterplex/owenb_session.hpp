#pragma once
// OWENB-P1B T5: the OB play loop. Demuxed packets (Annex-B AUs with 90 kHz PTS, 48 kHz s16le stereo
// PCM) go out to OwenbRing (owenb_ring.hpp) in container order, with the cast timeline's controls.
// Design: OWENB-P1B-CAST-DESIGN.md §a.4 (CAVLC rewrite + rate guard + lowered retry), §a.5 (audio,
// volume), §b.3 (pause / resume / seek held / stop / end hold / position), §c.4 (no DDR paint).
//
// It writes only the owenb ring (ctrl page 0x31400000 + ring 0x31800000, through the OwenbRing
// window). It never touches the idle FB at 0x31600000 and never paints DDR.
//
// Threads: run() is the play thread. pause / resume / seek / stop / setVolumePercent may be called
// from any thread; they queue commands that run() takes between records. positionMs() and the other
// readers are lock-free.
//   - Video AUs: dropped until the first keyframe of a stream (session start, seek). The last AU before
//     the end of the input carries END_OF_STREAM, so the core finishes it and holds it (one AU of
//     lookahead; audio behind it waits, so the order is the container's).
//   - CAVLC: the library rewrites it to CABAC (OwenbRing::setCavlcRewrite, default on). The guard
//     ends the session with NeedsTranscode when a CAVLC stream runs above cavlcMaxKbps
//     (OB_CAVLC_MAX_KBPS, default 4,000), or when the rewrite refuses a unit (-EOPNOTSUPP /
//     -EBADMSG). The daemon then asks PMS for one lowered (re-encoded, High/CABAC) request.
//   - Pause: h264ring_set_pause(1), and the input is not read until resume. Ignored before the first
//     picture (design §b.3: cast clients pause while buffering).
//   - Seek (held): pause (the picture stays), open the new input, read to its first keyframe, then
//     h264ring_flush (a new stream) and the keyframe, then resume unless the user paused.
//   - Stop: IDLE_SCREEN=last pauses and releases (the picture is held, nothing more plays); black
//     flushes and ends.
//   - End of input: END_OF_STREAM on the last AU, wait until the core has taken every record, then
//     end (hold) or flush + end (black).

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "owenb_ring.hpp"

namespace misterplex::owenb {

struct MediaPacket {
    enum class Kind : uint8_t { Video, Audio };
    Kind kind = Kind::Video;
    std::vector<uint8_t> data;              // Video: one Annex-B AU; Audio: 48 kHz s16le stereo
    int64_t pts90k = h264stream::kNoPts90k;
    bool keyframe = false;                  // Video: an IDR (random access point)
};

enum class SourceRead : uint8_t { Packet, Again, Eof, Error };

// One demuxed input (OwenbAvSource on the daemon: libav; a scripted source in tests).
class IMediaSource {
public:
    virtual ~IMediaSource() = default;
    virtual SourceRead read(MediaPacket& p, std::string& err) = 0;
    virtual void cancel() {}                // unblock a read from another thread (stop)
    // 0008: the container's display aspect (num:den), when the container states one (MP4 pasp, MKV
    // display size); false: none, the picture's own shape applies
    virtual bool containerAspect(int64_t& num, int64_t& den) const {
        (void)num;
        (void)den;
        return false;
    }
    // MEMSTAT. Defaults so a scripted test source stays zero.
    virtual size_t readyDepth() const { return 0; }
    virtual unsigned probePackets() const { return 0; }
};

struct OpenRequest {
    std::string url;
    std::string audioUrl;                  // separate DASH audio; empty when the url is muxed
    std::string headers;
    int64_t startMs = 0;                    // the position this input starts at (timeline)
    bool containerSeek = false;             // seek the container to startMs (a file); a PMS URL
                                            // carries the offset itself
    bool retryTransientOpen = false;        // empty body / I/O / helper 5xx: retry, stay on this play
    int64_t previousMs = -1;                // seek resume point; -1 when this open is not a seek
    // One googlevideo 403: rewrite url/audio and open again. Empty when this
    // open is not a YouTube cast. The callback clears itself after one try.
    std::function<bool(std::string& url, std::string& audio)> reresolve403;
};

using SourceFactory = std::function<std::unique_ptr<IMediaSource>(const OpenRequest&, std::string& err)>;

enum class EndReason : uint8_t { None, Eof, Stopped, NeedsTranscode, CoreLost, RingError, SourceError };
const char* toString(EndReason r);

// Plex volume 0..100 to the core's attenuation, 0.5 dB steps (design Q7): 0 mutes (255), 100 is 0,
// else min(254, round(-40 log10(v / 100))), so 50 -> 12 (-6 dB).
unsigned attenuationForVolume(int volume);

// The PPS's entropy_coding_mode_flag in an Annex-B AU: 1 CABAC, 0 CAVLC, -1 no (parsable) PPS.
int ppsEntropyCabac(const uint8_t* au, size_t len);

struct SessionConfig {
    bool holdLastPicture = true;        // IDLE_SCREEN=last (default); false = black (flush) at stop / end
    unsigned cavlcMaxKbps = 4000;       // OB_CAVLC_MAX_KBPS; 0 = no rate guard
    unsigned cavlcWindowMs = 2000;      // the guard's measuring window, in PTS time
    // v2-bugfix (a lab MiSTer bug 1): the guard ends a CAVLC stream only when the rate is above cavlcMaxKbps AND the
    // library's rewrite (timed around each CAVLC AU write) costs more than this share of one A9 over the
    // window. A stream whose rewrite keeps real time plays on. 0 = the rate alone decides (the T5 rule).
    // OB_CAVLC_MAX_LOAD_PCT. Design Q6: 70 % is the A9 budget next to the demux.
    unsigned cavlcMaxLoadPct = 70;
    // BUG1-CAP.md: above this the play ends whatever the measured load (a backstop for a clock that lies):
    // ~7.7 M bins/s x ~100 ns + the per-macroblock part is ~80 % of one A9 even at 24p. OB_CAVLC_HARD_MAX_KBPS.
    unsigned cavlcHardMaxKbps = 7000;
    // BUG1 (lab 7AE78D80): what the stream's entropy turned out to be, for the per-PMS memory
    // (owenb_pms_traits.hpp): (false, 0, 0) once when the PPS says CABAC; (true, kbps, load %) at each CAVLC
    // window whose load is known (also the one that ends the play). Called on the play thread.
    std::function<void(bool cavlc, unsigned kbps, unsigned loadPct)> onEntropy;
    std::function<int64_t()> nowNs;     // the rewrite timer's clock (steady_clock when unset; tests inject one)
    unsigned pcmRecordBytes = 3840;     // PCM records of at most 20 ms (48 kHz x 4 bytes x 0.02 s)
    unsigned backoffUs = 2000;          // a full ring: wait, then try again
    unsigned idleUs = 10000;            // paused / draining: the poll period
    unsigned drainTimeoutMs = 30000;    // end of input: give up waiting for the core after this
    // The loop's sleep. The daemon's is a real sleep; a test's advances its fake core.
    std::function<void(unsigned)> sleepUs;
    // Called once per loop iteration (tests inject commands here; the daemon leaves it unset).
    std::function<void()> onIteration;
    std::function<void(const std::string&)> log;
    // GCAST open retry: lounge BUFFERING, overlay stays off the idle card.
    std::function<void(int64_t posMs)> onBuffering;
};

struct SessionStats {
    uint64_t videoAus = 0, audioRecords = 0, audioBytes = 0;
    uint64_t droppedBeforeKey = 0, droppedAudioBeforeKey = 0;
    uint64_t flushes = 0, seeks = 0, pauses = 0, resumes = 0, fullWaits = 0;
    uint64_t cavlcAus = 0;
    bool eosSent = false;
};

class OwenbPlaySession {
public:
    OwenbPlaySession(OwenbRing& ring, SourceFactory factory, SessionConfig cfg);
    OwenbPlaySession(const OwenbPlaySession&) = delete;
    OwenbPlaySession& operator=(const OwenbPlaySession&) = delete;

    // The play thread: opens req, begins session sessionId on the ring, plays to the end or a stop.
    EndReason run(const OpenRequest& req, uint64_t sessionId);

    // Any thread.
    void pause();
    void resume();
    void seek(const OpenRequest& req);
    void stop();
    void setVolumePercent(int volume);

    int64_t positionMs() const { return positionMs_.load(); }
    bool paused() const { return userPaused_.load(); }
    bool started() const { return started_.load(); }      // the core showed this stream's first picture
    bool running() const { return running_.load(); }
    std::string error() const;
    SessionStats stats() const;
    size_t queuedBytes() const { return outBytes_; }
    size_t outDepth() const { return outDepth_.load(); }
    size_t readyDepth() const;
    unsigned probeBound() const;
    // Backpressure cap for out_. A sender that interrupts the ring write must not
    // keep reading the elementary stream into this deque.
    static constexpr size_t kOutByteCap = 4u * 1024u * 1024u;

    // Callbacks (set before run; called on the play thread).
    std::function<void()> onFirstPicture;                 // once per run (play started)

private:
    struct Cmd {
        enum class K : uint8_t { Pause, Resume, Seek, Stop, Volume } k;
        OpenRequest req;
        int volume = 100;
    };
    enum class Push : uint8_t { Ok, Interrupted, Fatal };

    bool openSource(const OpenRequest& req, int64_t* playedMs = nullptr);
    bool takeCommands();                        // false: the session ends (stop, fatal)
    bool interruptPending() const;
    bool applySeek(const OpenRequest& req);
    bool checkVideoIn(const MediaPacket& p);   // false: end_ set (the CAVLC rate guard)
    bool readOne();                             // false: end_ set (error); inputEof_ at the end
    bool sendReady(bool all);                   // false: end_ set or interrupted
    Push pushVideo(const MediaPacket& p, bool eos);
    Push pushAudio(const MediaPacket& p);
    Push pushWithBackoff(const std::function<h264stream::PushResult()>& f);
    void fatalFromRing(const char* what);
    void endWith(EndReason r, const std::string& why);
    void updatePosition();
    bool drainToEnd();
    void finish();
    void logLine(const std::string& s) const;
    void sleep(unsigned us);
    int64_t nowNs() const;

    OwenbRing& ring_;
    SourceFactory factory_;
    SessionConfig cfg_;
    uint64_t sid_ = 0;
    uint32_t seq_ = 0;

    mutable std::mutex cmdMu_;
    std::deque<Cmd> cmds_;
    std::atomic<int> pendingCmds_{0};

    mutable std::mutex srcMu_;
    std::unique_ptr<IMediaSource> src_;

    std::deque<MediaPacket> out_;               // read, not yet written (container order)
    std::atomic<size_t> outDepth_{0};
    size_t outBytes_ = 0;
    void syncOutDepth() { outDepth_.store(out_.size()); }
    bool needKey_ = true;
    bool flushBeforeKey_ = false;               // a held seek: flush the ring before its first key
    bool inputEof_ = false;
    bool corePaused_ = false;                   // the ring's pause word is set
    int entropyCabac_ = -1;                     // 1 CABAC, 0 CAVLC, -1 unknown
    int64_t rateStartPts_ = -1;
    uint64_t rateBytes_ = 0;
    int64_t rwFirstPts_ = -1, rwLastPts_ = -1;   // BUG1-CAP: the PTS span of the CAVLC AUs timed this window
    int64_t rewriteNs_ = 0;                     // time spent writing CAVLC AUs (the library rewrite) this window
    bool cavlcOverRateLogged_ = false;
    int64_t baseMs_ = 0;
    int64_t basePts_ = -1;
    int64_t lastVideoPts_ = -1;
    size_t videoInOut_ = 0;                     // video packets in out_
    size_t audioOffset_ = 0;                    // bytes of out_'s audio packet already written
    uint64_t drainStartUs_ = 0;                 // end of input: time spent waiting for the core
    int volume_ = 100;

    EndReason end_ = EndReason::None;
    mutable std::mutex errMu_;
    std::string error_;
    mutable std::mutex statsMu_;
    SessionStats stats_;

    std::atomic<int64_t> positionMs_{0};
    std::atomic<bool> userPaused_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopReq_{false};
};

} // namespace misterplex::owenb
