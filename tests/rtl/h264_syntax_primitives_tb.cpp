#include "Vh264_syntax_primitives_tb_top.h"
#include "verilated.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

static void tick(Vh264_syntax_primitives_tb_top& dut) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
}

static void reset(Vh264_syntax_primitives_tb_top& dut) {
    dut.reset = 1;
    dut.clear = 0;
    dut.in_valid = 0;
    dut.in_byte = 0;
    dut.in_last = 0;
    dut.out_ready = 1;
    dut.eg_start = 0;
    dut.eg_signed_mode = 0;
    dut.eg_bit_valid = 0;
    dut.eg_bit_value = 0;
    tick(dut);
    tick(dut);
    dut.reset = 0;
    tick(dut);
}

static std::vector<uint8_t> filterBytes(Vh264_syntax_primitives_tb_top& dut,
                                        const std::vector<uint8_t>& in) {
    dut.clear = 1;
    tick(dut);
    dut.clear = 0;
    std::vector<uint8_t> out;
    size_t sent = 0;
    for (int guard = 0; guard < 10000 && (!dut.filter_done || dut.out_valid || sent < in.size()); ++guard) {
        dut.out_ready = 1;
        dut.in_valid = (sent < in.size());
        if (sent < in.size()) {
            dut.in_byte = in[sent];
            dut.in_last = (sent + 1 == in.size());
        } else {
            dut.in_byte = 0;
            dut.in_last = 0;
        }
        const bool accepted = dut.in_valid && dut.in_ready;
        tick(dut);
        if (dut.out_valid)
            out.push_back(static_cast<uint8_t>(dut.out_byte));
        if (accepted)
            ++sent;
    }
    dut.in_valid = 0;
    dut.in_last = 0;
    tick(dut);
    return out;
}

static bool expectVec(const char* name, const std::vector<uint8_t>& got,
                      const std::vector<uint8_t>& want) {
    if (got == want)
        return true;
    std::cerr << name << " got";
    for (auto b : got)
        std::cerr << " " << std::hex << int(b);
    std::cerr << " want";
    for (auto b : want)
        std::cerr << " " << std::hex << int(b);
    std::cerr << std::dec << "\n";
    return false;
}

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

static std::vector<uint8_t> firstNalPayloadWithEpb(const std::vector<uint8_t>& annexb) {
    for (size_t i = 0; i + 4 < annexb.size(); ++i) {
        size_t sc = 0;
        if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 0 && annexb[i + 3] == 1)
            sc = 4;
        else if (annexb[i] == 0 && annexb[i + 1] == 0 && annexb[i + 2] == 1)
            sc = 3;
        if (!sc)
            continue;
        size_t j = i + sc;
        while (j + 3 < annexb.size()) {
            if (annexb[j] == 0 && annexb[j + 1] == 0 &&
                (annexb[j + 2] == 1 || (j + 3 < annexb.size() && annexb[j + 2] == 0 && annexb[j + 3] == 1)))
                break;
            ++j;
        }
        if (j + 3 >= annexb.size())
            j = annexb.size();
        if (i + sc + 1 >= j)
            continue;
        std::vector<uint8_t> pay(annexb.begin() + static_cast<long>(i + sc + 1), annexb.begin() + static_cast<long>(j));
        for (size_t k = 2; k < pay.size(); ++k)
            if (pay[k - 2] == 0 && pay[k - 1] == 0 && pay[k] == 3)
                return pay;
        i = j;
    }
    return {};
}

static std::string bitsForUe(uint32_t v) {
    uint32_t code = v + 1;
    int n = 0;
    for (uint32_t t = code; t > 1; t >>= 1)
        ++n;
    std::string s(static_cast<size_t>(n), '0');
    for (int i = n; i >= 0; --i)
        s.push_back(((code >> i) & 1) ? '1' : '0');
    return s;
}

static uint32_t ueForSe(int32_t v) {
    return (v <= 0) ? static_cast<uint32_t>(-2 * v) : static_cast<uint32_t>(2 * v - 1);
}

struct EgResult { bool ok; uint32_t ue; int32_t se; int bits; };

