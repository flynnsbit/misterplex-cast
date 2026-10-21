#include "Vstream_path_recon_integration_tb_top.h"
#include "verilated.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open bitstream: " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open golden json: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int parseIntAfter(const std::string& text, const std::string& key, std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle, start);
    if (p == std::string::npos) throw std::runtime_error("missing key: " + key);
    p = text.find(':', p);
    if (p == std::string::npos) throw std::runtime_error("malformed key: " + key);
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    char* end = nullptr;
    long v = std::strtol(text.c_str() + p, &end, 10);
    if (end == text.c_str() + p) throw std::runtime_error("invalid int for key: " + key);
    return static_cast<int>(v);
}

std::string parseStringAfter(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle);
    if (p == std::string::npos) throw std::runtime_error("missing key: " + key);
    p = text.find(':', p);
    p = text.find('"', p);
    if (p == std::string::npos) throw std::runtime_error("malformed string key: " + key);
    const std::size_t q = text.find('"', p + 1);
    if (q == std::string::npos) throw std::runtime_error("unterminated string key: " + key);
    return text.substr(p + 1, q - p - 1);
}

std::array<int, 16> parseArray16(const std::string& text, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = text.find(needle);
    if (p == std::string::npos) throw std::runtime_error("missing array: " + key);
    p = text.find('[', p);
    const std::size_t q = text.find(']', p);
    if (p == std::string::npos || q == std::string::npos) throw std::runtime_error("malformed array: " + key);
    std::array<int, 16> out{};
    int n = 0;
    const char* cur = text.c_str() + p + 1;
    const char* end = text.c_str() + q;
    while (cur < end) {
        while (cur < end && (std::isspace(static_cast<unsigned char>(*cur)) || *cur == ',')) ++cur;
        if (cur >= end) break;
        char* next = nullptr;
        long value = std::strtol(cur, &next, 10);
        if (next == cur) throw std::runtime_error("invalid number in array: " + key);
        if (n >= 16) throw std::runtime_error("too many entries in array: " + key);
        out[static_cast<std::size_t>(n++)] = static_cast<int>(value);
        cur = next;
    }
    if (n != 16) throw std::runtime_error("array does not have 16 entries: " + key);
    return out;
}

struct Nal {
    std::size_t start = 0;
    std::size_t sc_len = 0;
    std::size_t header = 0;
    std::size_t end = 0;
    int type = 0;
};

std::vector<Nal> findNals(const std::vector<uint8_t>& b) {
    std::vector<Nal> out;
    std::size_t i = 0;
    while (i + 3 < b.size()) {
        std::size_t sc = 0;
        if (i + 4 <= b.size() && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) sc = 4;
        else if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) sc = 3;
        if (!sc) {
            ++i;
            continue;
        }
        const std::size_t header = i + sc;
        std::size_t j = header + 1;
        while (j + 3 < b.size()) {
            if (b[j] == 0 && b[j + 1] == 0 && (b[j + 2] == 1 || (j + 3 < b.size() && b[j + 2] == 0 && b[j + 3] == 1))) break;
            ++j;
        }
        out.push_back({i, sc, header, j, header < b.size() ? (b[header] & 0x1f) : 0});
        i = j;
    }
    return out;
}

void putBits(std::vector<int>& out, int bits, int len) {
    for (int i = len - 1; i >= 0; --i) out.push_back((bits >> i) & 1);
}

int signedToLevelCode(int level) {
    return level > 0 ? 2 * level - 2 : -2 * level - 1;
}

int suffixNextFirst(int prefix, int suffixLength, int level) {
    if (prefix > 14 || (prefix == 14 && suffixLength == 0)) return 2;
    return 1 + (level + 3 > 6);
}

int suffixNext(int suffixLength, int level) {
    static const unsigned lim[7] = {0, 3, 6, 12, 24, 48, 0xffffffffu};
    if (suffixLength < 6 && lim[suffixLength] + static_cast<unsigned>(level) > 2u * lim[suffixLength])
        return suffixLength + 1;
    return suffixLength;
}

