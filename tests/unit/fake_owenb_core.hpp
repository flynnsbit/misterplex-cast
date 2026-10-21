#pragma once
// OWENB-P1B T5: a paced fake PlexOB core for the play-session tests, on a HeapDdrWindow. Like
// test_owenb_ring.cpp's FakeCore (a port of owenb321's hps/tests/test_ring.c fake core, MIT, (c) 2026
// owenb321 and contributors; third_party/owenb-h264ring/LICENSE) it implements the boot rule, the
// nonce/ack and the seqlocked status words. On top of that it:
//   - consumes whole records, at most perStep a poll, and none while the pause word's bit 0 is set;
//   - logs the records it took per session (a new nonce = a new session; the queue left in the ring
//     at a new nonce is dropped, as the core does at a flush);
//   - (T6) takes the overlay descriptor (words 72-79) when gen_lo == gen_hi, checks it with
//     h264ring_overlay_check and reports ovl_gen / ovl_state / ovl_err (word 46); echoes a raster
//     (words 47, 80, 30) and the OSD mirror's Video Output / Analog Aspect (word 27), and applies an
//     Analog Aspect settings request (word 35) as the core's Main does;
//   - runs a clock: STC = the PTS of the last video AU taken (L_STC_RUN), L_ENDED and state ENDED
//     after an END_OF_STREAM AU, L_SESSION / L_PLAYED, and the attenuation byte it last saw.
// Records are parsed from the ring bytes here, independently of the library.
#include "libmisterplex/owenb_ddr_window.hpp"

#include <cstdint>
#include <vector>

extern "C" {
#include "h264ring.h"
}

namespace fakeob {

struct Record {
    unsigned type = 0, flags = 0, seq = 0;
    uint32_t len = 0;
    int64_t pts = -1;               // -1: no PTS (H264RING_F_PTS_VALID clear)
    std::vector<uint8_t> payload;
};

struct PacedCore {
    explicit PacedCore(misterplex::owenb::HeapDdrWindow& w) : win(w) {
        win.onStep = [this] { step(); };
        status();
    }
    misterplex::owenb::HeapDdrWindow& win;
    bool alive = true, error = false;
    unsigned perStep = 4;
    bool booted = false, sess = false, stcRun = false, ended = false, played = false;
    uint32_t seen = 0, rw = 0, seq = 0, ack = 0;
    uint64_t stc = 0;
    unsigned atten = 0, refused = 0;
    uint64_t pausedPolls = 0;
    std::vector<std::vector<Record>> sessions;   // the records each session's core took
    std::vector<bool> pausedAtStart;             // the pause word when each session's nonce arrived
    // T6 overlay / raster
    bool ovlTake = true;                         // false: the core stops reading descriptors (a stall)
    uint32_t ovlGen = 0;
    unsigned ovlState = 0, ovlErr = 0, ovlTakes = 0;
    struct ::h264ring_overlay ovl{};
    bool ovlEnabled = false;
    unsigned rasterW = 720, rasterH = 480, videoOut = H264RING_VOUT_PROGRESSIVE, arX = 4, arY = 3;
    bool rasterIl = false, rasterLb = false;
    int picX0 = 0, picY0 = 0;
    unsigned picW = 0, picH = 0;
    unsigned osdVideoOut = 0, osdAnalogAspect = 0, osdAspect = 0, aspectApplied = 0, setSeq = 0, settingsApplied = 0;
    unsigned setErr = 0;
    bool settingsTake = true;                    // false: the core stops taking settings (an OSD Reset, a stall)
    unsigned startApplies = 0;                   // requests re-applied at a session's start (h264ring.h:245-247)