static EgResult readEg(Vh264_syntax_primitives_tb_top& dut, const std::string& bits, bool signedMode) {
    dut.eg_signed_mode = signedMode;
    dut.eg_start = 1;
    tick(dut);
    dut.eg_start = 0;
    size_t pos = 0;
    for (int guard = 0; guard < 1000 && !dut.eg_done; ++guard) {
        dut.eg_bit_valid = (pos < bits.size()) && dut.eg_bit_ready;
        dut.eg_bit_value = dut.eg_bit_valid && bits[pos] == '1';
        if (dut.eg_bit_valid)
            ++pos;
        tick(dut);
    }
    dut.eg_bit_valid = 0;
    tick(dut);
    return {bool(dut.eg_ok), dut.eg_ue_value, static_cast<int32_t>(dut.eg_se_value), int(dut.eg_bits_consumed)};
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vh264_syntax_primitives_tb_top dut;
    reset(dut);
    int failures = 0;

    struct Case { const char* name; std::vector<uint8_t> in; std::vector<uint8_t> want; int epb; };
    const Case cases[] = {
        {"epb_00", {0,0,3,0}, {0,0,0}, 1},
        {"epb_01", {0x12,0,0,3,1,0x34}, {0x12,0,0,1,0x34}, 1},
        {"epb_02", {0,0,3,2}, {0,0,2}, 1},
        {"epb_03_data", {0,0,3,3}, {0,0,3}, 1},
        {"no_epb_startcode_like", {0,0,1,0xaa}, {0,0,1,0xaa}, 0},
        {"trailing_zeros", {0,0,0}, {0,0,0}, 0},
    };
    for (const auto& c : cases) {
        auto got = filterBytes(dut, c.in);
        if (!expectVec(c.name, got, c.want))
            ++failures;
        if (dut.epb_removed != c.epb) {
            std::cerr << c.name << " epb_removed got " << dut.epb_removed << " want " << c.epb << "\n";
            ++failures;
        }
        if (dut.rbsp_len != c.want.size()) {
            std::cerr << c.name << " rbsp_len got " << dut.rbsp_len << " want " << c.want.size() << "\n";
            ++failures;
        }
    }

    auto annexb = readFile("tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264");
    auto epbNal = firstNalPayloadWithEpb(annexb);
    if (epbNal.empty()) {
        std::cerr << "real vector has no NAL with EPB to exercise\n";
        ++failures;
    } else {
        auto got = filterBytes(dut, epbNal);
        for (size_t i = 2; i < got.size(); ++i) {
            if (got[i - 2] == 0 && got[i - 1] == 0 && got[i] == 3) {
                std::cerr << "real NAL still contains EPB at RBSP offset " << i << "\n";
                ++failures;
                break;
            }
        }
        if (dut.epb_removed == 0) {
            std::cerr << "real NAL EPB removal count stayed zero\n";
            ++failures;
        }
    }

    const uint32_t ueVals[] = {0, 1, 2, 3, 4, 5, 13, 31, 255};
    for (uint32_t v : ueVals) {
        auto bits = bitsForUe(v);
        auto r = readEg(dut, bits, false);
        if (!r.ok || r.ue != v || r.bits != static_cast<int>(bits.size())) {
            std::cerr << "ue " << v << " failed got ok=" << r.ok << " ue=" << r.ue << " bits=" << r.bits << "\n";
            ++failures;
        }
    }
    const int32_t seVals[] = {0, 1, -1, 2, -2, 3, -3};
    for (int32_t v : seVals) {
        auto bits = bitsForUe(ueForSe(v));
        auto r = readEg(dut, bits, true);
        if (!r.ok || r.se != v) {
            std::cerr << "se " << v << " failed got ok=" << r.ok << " se=" << r.se << " ue=" << r.ue << "\n";
            ++failures;
        }
    }
    auto malformed = readEg(dut, std::string(26, '0') + "1", false);
    if (malformed.ok) {
        std::cerr << "malformed too-long ue unexpectedly ok\n";
        ++failures;
    }

    if (failures) {
        std::cerr << "h264 syntax primitives RTL check FAILED: " << failures << " failures\n";
        return 1;
    }
    std::cout << "h264 syntax primitives RTL check PASS: EPB synthetic=6 real_epb_removed=" << dut.epb_removed
              << " ue=9 se=7 malformed_long=rejected offsets=RBSP-after-EPB\n";
    return 0;
}
