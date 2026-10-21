#include "Vstream_path_inter_tb.h"
#include "verilated.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int kFrameW = 320;
constexpr int kFrameH = 240;

std::vector<uint8_t> readBytes(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("cannot open Annex-B fixture: ") + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

struct NalSeg {
    std::size_t start;
    std::size_t end;
    uint8_t type;
};

std::size_t startCodeLen(const std::vector<uint8_t>& b, std::size_t pos) {
    if (pos + 4 <= b.size() && b[pos] == 0 && b[pos + 1] == 0 && b[pos + 2] == 0 && b[pos + 3] == 1) return 4;
    if (pos + 3 <= b.size() && b[pos] == 0 && b[pos + 1] == 0 && b[pos + 2] == 1) return 3;
    return 0;
}

std::vector<NalSeg> findNals(const std::vector<uint8_t>& b) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 < b.size();) {
        std::size_t len = startCodeLen(b, i);
        if (len != 0) {
            starts.push_back(i);
            i += len;
        } else {
            ++i;
        }
    }
    std::vector<NalSeg> out;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::size_t scLen = startCodeLen(b, starts[i]);
        const std::size_t hdr = starts[i] + scLen;
        if (hdr >= b.size()) throw std::runtime_error("truncated NAL header");
        out.push_back({starts[i], (i + 1 < starts.size()) ? starts[i + 1] : b.size(), static_cast<uint8_t>(b[hdr] & 0x1f)});
    }
    return out;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

class Sim {
public:
    Vstream_path_inter_tb top{};
    uint64_t cycles = 0;
    int pxX = 0;
    int pxY = 0;
    std::vector<int> interBandSamples{0, 0, 0, 0};
    int interBadSamples = 0;

    void evalPosedgeSideEffects() {
        if (top.fs_wr_reset) {
            pxX = 0;
            pxY = 0;
        }
        if (top.fs_wr_en) {
            if (pxY >= 4 && pxY < 16 && pxX >= 16 && pxX < 32) {
                const int band = (pxX - 16) / 4;
                const uint8_t sigs[4] = {0x22, 0x69, 0x63, 0x5e};
                const uint16_t want = rgb565(0x10, 0xf0, sigs[band]);
                if (top.fs_wr_pixel == want) {
                    interBandSamples[band]++;
                } else {
                    interBadSamples++;
                    if (interBadSamples <= 4) {
                        std::cerr << "FAIL stream_path inter diag pixel: x=" << pxX
                                  << " y=" << pxY << " band=" << band
                                  << " got=0x" << std::hex << std::setw(4) << std::setfill('0')
                                  << static_cast<int>(top.fs_wr_pixel)
                                  << " want=0x" << std::setw(4) << static_cast<int>(want)
                                  << std::dec << std::setfill(' ') << "\n";
                    }
                }
            }
            ++pxX;
            if (pxX == kFrameW) {
                pxX = 0;
                ++pxY;
                if (pxY == kFrameH) pxY = 0;
            }
        }
    }

    void tick() {
        top.clk = 0;
        top.eval();
        top.clk = 1;
        top.eval();
        evalPosedgeSideEffects();
        top.clk = 0;
        top.eval();
        ++cycles;
    }

    void reset() {
        top.reset = 1;
        top.flush = 0;
        top.ioctl_download = 0;
        top.ioctl_wr = 0;
        top.ioctl_dout = 0;
        for (int i = 0; i < 8; ++i) tick();
        top.reset = 0;
        for (int i = 0; i < 4; ++i) tick();
    }

    void feedByte(uint8_t v) {
        top.ioctl_download = 1;
        top.ioctl_dout = v;
        top.ioctl_wr = 1;
        tick();
        top.ioctl_wr = 0;
        tick();
    }

    bool waitForBytes(uint32_t want, int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            if (top.bytes_seen >= want) return true;
            tick();
        }
        return top.bytes_seen >= want;
    }

    bool waitForFrames(uint16_t want, int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            if (top.stub_frames >= want && !top.stub_busy) return true;
            tick();
        }
        return top.stub_frames >= want && !top.stub_busy;
    }
};

