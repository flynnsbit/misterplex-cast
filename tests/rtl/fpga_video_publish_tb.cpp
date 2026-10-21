#include "Vfpga_video_publish_tb.h"
#include "verilated.h"
#include "ddr_bitstream_ring.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ring = misterplex::ddr_bitstream_ring;
static void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
constexpr uint32_t feedback = mailbox_abi::kVideoPresentationAddr / 8;
constexpr uint32_t commit = mailbox_abi::kVideoPresentationCommitAddr / 8;
constexpr uint64_t nonce1 = UINT64_C(0x8877665544332211);
constexpr uint64_t epoch1 = UINT64_C(0xfedcba9876543210);
#ifndef FULL_AU
#define FULL_AU 0
#endif
#ifndef FULL_AU_ORIGINAL_TIMING
#define FULL_AU_ORIGINAL_TIMING 0
#endif
#ifndef FULL_AU_RUNTIME_GEOMETRY
#define FULL_AU_RUNTIME_GEOMETRY 0
#endif
#ifndef FULL_AU_NATIVE_BEAM
#define FULL_AU_NATIVE_BEAM 0
#endif
#ifndef FULL_AU_SCANDOUBLE
#define FULL_AU_SCANDOUBLE 1
#endif

struct Sim {
    Vfpga_video_publish_tb d;
    std::unordered_map<uint32_t, uint64_t> memory;
    struct Response { uint64_t due, data; };
    std::deque<Response> responses;
    std::vector<std::pair<uint32_t,uint64_t>> status_writes;
    std::vector<uint8_t> captured_sps, captured_pps;
    uint64_t time = 0, ddr_cycles = 0;
    uint64_t vsync_period = 128;
    uint64_t displayed_cycle = 0;
    uint16_t previous_frames_done = 0;
    unsigned image = 0;
    unsigned verified_frames = 0;
    std::vector<uint32_t> native_pixels[2];
    std::vector<unsigned> native_lines[2];
    unsigned native_line_length = 0;
    bool native_complete[2]{};
    uint64_t native_requests[2]{}, native_responses[2]{};
    bool lease_held = false;
    uint32_t lease_base = 0;
    bool force_busy = false, vsync_enabled = false, force_vsync = false;
    bool hold_responses = false;
    bool pps_mismatch = false, bad_tail = false, negative_guard = false;
    bool watch_write = false;
    uint32_t watched_address = 0;
    uint64_t watched_data = 0;
    unsigned watched_writes = 0;
    uint64_t load(uint32_t address) { return memory[address]; }
    uint8_t native(uint32_t address) const {
        if (image >= 2) return address < 76800 ? (image == 2 ? 16 : 235) : 128;
        if (address < 76800) return uint8_t((address + image * 29) % 220 + 16);
        if (address < 96000) return uint8_t(96 + image);
        return uint8_t(160 - image);
    }
    static uint32_t rgb601(uint8_t y, uint8_t u, uint8_t v) {
        const int c = int(y) - 16, d = int(u) - 128, e = int(v) - 128;
        auto channel = [](int value) { return uint32_t(std::clamp(value >> 8, 0, 255)); };
        return channel(298 * c + 409 * e + 128) << 16 |
               channel(298 * c - 100 * d - 208 * e + 128) << 8 |
               channel(298 * c + 516 * d + 128);
    }
    void check_pixel(unsigned x, unsigned y, uint8_t luma, uint8_t u, uint8_t v) {
        d.sample_x = x;
        d.sample_y = y;
        const uint32_t expected = rgb601(luma, u, v);
        until([&] { return d.pixel_rgb == expected; }, "actual frame-store RGB conversion mismatch");
        run(26 * 16);
        check(d.pixel_rgb == expected, "frame-store RGB did not remain stable");
    }
    void tick() {
        ++time;
        const bool sys_rise = time % 26 == 13;
        const bool ddr_rise = time % 6 == 3;
        bool delivered = false;
        if (ddr_rise) {
            ++ddr_cycles;
            d.DDRAM_BUSY = force_busy || ddr_cycles % 29 < 7;
            delivered = !hold_responses && !responses.empty() &&
                        responses.front().due <= ddr_cycles;
            d.DDRAM_DOUT_READY = delivered;
            d.DDRAM_DOUT = delivered ? responses.front().data : 0;
        }
        if (sys_rise) d.vsync = force_vsync ||
                              (vsync_enabled && (time / 26) % vsync_period == 0);
        d.eval();
        if (FULL_AU && sys_rise) {
            if (d.native_transactions & 16) {
                lease_held = false;
                for (unsigned client = 0; client < 2; ++client)
                    native_requests[client] = native_responses[client] = 0;
            } else {
                if (d.decoded_picture_valid && d.frame_ready)
                    check(d.native_lease_pending && d.legacy_rgb_frames == d.count,
                          "native publication still depends on legacy RGB completion");
                if (lease_held)
                    check(d.native_lease_pending && d.native_lease_base == lease_base,
                          "native picture lease/bank lost before publisher retirement");
                if (d.native_lease_release)
                    check(d.native_lease_pending && d.publish_state == 0 &&
                          d.count != 0 && !(d.native_transactions & 12),
                          "native picture released before display/publication/read retirement");
                lease_held = d.native_lease_pending && !d.native_lease_release;
                lease_base = d.native_lease_base;
                check((d.native_transactions & 10) != 10, "native response delivered to both clients");
                for (unsigned client = 0; client < 2; ++client) {
                    native_requests[client] += (d.native_transactions >> (client * 2)) & 1;
                    native_responses[client] += (d.native_transactions >> (client * 2 + 1)) & 1;
                    check(native_responses[client] <= native_requests[client] &&
                          native_requests[client] - native_responses[client] <= 1,
                          "native read acceptance/response ownership mismatch");
                }
            }
            if (d.header_capture & (1u << 21)) captured_sps.clear();
            else if (d.header_capture & (1u << 20))
                captured_sps.push_back(uint8_t(d.header_capture >> 8));
            if (d.header_capture & (1u << 18)) captured_pps.clear();
            else if (d.header_capture & (1u << 17))
                captured_pps.push_back(uint8_t(d.header_capture));
        }
        if (FULL_AU && sys_rise && d.observed_vcl)
            check(d.next_vcl_ready, "new VCL arrived before decoder/publication ownership release");
        bool ram = sys_rise && d.mem_rd && !FULL_AU;
        uint8_t ram_data = ram ? native(d.mem_addr) : 0;
        if (ddr_rise) {
            if (delivered) responses.pop_front();
            if (!d.DDRAM_BUSY && d.DDRAM_RD) {
                check(responses.empty(), "overlapping DDR read ownership");
                for (unsigned i = 0; i < d.DDRAM_BURSTCNT; ++i)
                    responses.push_back({ddr_cycles + 3 + i, load(d.DDRAM_ADDR + i)});
            }
            if (!d.DDRAM_BUSY && d.DDRAM_WE) {
                if (watch_write && d.DDRAM_ADDR == watched_address &&
                    d.DDRAM_DIN == watched_data) ++watched_writes;
                auto& value = memory[d.DDRAM_ADDR];
                for (unsigned i = 0; i < 8; ++i)
                    if (d.DDRAM_BE & (1u << i)) {
                        const uint64_t mask = UINT64_C(0xff) << (i * 8);
                        value = (value & ~mask) | (uint64_t(d.DDRAM_DIN) & mask);
                    }
                if (d.DDRAM_ADDR >= feedback && d.DDRAM_ADDR <= commit)
                    status_writes.emplace_back(d.DDRAM_ADDR, value);
            }
        }
        d.clk = time % 26 >= 13;
        d.clk_ddr = time % 6 >= 3;
        d.eval();
        if (sys_rise) {
            if (negative_guard)
                check(!d.native_reference_ready && !d.decoded_picture_valid &&
                      !d.has_frame && !d.swap_pending && d.count == 0 && load(commit) == 0,
                      "rejected AU transiently completed DPB/source/display/MVPS");
            if (FULL_AU_NATIVE_BEAM && !d.reset && d.frames_done > 0 && d.frames_done <= 2) {
                const unsigned frame = d.frames_done - 1;
                if (d.native_frame_start) native_complete[frame] = true;
                if (!native_complete[frame] && d.native_ce) {
                    if (d.pixel_de) {
                        native_pixels[frame].push_back(d.pixel_rgb);
                        ++native_line_length;
                    } else if (native_line_length) {
                        native_lines[frame].push_back(native_line_length);
                        native_line_length = 0;
                    }
                }
            }
            if (!d.reset && d.frames_done == uint16_t(previous_frames_done + 1))
                displayed_cycle = time / 26;
            previous_frames_done = d.frames_done;
            d.mem_valid = ram;
            d.mem_data = ram_data;
        }
    }
    void run(uint64_t ticks) { while (ticks--) tick(); }
    template<class Predicate> void until(Predicate done, const char* why) {
        for (uint64_t i = 0; i < (FULL_AU ? UINT64_C(520000000) : UINT64_C(26000000)); ++i) {
            d.eval();
            if (done()) return;
            if (FULL_AU && ((load(ring::kStat6Phys / 8) >>
                (ring::kStat6StateFlagsShift + ring::kStat6FatalFlagBit)) & 1)) {
                std::cerr << "reader fatal STAT6=0x" << std::hex
                          << load(ring::kStat6Phys / 8) << std::dec
                          << " read=" << d.stream_read_count << '\n';
                throw std::runtime_error("actual AU reader rejected transport");
            }
            if (FULL_AU && d.decoder_error) {
                std::cerr << "decoder error=" << unsigned(d.decoder_error)
                          << " phase=" << unsigned(d.decoder_phase)
                          << " MB=" << d.decoder_mb << " bit_pos=" << d.decoder_bit_pos
                          << " sys_cycles=" << time / 26
                          << " verified_frames=" << verified_frames
                          << " presented=" << d.count
                          << " dpb_reads=" << native_requests[0] << "/" << native_responses[0]
                          << " parameter_flags=0x" << std::hex << unsigned(d.parameter_flags)
                          << " parameter_values=0x" << d.parameter_values << std::dec
                          << " geometry=" << (d.coded_geometry & 0xffff) << "x"
                          << (d.coded_geometry >> 16)
                          << " header_state=" << unsigned(d.header_state)
                          << " header_cursor=" << d.header_cursor << '\n';
                for (const auto& capture : {std::make_pair("SPS", &captured_sps),
                                            std::make_pair("PPS", &captured_pps)}) {
                    std::cerr << "actual " << capture.first << " RBSP=" << std::hex;
                    for (auto byte : *capture.second)
                        std::cerr << std::setfill('0') << std::setw(2) << unsigned(byte);
                    std::cerr << std::dec << std::setfill(' ') << '\n';
                }
                throw std::runtime_error("actual AU decoder aborted");
            }
            tick();
        }
        std::cerr << "state=" << unsigned(d.publish_state) << " count=" << d.count
                  << " frames_done=" << d.frames_done << " has_frame=" << unsigned(d.has_frame)
                  << " pending=" << unsigned(d.swap_pending)
                  << " ready=" << unsigned(d.pending_ready)
                  << " decoder_phase=" << unsigned(d.decoder_phase)
                  << " MB=" << d.decoder_mb << " bit_pos=" << d.decoder_bit_pos
                  << " reader=" << d.stream_read_count
                  << " error=" << unsigned(d.error) << " responses=" << responses.size() << '\n';
        throw std::runtime_error(why);
    }
    std::array<uint64_t,9> status() {
        std::array<uint64_t,9> out{};
        for (unsigned i = 0; i < 9; ++i) out[i] = load(feedback + i);
        return out;
    }
    void begin_frame(uint64_t epoch, unsigned seq, int64_t pts) {
        until([&] { return d.frame_ready; }, "publisher not ready");
        d.session_id = epoch;
        d.seq = seq;
        d.pts = uint64_t(pts);
        d.tb_num = 1001;
        d.tb_den = 24000;
        d.frame_valid = 1;
        do { tick(); } while (time % 26 != 13);
        d.frame_valid = 0;
    }
};

