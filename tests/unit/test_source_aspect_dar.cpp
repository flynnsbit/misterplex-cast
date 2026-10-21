// DAR-TRUNC (H264 Expert ticket daemon-dar-12bit-ticket-1004.md, Bug-accepted): the RTL keeps
// 12-bit PLXA terms ({b5[3:0],b4} / {b7[3:0],b6}, source_aspect_ack.sv:51-52), so no term above
// kSourceAspectMax (4095) may reach the wire. The demux paths call FFmpeg av_reduce with
// kSourceAspectMax; foldSourceAspect is the same bounded reduction (every expected value below
// was measured with libavutil 61.1.101 av_reduce(&n, &d, x, y, 4095)), and sourceAspectForSend
// is FpgaSpi::sendSourceAspect's guard.
#include "libmisterplex/source_aspect.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

using namespace misterplex;

namespace {

int fails = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL source aspect DAR: %s\n", msg.c_str());
        ++fails;
    }
}

std::string str(const SourceAspect& a) {
    return a.valid ? std::to_string(a.x) + ":" + std::to_string(a.y) : std::string("invalid");
}

void fold(int64_t x, int64_t y, const std::string& want, const char* note) {
    const auto got = foldSourceAspect(x, y);
    expect(str(got) == want, std::string(note) + " fold " + std::to_string(x) + ":" +
           std::to_string(y) + " -> " + str(got) + " want " + want);
}

// The RTL latch (source_aspect_ack.sv:51-52) applied to the packet bytes.
SourceAspect latched(const SourceAspect& sent) {
    const auto p = encodeSourceAspectPacket(sent, 1);
    const uint16_t x = static_cast<uint16_t>(((p[5] & 0x0F) << 8) | p[4]);
    const uint16_t y = static_cast<uint16_t>(((p[7] & 0x0F) << 8) | p[6]);
    return {x, y, true};
}

void send(SourceAspect in, const std::string& want, const std::string& wantNote, const char* note) {
    std::string clamp = "unset";
    const auto got = sourceAspectForSend(in, &clamp);
    expect(str(got) == want, std::string(note) + " send " + str(in) + " -> " + str(got) + " want " + want);
    expect(clamp == wantNote, std::string(note) + " note '" + clamp + "' want '" + wantNote + "'");
    if (got.valid) {
        expect(got.x <= kSourceAspectMax && got.y <= kSourceAspectMax && got.x && got.y,
               std::string(note) + " sent term out of 1..4095: " + str(got));
        // PLXJ ACK = the latched value; sendSourceAspect compares it with the sent value.
        const auto l = latched(got);
        expect(l.x == got.x && l.y == got.y, std::string(note) + " RTL latch " + str(l) +
               " differs from sent " + str(got));
        const uint64_t ack = mailbox_abi::kPlxjMagic | (uint64_t(l.x) << 32) |
                             (uint64_t(l.y) << 44) | (uint64_t(7) << 56);
        SourceAspectAck decoded;
        expect(decodeSourceAspectAckWord(ack, decoded) && decoded.aspect.x == got.x &&
               decoded.aspect.y == got.y && decoded.token == 7,
               std::string(note) + " PLXJ ACK of the sent value does not match it");
    }
}

}  // namespace

