#include "Vdecode_stub_recon_tb.h"
#include "verilated.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string readText(const char* path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int parseIntAfter(const std::string& text, const std::string& key, std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle, start);
    if (p == std::string::npos) {
        throw std::runtime_error("missing integer key: " + key);
    }
    p = text.find(':', p);
    if (p == std::string::npos) {
        throw std::runtime_error("malformed integer key: " + key);
    }
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) {
        ++p;
    }
    const char* begin = text.c_str() + p;
    char* end = nullptr;
    long value = std::strtol(begin, &end, 10);
    if (end == begin) {
        throw std::runtime_error("invalid integer value for key: " + key);
    }
    return static_cast<int>(value);
}

std::array<int, 16> parseArray16(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle);
    if (p == std::string::npos) {
        throw std::runtime_error("missing array key: " + key);
    }
    p = text.find('[', p);
    std::size_t q = text.find(']', p);
    if (p == std::string::npos || q == std::string::npos) {
        throw std::runtime_error("malformed array key: " + key);
    }
    std::array<int, 16> out{};
    int n = 0;
    const char* cur = text.c_str() + p + 1;
    const char* end = text.c_str() + q;
    while (cur < end) {
        while (cur < end && (std::isspace(static_cast<unsigned char>(*cur)) || *cur == ',')) {
            ++cur;
        }
        if (cur >= end) {
            break;
        }
        char* next = nullptr;
        long value = std::strtol(cur, &next, 10);
        if (next == cur) {
            throw std::runtime_error("invalid number in array: " + key);
        }
        if (n >= 16) {
            throw std::runtime_error("too many entries in array: " + key);
        }
        out[static_cast<std::size_t>(n++)] = static_cast<int>(value);
        cur = next;
    }
    if (n != 16) {
        std::ostringstream oss;
        oss << "array " << key << " has " << n << " entries, expected 16";
        throw std::runtime_error(oss.str());
    }
    return out;
}

int levelScale(int qp, int row, int col) {
    static constexpr int norm[6][3] = {
        {10, 13, 16}, {11, 14, 18}, {13, 16, 20},
        {14, 18, 23}, {16, 20, 25}, {18, 23, 29},
    };
    const int odd = (row & 1) + (col & 1);
    const int mi = odd == 0 ? 0 : (odd == 1 ? 1 : 2);
    return norm[qp % 6][mi];
}

int dequantValue(int coeff, int qp, int pos) {
    const int row = pos / 4;
    const int col = pos % 4;
    const int qmul = (levelScale(qp, row, col) * 16) << (qp / 6 + 2);
    return (coeff * qmul + 32) >> 6;
}

std::array<int, 16> deriveCoeffScan(const std::array<int, 16>& dequant, int qp) {
    static constexpr int zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
    std::array<int, 16> coeff{};
    for (int scan = 0; scan < 16; ++scan) {
        const int pos = zigzag[scan];
        const int want = dequant[static_cast<std::size_t>(pos)];
        if (want == 0) {
            coeff[static_cast<std::size_t>(scan)] = 0;
            continue;
        }
        bool found = false;
        for (int candidate = -4096; candidate <= 4095; ++candidate) {
            if (dequantValue(candidate, qp, pos) == want) {
                coeff[static_cast<std::size_t>(scan)] = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            std::ostringstream oss;
            oss << "cannot derive coeff for scan=" << scan << " pos=" << pos << " dequant=" << want;
            throw std::runtime_error(oss.str());
        }
    }
    return coeff;
}

int xor8(const std::array<int, 16>& values) {
    int sig = 0;
    for (int v : values) {
        sig ^= (v & 0xff);
    }
    return sig & 0xff;
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " tests/fixtures/p3_host_recon/mb0_luma_v1.json\n";
        return 2;
    }

    try {
        const std::string json = readText(argv[1]);
        const int qp = parseIntAfter(json, "qp");
        const std::size_t block0_pos = json.find("\"block\": 0");
        if (block0_pos == std::string::npos) {
            throw std::runtime_error("fixture missing block 0");
        }
        const std::size_t block1_pos = json.find("\"block\": 1", block0_pos);
        const std::string block0 = json.substr(block0_pos, block1_pos == std::string::npos ? std::string::npos : block1_pos - block0_pos);
        const auto dequant = parseArray16(block0, "dequant");
        const auto recon = parseArray16(block0, "recon");
        const auto coeff = deriveCoeffScan(dequant, qp);
        const int want_sig = xor8(recon);
        if (want_sig != 0x3b) {
            throw std::runtime_error("fixture recon signature is not 0x3b");
        }

        Vdecode_stub_recon_tb dut;
        dut.clk = 0;
        dut.reset = 1;
        dut.vcl_pulse = 0;
        dut.residual_ok = 0;
        dut.slice_qp = static_cast<uint8_t>(qp);
        for (int i = 0; i < 16; ++i) {
            dut.coeff[i] = static_cast<int16_t>(coeff[static_cast<std::size_t>(i)]);
        }

        auto tick = [&]() {
            dut.clk = 0;
            dut.eval();
            dut.clk = 1;
            dut.eval();
        };

        tick();
        tick();
        dut.reset = 0;
        tick();
        dut.vcl_pulse = 1;
        tick();
        dut.vcl_pulse = 0;
        dut.residual_ok = 1;
        for (int cycle = 0; cycle < 32 && !dut.recon_valid; ++cycle) {
            tick();
        }
        if (!dut.recon_valid) {
            std::cerr << "FAIL decode_stub RTL sim: recon_valid never asserted\n";
            return 1;
        }
        const int got_sig = static_cast<uint8_t>(dut.recon_sig);
        if (got_sig != want_sig) {
            std::cerr << "FAIL decode_stub RTL sim: recon_sig got 0x" << std::hex << got_sig
                      << " want 0x" << want_sig << std::dec << "\n";
            return 1;
        }
        if (!dut.recon_dbg_valid) {
            std::cerr << "FAIL decode_stub RTL sim: recon_dbg_valid never asserted\n";
            return 1;
        }
        const int got_dbg = static_cast<uint8_t>(dut.recon_dbg);
        const int want_dbg = 0xF9;
        if (got_dbg != want_dbg) {
            std::cerr << "FAIL decode_stub RTL sim: recon_dbg got 0x" << std::hex << got_dbg
                      << " want 0x" << want_dbg << std::dec << "\n";
            return 1;
        }

        std::cout << "OK decode_stub RTL sim: product decode path recon_sig=0x" << std::hex << got_sig
                  << " recon_dbg=0x" << got_dbg
                  << std::dec << " qp=" << qp << " fixture=" << argv[1] << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL decode_stub RTL sim: " << e.what() << '\n';
        return 1;
    }
}