using Bytes = std::vector<uint8_t>;
static Bytes read_bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    check(bool(file), "missing composed-test input");
    return Bytes(std::istreambuf_iterator<char>(file), {});
}

static void decoded_access_units(Sim& s, const std::string& directory,
                                 const std::string& actual_path) {
    uint32_t producer = 0;
    auto publish = [&] {
        s.memory[ring::kCtrlPhys / 8] = UINT64_C(1) << 63 |
            uint64_t(producer) << 32 | ring::kCtrlMagic;
    };
    auto append = [&](ring::Event event, uint64_t epoch, uint32_t seq, const Bytes& payload) {
        Bytes bytes(ring::kRecordHeaderBytes + payload.size());
        ring::putLe32(bytes.data(), ring::kRecordMagic);
        bytes[4] = uint8_t(event);
        bytes[6] = 2;
        ring::putLe64(bytes.data() + 8, epoch);
        ring::putLe32(bytes.data() + 16, seq);
        ring::putLe32(bytes.data() + 20, uint32_t(payload.size()));
        std::copy(payload.begin(), payload.end(), bytes.begin() + ring::kRecordHeaderBytes);
        for (uint8_t byte : bytes) {
            const uint32_t offset = producer & (ring::kRingBytes - 1);
            auto& word = s.memory[(ring::kDataPhys + offset) / 8];
            const unsigned shift = 8 * (offset & 7);
            word = (word & ~(UINT64_C(255) << shift)) | uint64_t(byte) << shift;
            producer = (producer + 1) & ring::kCountMask;
        }
        publish();
    };
    const auto reference = read_bytes(directory + "/reference.yuv");
    const auto keyframes = read_bytes(directory + "/keyframes.bin");
    unsigned coded_w = 320, coded_h = 240, crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (FULL_AU_RUNTIME_GEOMETRY) {
        std::ifstream geometry(directory + "/geometry.txt");
        check(bool(geometry >> coded_w >> coded_h >> crop_l >> crop_r >> crop_t >> crop_b),
              "missing independent coded/crop geometry");
        geometry >> std::ws;
        check(geometry.eof(), "extra independent geometry records");
    }
    check(coded_w && coded_w <= 320 && coded_h && coded_h <= 240 &&
          !(coded_w % 16) && !(coded_h % 16) &&
          crop_l + crop_r < coded_w && crop_t + crop_b < coded_h &&
          !((crop_l | crop_r | crop_t | crop_b) & 1),
          "unsupported independent coded/crop geometry");
    const unsigned visible_w = coded_w - crop_l - crop_r;
    const unsigned visible_h = coded_h - crop_t - crop_b;
    const unsigned visible_bytes = visible_w * visible_h * 3 / 2;
    const unsigned coded_bytes = coded_w * coded_h * 3 / 2;
    const auto coded_reference = FULL_AU_RUNTIME_GEOMETRY ?
        read_bytes(directory + "/reference-coded.yuv") : reference;
    check(reference.size() == 2 * visible_bytes &&
          coded_reference.size() == 2 * coded_bytes,
          "expected two independent visible and uncropped coded I420 frames");
    check(keyframes.size() == 2, "missing original packet keyframe flags");
    struct Timing {
        int64_t pts, duration;
        uint32_t numerator, denominator;
    };
    Timing timing[2] = {{-12345, 1001, 1, 24000}, {-11344, 1001, 1, 24000}};
    if (FULL_AU_ORIGINAL_TIMING) {
        std::ifstream input(directory + "/original-timing.txt");
        check(bool(input), "missing required original AU timing");
        for (auto& value : timing)
            check(bool(input >> value.pts >> value.duration >> value.numerator >> value.denominator),
                  "incomplete original AU timing");
        input >> std::ws;
        check(input.eof(), "extra original AU timing records");
    }
    publish();
    s.run(26000);
    append(ring::Event::Probe, nonce1, mailbox_abi::kFpgaVideoLayoutId, {});
    s.until([&] { return s.d.observed_nonce == nonce1; }, "composed Probe failed");
    ring::VideoCapabilities caps;
    s.until([&] {
        std::array<uint64_t, 8> words{};
        for (unsigned i = 0; i < words.size(); ++i)
            words[i] = s.load((mailbox_abi::kVideoCapsAddr >> 3) + i);
        return ring::decodeVideoCapabilities(words, nonce1, caps);
    }, "actual Probe capability commit did not reach DDR");
    check(caps.max_au_bytes == 65536, "modern source did not negotiate its coherent 64KiB capacity");
    check(caps.features == 0 && !caps.supportsVideo(),
          "unqualified source advertised product playback capabilities");
    append(ring::Event::Begin, epoch1, 0, {});
    s.until([&] { return s.d.stream_read_count == producer && s.d.stream_idle; },
            "composed Begin did not retire");
    append(ring::Event::Pause, epoch1, 0, {});
    append(ring::Event::Pause, epoch1, 0, {});
    s.until([&] {
        return s.d.stream_read_count == producer &&
               ((s.load(ring::kStat6Phys / 8) >>
                 (ring::kStat6StateFlagsShift + ring::kStat6PausedFlagBit)) & 1);
    }, "no-data repeated Pause did not retire through real CDC");
    const auto paused_read = producer;
    for (unsigned frame = 0; frame < 2; ++frame) {
        const auto encoded = read_bytes(directory + "/au" + std::to_string(frame) + ".264");
        check(!encoded.empty() && encoded.size() <= caps.max_au_bytes,
              "AU exceeds actual negotiated encoded capacity");
        ring::AccessUnit au;
        au.session_id = epoch1;
        au.seq = frame;
        au.annexb = encoded.data();
        au.len = encoded.size();
        au.pts = timing[frame].pts;
        au.duration = timing[frame].duration;
        au.timebase_num = timing[frame].numerator;
        au.timebase_den = timing[frame].denominator;
        au.flags = keyframes[frame] ? ring::kAccessUnitKeyframe : 0;
        check(ring::validAccessUnit(au), "invalid test AU metadata");
        const auto metadata = ring::encodeAccessUnitMetadata(au);
        Bytes payload(metadata.begin(), metadata.end());
        payload.insert(payload.end(), encoded.begin(), encoded.end());
        append(ring::Event::AccessUnit, epoch1, frame, payload);
    }
    s.run(26000);
    check(s.d.stream_bytes_out == 0 && s.d.stream_read_count == paused_read && s.d.count == 0,
          "paused ring lookahead delivered or consumed a preserved AU");
    append(ring::Event::Resume, epoch1, 0, {});
    append(ring::Event::Drain, epoch1, 0, {});
    std::ofstream actual(actual_path, std::ios::binary);
    check(bool(actual), "cannot create actual DDR picture output");
    std::ofstream allocation_output(actual_path + ".allocation", std::ios::binary);
    check(bool(allocation_output), "cannot create actual DDR allocation output");
    for (unsigned frame = 0; frame < 2; ++frame) {
        s.vsync_enabled = false;
        s.until([&] { return s.d.swap_pending; }, "decoded AU never reached pending display");
        check(s.d.native_lease_pending && !s.d.native_lease_release,
              "pending display did not retain the genuine native picture lease");
        const auto completed_rbsp_bytes = s.d.decoder_rbsp_bytes;
        const auto completed_bit_pos = s.d.decoder_bit_pos;
        check(completed_rbsp_bytes > 0 && completed_rbsp_bytes <= caps.max_au_bytes &&
              completed_bit_pos == completed_rbsp_bytes * 8,
              "published picture did not consume the complete full-width RBSP cursor");
        check(s.d.count == frame && s.d.stream_read_count < producer,
              "copy counted as display or Drain retired before display");
        check(s.d.geometry_width == coded_w && s.d.geometry_height == coded_h &&
              s.d.geometry_left == crop_l && s.d.geometry_right == crop_r &&
              s.d.geometry_top == crop_t && s.d.geometry_bottom == crop_b,
              "completed picture geometry was lost before display");
        check(s.d.reference_width == coded_w && s.d.reference_height == coded_h &&
              s.d.native_sample_count == coded_bytes &&
              s.d.decoder_mb == (coded_w / 16) * (coded_h / 16),
              "decoder completion/reference bounds still use maximum or visible geometry");
        if (frame == 0)
            check(!s.d.has_frame && s.d.active_coded_width == 320 &&
                  s.d.active_coded_height == 240,
                  "pending copy changed active geometry before display ACK");
        Bytes allocation(115200);
        const uint32_t base = frame == 0 ? 0x30080000 : 0x30000000;
        for (uint32_t i = 0; i < allocation.size(); ++i)
            allocation[i] = uint8_t(s.load((base + i) / 8) >> (8 * (i & 7)));
        allocation_output.write(reinterpret_cast<const char*>(allocation.data()), allocation.size());
        allocation_output.flush();
        check(bool(allocation_output), "cannot write actual DDR allocation output");
        Bytes pixels;
        pixels.reserve(visible_bytes);
        const unsigned allocation_bases[] = {0, 76800, 96000};
        const unsigned coded_bases[] = {0, coded_w * coded_h, coded_w * coded_h * 5 / 4};
        for (unsigned plane = 0; plane < 3; ++plane) {
            const unsigned divisor = plane ? 2 : 1;
            const unsigned stride = 320 / divisor;
            const unsigned rows = 240 / divisor;
            const unsigned cw = coded_w / divisor, ch = coded_h / divisor;
            for (unsigned y = 0; y < rows; ++y)
                for (unsigned x = 0; x < stride; ++x) {
                    const auto value = allocation[allocation_bases[plane] + y * stride + x];
                    const auto expected = x < cw && y < ch ?
                        coded_reference[frame * coded_bytes + coded_bases[plane] + y * cw + x] :
                        uint8_t(plane ? 128 : 16);
                    if (value != expected) {
                        std::cerr << "frame=" << frame << " plane=" << plane
                                  << " x=" << x << " y=" << y << " actual=" << unsigned(value)
                                  << " ffmpeg_or_padding=" << unsigned(expected) << '\n';
                        throw std::runtime_error("fixed-stride coded DDR picture differs from ordinary ffmpeg");
                    }
                }
            for (unsigned y = 0; y < visible_h / divisor; ++y)
                for (unsigned x = 0; x < visible_w / divisor; ++x)
                    pixels.push_back(allocation[allocation_bases[plane] +
                        (crop_t / divisor + y) * stride + crop_l / divisor + x]);
        }
        actual.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        actual.flush();
        check(bool(actual), "cannot write actual DDR picture output");
        for (uint32_t i = 0; i < pixels.size(); ++i) {
            const auto expected = reference[frame * visible_bytes + i];
            if (pixels[i] != expected) {
                std::cerr << "frame=" << frame << " I420_byte=" << i
                          << " actual=" << unsigned(pixels[i])
                          << " ffmpeg=" << unsigned(expected) << '\n';
                throw std::runtime_error("actual decoded DDR picture differs from ordinary ffmpeg");
            }
        }
        s.vsync_enabled = true;
        s.until([&] { return s.d.idle && s.d.count == frame + 1; },
                "decoded safe-boundary MVPS publication failed");
        s.vsync_enabled = false;
        check(s.d.active_coded_width == coded_w && s.d.active_coded_height == coded_h &&
              s.d.active_visible_width == visible_w && s.d.active_visible_height == visible_h,
              "display bank and geometry did not commit together");
        ring::VideoPresentation p;
        check(ring::decodeVideoPresentation(s.status(), epoch1, nonce1, p) &&
              p.seq == frame && p.pts == timing[frame].pts &&
              p.timebase_num == timing[frame].numerator && p.timebase_den == timing[frame].denominator &&
              p.presentation_count == frame + 1 && !p.has_audio_clock,
              "decoded AU metadata lost before actual presentation");
        const auto offset = frame * visible_bytes;
        const unsigned y_bytes = visible_w * visible_h;
        const unsigned c_bytes = y_bytes / 4;
        const unsigned origin_x = (320 - visible_w) / 2, origin_y = (240 - visible_h) / 2;
        if (FULL_AU_NATIVE_BEAM) {
            s.until([&] { return s.native_complete[frame]; },
                    "actual presenter did not finish the displayed native raster");
            std::vector<uint32_t> expected;
            unsigned line_width = 0;
            for (unsigned row = 0; row < 240 * (FULL_AU_SCANDOUBLE ? 2 : 1); ++row) {
                const unsigned py = FULL_AU_SCANDOUBLE ? row / 2 : row;
                if (py < origin_y || py >= origin_y + visible_h) continue;
                unsigned width = 0;
                for (unsigned hc = 0; hc < 529; ++hc) {
                    const unsigned px = std::min((hc * 39647) >> 16, 319u);
                    if (px < origin_x || px >= origin_x + visible_w) continue;
                    const unsigned x = px - origin_x, y = py - origin_y;
                    const unsigned yi = y * visible_w + x;
                    const unsigned ci = (y / 2) * (visible_w / 2) + x / 2;
                    expected.push_back(Sim::rgb601(reference[offset + yi],
                        reference[offset + y_bytes + ci],
                        reference[offset + y_bytes + c_bytes + ci]));
                    ++width;
                }
                line_width = width;
            }
            check(s.native_lines[frame].size() == visible_h * (FULL_AU_SCANDOUBLE ? 2 : 1),
                  "actual native DE height includes padding or loses cropped rows");
            for (auto width : s.native_lines[frame])
                check(width == line_width, "actual native DE width lost an edge or included padding");
            check(s.native_pixels[frame].size() == expected.size(),
                  "native CE/DE aperture does not match displayed geometry");
            for (size_t i = 0; i < expected.size(); ++i)
                if (s.native_pixels[frame][i] != expected[i]) {
                    std::cerr << "native frame=" << frame << " x=" << i % line_width
                              << " y=" << i / line_width << " RGB=0x" << std::hex
                              << s.native_pixels[frame][i] << " expected=0x" << expected[i]
                              << std::dec << '\n';
                    throw std::runtime_error("actual present_core CE/RGB differs from decoded reference");
                }
            std::cout << "NATIVE_PASS seq=" << frame << " DE=" << line_width << "x"
                      << s.native_lines[frame].size() << " RGB_samples=" << expected.size()
                      << " scandouble=" << FULL_AU_SCANDOUBLE << " source_DAR=content-only\n";
        } else {
        for (const auto point : {std::pair<unsigned, unsigned>{0, 0},
                                 {visible_w - 1, 0}, {0, visible_h - 1},
                                 {visible_w - 1, visible_h - 1}}) {
            const auto yi = point.second * visible_w + point.first;
            const auto ci = (point.second / 2) * (visible_w / 2) + point.first / 2;
            s.check_pixel(origin_x + point.first, origin_y + point.second,
                          reference[offset + yi], reference[offset + y_bytes + ci],
                          reference[offset + y_bytes + c_bytes + ci]);
            check(s.d.pixel_de, "visible picture excluded from scaler content aperture");
        }
        if (origin_y) {
            s.check_pixel(origin_x, origin_y - 1, 16, 128, 128);
            check(!s.d.pixel_de, "upper allocation padding included in source DAR");
            s.check_pixel(origin_x, origin_y + visible_h, 16, 128, 128);
            check(!s.d.pixel_de, "lower allocation padding included in source DAR");
        }
        if (origin_x) {
            s.check_pixel(origin_x - 1, origin_y, 16, 128, 128);
            check(!s.d.pixel_de, "horizontal allocation padding included in source DAR");
        }
        }
        ++s.verified_frames;
        std::cout << "FRAME_PASS seq=" << frame << " native_bytes=" << visible_bytes
                  << " presentation_count=" << p.presentation_count
                  << " pts=" << p.pts << " tb=" << p.timebase_num << "/"
                  << p.timebase_den << " sys_cycles=" << s.time / 26
                  << " display_sys_cycles=" << s.displayed_cycle
                  << " rbsp_bytes=" << completed_rbsp_bytes
                  << " bit_pos=" << completed_bit_pos
                  << " coded_bytes=" << coded_bytes << " coded=" << coded_w << "x" << coded_h
                  << " visible=" << visible_w << "x" << visible_h << '\n';
    }
    s.vsync_enabled = true;
    s.until([&] { return s.d.stream_idle && s.d.stream_read_count == producer; },
            "Drain did not fence actual decoder and presentation");
    s.run(s.vsync_period * 26 * 8);
    check(s.d.count == 2, "idle vsync fabricated another decoded presentation");
    for (unsigned client = 0; client < 2; ++client)
        check(s.native_requests[client] == s.native_responses[client],
              "native read response left outstanding after Drain");
    check(s.d.native_reference_ready && s.d.has_frame,
          "Drain flushed the native reference or displayed picture");
    const auto frames_before_flush = s.d.frames_done;
    append(ring::Event::Flush, epoch1, 0, {});
    publish();
    s.until([&] { return s.d.stream_read_count == producer && s.d.stream_idle &&
                        s.d.idle && s.load(commit) == 0; },
            "actual Flush did not fence generation retirement");
    s.run(s.vsync_period * 26 * 4);
    check(!s.d.native_reference_ready && !s.d.has_frame && !s.d.swap_pending &&
          s.d.count == 0 && s.d.frames_done == frames_before_flush,
          "actual Flush retained old DPB/display ownership");

    auto malformed = read_bytes(directory + "/au0.264");
    s.negative_guard = true;
    if (s.bad_tail) {
        while (!malformed.empty() && malformed.back() == 0) malformed.pop_back();
        check(!malformed.empty() && !(malformed.back() & 1),
              "tail negative needs a nonzero final RBSP byte with an alignment-zero bit");
        // Change padding only, after every macroblock's syntax/reconstruction.
        malformed.back() ^= 1;
    } else {
    bool inserted = false;
    for (size_t i = 0; i + 4 < malformed.size(); ++i) {
        if (malformed[i] || malformed[i + 1]) continue;
        const size_t header = malformed[i + 2] == 1 ? i + 3 :
            (malformed[i + 2] == 0 && malformed[i + 3] == 1 ? i + 4 : 0);
        if (header && ((malformed[header] & 31) == 1 || (malformed[header] & 31) == 5)) {
            // Negative only: truncated PPS or valid PPS referencing unavailable SPS 1.
            const Bytes replacement = s.pps_mismatch ?
                Bytes{0, 0, 0, 1, 0x68, 0xa3, 0x8f, 0x20} :
                Bytes{0, 0, 0, 1, 0x68, 0x80};
            malformed.insert(malformed.begin() + i, replacement.begin(), replacement.end());
            inserted = true;
            break;
        }
    }
    check(inserted && malformed.size() <= caps.max_au_bytes,
          "cannot construct bounded malformed-PPS AU");
    }
    ring::AccessUnit invalid_au;
    invalid_au.session_id = epoch1;
    invalid_au.seq = 0;
    invalid_au.annexb = malformed.data();
    invalid_au.len = malformed.size();
    invalid_au.pts = 0;
    invalid_au.duration = 1001;
    invalid_au.timebase_num = 1;
    invalid_au.timebase_den = 24000;
    invalid_au.flags = ring::kAccessUnitKeyframe;
    check(ring::validAccessUnit(invalid_au), "decoder negative broke transport metadata");
    const auto bad_metadata = ring::encodeAccessUnitMetadata(invalid_au);
    Bytes bad_payload(bad_metadata.begin(), bad_metadata.end());
    bad_payload.insert(bad_payload.end(), malformed.begin(), malformed.end());
    append(ring::Event::AccessUnit, epoch1, 0, bad_payload);
    if (s.bad_tail) {
        s.until([&] { return s.d.decoder_error != 0; },
                "corrupted RBSP alignment did not abort after reconstruction");
        check(s.d.decoder_error == 15 && s.d.native_sample_count == coded_bytes &&
              s.d.decoder_mb == (coded_w / 16) * (coded_h / 16),
              "tail negative did not reach error15 after all coded native writes");
        std::cout << "TAIL_REJECT error=15 native_writes=" << s.d.native_sample_count
                  << " DPB/source/display/MVPS_completion=0\n";
    } else {
    s.until([&] { return s.pps_mismatch ? s.d.parameter_id_mismatch :
                                        (s.d.parser_errors & 2) != 0; },
            "negative PPS did not reach parameter validation");
    if (s.pps_mismatch) check((s.d.parser_errors & 6) == 0,
                             "identity negative used malformed SPS/PPS instead");
    check(s.d.controller_header_error, "PPS parser error/identity mismatch did not reach controller abort");
    }
    s.run(26 * 100000);
    check(s.d.decoder_error && !s.d.native_reference_ready && !s.d.has_frame &&
          !s.d.swap_pending && s.d.count == 0 && s.load(commit) == 0 &&
          s.d.frames_done == frames_before_flush,
          "rejected AU produced a reference, display or committed feedback");
    std::cout << "PASS two actual DDR-ring AUs -> decoder -> native I420 -> safe display -> MVPS; "
              << 2 * visible_bytes << " visible bytes and " << 2 * coded_bytes
              << " coded bytes exact ordinary ffmpeg, fixed strides/plane bases/padding, "
                 "Pause replay, held metadata, Drain fence and actual Flush reference/display "
                 "invalidation; decoder negative cannot publish; no glass claim\n";
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Sim s;
        s.d.clk = s.d.clk_ddr = 0;
        s.d.reset = 1;
        s.d.clear = 0;
        s.d.nonce = 0;
        s.d.frame_valid = s.d.mem_valid = s.d.vsync = 0;
        s.d.sample_x = s.d.sample_y = 0;
        s.d.test_width = 320;
        s.d.test_height = 240;
        s.d.test_left = s.d.test_right = s.d.test_top = s.d.test_bottom = 0;
        s.d.mem_data = 0;
        s.d.DDRAM_BUSY = s.d.DDRAM_DOUT_READY = 0;
        s.d.DDRAM_DOUT = 0;
        s.memory[commit] = UINT64_C(0x123456784d565043);
        s.memory[0x300ff000 / 8] = UINT64_C(0x80000015504c584b);
        s.run(1040);
        s.d.reset = 0;
        if (FULL_AU) {
            check(argc >= 3 && argc <= 5,
                  "full-AU mode requires fixture directory, output path and optional VSync cycles");
            if (argc >= 4) {
                size_t consumed = 0;
                s.vsync_period = std::stoull(argv[3], &consumed);
                check(consumed == std::string(argv[3]).size() &&
                      s.vsync_period > 0 && s.vsync_period <= 1000000,
                      "VSync cycle interval outside bounded simulation range");
            }
            if (argc == 5) {
                const std::string mode = argv[4];
                check(mode == "truncated" || mode == "sps-mismatch" || mode == "bad-tail",
                      "unsupported decoder negative mode");
                s.pps_mismatch = mode == "sps-mismatch";
                s.bad_tail = mode == "bad-tail";
            }
            if (FULL_AU_NATIVE_BEAM)
                std::cout << "SIM_NATIVE sys_cycles=334312 scandouble=" << FULL_AU_SCANDOUBLE;
            else
                std::cout << "SIM_VSYNC sys_cycles=" << s.vsync_period;
            std::cout
                      << " negative=" << (s.bad_tail ? "bad-tail" :
                                         s.pps_mismatch ? "sps-mismatch" : "truncated")
                      << " clock/rate/hardware qualification=none\n";
            decoded_access_units(s, argv[1], argv[2]);
            return 0;
        }
        s.d.nonce = nonce1;
        s.until([&] { return s.d.frame_ready; }, "initial nonce invalidation timeout");
        check(s.load(commit) == 0 && !s.d.has_frame && s.d.count == 0,
              "stale boot frame/feedback accepted");
        s.begin_frame(epoch1, 7, -12345);
        s.until([&] { return s.d.swap_pending; }, "copied picture did not reach presenter");
        check(s.d.count == 0 && s.load(commit) == 0, "copy/reconstruction counted as display");
        for (uint32_t i = 0; i < 115200; ++i) {
            const auto got = uint8_t(s.load((0x30080000 + i) / 8) >> (8 * (i & 7)));
            check(got == s.native(i), "native I420 DDR publication mismatch");
        }
        s.vsync_enabled = true;
        s.until([&] { return s.d.idle && s.d.count == 1; }, "safe-boundary publication timeout");
        ring::VideoPresentation p;
        check(ring::decodeVideoPresentation(s.status(), epoch1, nonce1, p),
              "authoritative host MVPS decoder rejected publication");
        check(p.has_frame && p.active && !p.has_audio_clock && p.audio_samples_consumed == 0 &&
              p.seq == 7 && p.pts == -12345 && p.timebase_num == 1001 &&
              p.timebase_den == 24000 && p.presentation_count == 1,
              "presentation metadata mismatch");
        for (const auto point : std::array<std::pair<unsigned,unsigned>,4>{
                 {{0,0}, {319,0}, {0,239}, {319,239}}}) {
            const auto address = point.second * 320 + point.first;
            s.check_pixel(point.first, point.second, s.native(address),
                          s.native(76800), s.native(96000));
        }
        s.d.sample_x = s.d.sample_y = 0;
        check(s.status_writes.size() >= 10, "missing atomic publication writes");
        auto n = s.status_writes.size();
        check(s.status_writes[n-10].first == commit && s.status_writes[n-10].second == 0,
              "MVPS commit not invalidated before body");
        for (unsigned i = 0; i < 8; ++i)
            check(s.status_writes[n-9+i].first == feedback + i, "MVPS body order mismatch");
        check(s.status_writes.back().first == commit &&
              uint32_t(s.status_writes.back().second) == mailbox_abi::kVideoPresentationCommitMagic,
              "fresh MVPC commit was not last");
        const auto publication = p.publication;
        s.run(128 * 26 * 8);
        check(s.d.count == 1 && uint32_t(s.load(commit) >> 32) == publication,
              "timer/vsync falsely advanced displayed AU count");
        check(!ring::decodeVideoPresentation(s.status(), epoch1 + 1, nonce1, p),
              "stale epoch accepted");
        s.d.nonce = nonce1 + 1;
        s.until([&] { return s.d.frame_ready; }, "fresh probe invalidation timeout");
        check(s.load(commit) == 0, "new probe retained prior MVPS");
        s.run(128 * 26 * 4);
        check(s.d.count == 1 && s.load(commit) == 0, "last frame republished for fresh nonce");
        s.image = 1;
        s.begin_frame(epoch1 + 1, 0, 0);
        s.until([&] { return s.d.idle && s.d.count == 2; }, "second bank publication timeout");
        check(ring::decodeVideoPresentation(s.status(), epoch1 + 1, nonce1 + 1, p) &&
              p.seq == 0 && p.presentation_count == 2, "new epoch publication mismatch");
        s.vsync_enabled = false;
        s.begin_frame(epoch1 + 1, 1, 1001);
        s.until([&] { return s.d.swap_pending && s.d.pending_ready; },
                "seek test has no ready pending swap");
        s.run(26 * 4);
        s.until([&] { return !s.responses.empty(); }, "seek has no accepted display read");
        const auto frames_before_clear = s.d.frames_done;
        s.hold_responses = true;
        s.d.clear = 1;
        s.run(104);
        s.d.clear = 0;
        s.run(5200);
        check(!s.d.idle && !s.d.generation_idle && !s.d.has_frame && !s.d.swap_pending,
              "seek discarded accepted display ownership or retained visible picture");
        s.hold_responses = false;
        s.vsync_enabled = true;
        s.until([&] { return s.d.idle && s.load(commit) == 0; },
                "seek did not retire/invalidate pending presentation");
        s.run(128 * 26 * 4);
        check(s.d.frames_done == frames_before_clear && !s.d.has_frame &&
              !s.d.swap_pending && s.d.count == 0,
              "cancelled old-epoch picture reached display after clear");

        s.vsync_enabled = false;
        s.begin_frame(epoch1 + 2, 0, 0);
        s.until([&] { return s.d.swap_pending && s.d.pending_ready; },
                "new generation did not acquire display ownership");
        s.run(26 * 4);
        s.d.clear = 1;
        s.force_vsync = true;
        s.run(26);
        s.d.clear = 0;
        s.force_vsync = false;
        s.vsync_enabled = true;
        s.until([&] { return s.d.idle && s.load(commit) == 0; },
                "coincident clear/vsync did not retire");
        s.run(128 * 26 * 4);
        check(s.d.frames_done == frames_before_clear && !s.d.has_frame &&
              !s.d.swap_pending && s.d.count == 0,
              "clear lost priority over coincident safe-boundary swap");

        s.begin_frame(epoch1 + 2, 1, 1001);
        // R14 OVERLAP: after SWAP the FSM returns to IDLE with present_pending
        // instead of lingering in DISPLAY_WAIT (10).
        s.until([&] {
            return s.d.publish_state == 10 || s.d.present_pending;
        }, "no start-toggle in flight");
        check(!s.d.swap_pending, "start-toggle cancellation missed its CDC window");
        s.d.clear = 1;
        s.run(26);
        s.d.clear = 0;
        s.run(26);
        s.d.clear = 1;
        s.run(26);
        s.d.clear = 0;
        s.until([&] { return s.d.idle && s.load(commit) == 0; },
                "repeated clear did not retire in-flight start toggle");
        s.run(128 * 26 * 4);
        check(s.d.frames_done == frames_before_clear && !s.d.has_frame &&
              !s.d.swap_pending && s.d.count == 0,
              "late start/swap toggle resurrected cancelled picture");

        s.image = 2;
        s.begin_frame(epoch1 + 2, 2, 2002);
        s.until([&] { return s.d.idle && s.d.count == 1; },
                "fresh picture failed after functional cancellation");
        s.check_pixel(0, 0, 16, 128, 128);
        s.run(128 * 26 * 4);
        check(s.d.frames_done == frames_before_clear + 1 &&
              ring::decodeVideoPresentation(s.status(), epoch1 + 2, nonce1 + 1, p) &&
              p.seq == 2 && p.presentation_count == 1,
              "fresh generation inherited old display/cached pixels/feedback");
        const auto commit_before_same_epoch_clear = s.load(commit);

        s.until([&] { return s.d.DDRAM_WE; }, "no offered display write to cancel");
        s.watched_address = s.d.DDRAM_ADDR;
        s.watched_data = s.d.DDRAM_DIN;
        s.watch_write = true;
        s.force_busy = true;
        s.d.clear = 1;
        s.run(26);
        s.d.clear = 0;
        s.run(5200);
        check(!s.d.idle && !s.d.generation_idle && s.watched_writes == 0,
              "generation clear abandoned a held display write");
        s.force_busy = false;
        s.until([&] { return s.d.idle && s.load(commit) == 0; },
                "held display write failed to retire after clear");
        check(s.watched_writes == 1, "held display write was lost or repeated");
        s.watch_write = false;

        s.image = 3;
        s.begin_frame(epoch1 + 2, 0, 5005);
        s.until([&] { return s.d.idle && s.d.count == 1; },
                "same-epoch publication did not recover after clear");
        check(ring::decodeVideoPresentation(s.status(), epoch1 + 2, nonce1 + 1, p) &&
              p.seq == 0 && p.pts == 5005 && p.presentation_count == 1,
              "same-epoch post-clear metadata is not fresh");
        check(s.load(commit) != commit_before_same_epoch_clear,
              "MVPC token reused across clear permits a torn same-epoch snapshot");

        s.force_busy = true;
        s.begin_frame(epoch1 + 2, 3, 3003);
        // WRITE_WORD=6 or PIPE_COPY=17 (PIPELINED_COPY path).
        s.until([&] {
            return s.d.publish_state == 6 || s.d.publish_state == 17;
        }, "no publisher write to cancel");
        s.d.clear = 1;
        s.run(26);
        s.d.clear = 0;
        s.run(5200);
        check(!s.d.idle && !s.d.has_frame, "clear abandoned publisher DDR ownership");
        s.force_busy = false;
        s.until([&] { return s.d.frame_ready && s.load(commit) == 0; },
                "publisher write failed to retire after functional clear");
        check(s.d.count == 0 && !s.d.has_frame && !s.d.swap_pending,
              "partial copy was published after functional clear");

        s.force_busy = true;
        s.begin_frame(epoch1 + 2, 0, 0);
        s.run(5200);
        s.d.reset = 1;
        s.d.nonce = 0;
        s.run(1040);
        s.d.reset = 0;
        s.force_busy = false;
        s.d.nonce = nonce1 + 2;
        s.until([&] { return s.d.frame_ready && s.load(commit) == 0; },
                "reset lost accepted DDR transaction retirement");
        s.run(128 * 26 * 4);
        check(s.d.count == 0 && !s.d.has_frame && s.load(commit) == 0,
              "reset fabricated an initial/last frame");
        for (unsigned level = 2; level < 4; ++level) {
            s.image = level;
            s.begin_frame(epoch1 + 3, level - 2, int64_t(level - 2) * 1001);
            s.until([&] { return s.d.idle && s.d.count == level - 1; },
                    "limited-range endpoint frame did not present");
            s.check_pixel(0, 0, s.native(0), 128, 128);
            check(s.d.pixel_rgb == (level == 2 ? 0u : 0xffffffu),
                  "limited-range black/white endpoints were not preserved");
        }
        check(!s.d.error, "publisher error");
        s.image = 0;
        s.d.test_width = 288; s.d.test_height = 208;
        s.d.test_left = 2; s.d.test_right = 6;
        s.d.test_top = 8; s.d.test_bottom = 4;
        s.vsync_enabled = false;
        s.begin_frame(epoch1 + 3, 2, 2002);
        s.until([&] { return s.d.swap_pending && s.d.pending_ready; },
                "cropped pending bank did not prefetch");
        s.d.sample_x = 20; s.d.sample_y = 22;
        const auto before_crop = s.d.frames_done;
        s.vsync_enabled = true;
        s.until([&] { return s.d.frames_done == before_crop + 1; },
                "cropped bank never committed");
        s.run(26 * 4);
        check(s.d.pixel_de &&
              s.d.pixel_rgb == Sim::rgb601(s.native(8 * 320 + 2), s.native(76800), s.native(96000)),
              "first cropped pixel was not prefetched/aligned at actual swap");
        s.until([&] { return s.d.idle && s.d.count == 3; },
                "cropped bank did not publish");
        check(s.d.active_coded_width == 288 && s.d.active_coded_height == 208 &&
              s.d.active_visible_width == 280 && s.d.active_visible_height == 196,
              "smaller coded/cropped geometry did not commit atomically");
        s.check_pixel(299, 217, s.native(203 * 320 + 281), s.native(76800), s.native(96000));
        check(s.d.pixel_de, "bottom-right cropped pixel excluded from DE");
        for (const auto point : {std::pair<unsigned, unsigned>{19, 22}, {300, 22},
                                 {20, 21}, {20, 218}}) {
            s.check_pixel(point.first, point.second, 16, 128, 128);
            check(!s.d.pixel_de, "allocation padding leaked into cropped content DE");
        }
        std::cout << "PASS native I420 -> CDC DDR -> real frame-store safe swap -> MVPS; "
                     "115200 bytes, stalls, seek/reset retirement, stale epoch/nonce, "
                     "atomic commit, limited BT601 RGB corners, runtime crop/top-prefill/DE, "
                     "no timer/audio claims\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL publication composition: " << e.what() << '\n';
        return 1;
    }
}