int main() {
    // Bounded reduction (av_reduce(..., 4095) results).
    fold(185, 100, "37:20", "gcd 1.85");
    fold(239, 100, "239:100", "2.39 unchanged");
    fold(16, 9, "16:9", "16:9");
    fold(4, 3, "4:3", "4:3");
    fold(4095, 1, "4095:1", "boundary 4095");
    fold(4096, 1, "4095:1", "boundary 4096 (today latches 0:1)");
    fold(4097, 3001, "2127:1558", "4097:3001 (today latches 1:3001)");
    fold(65535, 47999, "2403:1760", "65535:47999 (today latches 4095:2943)");
    fold(8191, 4096, "2:1", "8191:4096 tie keeps the convergent (today latches 4095:0)");
    fold(4095, 4095, "1:1", "gcd edge 4095:4095");
    fold(4096, 4096, "1:1", "gcd edge 4096:4096");
    fold(4096, 4095, "4095:4094", "4096 boundary");
    fold(4095, 4096, "4094:4095", "4096 boundary");
    fold(1998, 1080, "37:20", "DCI flat 1.85");
    fold(3996, 2160, "37:20", "4K flat 1.85");
    fold(7585, 4100, "37:20", "1.85 terms > 4095, exact after gcd");
    fold(2048, 858, "1024:429", "DCI scope 2.39");
    fold(8192, 3432, "1024:429", "8K scope 2.39, exact after gcd");
    fold(10278, 4300, "4061:1699", "odd 2.39");
    fold(7200, 5280, "15:11", "720x480 SAR 10:11");
    fold(int64_t(640) * 65521, int64_t(480) * 65519, "4:3", "odd VUI SAR 65521:65519");
    fold(int64_t(704) * 65535, int64_t(480) * 65533, "2181:1487", "odd VUI SAR 65535:65533");
    fold(65535, 16383, "4:1", "ratio 4.0 edge");
    fold(16383, 65535, "1:4", "ratio 0.25 edge");
    fold(1, 65535, "invalid", "rounds to 0:1");
    fold(0, 0, "invalid", "0:0");
    fold(0, 5, "invalid", "0:x");
    fold(5, 0, "invalid", "x:0");

    // The send guard: in-range terms are bit-identical to the pre-guard wire value.
    send({185, 100, true}, "185:100", "", "in range, unreduced, unchanged");
    send({640, 480, true}, "640:480", "", "coded W:H fallback unchanged");
    send({4095, 4095, true}, "4095:4095", "", "boundary unchanged");
    send({4095, 1, true}, "4095:1", "", "boundary unchanged");
    send({37, 20, true}, "37:20", "", "1.85");
    send({239, 100, true}, "239:100", "", "2.39");
    send({4096, 1, true}, "4095:1", "clamped 4096:1 -> 4095:1", "4096:1");
    send({4097, 3001, true}, "2127:1558", "clamped 4097:3001 -> 2127:1558", "4097:3001");
    send({65535, 47999, true}, "2403:1760", "clamped 65535:47999 -> 2403:1760", "65535:47999");
    send({8191, 4096, true}, "2:1", "clamped 8191:4096 -> 2:1", "8191:4096");
    send({7585, 4100, true}, "37:20", "clamped 7585:4100 -> 37:20", "1.85 > 4095");
    send({1, 65535, true}, "4:3", "clamped 1:65535 -> 4:3 fallback", "unusable fold");
    send({0, 0, true}, "invalid", "", "0:0 no send");
    send({0, 9, true}, "invalid", "", "0:x no send");
    send({9, 0, true}, "invalid", "", "x:0 no send");
    send({}, "invalid", "", "invalid no send");
    {   // Bug nit: both terms <= 4095 -> no reduce; the packet bytes equal today's (pre-guard) wire.
        for (const SourceAspect in : {SourceAspect{640, 480, true}, SourceAspect{185, 100, true},
                                      SourceAspect{4095, 4095, true}}) {
            const auto before = encodeSourceAspectPacket(in, 0x5A);
            const auto after = encodeSourceAspectPacket(sourceAspectForSend(in), 0x5A);
            expect(before == after, "in-range " + str(in) + " packet not byte-identical");
        }
    }
    {   // the bug being fixed: unguarded terms latch mod 4096
        const auto l = latched({4097, 3001, true});
        expect(l.x == 1 && l.y == 3001, "old wire 4097:3001 latches 1:3001");
        const auto m = latched({8191, 4096, true});
        expect(m.x == 4095 && m.y == 0, "old wire 8191:4096 latches 4095:0");
    }

    // Structural sweep (no libavutil here; bit-exact av_reduce equality is
    // test_source_aspect_dar_avutil): every in-range ratio folds to valid terms <= 4095. The worst
    // relative error is reported for information only (canonical criterion: equality with av_reduce).
    std::mt19937_64 rng(0xDA12);
    double worst = 0;
    unsigned exactChecked = 0;
    for (int i = 0; i < 2000000; ++i) {
        const int64_t y = int64_t(rng() % ((uint64_t(1) << 31) - 1)) + 1;
        const double ratio = 0.25 * std::pow(16.0, double(rng() % 1000000) / 999999.0);
        const int64_t x = std::llround(double(y) * ratio);
        if (x <= 0 || x >= (int64_t(1) << 31))
            continue;
        const double exact = double(x) / double(y);
        if (exact < kSourceAspectMinRatio || exact > kSourceAspectMaxRatio)
            continue;
        const auto f = foldSourceAspect(x, y);
        if (!f.valid || f.x > kSourceAspectMax || f.y > kSourceAspectMax) {
            expect(false, "sweep: " + std::to_string(x) + ":" + std::to_string(y) + " -> " + str(f));
            continue;
        }
        worst = std::max(worst, std::abs(double(f.x) / double(f.y) - exact) / exact);
    }
    for (int64_t x = 1; x <= kSourceAspectMax; x += 7)
        for (int64_t y = 1; y <= kSourceAspectMax; y += 11) {
            const auto r = reduceSourceAspect(x, y), f = foldSourceAspect(x, y);
            expect(r.valid && f.valid && r.x == f.x && r.y == f.y,
                   "in range " + std::to_string(x) + ":" + std::to_string(y) + " reduce " + str(r) +
                   " fold " + str(f));
            const auto s = sourceAspectForSend({uint16_t(x), uint16_t(y), true});
            expect(s.valid && s.x == x && s.y == y, "send not bit-identical for " +
                   std::to_string(x) + ":" + std::to_string(y));
            ++exactChecked;
        }
    std::printf("source aspect DAR: worst_rel_err_in_ratio_range=%.3g in_range_pairs=%u fails=%d\n",
                worst, exactChecked, fails);
    if (fails) return 1;
    std::printf("PASS source aspect DAR (fold == av_reduce 4095, send guard, PLXJ ack)\n");
    return 0;
}