void encodeLevel(std::vector<int>& bits, int level, bool firstNonT1, int t1, int& suffixLength) {
    int levelCode = signedToLevelCode(level);
    if (firstNonT1 && t1 < 3) levelCode -= 2;
    int prefix = 0;
    int suffixBits = 0;
    int suffixLen = 0;
    if (firstNonT1) {
        if (suffixLength == 0) {
            if (levelCode < 14) {
                prefix = levelCode;
            } else if (levelCode < 30) {
                prefix = 14; suffixLen = 4; suffixBits = levelCode - 14;
            } else {
                prefix = 15; suffixLen = 12; suffixBits = levelCode - 30;
            }
        } else if (levelCode < (14 << suffixLength)) {
            prefix = levelCode >> suffixLength; suffixLen = suffixLength; suffixBits = levelCode & ((1 << suffixLength) - 1);
        } else {
            prefix = 15; suffixLen = 12; suffixBits = levelCode - (15 << suffixLength);
        }
    } else if (levelCode < (15 << suffixLength)) {
        prefix = levelCode >> suffixLength; suffixLen = suffixLength; suffixBits = levelCode & ((1 << suffixLength) - 1);
    } else {
        prefix = 15; suffixLen = 12; suffixBits = levelCode - (15 << suffixLength);
    }
    for (int i = 0; i < prefix; ++i) bits.push_back(0);
    bits.push_back(1);
    putBits(bits, suffixBits, suffixLen);
    suffixLength = firstNonT1 ? suffixNextFirst(prefix, suffixLength, level) : suffixNext(suffixLength, level);
}

std::vector<int> encodeEscapeCoeffBlock() {
    // Table 0, TotalCoeff=12, TrailingOnes=2. Coefficients are placed at scan
    // positions 0..11 with no zeros, so total_zeros is absent after levels.
    static const int coeff[12] = {300, -301, 255, -256, 64, -33, 17, -8, 4, -2, 1, -1};
    std::vector<int> bits;
    putBits(bits, 0x7, 14); // coeff_token table 0, TotalCoeff=12, T1=2
    bits.push_back(1);      // trailing one sign for -1 at highest scan
    bits.push_back(0);      // trailing one sign for +1
    int suffixLength = 1;   // tc > 10 and t1 < 3
    for (int i = 9; i >= 0; --i) {
        encodeLevel(bits, coeff[i], i == 9, 2, suffixLength);
    }
    return bits;
}

void patchFirstResidual(std::vector<uint8_t>& b, int rbspBitOffset) {
    auto nals = findNals(b);
    auto it = std::find_if(nals.begin(), nals.end(), [](const Nal& n) { return n.type == 1 || n.type == 5; });
    if (it == nals.end()) throw std::runtime_error("no VCL NAL to patch");
    const std::size_t payload = it->header + 1;
    for (std::size_t p = payload; p + 2 < it->end && p < payload + 80; ++p) {
        if (b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 3) throw std::runtime_error("EPB before patch offset not supported");
    }
    const auto bits = encodeEscapeCoeffBlock();
    for (std::size_t k = 0; k < bits.size(); ++k) {
        const std::size_t bit = static_cast<std::size_t>(rbspBitOffset) + k;
        const std::size_t byte = payload + bit / 8;
        if (byte >= it->end) throw std::runtime_error("patched residual exceeds NAL");
        const uint8_t mask = static_cast<uint8_t>(1u << (7 - (bit & 7)));
        if (bits[k]) b[byte] |= mask;
        else b[byte] &= static_cast<uint8_t>(~mask);
    }
}

int levelScale(int qp, int row, int col) {
    static constexpr int norm[6][3] = {{10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}};
    const int odd = (row & 1) + (col & 1);
    return norm[qp % 6][odd == 0 ? 0 : (odd == 1 ? 1 : 2)];
}

int clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

uint8_t expectedEscapeReconSig(int qp) {
    static constexpr int zigzag[16] = {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
    static const int coeff[16] = {300, -301, 255, -256, 64, -33, 17, -8, 4, -2, 1, -1, 0, 0, 0, 0};
    int b[4][4]{};
    for (int scan = 0; scan < 16; ++scan) {
        const int c = coeff[scan];
        if (!c) continue;
        const int pos = zigzag[scan];
        const int row = pos / 4, col = pos % 4;
        const int qmul = (levelScale(qp, row, col) * 16) << (qp / 6 + 2);
        b[row][col] = (c * qmul + 32) >> 6;
    }
    b[0][0] += 32;
    int t[4][4]{};
    for (int i = 0; i < 4; ++i) {
        int z0 = b[i][0] + b[i][2];
        int z1 = b[i][0] - b[i][2];
        int z2 = (b[i][1] >> 1) - b[i][3];
        int z3 = b[i][1] + (b[i][3] >> 1);
        t[i][0] = z0 + z3; t[i][1] = z1 + z2; t[i][2] = z1 - z2; t[i][3] = z0 - z3;
    }
    uint8_t sig = 0;
    for (int j = 0; j < 4; ++j) {
        int z0 = t[0][j] + t[2][j];
        int z1 = t[0][j] - t[2][j];
        int z2 = (t[1][j] >> 1) - t[3][j];
        int z3 = t[1][j] + (t[3][j] >> 1);
        sig ^= static_cast<uint8_t>(clip8(128 + ((z0 + z3) >> 6)));
        sig ^= static_cast<uint8_t>(clip8(128 + ((z1 + z2) >> 6)));
        sig ^= static_cast<uint8_t>(clip8(128 + ((z1 - z2) >> 6)));
        sig ^= static_cast<uint8_t>(clip8(128 + ((z0 - z3) >> 6)));
    }
    return sig;
}

void tick(Vstream_path_recon_integration_tb_top& dut) {
    dut.clk = 0; dut.eval();
    dut.clk = 1; dut.eval();
}

struct RunResult {
    bool reconValid = false;
    int reconSig = -1;
    int reconDbg = -1;
    int residualCsum = -1;
    int naluCount = 0;
    int sliceCount = 0;
    int idrCount = 0;
    int bytesSeen = 0;
    int cycles = 0;
};

RunResult runStream(std::vector<uint8_t> bytes) {
    Vstream_path_recon_integration_tb_top dut;
    dut.clk = 0;
    dut.reset = 1;
    dut.ioctl_download = 0;
    dut.ioctl_wr = 0;
    dut.ioctl_dout = 0;
    dut.enable = 1;
    dut.flush = 0;
    tick(dut); tick(dut);
    dut.reset = 0;
    tick(dut);

    dut.ioctl_download = 1;
    for (uint8_t byte : bytes) {
        dut.ioctl_dout = byte;
        dut.ioctl_wr = 1;
        tick(dut);
        dut.ioctl_wr = 0;
        tick(dut);
    }
    dut.ioctl_download = 0;

    RunResult r;
    for (int cyc = 0; cyc < 400000; ++cyc) {
        tick(dut);
        if (dut.recon_valid && !r.reconValid) {
            r.reconValid = true;
            r.reconSig = static_cast<uint8_t>(dut.recon_sig);
            r.reconDbg = static_cast<uint8_t>(dut.recon_dbg);
            r.residualCsum = static_cast<uint8_t>(dut.residual_csum);
            r.cycles = cyc;
        }
        if (r.reconValid && dut.bytes_seen >= bytes.size() && dut.nalu_count >= 5) break;
    }
    r.naluCount = dut.nalu_count;
    r.sliceCount = dut.slice_count;
    r.idrCount = dut.idr_count;
    r.bytesSeen = dut.bytes_seen;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " normal|escape-red bitstream.264 mb_golden.json\n";
        return 2;
    }

    try {
        const std::string mode = argv[1];
        std::vector<uint8_t> base = readBytes(argv[2]);
        const std::string golden = readText(argv[3]);
        if (parseStringAfter(golden, "format") != "misterplex.p3.mb_golden.v1")
            throw std::runtime_error("golden is not misterplex.p3.mb_golden.v1");
        const int rbspStart = parseIntAfter(golden, "bit_offset_start");
        const int qp = parseIntAfter(golden, "qp");

        auto stream = base;
        if (mode == "escape-red") patchFirstResidual(stream, rbspStart);
        const auto originalSize = stream.size();
        stream.insert(stream.end(), base.begin(), base.end());
        const auto nals = findNals(stream);
        if (nals.size() < 2) throw std::runtime_error("fixture has fewer than two NALs");

        const RunResult r = runStream(stream);
        std::cout << "STREAM_PATH_INTEGRATION raw mode=" << mode
                  << " bytes=" << originalSize
                  << " doubled_bytes=" << stream.size()
                  << " nal_count=" << r.naluCount
                  << " slice_count=" << r.sliceCount
                  << " idr_count=" << r.idrCount
                  << " bytes_seen=" << r.bytesSeen
                  << " recon_valid=" << (r.reconValid ? 1 : 0)
                  << " recon_sig=0x" << std::hex << std::setw(2) << std::setfill('0') << (r.reconSig & 0xff)
                  << " recon_dbg=0x" << std::setw(2) << (r.reconDbg & 0xff)
                  << " residual_csum=0x" << std::setw(2) << (r.residualCsum & 0xff)
                  << std::dec << " cycles_to_recon=" << r.cycles << "\n";

        if (r.naluCount < 5) {
            std::cerr << "FAIL stream_path integration: doubled fixture did not drain past first VCL; nalu_count=" << r.naluCount << "\n";
            return 1;
        }
        const int trueRecon = std::strtol(parseStringAfter(golden, "first_recon_signature8_hex").c_str(), nullptr, 16);
        if (mode == "normal") {
            if (!r.reconValid) {
                std::cerr << "FAIL stream_path normal: recon_valid never asserted\n";
                return 1;
            }
            const int want = trueRecon;
            if (r.reconSig != want) {
                std::cerr << "FAIL stream_path normal: recon_sig got 0x" << std::hex << r.reconSig
                          << " want 0x" << want << std::dec << "\n";
                return 1;
            }
            if (r.reconSig == 0x00) {
                std::cerr << "FAIL stream_path normal: accepted latency red-check signature 0x00\n";
                return 1;
            }
            std::cout << "OK stream_path normal: true recon signature matched golden and rejected 0x00 latency signatures\n";
            return 0;
        }
        if (mode == "escape-red") {
            const int want = expectedEscapeReconSig(qp);
            if (!r.reconValid) {
                std::cout << "OK escape red-check: high-level CAVLC stream through real parser produced no recon_valid"
                          << " expected_signature=0x" << std::hex << (want & 0xff)
                          << std::dec << " (old hand-fed decode_stub bench would not exercise parser level decode)\n";
                return 0;
            }
            if (r.reconSig == want) {
                std::cerr << "FAIL escape red-check: integrated parser unexpectedly matched high-level expected signature 0x"
                          << std::hex << want << std::dec << "\n";
                return 1;
            }
            std::cout << "OK escape red-check: high-level CAVLC stream through real parser mismatched expected signature"
                      << " got=0x" << std::hex << (r.reconSig & 0xff)
                      << " expected=0x" << (want & 0xff)
                      << std::dec << " (old hand-fed decode_stub bench would not exercise parser level decode)\n";
            return 0;
        }
        std::cerr << "unknown mode: " << mode << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR stream_path integration: " << e.what() << "\n";
        return 2;
    }
}
