#pragma once
// MPX OwenbRing: misterplexd's producer for owenb321's MiSTer H.264 decoder core, ring v1.x
// (design: OWENB-P1B-CAST-DESIGN.md). It is a thin, mutex-guarded adapter over the vendored
// MIT client library (third_party/owenb-h264ring at eac1064). It implements the same
// h264stream::IBitstreamProducer contract as FpgaBitstreamProducer, plus IAvRecordProducer
// for the record-level parts: PTS, flags and PCM.
// T3: the core gate and sessionVideoBackend() select VideoBackend::OwenbRing per core. T5: the
// daemon links the library and feeds this producer through OwenbPlaySession (owenb_session.hpp).
// No RTL changes.

#include <cstdint>
#include <mutex>
#include <string>

#include "h264_bitstream_transport.hpp"
#include "owenb_ddr_window.hpp"

namespace misterplex::owenb {

enum class ProbeVerdict : uint8_t {
    NoMap,        // the DDR window is not mapped
    NoCore,       // no "MiSTH264" magic: another core, or none
    BadVersion,   // the major version differs, or the core's minor is older than ours (0x101)
    BadGeometry,  // the core reports a ring size that differs from the window's
    Stale,        // the words are present but the heartbeat is frozen (a previous load's words)
    Alive,
};
const char* toString(ProbeVerdict v);

struct RingProbe {
    ProbeVerdict verdict = ProbeVerdict::NoMap;
    uint64_t magic = 0;
    uint32_t version = 0;      // ctrl[1] [15:0], major << 8 | minor
    uint32_t ring_size = 0;    // ctrl[2] [63:32]
    uint32_t heartbeat = 0;    // ctrl[3] [31:0]: the status sequence, +1 per frame
};

// The core's status block, as T5's play loop reads it (h264ring_status, a seqlocked copy).
struct CoreStatus {
    bool valid = false;        // a consistent copy was read
    unsigned state = 0;        // H264RING_S_*
    unsigned err = 0;          // H264RING_E_* (the session's first)
    unsigned live = 0;         // H264RING_L_* bits
    int64_t stc90k = -1;       // the presentation clock (90 kHz), valid when live has L_STC_RUN
    bool sessionOk = false;    // the core still runs this handle's session (no OSD Reset)
    uint32_t ringLevel = 0;    // bytes written and not yet released by the core
    unsigned refusedCode = 0;  // FE_REFUSED_CODE: the last stream refusal (h264ring_refusal_name)
};

// Converts a timestamp in time base num/den seconds to 90 kHz, wrapped to 33 bits like the
// header's PTS field. Negative input, or a zero num or den, gives kNoPts90k.
int64_t toPts90k(int64_t ts, uint32_t num, uint32_t den);

// OWENB-P1B T6 (ARM overlay, interface 0.9): what the overlay renderer reads each tick. One
// h264ring_status copy: words 46 (ovl_*), 47 (aspect), 30/80 (raster, picture), 27 (the OSD mirror).
struct OverlayCoreStatus {
    bool valid = false;
    uint32_t gen = 0;              // ovl_gen: the generation of the last consistent descriptor read
    unsigned state = 0;            // H264RING_OVL_S_OFF / _ACCEPTED / _REFUSED
    unsigned err = 0;              // H264RING_OVL_E_* when refused
    bool at = false;               // the overlay is anti-twittered (480i / 576i)
    struct ::h264ring_raster raster{};
    unsigned videoOut = 0;         // h264ring_video_out(): 1 Interlaced, 2 Progressive, 3 Native
    unsigned osdAnalogAspect = 0;  // the menu's Analog Aspect: 0 Auto, 1 Fit, 2 Letterbox, 3 Crop
    unsigned osdAspect = 0;        // 0008: the menu's Aspect Ratio: 0 Auto, 1 4:3, 2 16:9
    unsigned setSeq = 0;           // 0006: SET_SEQ, the last settings request the core consumed
    unsigned setErr = 0;           // 0006: SET_ERR, its result (0 applied)
    bool session = false;          // the core runs this handle's session (the overlay is shown only then)
};

class OwenbRing final : public h264stream::IAvRecordProducer {
public:
    explicit OwenbRing(OwenbDdrWindow& window, unsigned timeout_ms = 250);
    ~OwenbRing() override;
    OwenbRing(const OwenbRing&) = delete;
    OwenbRing& operator=(const OwenbRing&) = delete;

