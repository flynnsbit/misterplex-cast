// set_status: drive Plex OSD CONF_STR bits via FpgaSpi::setStatusBits.
// Exits 3 without touching the FPGA unless the core gate verifies a Plex build and
// misterplexd is not running (core_gate_tool.hpp).
// Usage:
//   set_status --status | --raw
//   set_status --pattern bars|bars_block|grid|ramp
//   set_status --force-bars 0|1 --tv ntsc|pal --fps 24|30|60|12
//   set_status --audio on|off --ar original|full|arc1|arc2
//   set_status --pulse 10|11|0   # flush audio / bitstream / reset
//   set_status --bit N 0|1
#include "core_gate_tool.hpp"
#include "fpga_spi.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: set_status [--status|--raw]\n"
                 "       set_status [--pattern none|bars|bars_block|grid]\n"
                 "                  [--force-bars 0|1] [--tv ntsc|pal]\n"
                 "                  [--fps 24|30|60|12] [--audio on|off]\n"
                 "                  [--ar original|full|arc1|arc2]\n"
                 "                  [--bit N 0|1] [--pulse N]... [--confstr]\n");
}

const char* patternName(int p) {
    switch (p & 3) {
    case 0: return "none";
    case 1: return "bars";
    case 2: return "bars_block";
    default: return "grid";
    }
}

void printRaw(const uint8_t raw[16]) {
    std::printf("raw:");
    for (int i = 0; i < 16; ++i)
        std::printf(" %02x", raw[i]);
    std::printf("\n");
    const uint16_t lo = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
    const int pat = (lo >> 6) & 3;
    const int fps = (lo >> 4) & 3;
    // bits 121-122 → byte 15 bits 1-2
    const int ar = (raw[15] >> 1) & 3;
    std::printf(
        "osd lo=0x%04x tv=%s fps_sel=%d(%s) pat=%d(%s) audio=%s force_bars=%d "
        "T10=%d T11=%d R0=%d AR_bits=%d\n",
        lo, (lo & 4) ? "PAL" : "NTSC", fps,
        fps == 0 ? "24" : fps == 1 ? "30" : fps == 2 ? "60" : "12", pat,
        patternName(pat), (lo & 0x100) ? "On" : "Off", (lo >> 9) & 1,
        (lo >> 10) & 1, (lo >> 11) & 1, lo & 1, ar);
}

