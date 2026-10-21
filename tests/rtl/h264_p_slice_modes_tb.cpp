#include "Vh264_p_slice_modes_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <iostream>

struct Case {
    const char* name;
    bool skipped;
    uint8_t mbType;
    bool subValid;
    uint8_t subType;
    bool isSkip;
    bool inter;
    bool intra;
    bool usesSub;
    bool ref0;
    bool unsupported;
    uint8_t mode;
    uint8_t mbParts;
    uint8_t mbW;
    uint8_t mbH;
    uint8_t subParts;
    uint8_t subW;
    uint8_t subH;
};

static bool runCase(Vh264_p_slice_modes_tb_top& dut, const Case& c) {
    dut.skipped = c.skipped;
    dut.mb_type = c.mbType;
    dut.sub_mb_valid = c.subValid;
    dut.sub_mb_type = c.subType;
    dut.eval();
    bool ok = true;
    auto ck = [&](bool cond, const char* field, int got, int want) {
        if (!cond) {
            std::cerr << "FAIL p-slice mode " << c.name << " " << field
                      << " got=" << got << " want=" << want << "\n";
            ok = false;
        }
    };
    ck(dut.is_p_skip == c.isSkip, "is_p_skip", dut.is_p_skip, c.isSkip);
    ck(dut.is_inter == c.inter, "is_inter", dut.is_inter, c.inter);
    ck(dut.is_intra == c.intra, "is_intra", dut.is_intra, c.intra);
    ck(dut.uses_sub_mb == c.usesSub, "uses_sub_mb", dut.uses_sub_mb, c.usesSub);
    ck(dut.ref0_only == c.ref0, "ref0_only", dut.ref0_only, c.ref0);
    ck(dut.unsupported == c.unsupported, "unsupported", dut.unsupported, c.unsupported);
    ck(dut.part_mode == c.mode, "part_mode", dut.part_mode, c.mode);
    ck(dut.mb_part_count == c.mbParts, "mb_part_count", dut.mb_part_count, c.mbParts);
    ck(dut.mb_part_w == c.mbW, "mb_part_w", dut.mb_part_w, c.mbW);
    ck(dut.mb_part_h == c.mbH, "mb_part_h", dut.mb_part_h, c.mbH);
    ck(dut.sub_part_count == c.subParts, "sub_part_count", dut.sub_part_count, c.subParts);
    ck(dut.sub_part_w == c.subW, "sub_part_w", dut.sub_part_w, c.subW);
    ck(dut.sub_part_h == c.subH, "sub_part_h", dut.sub_part_h, c.subH);
    return ok;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vh264_p_slice_modes_tb_top dut;
    const Case cases[] = {
        {"P_Skip", true, 0, false, 0, true, true, false, false, false, false, 0, 1, 16, 16, 0, 0, 0},
        {"P_L0_16x16", false, 0, false, 0, false, true, false, false, false, false, 0, 1, 16, 16, 0, 0, 0},
        {"P_L0_16x8", false, 1, false, 0, false, true, false, false, false, false, 1, 2, 16, 8, 0, 0, 0},
        {"P_L0_8x16", false, 2, false, 0, false, true, false, false, false, false, 2, 2, 8, 16, 0, 0, 0},
        {"P_8x8", false, 3, true, 0, false, true, false, true, false, false, 4, 4, 8, 8, 1, 8, 8},
        {"P_8x8ref0", false, 4, true, 1, false, true, false, true, true, false, 4, 4, 8, 8, 2, 8, 4},
        {"P_SUB_4x8", false, 3, true, 2, false, true, false, true, false, false, 4, 4, 8, 8, 2, 4, 8},
        {"P_SUB_4x4", false, 3, true, 3, false, true, false, true, false, false, 4, 4, 8, 8, 4, 4, 4},
        {"P_INTRA", false, 5, false, 0, false, false, true, false, false, false, 7, 0, 0, 0, 0, 0, 0},
        {"P_UNSUPPORTED", false, 31, false, 0, false, false, false, false, false, true, 7, 0, 0, 0, 0, 0, 0},
    };
    int failures = 0;
    int skip = 0, p16 = 0, p16x8 = 0, p8x16 = 0, p8x8 = 0, sub = 0, intra = 0;
    for (const auto& c : cases) {
        if (!runCase(dut, c))
            ++failures;
        skip += c.isSkip;
        p16 += (!c.skipped && c.mbType == 0);
        p16x8 += (!c.skipped && c.mbType == 1);
        p8x16 += (!c.skipped && c.mbType == 2);
        p8x8 += (!c.skipped && (c.mbType == 3 || c.mbType == 4));
        sub += c.usesSub && c.subValid;
        intra += c.intra;
    }
    if (failures) {
        std::cerr << "h264 P-slice mode RTL check FAILED: failures=" << failures << "\n";
        return 1;
    }
    std::cout << "h264 P-slice mode RTL check PASS: cases=" << (sizeof(cases) / sizeof(cases[0]))
              << " skip=" << skip
              << " p16x16=" << p16
              << " p16x8=" << p16x8
              << " p8x16=" << p8x16
              << " p8x8_mb=" << p8x8
              << " subpartitions=" << sub
              << " intra_map=" << intra
              << " unsupported=1\n";
    return 0;
}