    uint64_t* c() { return win.ctrlWords(); }
    // 0010: the input region as the core writes it (h264ring.h:382-401, the library's reader h264ring.c:102):
    // in_tail (word 71's low half) first, the words, in_head (word 48's low half) last, equal.
    uint32_t inSeq = 0;
    void writeInput(const struct ::h264ring_input& in) {
        ++inSeq;
        const unsigned last = H264RING_IN_WORDS - 1;
        c()[H264RING_IN_WORD + last] = (in.w[last] & ~0xFFFFFFFFull) | inSeq;
        for (unsigned i = 1; i < last; ++i)
            c()[H264RING_IN_WORD + i] = in.w[i];
        c()[H264RING_IN_WORD] = (in.w[0] & ~0xFFFFFFFFull) | inSeq;
    }
    uint32_t lo(unsigned i) { return static_cast<uint32_t>(c()[i]); }
    uint8_t byteAt(uint32_t i) {
        const uint32_t n = win.ringSize();
        return static_cast<uint8_t>(win.ringWords()[(i % n) / 8] >> (8 * (i % 8)));
    }
    uint64_t le64At(uint32_t i) {
        uint64_t v = 0;
        for (int k = 7; k >= 0; --k)
            v = (v << 8) | byteAt(i + static_cast<uint32_t>(k));
        return v;
    }
    void status() {
        if (!alive)
            return;
        seq++;
        const unsigned state = !sess ? H264RING_S_NO_SESSION
                             : error ? H264RING_S_ERROR
                             : ended ? H264RING_S_ENDED : H264RING_S_PLAYING;
        unsigned live = 0;
        if (sess) live |= H264RING_L_SESSION;
        if (stcRun) live |= H264RING_L_STC_RUN;
        if (ended) live |= H264RING_L_ENDED;
        if (played) live |= H264RING_L_PLAYED;
        c()[0] = H264RING_MAGIC;
        c()[1] = H264RING_VERSION | (32ull << 16) | (32ull << 32);
        c()[2] = 0x31800000ull | (static_cast<uint64_t>(win.ringSize()) << 32);
        c()[3] = seq | (static_cast<uint64_t>(H264RING_MAX_PAYLOAD) << 32);
        c()[4] = state | (static_cast<uint64_t>(live) << 16);
        c()[5] = rw | (static_cast<uint64_t>(ack) << 32);
        c()[7] = stc & ((1ull << 33) - 1);
        c()[18] = refused;
        c()[27] = (uint64_t(setErr & 7u) << H264RING_SET_ERR_LSB) |
                  (uint64_t(osdVideoOut & 3u) << H264RING_OSD_VIDEO_OUT_LSB) |
                  (uint64_t(osdAnalogAspect & 3u) << H264RING_OSD_ANALOG_ASPECT_LSB) |
                  (uint64_t(osdAspect & 3u) << H264RING_OSD_ASPECT_LSB);
        c()[30] = (uint64_t(rasterIl) << H264RING_RASTER_IL_LSB) | (uint64_t(setSeq & 0xFFu) << H264RING_SET_SEQ_LSB) |
                  (uint64_t(picW & 0x1FFFu) << H264RING_FIT_OW_LSB) | (uint64_t(picW && picH) << H264RING_FIT_VALID_LSB) |
                  (uint64_t(picH & 0x1FFFu) << H264RING_FIT_OH_LSB);
        c()[46] = ovlGen | (uint64_t(ovlState & 3u) << H264RING_OVL_STATE_LSB) |
                  (uint64_t(rasterIl && ovlState == 1) << H264RING_OVL_AT_LSB) | (uint64_t(ovlErr & 15u) << H264RING_OVL_ERR_LSB);
        c()[47] = (arX & 0xFFFu) | (uint64_t(arY & 0xFFFu) << H264RING_AR_Y_LSB);
        c()[80] = (rasterW & 0xFFFu) | (uint64_t(rasterH & 0xFFFu) << H264RING_RASTER_H_LSB) |
                  (uint64_t(rasterLb) << H264RING_RASTER_LB_LSB) | (uint64_t(videoOut & 3u) << H264RING_VIDEO_OUT_LSB) |
                  (uint64_t(uint32_t(picX0) & 0x1FFFu) << H264RING_PIC_X0_LSB) |
                  (uint64_t(uint32_t(picY0) & 0x1FFFu) << H264RING_PIC_Y0_LSB);
        c()[31] = seq;
    }
    void take(uint32_t wi) {
        for (unsigned k = 0; k < perStep && wi - rw >= 16; ++k) {
            const uint64_t w0 = le64At(rw), w1 = le64At(rw + 8);
            Record r;
            r.type = w0 & 0xFF;
            r.flags = (w0 >> 8) & 0xFF;
            r.seq = (w0 >> 16) & 0xFFFF;
            r.len = static_cast<uint32_t>(w0 >> 32);
            if (r.flags & H264RING_F_PTS_VALID)
                r.pts = static_cast<int64_t>(w1 & ((1ull << 33) - 1));
            const uint32_t n = 16u + ((r.len + 15u) & ~15u);
            if (wi - rw < n)
                return;
            if (r.type != H264RING_T_PAD) {
                for (uint32_t i = 0; i < r.len; ++i)
                    r.payload.push_back(byteAt(rw + 16 + i));
                if (r.type == H264RING_T_VIDEO_AU) {
                    played = true;
                    if (r.pts >= 0) {
                        stc = static_cast<uint64_t>(r.pts);
                        stcRun = true;
                    }
                    if (r.flags & H264RING_F_END_OF_STREAM)
                        ended = true;
                }
                if (sessions.empty())
                    sessions.emplace_back();
                sessions.back().push_back(std::move(r));
            }
            rw += n;
        }
    }
    void step() {
        if (!alive)
            return;
        const uint32_t wi = lo(H264RING_W_WRITE_INDEX), nonce = lo(H264RING_W_NONCE);
        atten = (lo(H264RING_W_PAUSE) >> 8) & 0xFF;
        if (!booted) {
            seen = nonce;   // an earlier program's nonce: ignored (the boot rule)
            booted = true;
        } else if (nonce != seen) {
            seen = nonce;
            sess = true;
            rw = wi & ~15u;   // the old queue is dropped
            ack = nonce;
            stcRun = ended = false;
            sessions.emplace_back();
            pausedAtStart.push_back(lo(H264RING_W_PAUSE) & 1u);
            if (settingsTake && (lo(H264RING_W_SETTINGS) >> H264RING_REQ_SEQ_LSB)) {   // the word, again
                applySettings(lo(H264RING_W_SETTINGS));
                ++startApplies;
            }
        } else if (sess && !error) {
            if (lo(H264RING_W_PAUSE) & 1u)
                ++pausedPolls;
            else
                take(wi);
        }
        overlayStep();
        status();
    }
    void overlayStep() {
        const uint32_t lo32 = lo(H264RING_W_OVL_GEN_LO), hi32 = lo(H264RING_W_OVL_GEN_HI);
        if (ovlTake && lo32 == hi32 && lo32 != ovlGen && lo32 != 0) {
            const uint32_t ctl = lo(H264RING_W_OVL_CTL);
            ovlGen = lo32;
            ++ovlTakes;
            ovlEnabled = ctl & 1u;
            if (!ovlEnabled) {
                ovlState = H264RING_OVL_S_OFF;
                ovlErr = 0;
            } else {
                struct ::h264ring_overlay o{};
                o.base = lo(H264RING_W_OVL_BASE);
                o.width = uint16_t(lo(H264RING_W_OVL_SIZE));
                o.height = uint16_t(lo(H264RING_W_OVL_SIZE) >> 16);
                o.stride = uint16_t(lo(H264RING_W_OVL_STRIDE));
                o.format = uint8_t((ctl >> 1) & 3u);
                o.band_first = uint16_t(lo(H264RING_W_OVL_BAND));
                o.band_last = uint16_t(lo(H264RING_W_OVL_BAND) >> 16);
                ovl = o;
                ovlErr = unsigned(h264ring_overlay_check(&o));
                ovlState = ovlErr ? H264RING_OVL_S_REFUSED : H264RING_OVL_S_ACCEPTED;
            }
        }
        const uint32_t sw = lo(H264RING_W_SETTINGS);
        const unsigned sq = sw >> H264RING_REQ_SEQ_LSB;
        if (sess && settingsTake && sq && sq != setSeq)
            applySettings(sw);
    }
    void applySettings(uint32_t sw) {
        setSeq = sw >> H264RING_REQ_SEQ_LSB;
        const unsigned mask = (sw >> H264RING_REQ_MASK_LSB) & 0x7Fu;
        setErr = mask ? 0u : 3u;
        if (mask & (1u << H264RING_OPT_ANALOG_ASPECT)) {
            osdAnalogAspect = (sw >> 7) & 3u;   // values: 2+2+2+1 bits before Analog Aspect
            ++settingsApplied;
        }
        if (mask & (1u << H264RING_OPT_ASPECT)) {   // 0008: values bits 0-1 (h264ring.c:1001)
            osdAspect = sw & 3u;
            ++aspectApplied;
        }
        if (mask & (1u << H264RING_OPT_VIDEO_OUTPUT)) {
            osdVideoOut = (sw >> 4) & 3u;
            ++videoOutApplied;
        }
    }
    unsigned videoOutApplied = 0;
    bool paused() { return lo(H264RING_W_PAUSE) & 1u; }
    std::vector<Record> all() const {
        std::vector<Record> v;
        for (const auto& s : sessions)
            v.insert(v.end(), s.begin(), s.end());
        return v;
    }
};

} // namespace fakeob
