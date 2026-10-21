#include "Vh264_sps_geometry_tb_top.h"
#include "libmisterplex/h264_sps.hpp"
#include "verilated.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

static void tick(Vh264_sps_geometry_tb_top& dut) { dut.clk = 0; dut.eval(); dut.clk = 1; dut.eval(); }
static void reset(Vh264_sps_geometry_tb_top& dut) {
    dut.reset = 1; dut.clear = 0; dut.in_valid = 0; dut.in_byte = 0; dut.in_last = 0;
    tick(dut); tick(dut); dut.reset = 0; tick(dut);
}

struct Bits {
    std::vector<int> b;
    void bit(int v) { b.push_back(v ? 1 : 0); }
    void u(uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bit((v >> i) & 1u); }
    void ue(uint32_t v) {
        uint32_t code = v + 1;
        int leading = 0;
        for (uint32_t t = code; t > 1; t >>= 1) ++leading;
        for (int i = 0; i < leading; ++i) bit(0);
        for (int i = leading; i >= 0; --i) bit((code >> i) & 1u);
    }
    std::vector<uint8_t> bytes() {
        bit(1); // rbsp_stop_one_bit
        while (b.size() % 8) bit(0);
        std::vector<uint8_t> out(b.size() / 8, 0);
        for (size_t i = 0; i < b.size(); ++i) out[i / 8] |= uint8_t(b[i] << (7 - (i & 7)));
        return out;
    }
};

static std::vector<uint8_t> makeSps(int widthMbs, int heightMapUnits, bool crop, int cr) {
    Bits w;
    w.u(66, 8); w.u(0, 8); w.u(30, 8); // profile, constraints, level
    w.ue(0); // sps id
    w.ue(3); // log2_max_frame_num_minus4 -> 7 bits
    w.ue(2); // poc type 2
    w.ue(1); // max_num_ref_frames
    w.bit(0); // gaps flag
    w.ue(uint32_t(widthMbs - 1));
    w.ue(uint32_t(heightMapUnits - 1));
    w.bit(1); // frame_mbs_only
    w.bit(1); // direct_8x8_inference
    w.bit(crop ? 1 : 0);
    if (crop) { w.ue(0); w.ue(uint32_t(cr)); w.ue(0); w.ue(0); }
    return w.bytes();
}

static bool feed(Vh264_sps_geometry_tb_top& dut, const std::vector<uint8_t>& rbsp) {
    dut.clear = 1; tick(dut); dut.clear = 0;
    size_t sent = 0;
    for (int guard = 0; guard < 20000 && !(dut.valid || dut.error); ++guard) {
        dut.in_valid = (sent < rbsp.size()) && !dut.busy;
        if (dut.in_valid) {
            dut.in_byte = rbsp[sent];
            dut.in_last = (sent + 1 == rbsp.size());
        }
        bool accepted = dut.in_valid && dut.in_ready;
        tick(dut);
        if (accepted) ++sent;
    }
    dut.in_valid = 0; tick(dut);
    return dut.valid && !dut.error;
}

static int failures = 0;
static void expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << msg << "\n"; ++failures; }
}

static void checkCase(Vh264_sps_geometry_tb_top& dut, const char* name, const std::vector<uint8_t>& rbsp,
                      int codedW, int codedH, int displayW, int displayH, int cropRight) {
    auto host = misterplex::parseSpsRbsp(rbsp.data(), rbsp.size());
    bool ok = feed(dut, rbsp);
    expect(ok, std::string(name) + " parser did not become valid");
    expect(host.valid, std::string(name) + " host SPS invalid");
    expect(dut.profile_idc == 66 && dut.level_idc == 30, std::string(name) + " profile/level mismatch");
    expect(dut.log2_max_frame_num == 7, std::string(name) + " log2_max_frame_num mismatch");
    expect(dut.poc_type == 2, std::string(name) + " poc_type mismatch");
    expect(dut.coded_width == codedW && dut.coded_height == codedH, std::string(name) + " coded geometry mismatch");
    expect(dut.display_width == displayW && dut.display_height == displayH, std::string(name) + " display geometry mismatch");
    expect(host.width == displayW && host.height == displayH, std::string(name) + " host display geometry mismatch");
    expect(dut.crop_right == cropRight, std::string(name) + " crop_right mismatch");
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vh264_sps_geometry_tb_top dut;
    reset(dut);
    checkCase(dut, "baseline_320x240", makeSps(20, 15, false, 0), 320, 240, 320, 240, 0);
    checkCase(dut, "cropped_624_to_618", makeSps(39, 30, true, 3), 624, 480, 618, 480, 3);
    if (failures) {
        std::cerr << "h264 SPS geometry RTL check FAILED: " << failures << " failures\n";
        return 1;
    }
    std::cout << "h264 SPS geometry RTL check PASS: baseline_320x240 and cropped coded=624x480 display=618x480 crop_right=3 log2_frame_num=7 poc_type=2\n";
    return 0;
}
