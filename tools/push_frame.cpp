// Push YUV420p video via DDR, or non-video PCM/annex-B via SPI ioctl,
// or dump core status. Exits 3 without touching the FPGA unless the core gate verifies
// a Plex build and misterplexd is not running (core_gate_tool.hpp).
// Usage:
//   push_frame --ddr [--bank N] [--yuv420p WxH] file
//   push_frame --index 2|3 file
//   push_frame --status
//   push_frame --raw
//   push_frame --set-bit N 0|1
//   push_frame --pulse N
#include "core_gate_tool.hpp"
#include "fpga_spi.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    uint8_t index = 1;
    int rgb24w = 0, rgb24h = 0;
    int yuvW = 0, yuvH = 0;
    bool do_status = false;
    bool do_raw = false;
    bool use_ddr = false;
    int bank = 0;
    int set_bit = -1, set_val = 0;
    int pulse_bit = -1;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--index") == 0 && i + 1 < argc)
            index = static_cast<uint8_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--rgb24") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &rgb24w, &rgb24h);
        } else if (std::strcmp(argv[i], "--yuv420p") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &yuvW, &yuvH);
        } else if (std::strcmp(argv[i], "--status") == 0) {
            do_status = true;
        } else if (std::strcmp(argv[i], "--raw") == 0) {
            do_raw = true;
        } else if (std::strcmp(argv[i], "--set-bit") == 0 && i + 2 < argc) {
            set_bit = std::atoi(argv[++i]);
            set_val = std::atoi(argv[++i]) ? 1 : 0;
        } else if (std::strcmp(argv[i], "--pulse") == 0 && i + 1 < argc) {
            pulse_bit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--ddr") == 0) {
            use_ddr = true;
        } else if (std::strcmp(argv[i], "--bank") == 0 && i + 1 < argc) {
            bank = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-')
            path = argv[i];
    }

    if (path && use_ddr && (rgb24w > 0 || rgb24h > 0)) {
        std::fprintf(stderr,
                     "non-YUV frame send refused: DDR frame-store path is YUV420p only; "
                     "--rgb24 is disabled for F1\n");
        return 1;
    }
    if (path && !use_ddr && (index == 1 || rgb24w > 0 || rgb24h > 0)) {
        std::fprintf(stderr,
                     "non-YUV frame send refused: F1 frame-store path is DDR YUV420p only; "
                     "use --ddr --yuv420p WxH\n");
        return 1;
    }

    misterplex::FpgaSpi spi;
    if (!spi.open()) {
        std::fprintf(stderr, "fpga open: %s\n", spi.lastError().c_str());
        return 1;
    }
    // Core gate (core_gate_tool.hpp): only a verified Plex build, never under misterplexd.
    if (!misterplex::coreGateVerifyForTool(spi, "push_frame"))
        return 3;

    if (set_bit >= 0) {
        if (!spi.setStatusBit(set_bit, set_val)) {
            std::fprintf(stderr, "set-bit: %s\n", spi.lastError().c_str());
            return 1;
        }
        std::printf("set bit %d = %d\n", set_bit, set_val);
        usleep(50000);
    }
    if (pulse_bit >= 0) {
        if (!spi.setStatusBit(pulse_bit, 1)) {
            std::fprintf(stderr, "pulse: %s\n", spi.lastError().c_str());
            return 1;
        }
        usleep(5000);
        if (!spi.setStatusBit(pulse_bit, 0)) {
            std::fprintf(stderr, "pulse low: %s\n", spi.lastError().c_str());
            return 1;
        }
        std::printf("pulsed bit %d\n", pulse_bit);
        usleep(50000);
    }

    if (do_raw) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            uint8_t raw[16]{};
            if (!spi.getCoreStatus(raw)) {
                std::fprintf(stderr, "raw: %s\n", spi.lastError().c_str());
                return 1;
            }
            std::printf("raw[%d]:", attempt);
            for (int i = 0; i < 16; ++i)
                std::printf(" %02x", raw[i]);
            std::printf("  lo=0x%04x\n", raw[0] | (raw[1] << 8));
            usleep(20000);
        }
        return 0;
    }

    if (do_status || set_bit >= 0 || pulse_bit >= 0) {
        misterplex::FpgaSpi::CoreStatus st;
        if (!spi.readCoreStatus(st)) {
            std::fprintf(stderr, "status: %s\n", spi.lastError().c_str());
            return 1;
        }
        misterplex::FrameStoreStatus fs{};
        const bool haveFrameStoreStatus = spi.readFrameStoreStatus(fs);
        std::printf(
            "status has_frame=%d has_audio=%d has_stream=%d underrun=%d "
            "has_idr=%d stub_busy=%d sps_valid=%d pps_valid=%d nalu=%u last_nal=0x%02x "
            "slice_type=%u mb0=%u qp=%u res_ok=%d res_tc=%u res_t1=%u res_dc=%d res_csum=%u "
            "recon_sig=%u recon_dbg=0x%02x ddr_busy=%d sps=%ux%u "
            "stream_nalus=%u bytes_in_unavailable=1",
            st.has_frame ? 1 : 0, st.has_audio ? 1 : 0, st.has_stream ? 1 : 0,
            st.audio_underrun ? 1 : 0, st.has_idr ? 1 : 0, st.stub_busy ? 1 : 0,
            st.sps_valid ? 1 : 0, st.pps_valid ? 1 : 0, st.nalu_count, st.last_nal_type,
            st.slice_type, st.first_mb_type, st.slice_qp, st.residual_ok ? 1 : 0, st.residual_tc,
            st.residual_t1, static_cast<int>(st.residual_dc),
            static_cast<unsigned>(st.residual_csum), static_cast<unsigned>(st.recon_sig),
            static_cast<unsigned>(st.recon_dbg),
            st.ddr_busy ? 1 : 0, st.sps_width, st.sps_height, st.stream_nalus);
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
        if (!path)
            return 0;
    }

    if (!path) {
        std::fprintf(stderr,
                     "usage: push_frame --ddr [--bank 0|1] [--yuv420p 624x480] file\n"
                     "       push_frame --index 2|3 file\n"
                     "       push_frame --status\n");
        return 1;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::perror(path);
        return 1;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), {});
    bool ok = false;
    if (use_ddr) {
        if (rgb24w > 0 || rgb24h > 0) {
            std::fprintf(stderr,
                         "non-YUV frame send refused: DDR frame-store path is YUV420p only; "
                         "--rgb24 is disabled for F1\n");
            return 1;
        }
        misterplex::DdrFrameGeometry g{};
        if (yuvW > 0 && yuvH > 0) {
            g = misterplex::makeDdrFrameGeometry(yuvW, yuvH);
        } else if (buf.size() == static_cast<size_t>(misterplex::kPlex480pYuv420pBytes)) {
            g = misterplex::plex480pDdrFrameGeometry();
        } else {
            std::fprintf(stderr,
                         "DDR frame-store path is YUV420p only; pass --yuv420p WxH or a "
                         "%d-byte Plex 480p I420 frame\n",
                         misterplex::kPlex480pYuv420pBytes);
            return 1;
        }
        const size_t want = misterplex::yuv420pFrameBytes(g.coded_width, g.coded_height);
        if (buf.size() < want) {
            std::fprintf(stderr, "file too small for %dx%d YUV420p\n", g.coded_width,
                         g.coded_height);
            return 1;
        }
        ok = spi.sendYuv420pFrameDdr(buf.data(), want, g, bank);
    } else if (index == 1 || rgb24w > 0 || rgb24h > 0) {
        std::fprintf(stderr,
                     "non-YUV frame send refused: F1 frame-store path is DDR YUV420p only; "
                     "use --ddr --yuv420p WxH\n");
        return 1;
    } else {
        ok = spi.sendFileTx(buf.data(), buf.size(), index);
    }
    if (!ok) {
        std::fprintf(stderr, "push failed: %s\n", spi.lastError().c_str());
        return 1;
    }
    // Ensure Force-bars debug is off (status[9]=0) so auto frame present shows
    if (!use_ddr)
        spi.setStatusBit(9, 0);
    const char* via = use_ddr ? "DDR" : "SPI";
    std::printf("pushed %zu bytes %s bank=%d index=%u OK (%.1f ms)\n", buf.size(), via, bank,
                index, spi.lastPushMs());
    return 0;
}
