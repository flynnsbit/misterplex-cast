#include "Vh264_iq_idct_4x4.h"
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
#include <vector>

namespace {

struct BlockGolden {
    int block = -1;
    int total_coeff = 0;
    std::array<int, 16> pred{};
    std::array<int, 16> dequant{};
    std::array<int, 16> idct{};
    std::array<int, 16> recon{};
};

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

std::vector<BlockGolden> parseBlocks(const std::string& json) {
    std::vector<BlockGolden> blocks;
    std::size_t p = json.find("\"blocks\"");
    if (p == std::string::npos) {
        throw std::runtime_error("fixture has no blocks array");
    }
    while (true) {
        p = json.find("\"block\"", p);
        if (p == std::string::npos) {
            break;
        }
        std::size_t next = json.find("\"block\"", p + 7);
        std::string chunk = json.substr(p, next == std::string::npos ? std::string::npos : next - p);
        BlockGolden b;
        b.block = parseIntAfter(chunk, "block");
        b.total_coeff = parseIntAfter(chunk, "total_coeff");
        b.pred = parseArray16(chunk, "pred");
        b.dequant = parseArray16(chunk, "dequant");
        b.idct = parseArray16(chunk, "idct");
        b.recon = parseArray16(chunk, "recon");
        blocks.push_back(b);
        if (next == std::string::npos) {
            break;
        }
        p = next;
    }
    return blocks;
}

int levelScale(int qp, int row, int col) {
    static constexpr int norm[6][3] = {
        {10, 13, 16}, {11, 14, 18}, {13, 16, 20},
        {14, 18, 23}, {16, 20, 25}, {18, 23, 29},
    };
    int mi = (((row & 1) + (col & 1)) == 0) ? 0 : ((((row & 1) + (col & 1)) == 1) ? 1 : 2);
    return norm[qp % 6][mi];
}

int dequantValue(int coeff, int qp, int pos) {
    int row = pos / 4;
    int col = pos % 4;
    int qmul = (levelScale(qp, row, col) * 16) << (qp / 6 + 2);
    return (coeff * qmul + 32) >> 6;
}

std::array<int, 16> deriveCoeffScan(const std::array<int, 16>& dequant, int qp) {
    static constexpr int zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
    std::array<int, 16> coeff{};
    for (int scan = 0; scan < 16; ++scan) {
        int pos = zigzag[scan];
        int want = dequant[static_cast<std::size_t>(pos)];
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

int signExtend(int value, int bits) {
    const int sign = 1 << (bits - 1);
    const int mask = (1 << bits) - 1;
    value &= mask;
    return (value ^ sign) - sign;
}

bool compareSignal(const char* name, int block, int index, int got, int want) {
    if (got == want) {
        return true;
    }
    std::cerr << "FAIL real RTL sim: block=" << block << " " << name << "[" << index
              << "] got " << got << " want " << want << '\n';
    return false;
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
        const auto blocks = parseBlocks(json);
        if (blocks.size() != 16) {
            std::cerr << "FAIL real RTL sim: parsed " << blocks.size() << " blocks, expected 16\n";
            return 1;
        }

        Vh264_iq_idct_4x4 dut;
        int compared = 0;
        for (const auto& block : blocks) {
            const auto coeff = deriveCoeffScan(block.dequant, qp);
            dut.max_coeff = 16;
            dut.qp = static_cast<uint8_t>(qp);
            for (int i = 0; i < 16; ++i) {
                dut.coeff[i] = static_cast<int16_t>(coeff[static_cast<std::size_t>(i)]);
                dut.pred[i] = static_cast<uint8_t>(block.pred[static_cast<std::size_t>(i)]);
            }
            dut.eval();

            for (int i = 0; i < 16; ++i) {
                const int gotDequant = signExtend(static_cast<int>(dut.dequant[i]), 18);
                const int gotIdct = signExtend(static_cast<int>(dut.idct[i]), 18);
                const int gotRecon = static_cast<uint8_t>(dut.recon[i]);
                if (!compareSignal("dequant", block.block, i, gotDequant, block.dequant[static_cast<std::size_t>(i)]) ||
                    !compareSignal("idct", block.block, i, gotIdct, block.idct[static_cast<std::size_t>(i)]) ||
                    !compareSignal("recon", block.block, i, gotRecon, block.recon[static_cast<std::size_t>(i)])) {
                    return 1;
                }
                compared += 3;
            }
        }
        std::cout << "OK real RTL sim: h264_iq_idct_4x4 elaborated with Verilator; blocks="
                  << blocks.size() << " compared_values=" << compared << " qp=" << qp
                  << " fixture=" << argv[1] << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL real RTL sim: " << e.what() << '\n';
        return 1;
    }
}