    // Reads the descriptor and waits up to heartbeat_ms for the heartbeat to advance. On Alive,
    // the handle stays attached for begin(). Probe never writes a word.
    RingProbe probe(unsigned heartbeat_ms = 100);

    h264stream::ControlResult begin(uint64_t session_id) override;
    // Once per ring handle. Does not open a session: the next begin() is a
    // new session, and a new session resets the decoder and display.
    h264stream::ControlResult reinitOutput();
    h264stream::PushResult pushNal(const h264stream::NalView& nal) override;
    // flush() is SEEK: the core drops what it has queued and shows black until the next IDR.
    h264stream::ControlResult flush(uint64_t session_id) override;
    // end() keeps the last picture on screen (IDLE_SCREEN=last). It releases the session
    // without a flush. Mark the last AU end_of_stream so the core finishes decoding it.
    h264stream::ControlResult end(uint64_t session_id) override;
    h264stream::ControlResult pause(uint64_t session_id) override;
    h264stream::ControlResult resume(uint64_t session_id) override;
    h264stream::Telemetry status() const override;

    h264stream::PushResult pushAccessUnit(const h264stream::AccessUnitView& au) override;
    h264stream::PushResult pushPcm(const h264stream::PcmView& pcm) override;

    // Volume: attenuation in 0.5 dB steps, 0 = full volume, 255 = mute.
    h264stream::ControlResult setAttenuation(uint64_t session_id, unsigned half_db);

    // T5: release the session without touching the pause word. A stop with IDLE_SCREEN=last
    // pauses first, so the core keeps the current picture and plays nothing more. The next session
    // start rewrites the pause word (the library writes this program's state at every start).
    h264stream::ControlResult release(uint64_t session_id);
    // T5: the status block (valid=false when no handle or no consistent copy).
    CoreStatus coreStatus() const;
    // T6 overlay port, each under the handle's mutex for microseconds (the handle is not
    // thread-safe, h264ring.h:33): show returns the generation (> 0) or -errno.
    int overlayShow(const struct ::h264ring_overlay& o);
    int overlayHide();
    OverlayCoreStatus overlayStatus() const;
    // T6: the core's OSD settings (interface 0.4). request: the sequence (1..255) or -errno.
    int requestSettings(unsigned mask, const unsigned values[H264RING_OPT_COUNT]);
    int clearSettings();
    // 0010: the input region (h264ring_input: a seqlocked copy of words 48-71, no session needed,
    // h264ring.h:382-401). 0, -ENODEV with no handle, or the library's -EAGAIN.
    int input(struct ::h264ring_input& in) const;
    // T5: why the CAVLC rewrite refused the last unit (h264ring_rewrite_why), "" if it did not.
    std::string rewriteWhy() const;

    // CAVLC streams are rewritten to CABAC in the library (default on; design Q6). Off: AUs go
    // out as given, and the core refuses CAVLC itself.
    void setCavlcRewrite(bool on);

    int lastErrno() const;           // the library's last negative return (0 = none)
    std::string lastError() const;

private:
    h264stream::PushResult mapWrite(int rc, const char* what);
    void fail(int rc, const char* what);

    OwenbDdrWindow& window_;
    unsigned timeout_ms_;
    mutable std::mutex mu_;
    struct ::h264ring* ring_ = nullptr;
    uint64_t session_id_ = 0;
    uint32_t producer_seq_ = 0;
    uint32_t consumer_seq_ = 0;
    uint64_t bytes_accepted_ = 0;
    uint64_t nal_accepted_ = 0;
    uint64_t overrun_count_ = 0;
    uint64_t desync_count_ = 0;
    uint32_t last_bad_seq_ = 0;
    bool active_ = false;
    bool outputReinited_ = false;
    bool paused_ = false;
    bool rewrite_ = true;
    int last_errno_ = 0;
    std::string last_error_;
};

}  // namespace misterplex::owenb
