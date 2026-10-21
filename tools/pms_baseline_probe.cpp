#include "misterplexd/plex_resolve.hpp"
#include "libmisterplex/h264_nal.hpp"
#include "libmisterplex/h264_sps.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string envOrEmpty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::string runCommandBytes(const std::string& cmd, size_t maxBytes) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return {};
    std::string data;
    data.reserve(maxBytes < (4u << 20) ? maxBytes : (4u << 20));
    char buf[32768];
    while (data.size() < maxBytes) {
        const size_t want = std::min(sizeof(buf), maxBytes - data.size());
        const size_t n = std::fread(buf, 1, want, pipe);
        if (n > 0)
            data.append(buf, n);
        if (n < want)
            break;
    }
    pclose(pipe);
    return data;
}

std::string readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::string ffmpegExtractH264(const std::string& url, const std::string& headers, int seconds) {
    // The PMS response is MPEG-TS. Extracting the video elementary stream avoids
    // false Annex-B start-code matches in TS packet metadata or unrelated payload.
    std::ostringstream cmd;
    cmd << "ffmpeg -hide_banner -loglevel error -nostdin"
        << " -headers " << shellQuote(headers)
        << " -i " << shellQuote(url)
        << " -map 0:v:0 -t " << seconds
        << " -c:v copy -an -f h264 - 2>/dev/null";
    return runCommandBytes(cmd.str(), 16u << 20);
}

bool parseSizeSpec(const std::string& spec, int& w, int& h) {
    char x = 0;
    std::istringstream in(spec);
    return (in >> w >> x >> h) && x == 'x' && w > 0 && h > 0 && in.eof();
}

struct Expectations {
    int profile_idc = 66;
    int entropy_cabac = 0;
    int max_num_ref_frames = 1;
    int b_slices = 0;
    int coded_width = 624;
    int coded_height = 480;
    int display_width = 618;
    int display_height = 480;
    bool check_geometry = true;
};

struct SpsProbe {
    bool valid = false;
    uint8_t profile_idc = 0;
    uint8_t level_idc = 0;
    uint32_t max_num_ref_frames = 0;
    uint32_t coded_width = 0;
    uint32_t coded_height = 0;
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    bool crop_flag = false;
    uint32_t crop_left = 0;
    uint32_t crop_right = 0;
    uint32_t crop_top = 0;
    uint32_t crop_bottom = 0;
    uint32_t crop_unit_x = 2;
    uint32_t crop_unit_y = 2;
};

