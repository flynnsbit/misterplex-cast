// Host mirror of nalu_scanner start-code + type classify (unit, no FPGA).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct NaluStats {
    int count = 0;
    int sps = 0, pps = 0, idr = 0, slice = 0;
    int vcl = 0;
    uint8_t last = 0;
};

static NaluStats scanNalu(const uint8_t* p, size_t n) {
    NaluStats s;
    int zrun = 0;
    bool pend = false;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = p[i];
        if (pend) {
            ++s.count;
            s.last = b;
            uint8_t t = b & 0x1f;
            if (t == 7)
                ++s.sps;
            else if (t == 8)
                ++s.pps;
            else if (t == 5) {
                ++s.idr;
                ++s.vcl;
            } else if (t == 1) {
                ++s.slice;
                ++s.vcl;
            }
            pend = false;
            zrun = 0;
            continue;
        }
        if (b == 0x00) {
            if (zrun < 3)
                ++zrun;
        } else if (b == 0x01 && zrun >= 2) {
            pend = true;
            zrun = 0;
        } else {
            zrun = 0;
        }
    }
    return s;
}

int main() {
    // Same layout as scripts/gen_test_annexb.py
    std::vector<uint8_t> blob;
    auto add4 = [&](uint8_t t, const std::string& pay) {
        blob.insert(blob.end(), {0, 0, 0, 1, static_cast<uint8_t>(0x60 | (t & 0x1f))});
        blob.insert(blob.end(), pay.begin(), pay.end());
    };
    add4(7, "SPS");
    add4(8, "PPS");
    add4(5, "IDR");
    blob.insert(blob.end(), {0, 0, 1, 0x61, 'P'});
    auto s = scanNalu(blob.data(), blob.size());
    if (s.count != 4 || s.sps != 1 || s.pps != 1 || s.idr != 1 || s.slice != 1 || s.vcl != 2) {
        std::printf("FAIL count=%d sps=%d pps=%d idr=%d slice=%d vcl=%d\n", s.count, s.sps, s.pps,
                    s.idr, s.slice, s.vcl);
        return 1;
    }
    std::printf("test_annexb_count: OK (count=%d vcl=%d idr=%d)\n", s.count, s.vcl, s.idr);
    return 0;
}