bool readRawStable(misterplex::FpgaSpi& spi, uint8_t out[16]) {
    uint8_t a[16]{}, b[16]{};
    for (int i = 0; i < 8; ++i) {
        if (!spi.getCoreStatus(a))
            return false;
        usleep(10000);
        if (!spi.getCoreStatus(b))
            return false;
        if (std::memcmp(a, b, 16) == 0) {
            std::memcpy(out, a, 16);
            return true;
        }
    }
    std::memcpy(out, b, 16);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    bool do_status = false;
    bool do_raw = false;
    bool do_confstr = false;
    std::vector<int> pairs; // flat bit,val,bit,val,...
    std::vector<int> pulses;

    auto add_field = [&](int lo_bit, int width, int value) {
        for (int i = 0; i < width; ++i) {
            pairs.push_back(lo_bit + i);
            pairs.push_back((value >> i) & 1);
        }
    };

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--status") == 0) {
            do_status = true;
        } else if (std::strcmp(argv[i], "--raw") == 0) {
            do_raw = true;
        } else if (std::strcmp(argv[i], "--confstr") == 0) {
            do_confstr = true;
        } else if (std::strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            int v = 0;
            if (!std::strcmp(p, "none") || !std::strcmp(p, "off") || !std::strcmp(p, "0"))
                v = 0;
            else if (!std::strcmp(p, "bars") || !std::strcmp(p, "1"))
                v = 1;
            else if (!std::strcmp(p, "bars_block") || !std::strcmp(p, "block") ||
                     !std::strcmp(p, "2"))
                v = 2;
            else if (!std::strcmp(p, "grid") || !std::strcmp(p, "3"))
                v = 3;
            else {
                std::fprintf(stderr, "bad pattern: %s (none|bars|bars_block|grid)\n", p);
                return 1;
            }
            add_field(6, 2, v);
        } else if (std::strcmp(argv[i], "--force-bars") == 0 && i + 1 < argc) {
            pairs.push_back(9);
            pairs.push_back(std::atoi(argv[++i]) ? 1 : 0);
        } else if (std::strcmp(argv[i], "--tv") == 0 && i + 1 < argc) {
            const char* t = argv[++i];
            pairs.push_back(2);
            pairs.push_back((!std::strcmp(t, "pal") || !std::strcmp(t, "1")) ? 1 : 0);
        } else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            int f = std::atoi(argv[++i]);
            int v = 0;
            if (f == 24)
                v = 0;
            else if (f == 30)
                v = 1;
            else if (f == 60)
                v = 2;
            else if (f == 12)
                v = 3;
            else {
                std::fprintf(stderr, "bad fps: %d\n", f);
                return 1;
            }
            add_field(4, 2, v);
        } else if (std::strcmp(argv[i], "--audio") == 0 && i + 1 < argc) {
            const char* a = argv[++i];
            pairs.push_back(8);
            // CONF "Audio tone,Off,On": bit0=Off, bit1=On (was inverted On,Off)
            pairs.push_back((!std::strcmp(a, "on") || !std::strcmp(a, "1") ||
                             !std::strcmp(a, "yes") || !std::strcmp(a, "true"))
                                ? 1
                                : 0);
        } else if (std::strcmp(argv[i], "--ar") == 0 && i + 1 < argc) {
            const char* a = argv[++i];
            int v = 0;
            if (!std::strcmp(a, "original") || !std::strcmp(a, "0"))
                v = 0;
            else if (!std::strcmp(a, "full") || !std::strcmp(a, "1"))
                v = 1;
            else if (!std::strcmp(a, "arc1") || !std::strcmp(a, "2"))
                v = 2;
            else if (!std::strcmp(a, "arc2") || !std::strcmp(a, "3"))
                v = 3;
            else {
                std::fprintf(stderr, "bad ar: %s\n", a);
                return 1;
            }
            add_field(121, 2, v);
        } else if (std::strcmp(argv[i], "--bit") == 0 && i + 2 < argc) {
            pairs.push_back(std::atoi(argv[++i]));
            pairs.push_back(std::atoi(argv[++i]) ? 1 : 0);
        } else if (std::strcmp(argv[i], "--pulse") == 0 && i + 1 < argc) {
            pulses.push_back(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-h") == 0 ||
                   std::strcmp(argv[i], "--help") == 0) {
            printUsage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            printUsage();
            return 1;
        }
    }

    if (!do_status && !do_raw && !do_confstr && pairs.empty() && pulses.empty()) {
        printUsage();
        return 1;
    }

    misterplex::FpgaSpi spi;
    if (!spi.open()) {
        std::fprintf(stderr, "fpga open: %s\n", spi.lastError().c_str());
        return 1;
    }
    // Core gate (core_gate_tool.hpp): only a verified Plex build, never under misterplexd.
    if (!misterplex::coreGateVerifyForTool(spi, "set_status"))
        return 3;

    if (!pairs.empty()) {
        if (!spi.setStatusBits(pairs.data(), static_cast<int>(pairs.size() / 2))) {
            std::fprintf(stderr, "set: %s\n", spi.lastError().c_str());
            return 1;
        }
        // Re-apply once so Main's check_status_change cannot race-stomp partial word.
        usleep(20000);
        if (!spi.setStatusBits(pairs.data(), static_cast<int>(pairs.size() / 2))) {
            std::fprintf(stderr, "set2: %s\n", spi.lastError().c_str());
            return 1;
        }
    }

    for (int bit : pulses) {
        // Pulse via full-word RMW so we do not wipe other bits.
        std::vector<int> hi = {bit, 1};
        std::vector<int> lo = {bit, 0};
        if (!spi.setStatusBits(hi.data(), 1)) {
            std::fprintf(stderr, "pulse high %d: %s\n", bit, spi.lastError().c_str());
            return 1;
        }
        usleep(5000);
        if (!spi.setStatusBits(lo.data(), 1)) {
            std::fprintf(stderr, "pulse low %d: %s\n", bit, spi.lastError().c_str());
            return 1;
        }
        usleep(30000);
        std::printf("pulsed bit %d\n", bit);
    }

    if (do_confstr) {
        std::string cs;
        if (!spi.getConfigString(cs)) {
            std::fprintf(stderr, "confstr: %s\n", spi.lastError().c_str());
            return 1;
        }
        // Split on ';' so a 20-item menu is readable in a terminal.
        std::string line;
        for (char c : cs) {
            line.push_back(c);
            if (c == ';') {
                std::printf("%s\n", line.c_str());
                line.clear();
            }
        }
        if (!line.empty())
            std::printf("%s\n", line.c_str());
    }

    uint8_t raw[16]{};
    if (!readRawStable(spi, raw)) {
        std::fprintf(stderr, "status: %s\n", spi.lastError().c_str());
        return 1;
    }
    if (do_raw || !pairs.empty() || !pulses.empty())
        printRaw(raw);

    if (do_status || !pairs.empty() || !pulses.empty()) {
        misterplex::FpgaSpi::CoreStatus st =
            misterplex::FpgaSpi::parseCoreStatus(raw);
        misterplex::FrameStoreStatus fs{};
        const bool haveFrameStoreStatus = spi.readFrameStoreStatus(fs);
        std::printf(
            "status has_frame=%d has_audio=%d has_stream=%d underrun=%d "
            "has_idr=%d sps_valid=%d pps_valid=%d nalu=%u last_nal=0x%02x "
            "res_csum=%u recon_sig=%u recon_dbg=0x%02x sps=%ux%u "
            "stream_nalus=%u bytes_in_unavailable=1",
            st.has_frame ? 1 : 0, st.has_audio ? 1 : 0, st.has_stream ? 1 : 0,
            st.audio_underrun ? 1 : 0, st.has_idr ? 1 : 0,
            st.sps_valid ? 1 : 0, st.pps_valid ? 1 : 0, st.nalu_count, st.last_nal_type,
            static_cast<unsigned>(st.residual_csum), static_cast<unsigned>(st.recon_sig),
            static_cast<unsigned>(st.recon_dbg),
            st.sps_width, st.sps_height, st.stream_nalus);
        misterplex::FpgaSpi::DdrDoorbellStatus tok;
        if (spi.readDdrDoorbellStatus(tok)) {
            std::printf(" frame_bank=%d frame_format=yuv420p frame_seq=%u",
                        tok.bank, tok.seq);
        }
        if (haveFrameStoreStatus) {
            std::printf(" frame_debug=0x%02x frame_underrun=%u frame_status_seq=%u",
                        static_cast<unsigned>(fs.debug_state),
                        static_cast<unsigned>(fs.underrun_count),
                        static_cast<unsigned>(fs.seq));
        } else {
            std::printf(" frame_status=absent");
        }
        std::printf("\n");
        if (haveFrameStoreStatus && fs.nonYuvDoorbellRejected())
            std::printf("ERROR %s\n", misterplex::frameStoreDebugDescription(fs.debug_state));
        else if (!haveFrameStoreStatus)
            std::printf("ERROR %s: %s\n",
                        misterplex::frameStoreStatusUnavailableDescription(),
                        spi.lastError().c_str());
    }
    return 0;
}