SpsProbe parseSpsProbe(const uint8_t* payload, size_t len) {
    SpsProbe out;
    if (!payload || len < 4)
        return out;
    auto rbsp = misterplex::detail::removeEpb(payload, len);
    misterplex::detail::BitReader br(rbsp.data(), rbsp.size());

    out.profile_idc = static_cast<uint8_t>(br.u(8));
    br.u(8); // constraint flags
    out.level_idc = static_cast<uint8_t>(br.u(8));
    br.ue(); // seq_parameter_set_id

    uint32_t chroma = 1;
    const uint8_t p = out.profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 ||
        p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        chroma = br.ue();
        if (chroma == 3)
            br.u(1); // separate_colour_plane_flag
        br.ue();     // bit_depth_luma_minus8
        br.ue();     // bit_depth_chroma_minus8
        br.u(1);     // qpprime_y_zero_transform_bypass_flag
        if (br.u(1)) {
            int n = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < n && br.ok; ++i) {
                if (br.u(1)) {
                    int last = 8, next = 8;
                    int size = (i < 6) ? 16 : 64;
                    for (int j = 0; j < size && br.ok; ++j) {
                        if (next != 0)
                            next = (last + br.se() + 256) % 256;
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }

    br.ue(); // log2_max_frame_num_minus4
    uint32_t poc = br.ue();
    if (poc == 0) {
        br.ue();
    } else if (poc == 1) {
        br.u(1);
        br.se();
        br.se();
        uint32_t n = br.ue();
        for (uint32_t i = 0; i < n && br.ok; ++i)
            br.se();
    }
    out.max_num_ref_frames = br.ue();
    br.u(1); // gaps_in_frame_num_value_allowed_flag
    uint32_t w_mbs = br.ue() + 1;
    uint32_t h_map = br.ue() + 1;
    uint32_t frame_mbs_only = br.u(1);
    if (!frame_mbs_only)
        br.u(1);
    br.u(1); // direct_8x8_inference_flag
    out.crop_flag = br.u(1) != 0;
    if (out.crop_flag) {
        out.crop_left = br.ue();
        out.crop_right = br.ue();
        out.crop_top = br.ue();
        out.crop_bottom = br.ue();
    }
    out.coded_width = w_mbs * 16;
    out.coded_height = h_map * 16 * (frame_mbs_only ? 1u : 2u);
    if (chroma == 0) {
        out.crop_unit_x = 1;
        out.crop_unit_y = 2 - frame_mbs_only;
    } else if (chroma == 3) {
        out.crop_unit_x = 1;
        out.crop_unit_y = 2 - frame_mbs_only;
    } else {
        out.crop_unit_x = 2;
        out.crop_unit_y = 2 * (2 - frame_mbs_only);
    }
    out.display_width = out.coded_width - (out.crop_left + out.crop_right) * out.crop_unit_x;
    out.display_height = out.coded_height - (out.crop_top + out.crop_bottom) * out.crop_unit_y;
    out.valid = br.ok && out.coded_width > 0 && out.coded_height > 0 && out.display_width > 0 &&
                out.display_height > 0;
    return out;
}

int sliceClassFromType(uint32_t sliceType) {
    return static_cast<int>(sliceType % 5); // 0=P, 1=B, 2=I, 3=SP, 4=SI
}

bool parseSliceClassOnly(const uint8_t* payload, size_t len, int& cls) {
    if (!payload || len < 1)
        return false;
    auto rbsp = misterplex::detail::removeEpb(payload, len);
    misterplex::detail::BitReader br(rbsp.data(), rbsp.size());
    br.ue();
    uint32_t sliceType = br.ue();
    if (!br.ok)
        return false;
    cls = sliceClassFromType(sliceType);
    return true;
}

struct StreamProbe {
    SpsProbe sps;
    bool pps_seen = false;
    misterplex::PpsInfo pps;
    int vcl = 0;
    int idr = 0;
    int nonidr = 0;
    int i = 0;
    int p = 0;
    int b = 0;
    int other = 0;
};

StreamProbe probeAnnexBInBytes(const std::string& data) {
    StreamProbe out;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    const size_t n = data.size();
    size_t pos = 0;
    while (pos + 3 < n) {
        size_t sc = 0;
        if (pos + 4 <= n && bytes[pos] == 0 && bytes[pos + 1] == 0 && bytes[pos + 2] == 0 &&
            bytes[pos + 3] == 1)
            sc = 4;
        else if (bytes[pos] == 0 && bytes[pos + 1] == 0 && bytes[pos + 2] == 1)
            sc = 3;
        else {
            ++pos;
            continue;
        }
        size_t nal = pos + sc;
        size_t next = nal;
        while (next + 3 < n) {
            if (bytes[next] == 0 && bytes[next + 1] == 0 &&
                (bytes[next + 2] == 1 || (next + 3 < n && bytes[next + 2] == 0 && bytes[next + 3] == 1)))
                break;
            ++next;
        }
        if (next + 3 >= n)
            next = n;
        if (nal < next) {
            const uint8_t hdr = bytes[nal];
            const uint8_t type = hdr & 0x1f;
            const uint8_t* payload = bytes + nal + 1;
            const size_t plen = next > nal + 1 ? next - nal - 1 : 0;
            if (type == 7 && !out.sps.valid) {
                out.sps = parseSpsProbe(payload, plen);
            } else if (type == 8 && !out.pps_seen) {
                out.pps_seen = true;
                out.pps = misterplex::parsePpsRbsp(payload, plen);
            } else if (type == 1 || type == 5) {
                ++out.vcl;
                if (type == 5)
                    ++out.idr;
                else
                    ++out.nonidr;
                int cls = -1;
                if (parseSliceClassOnly(payload, plen, cls)) {
                    if (cls == 0)
                        ++out.p;
                    else if (cls == 1)
                        ++out.b;
                    else if (cls == 2)
                        ++out.i;
                    else
                        ++out.other;
                } else {
                    ++out.other;
                }
            }
        }
        pos = next;
    }
    return out;
}

int usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " --base URL --token TOKEN --key /library/metadata/N [--seconds N]\n"
              << "       " << argv0 << " --annexb delivered.264\n"
              << "       Env equivalents: PLEX_BASE, PLEX_TOKEN, MISTERPLEX_BASELINE_KEY\n"
              << "       Defaults enforce profile_idc=66, entropy_cabac=0, max_num_ref_frames=1,\n"
              << "       B-slices=0, coded=624x480, display=618x480.\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string base = envOrEmpty("PLEX_BASE");
    std::string token = envOrEmpty("PLEX_TOKEN");
    std::string key = envOrEmpty("MISTERPLEX_BASELINE_KEY");
    std::string annexbPath;
    int seconds = 14;
    Expectations expect;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto needValue = [&](std::string& out) -> bool {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        };
        if (a == "--base") {
            if (!needValue(base))
                return usage(argv[0]);
        } else if (a == "--token") {
            if (!needValue(token))
                return usage(argv[0]);
        } else if (a == "--key") {
            if (!needValue(key))
                return usage(argv[0]);
        } else if (a == "--seconds") {
            std::string v;
            if (!needValue(v))
                return usage(argv[0]);
            seconds = std::atoi(v.c_str());
        } else if (a == "--annexb") {
            if (!needValue(annexbPath))
                return usage(argv[0]);
        } else if (a == "--expect-profile") {
            std::string v;
            if (!needValue(v))
                return usage(argv[0]);
            expect.profile_idc = std::atoi(v.c_str());
        } else if (a == "--expect-cabac") {
            std::string v;
            if (!needValue(v))
                return usage(argv[0]);
            expect.entropy_cabac = std::atoi(v.c_str());
        } else if (a == "--expect-max-ref" || a == "--expect-max-num-ref-frames") {
            std::string v;
            if (!needValue(v))
                return usage(argv[0]);
            expect.max_num_ref_frames = std::atoi(v.c_str());
        } else if (a == "--expect-b-slices") {
            std::string v;
            if (!needValue(v))
                return usage(argv[0]);
            expect.b_slices = std::atoi(v.c_str());
        } else if (a == "--expect-coded") {
            std::string v;
            if (!needValue(v) || !parseSizeSpec(v, expect.coded_width, expect.coded_height))
                return usage(argv[0]);
        } else if (a == "--expect-display") {
            std::string v;
            if (!needValue(v) || !parseSizeSpec(v, expect.display_width, expect.display_height))
                return usage(argv[0]);
        } else if (a == "--no-geometry-check") {
            expect.check_geometry = false;
        } else if (a == "--help" || a == "-h") {
            return usage(argv[0]);
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return usage(argv[0]);
        }
    }

    std::string body;
    std::string sourceDesc;
    if (!annexbPath.empty()) {
        body = readFileBytes(annexbPath);
        sourceDesc = "annexb=" + annexbPath;
    } else {
        base = misterplex::normalizePlexBase(base);
        if (base.empty() || token.empty() || key.empty()) {
            std::cerr << "SKIP-NOT-PASS pms_baseline_profile: live PMS inputs missing; set PLEX_BASE, "
                         "PLEX_TOKEN, and MISTERPLEX_BASELINE_KEY. This is not a pass.\n";
            return 77;
        }
        if (seconds < 4)
            seconds = 4;
        if (seconds > 30)
            seconds = 30;

        misterplex::WeakLadder weak;
        if (!misterplex::applyPlexTranscodeProfile("480p", weak)) {
            std::cerr << "FAIL pms_baseline_profile: built-in 480p transcode profile missing\n";
            return 1;
        }
        const std::string session = "mplex-baseline-check";
        const std::string startUrl =
            misterplex::buildUniversalTranscodeUrl(base, key, token, session, 0, weak);

        if (!misterplex::ensureUniversalDecision(startUrl, session, token, weak)) {
            std::cerr << "FAIL pms_baseline_profile: PMS universal decision request failed before stream fetch\n";
            return 1;
        }

        const std::string headerBlock = misterplex::plexFfmpegHeaders(session, token, weak);
        body = ffmpegExtractH264(startUrl, headerBlock, seconds);
        sourceDesc = "live_pms=" + base + " key=" + key;
    }
    if (body.empty()) {
        std::cerr << "FAIL pms_baseline_profile: no H.264 bytes from " << sourceDesc
                  << "; check PMS reachability/token or Annex-B path\n";
        return 1;
    }

    const StreamProbe probe = probeAnnexBInBytes(body);
    if (!probe.sps.valid || !probe.pps_seen) {
        std::cerr << "FAIL pms_baseline_profile: could not find delivered H.264 SPS/PPS in PMS transcode "
                     "response; bytes=" << body.size() << "\n";
        return 1;
    }

    std::cout << "PMS_BASELINE_SOURCE " << sourceDesc << "\n";
    std::cout << "PMS_BASELINE_DELIVERED profile_idc=" << static_cast<unsigned>(probe.sps.profile_idc)
              << " level_idc=" << static_cast<unsigned>(probe.sps.level_idc)
              << " pps_valid=" << (probe.pps.valid ? 1 : 0)
              << " entropy_cabac=" << (probe.pps.entropy_cabac ? 1 : 0)
              << " max_num_ref_frames=" << probe.sps.max_num_ref_frames
              << " coded=" << probe.sps.coded_width << "x" << probe.sps.coded_height
              << " display=" << probe.sps.display_width << "x" << probe.sps.display_height
              << " crop_flag=" << (probe.sps.crop_flag ? 1 : 0)
              << " crop_lrtb=" << probe.sps.crop_left << "," << probe.sps.crop_right << ","
              << probe.sps.crop_top << "," << probe.sps.crop_bottom
              << " crop_unit=" << probe.sps.crop_unit_x << "x" << probe.sps.crop_unit_y << "\n";
    std::cout << "PMS_BASELINE_SLICES vcl=" << probe.vcl << " idr=" << probe.idr
              << " nonidr=" << probe.nonidr << " i=" << probe.i << " p=" << probe.p
              << " b=" << probe.b << " other=" << probe.other << " bytes=" << body.size() << "\n";
    std::cout.flush();

    std::vector<std::string> problems;
    auto addProblem = [&](const std::string& p) { problems.push_back(p); };
    if (probe.sps.profile_idc != expect.profile_idc) {
        std::ostringstream p;
        p << "profile_idc=" << static_cast<unsigned>(probe.sps.profile_idc) << ", expected "
          << expect.profile_idc;
        addProblem(p.str());
    }
    if ((probe.pps.entropy_cabac ? 1 : 0) != expect.entropy_cabac) {
        std::ostringstream p;
        p << "entropy_cabac=" << (probe.pps.entropy_cabac ? 1 : 0) << ", expected "
          << expect.entropy_cabac;
        addProblem(p.str());
    }
    if (expect.entropy_cabac == 0 && !probe.pps.valid)
        addProblem("pps_valid=0 for expected CAVLC stream");
    if (static_cast<int>(probe.sps.max_num_ref_frames) != expect.max_num_ref_frames) {
        std::ostringstream p;
        p << "max_num_ref_frames=" << probe.sps.max_num_ref_frames << ", expected "
          << expect.max_num_ref_frames;
        addProblem(p.str());
    }
    if (probe.b != expect.b_slices) {
        std::ostringstream p;
        p << "b_slices=" << probe.b << ", expected " << expect.b_slices;
        addProblem(p.str());
    }
    if (expect.check_geometry) {
        if (static_cast<int>(probe.sps.coded_width) != expect.coded_width ||
            static_cast<int>(probe.sps.coded_height) != expect.coded_height) {
            std::ostringstream p;
            p << "coded=" << probe.sps.coded_width << "x" << probe.sps.coded_height
              << ", expected " << expect.coded_width << "x" << expect.coded_height
              << " (39x30 macroblocks; do not silently assume 640)";
            addProblem(p.str());
        }
        if (static_cast<int>(probe.sps.display_width) != expect.display_width ||
            static_cast<int>(probe.sps.display_height) != expect.display_height) {
            std::ostringstream p;
            p << "display=" << probe.sps.display_width << "x" << probe.sps.display_height
              << ", expected " << expect.display_width << "x" << expect.display_height
              << " (640x480 presentation uses 11px pillars)";
            addProblem(p.str());
        }
    }
    if (!problems.empty()) {
        std::cerr << "FAIL pms_baseline_profile: delivered stream violates MiSTerPlex 480p "
                     "FPGA contract:";
        for (const auto& p : problems)
            std::cerr << " " << p << ";";
        std::cerr << " is MiSTerPlex.xml still installed in the PMS Profiles directory and has "
                     "the plex container been restarted? See docs/pms-baseline-profile.md.\n";
        return 1;
    }

    std::cout << "test_pms_baseline_profile: OK delivered Baseline/CAVLC/ref=1/no-B 624x480 stream\n";
    return 0;
}
