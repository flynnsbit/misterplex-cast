#include "Vstream_path_recon_tb.h"
#include "verilated.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::string("open failed: ") + path);
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) {
        throw std::runtime_error(std::string("empty fixture: ") + path);
    }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in) {
        throw std::runtime_error(std::string("short read: ") + path);
    }
    return data;
}

void tick(Vstream_path_recon_tb& dut, uint64_t& cyc) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    dut.clk = 0;
    dut.eval();
    ++cyc;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " tests/fixtures/.../file.264\n";
        return 2;
    }
    try {
        const auto data = readFile(argv[1]);
        Verilated::commandArgs(argc, argv);
        Vstream_path_recon_tb dut;
        uint64_t cyc = 0;

        dut.reset = 1;
        dut.ioctl_download = 0;
        dut.ioctl_wr = 0;
        dut.ioctl_dout = 0;
        dut.enable = 0;
        dut.flush = 0;
        bool saw_place = false;
        bool saw_recon_dbg = false;
        bool saw_frame = false;
        uint64_t place_cyc = 0;
        uint64_t recon_cyc = 0;
        auto observe = [&]() {
            if (dut.residual_place_pulse) {
                saw_place = true;
                place_cyc = cyc;
            }
            if (dut.recon_dbg_valid) {
                saw_recon_dbg = true;
                recon_cyc = cyc;
            }
            if (dut.stub_frames != 0) {
                saw_frame = true;
            }
        };
        for (int i = 0; i < 8; ++i) tick(dut, cyc);

        dut.reset = 0;
        dut.enable = 1;
        for (int i = 0; i < 4; ++i) {
            tick(dut, cyc);
            observe();
        }

        auto feedFixture = [&]() {
            dut.ioctl_download = 1;
            for (uint8_t b : data) {
                dut.ioctl_dout = b;
                dut.ioctl_wr = 1;
                tick(dut, cyc);
                observe();
            }
            dut.ioctl_wr = 0;
            for (int i = 0; i < 4; ++i) {
                tick(dut, cyc);
                observe();
            }
            dut.ioctl_download = 0;
        };

        auto waitForFrame = [&](int pass, uint16_t previous_frames) -> bool {
            for (int i = 0; i < 300000; ++i) {
                tick(dut, cyc);
                observe();
                if (dut.stub_frames != previous_frames) {
                    saw_frame = true;
                    return true;
                }
            }
            std::cerr << "FAIL stream_path integrated sim: pass " << pass
                      << " did not produce a new frame previous_frames=" << previous_frames
                      << " current_frames=" << dut.stub_frames
                      << " nalu=" << dut.nalu_count << " bytes=" << dut.bytes_in << "\n";
            return false;
        };

        uint16_t previous_frames = dut.stub_frames;
        uint64_t last_place_cyc = 0;
        uint64_t last_recon_cyc = 0;
        for (int pass = 1; pass <= 2; ++pass) {
            saw_place = false;
            saw_recon_dbg = false;
            saw_frame = false;
            feedFixture();
            if (!waitForFrame(pass, previous_frames)) return 1;
            previous_frames = dut.stub_frames;

            const int got_sig = static_cast<uint8_t>(dut.recon_sig);
            const int got_dbg = static_cast<uint8_t>(dut.recon_dbg);
            if (!saw_place) {
                std::cerr << "FAIL stream_path integrated sim: pass " << pass
                          << " residual_place_pulse never asserted"
                          << " nalu=" << dut.nalu_count << " bytes=" << dut.bytes_in
                          << " csum=0x" << std::hex << static_cast<int>(dut.residual_csum)
                          << std::dec << "\n";
                return 1;
            }
            if (!saw_recon_dbg) {
                std::cerr << "FAIL stream_path integrated sim: pass " << pass
                          << " recon_dbg_valid never asserted"
                          << " place_cyc=" << place_cyc << " nalu=" << dut.nalu_count << "\n";
                return 1;
            }
            if (!dut.recon_valid || got_sig != 0x3B || ((got_dbg & ~0x06) != (0xF9 & ~0x06))) {
                std::cerr << "FAIL stream_path integrated sim: pass " << pass
                          << " recon_valid=" << (dut.recon_valid ? 1 : 0)
                          << " recon_sig got 0x" << std::hex << got_sig << " want 0x3b"
                          << " recon_dbg got 0x" << got_dbg << " usable want 0xf9"
                          << std::dec << " place_cyc=" << place_cyc
                          << " recon_cyc=" << recon_cyc
                          << " nalu=" << dut.nalu_count << " bytes=" << dut.bytes_in
                          << " residual_csum=0x" << std::hex << static_cast<int>(dut.residual_csum)
                          << std::dec << "\n";
                return 1;
            }
            last_place_cyc = place_cyc;
            last_recon_cyc = recon_cyc;
        }

        std::cout << "OK stream_path integrated sim: slice_hdr_parser->decode_stub handoff"
                  << " two_pushes=PASS recon_sig=0x" << std::hex
                  << static_cast<int>(static_cast<uint8_t>(dut.recon_sig))
                  << " recon_dbg=0x" << static_cast<int>(static_cast<uint8_t>(dut.recon_dbg))
                  << " residual_csum=0x" << static_cast<int>(dut.residual_csum)
                  << std::dec << " nalu=" << dut.nalu_count
                  << " bytes=" << dut.bytes_in
                  << " frames=" << dut.stub_frames
                  << " last_place_cyc=" << last_place_cyc
                  << " last_recon_cyc=" << last_recon_cyc
                  << " fixture=" << argv[1] << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL stream_path integrated sim: " << e.what() << "\n";
        return 1;
    }
}
