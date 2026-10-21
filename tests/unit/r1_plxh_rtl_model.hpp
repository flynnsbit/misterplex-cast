#pragma once
// Test-only model of the R1 PLX slots as the RTL sees them (r1-idlefb-mvpb ac6c3c3b:
// ddr_frame_store.sv:1996-2013 accept, plex_fb480.sv:325-346 FB_VBL registration and PLXO).
// - The daemon's stores land in a 64-bit PLXH slot (lo = magic word at PLXH+0, hi = upper word
//   at PLXH+4).
// - Accept (the free-running doorbell poll, no FB_VBL needed): the poll samples the slot as ONE
//   beat. The first good-magic poll after a reset records the token as the snap and accepts
//   nothing; later polls accept a token != the last accepted one (or the snap), with {buf_sel,
//   en} from that same beat. Every vbl() runs one poll first; poll() runs one on its own.
// - At each FB_VBL (vbl()) the accepted request registers, and PLXO is written only when
//   {idle_sel, idle_sel ? idle_buf : 0, tok_r} changes (change-only; K2's zero does not count).
// - frozen = no FB_VBL (HDMI down, a mode change): polls still accept, nothing registers.
//   vblAfterStore32 injects a VBL right after the N-th 32-bit store (between the two beats of
//   one request). coreReset() is a core reset: the accept and FB_VBL state clear, DDR stays;
//   coreChanges() counts them. settle() (the prime's >= 1 ms) runs one poll, and an FB_VBL too
//   with vblInSettle.
#include "libmisterplex/r1_plxh.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace r1test {

using misterplex::r1::kPlxhMagic;
using misterplex::r1::kPlxhPhys;
using misterplex::r1::kPlxoMagic;
using misterplex::r1::kPlxoPhys;

struct Op {
    char kind;   // 'w' store32, 'W' store64, 'b' barrier
    uint32_t phys;
    uint64_t value;
};

struct Latch {
    uint16_t token;
    bool en;
    int sel;
};

class RtlPlxModel final : public misterplex::r1::PlxPort {
public:
    bool store32(uint32_t phys, uint32_t value) override {
        ops.push_back({'w', phys, value});
        if (phys == kPlxhPhys)
            lo = value;
        else if (phys == kPlxhPhys + 4) {
            hi = value;
            requested.insert(static_cast<uint16_t>(value >> 16));   // a token some request carried
        } else
            return false;
        if (++stores32 == vblAfterStore32)
            vbl();
        return true;
    }
    bool store64(uint32_t phys, uint64_t value) override {
        ops.push_back({'W', phys, value});
        if (phys != kPlxoPhys) {
            ++store64Violations;   // a 64-bit store anywhere but the K2 PLXO zero (review 20:00 N2)
            return false;
        }
        plxo = value;
        return true;
    }
    bool load64(uint32_t phys, uint64_t* value) override {
        if (loadFails)
            return false;
        if (phys == kPlxhPhys) {
            if (plxhLoadFails)
                return false;   // a port that reads PLXO only
            *value = (uint64_t(hi) << 32) | lo;
            return true;
        }
        if (phys != kPlxoPhys)
            return false;
        *value = plxo;
        return true;
    }
    void barrier() override { ops.push_back({'b', 0, 0}); }
    uint64_t coreChanges() override { return resets; }
    // The prime's settle (>= 1 ms): the free-running poll samples the slot; vblInSettle puts an
    // FB_VBL inside it too (about 1 in 17 at 60 Hz), the case where T itself registers.
    void settle() override {
        ++settles;
        poll();
        if (vblInSettle)
            vbl();
    }

    // The doorbell poll: one sample of the slot.
    void poll() {
        if (lo != kPlxhMagic)
            return;
        const uint16_t token = static_cast<uint16_t>(hi >> 16);
        if (!armed) {
            snap = token;   // a stale (or the first) request: recorded, never accepted
            armed = true;
            return;
        }
        if (token == (haveAcc ? acc : snap))
            return;
        acc = token;
        haveAcc = true;
        pend = {token, (hi & 1u) != 0, static_cast<int>((hi >> 1) & 1u)};
        if (!requested.count(token))
            ++mixedLatches;   // a token no upper-word store carried (a torn or stale beat)
    }

    void vbl() {
        ++vbls;
        poll();
        if (frozen || !haveAcc)
            return;   // no FB_VBL, or nothing accepted since the reset
        const Latch key{pend.token, pend.en, pend.en ? pend.sel : 0};
        if (latched && key.token == cur.token && key.en == cur.en && key.sel == cur.sel)
            return;   // the key did not change: PLXO is not rewritten
        latched = true;
        cur = key;
        latches.push_back(cur);
        plxo = (uint64_t(key.token) << 48) | (uint64_t(key.sel) << 33) |
               (uint64_t(key.en ? 1 : 0) << 32) | kPlxoMagic;
    }

    void coreReset() {
        ++resets;
        armed = haveAcc = latched = false;
        snap = acc = 0;
        pend = cur = Latch{0, false, 0};
    }

    // A slot left by an earlier daemon whose last token was `token`, already snapped by the RTL
    // (a core load or reset since): the state every Rig starts from.
    void staleSlot(uint16_t token) {
        lo = kPlxhMagic;
        hi = uint32_t(token) << 16;
        armed = true;
        snap = token;
    }

    std::vector<Op> ops;
    std::vector<Latch> latches;
    std::set<uint16_t> requested;
    uint32_t lo = 0, hi = 0;
    uint64_t plxo = 0xDEADBEEFCAFEF00Dull;   // DDR garbage until K2 zeroes it
    bool frozen = false;
    bool loadFails = false;
    bool plxhLoadFails = false;
    bool vblInSettle = false;
    int settles = 0;
    uint64_t resets = 0;
    bool armed = false, haveAcc = false;
    uint16_t snap = 0, acc = 0;
    Latch pend{0, false, 0};
    bool latched = false;
    Latch cur{0, false, 0};
    int vbls = 0;
    int stores32 = 0;
    int vblAfterStore32 = -1;
    int mixedLatches = 0;
    int store64Violations = 0;
};

}  // namespace r1test
