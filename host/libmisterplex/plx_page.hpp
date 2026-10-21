#pragma once
// PLX frame-store mailbox page per FPGA build.
//
// mailbox_abi_spec.hpp holds the offsets (kPlxsOff..kPlxjOff). The page is the
// build's row in fpga_build_ids.hpp (FpgaBuildInfo::plxPage): the present_core
// branch its qsf selects, checked in the tree the row cites.
//   PLEX_PRESENT_480P or PLEX_META_DPB  page 0x300FF000 (FS_DOORBELL,
//                                       DDR_FRAME_480P_FPGA_* mailboxes)
//   PLEX_PRESENT_480I                   page 0x301FF000 (FS_DOORBELL + offset)
//   PLEX_PRESENT_720P_L4                page 0x3047F000 (FS_DOORBELL + offset)
//   none of them                        page 0x3007F000: present_core overrides
//                                       only DOORBELL_PHYS (0x300FF000), so the
//                                       mailboxes keep the ddr_frame_store module
//                                       defaults. No recognised build uses it.
// Each page is also the doorbell of the daemon layout of that geometry
// (640x480, 720x480, 1280x720, 320x240). A build without a row has no page,
// so nothing reads its PLX words. kSdramDiagAddr and the 0x30140000 family are
// absolute and do not move with the page.

#include "ddr_frame_layout.hpp"
#include "mailbox_abi_spec.hpp"

#include <cstdint>

namespace misterplex {

enum class PlxPageClass : uint8_t {
    Present480pOrMetaDpb,
    Present480i,
    Present720pL4,
    NoPresentMacro,
};

inline constexpr uint32_t kPlxPageNoPresentMacro = 0x3007F000u;

inline constexpr uint32_t plxPageForClass(PlxPageClass c) {
    switch (c) {
    case PlxPageClass::Present480pOrMetaDpb:
        return kPlex480pYuv420pDoorbellPhys;
    case PlxPageClass::Present480i:
        return kPlex480iYuv420pDoorbellPhys;
    case PlxPageClass::Present720pL4:
        return kPlex720pYuv420pDoorbellPhys;
    case PlxPageClass::NoPresentMacro:
        return kPlxPageNoPresentMacro;
    }
    return 0u;
}

inline constexpr const char* plxPageClassName(PlxPageClass c) {
    switch (c) {
    case PlxPageClass::Present480pOrMetaDpb:
        return "PRESENT_480P|META_DPB";
    case PlxPageClass::Present480i:
        return "PRESENT_480I";
    case PlxPageClass::Present720pL4:
        return "PRESENT_720P_L4";
    case PlxPageClass::NoPresentMacro:
        return "no PRESENT/META_DPB macro";
    }
    return "?";
}

static_assert(plxPageForClass(PlxPageClass::Present480pOrMetaDpb) == 0x300FF000u);
static_assert(plxPageForClass(PlxPageClass::Present480i) == 0x301FF000u);
static_assert(plxPageForClass(PlxPageClass::Present720pL4) == 0x3047F000u);

// A mailbox word on a page: the low (magic) word, or the high word (+4).
inline constexpr uint32_t plxWordAddr(uint32_t page, uint32_t offset, bool high = false) {
    return page + offset + (high ? 4u : 0u);
}

} // namespace misterplex
