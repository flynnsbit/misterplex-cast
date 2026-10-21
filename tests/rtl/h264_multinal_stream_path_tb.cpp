#include "Vh264_multinal_stream_path_tb.h"
#include "verilated.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<uint8_t> readBinary(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error(std::string("cannot open fixture: ") + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc != 5) {
        std::cerr << "usage: " << argv[0] << " multi-nal.264 min_nals min_slices expect_csum_hex\n";
        std::cerr << "Refuse implicit defaults: each fixture must publish its proven counts/checksum.\n";
        return 2;
    }

    try {
        const auto bytes = readBinary(argv[1]);
        const int minNals = std::stoi(argv[2]);
        const int minSlices = std::stoi(argv[3]);
        const int expectCsum = std::stoi(argv[4], nullptr, 0);
        if (bytes.size() < 2)
            throw std::runtime_error("fixture too small");

        Vh264_multinal_stream_path_tb dut;
        dut.clk = 0;
        dut.reset = 1;
        dut.ioctl_download = 0;
        dut.ioctl_wr = 0;
        dut.ioctl_dout = 0;
        dut.enable = 1;
        dut.flush = 0;

        uint64_t cycle = 0;
        int placePulses = 0;
        int sawExpectedCsum = 0;
        int reconSig3bCycles = 0;
        int sawI = 0;
        int sawP = 0;
        int idleBetweenVcl = 0;
        int prevSliceValid = 0;
        int pFirstMbSeen = 0;
        int pFirstMbMode0 = 0;
        int pFirstMbMode1 = 0;
        int pFirstMbMode2 = 0;
        int pFirstMbBad = 0;

        auto tick = [&]() {
            dut.clk = 0;
            dut.eval();
            dut.clk = 1;
            dut.eval();
            ++cycle;
            if (dut.residual_place_pulse)
                ++placePulses;
            if (static_cast<uint8_t>(dut.residual_csum) == expectCsum)
                sawExpectedCsum = 1;
            if (dut.recon_valid && static_cast<uint8_t>(dut.recon_sig) == 0x3b)
                ++reconSig3bCycles;
            if (dut.slice_valid && !prevSliceValid) {
                const uint8_t st = static_cast<uint8_t>(dut.slice_type) % 5;
                if (st == 2 && dut.slice_is_i)
                    sawI = 1;
                if (st == 0 && !dut.slice_is_i)
                    sawP = 1;
                if (st == 0 && !dut.slice_is_i && dut.has_mb_type) {
                    ++pFirstMbSeen;
                    const int mt = static_cast<int>(dut.first_mb_type);
                    const int pm = static_cast<int>(dut.first_mb_part_mode);
                    const int pc = static_cast<int>(dut.first_mb_part_count);
                    if (dut.p_skip_run != 0 || dut.first_mb_p_skip || dut.first_mb_uses_sub_mb || dut.first_mb_intra) {
                        ++pFirstMbBad;
                    } else if (mt == 0 && pm == 0 && pc == 1) {
                        ++pFirstMbMode0;
                    } else if (mt == 1 && pm == 1 && pc == 2) {
                        ++pFirstMbMode1;
                    } else if (mt == 2 && pm == 2 && pc == 2) {
                        ++pFirstMbMode2;
                    } else {
                        ++pFirstMbBad;
                    }
                }
            }
            prevSliceValid = dut.slice_valid ? 1 : 0;
            if (sawI && !sawP && placePulses >= 1 &&
                static_cast<uint8_t>(dut.slice_parser_state) == 0)
                idleBetweenVcl = 1;
        };

        for (int i = 0; i < 8; ++i)
            tick();
        dut.reset = 0;
        for (int i = 0; i < 4; ++i)
            tick();

        dut.ioctl_download = 1;
        for (uint8_t b : bytes) {
            dut.ioctl_dout = b;
            dut.ioctl_wr = 1;
            tick();
            dut.ioctl_wr = 0;
            tick();
        }
        dut.ioctl_download = 0;
        dut.ioctl_wr = 0;

        const uint64_t deadline = cycle + 2000000;
        while (cycle < deadline) {
            tick();
            const bool countsDone = dut.nalu_count >= minNals && dut.sps_count >= 1 && dut.pps_count >= 1 &&
                                    dut.idr_count >= 1 && dut.slice_count >= minSlices;
            const bool parsedIAndP = sawI && sawP;
            const bool decodedIdr = placePulses >= 1 && sawExpectedCsum;
            const bool paintedSeveral = dut.stub_frames >= 2;
            if (countsDone && parsedIAndP && decodedIdr && idleBetweenVcl && paintedSeveral)
                break;
        }

        bool ok = true;
        auto expect = [&](bool cond, const char* msg) {
            if (!cond) {
                std::cerr << "FAIL multi-NAL stream_path: " << msg << "\n";
                ok = false;
            }
        };
        expect(dut.bytes_in == bytes.size(), "ioctl bytes_in did not match fixture size");
        expect(dut.nalu_count >= minNals, "expected minimum NALs through nalu_scanner");
        expect(dut.sps_count >= 1, "expected SPS count");
        expect(dut.pps_count >= 1, "expected PPS count");
        expect(dut.idr_count >= 1, "expected IDR count");
        expect(dut.slice_count >= minSlices, "expected P-slice count after IDR");
        expect(dut.sps_valid && dut.pps_valid, "SPS/PPS did not validate");
        expect(dut.sps_width == 320 && dut.sps_height == 240, "unexpected SPS geometry");
        expect(placePulses >= 1, "IDR residual ST_PLACE pulse missing");
        expect(sawExpectedCsum, "expected residual_csum was never observed");
        expect(sawI, "I-slice parse not observed");
        expect(idleBetweenVcl, "slice_hdr_parser ST_IDLE was not observed between IDR and P VCLs");
        expect(sawP, "P-slice parse not observed after IDR; parser did not prove idle/re-entry");
        if (minSlices >= 11) {
            expect(pFirstMbSeen == 11, "expected first P macroblock syntax for all 11 P slices");
            expect(pFirstMbMode0 == 8, "expected eight first-MB P_L0_16x16 slices");
            expect(pFirstMbMode1 == 2, "expected two first-MB P_L0_16x8 slices");
            expect(pFirstMbMode2 == 1, "expected one first-MB P_L0_8x16 slice");
            expect(pFirstMbBad == 0, "unexpected first P macroblock syntax/classification");
            expect(reconSig3bCycles > 0, "parsed P DPB/MC recon signature missing");
        }
        expect(dut.stub_frames >= 2, "decode_stub did not consume multiple VCL pulses");

        std::cout << "multi-NAL stream_path raw: bytes=" << bytes.size()
                  << " bytes_in=" << dut.bytes_in
                  << " bytes_seen=" << dut.bytes_seen
                  << " nalu=" << dut.nalu_count
                  << " sps=" << static_cast<int>(dut.sps_count)
                  << " pps=" << static_cast<int>(dut.pps_count)
                  << " idr=" << static_cast<int>(dut.idr_count)
                  << " slice=" << static_cast<int>(dut.slice_count)
                  << " place_pulses=" << placePulses
                  << " saw_expected_csum=" << sawExpectedCsum
                  << " recon_sig_3b_cycles=" << reconSig3bCycles
                  << " frames=" << dut.stub_frames
                  << " saw_i=" << sawI
                  << " idle_between_vcl=" << idleBetweenVcl
                  << " saw_p=" << sawP
                  << " p_first_mb_seen=" << pFirstMbSeen
                  << " p_first_modes=" << pFirstMbMode0 << "/" << pFirstMbMode1 << "/" << pFirstMbMode2
                  << " p_first_bad=" << pFirstMbBad
                  << " slice_parser_state=" << static_cast<int>(dut.slice_parser_state)
                  << " final_slice_type=" << static_cast<int>(dut.slice_type)
                  << " final_p_skip_run=" << static_cast<int>(dut.p_skip_run)
                  << " final_first_mb_type=" << static_cast<int>(dut.first_mb_type)
                  << " final_first_part_mode=" << static_cast<int>(dut.first_mb_part_mode)
                  << " expect_csum=0x" << std::hex << expectCsum
                  << " residual_csum=0x" << static_cast<int>(static_cast<uint8_t>(dut.residual_csum))
                  << std::dec << " cycles=" << cycle << "\n";
        if (!ok)
            return 1;
        if (reconSig3bCycles == 0) {
            std::cout << "test_h264_multinal_stream_path: OK fixture exposes current RED "
                         "class (IDR->P re-entry works, residual csum observed, recon 0x3b absent)\n";
        } else {
            std::cout << "test_h264_multinal_stream_path: OK fixture exercises GREEN class "
                         "(IDR->P re-entry plus recon 0x3b observed)\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL multi-NAL stream_path: " << e.what() << "\n";
        return 1;
    }
}