bool fail(const std::string& msg) {
    std::cerr << "FAIL stream_path inter RTL: " << msg << "\n";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " plex_inter_p16_baseline_320x240_12f.264\n";
        return 2;
    }

    try {
        const auto bytes = readBytes(argv[1]);
        const auto nals = findNals(bytes);
        if (nals.size() < 4) return fail("fixture has too few NALs") ? 0 : 1;
        int idrNals = 0;
        int pNals = 0;
        for (const auto& n : nals) {
            if (n.type == 5) ++idrNals;
            if (n.type == 1) ++pNals;
        }
        if (idrNals == 0) return fail("fixture must contain an IDR slice") ? 0 : 1;
        Sim sim;
        sim.reset();
        uint32_t fed = 0;
        uint16_t expectedFrames = 0;
        for (const auto& n : nals) {
            for (std::size_t i = n.start; i < n.end; ++i) sim.feedByte(bytes[i]);
            fed += static_cast<uint32_t>(n.end - n.start);
            sim.top.ioctl_download = 0;
            sim.tick();
            if (!sim.waitForBytes(fed, 20000)) {
                return fail("scanner did not drain bytes after NAL type " + std::to_string(n.type)) ? 0 : 1;
            }
            if (n.type == 5 || n.type == 1) {
                ++expectedFrames;
                if (!sim.waitForFrames(expectedFrames, std::max(200000, kFrameW * kFrameH * 3))) {
                    return fail("decode_stub did not return idle after VCL frame " + std::to_string(expectedFrames)) ? 0 : 1;
                }
            } else {
                for (int i = 0; i < 256; ++i) sim.tick();
            }
        }
        for (int i = 0; i < 1024; ++i) sim.tick();

        bool ok = true;
        auto check = [&](bool cond, const std::string& msg) {
            if (!cond) {
                fail(msg);
                ok = false;
            }
        };
        check(sim.top.bytes_in == bytes.size(), "bytes_in mismatch");
        check(sim.top.bytes_seen == bytes.size(), "bytes_seen mismatch");
        check(sim.top.nalu_count == nals.size(), "nalu_count mismatch");
        check(sim.top.sps_count == 1, "sps_count mismatch");
        check(sim.top.pps_count == 1, "pps_count mismatch");
        check(sim.top.idr_count == idrNals, "idr_count mismatch");
        check(sim.top.slice_count == pNals, "P slice_count mismatch");
        check(sim.top.has_idr, "has_idr not set");
        check(sim.top.sps_valid, "SPS did not validate");
        check(sim.top.pps_valid, "PPS did not validate");
        check(sim.top.sps_profile == 66, "SPS profile is not Baseline 66");
        check(sim.top.sps_width == 320 && sim.top.sps_height == 240, "SPS geometry mismatch");
        check(sim.top.sps_mb_w == 20 && sim.top.sps_mb_h == 15, "SPS MB geometry mismatch");
        check(sim.top.stub_frames == expectedFrames, "stub frame count mismatch");
        check(!sim.top.stub_busy, "stub did not return idle after final VCL");
        for (int i = 0; i < 4; ++i) {
            check(sim.interBandSamples[i] > 0, "missing green inter diagnostic band " + std::to_string(i));
        }
        check(sim.interBadSamples == 0, "red/mismatched inter diagnostic pixels observed");
        if (!ok) return 1;

        std::cout << "OK real RTL sim: stream_path integrated inter vector nals=" << nals.size()
                  << " idr=" << idrNals << " p=" << pNals
                  << " frames=" << sim.top.stub_frames
                  << " bytes=" << bytes.size()
                  << " sps=" << static_cast<int>(sim.top.sps_width) << "x" << static_cast<int>(sim.top.sps_height)
                  << " mb=" << static_cast<int>(sim.top.sps_mb_w) << "x" << static_cast<int>(sim.top.sps_mb_h)
                  << " inter_band_samples=" << sim.interBandSamples[0] << "/" << sim.interBandSamples[1]
                  << "/" << sim.interBandSamples[2] << "/" << sim.interBandSamples[3]
                  << (pNals == 0 ? " idr-multinal" : " p-slice-multinal")
                  << " cycles=" << sim.cycles << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL stream_path inter RTL: " << e.what() << "\n";
        return 1;
    }
}
