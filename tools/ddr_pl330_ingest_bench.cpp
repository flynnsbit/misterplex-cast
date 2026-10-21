// Parent-facing bench: serial bank copy vs PL330 with fixed ABI scratch.
// Does NOT ring the frame doorbell (safe on a live core).
//
// INCIDENT FIX: prior --dma-only reported 98k fps / 136 GB/s (impossible).
// DMAGO DBGINST0 encoding was wrong; CSR stayed STOPPED; loop counted fakes.
// This build: Linux DMAGO encode, require saw_executing, content verify, ceiling fail.
//
// PRE-REGISTER (device):
//   P1: first transfer either real (saw_exec+content match) OR honest never_executed
//   P2: if real, dma_ms >= pl330MinPlausibleMs (~0.43ms @3.2GB/s) else INSTRUMENT_FAIL
//   P3: ok_frac == content match fraction (can be < 1.0); full memcmp every N
//   P4: poll_mode=usleep (no tight spin); cpu_time_s << wall if DMA is real off-CPU
//   P5: bank-sweep STAND DOWN (parent: contaminated by rogue awk)
//
// Parent gate (only if bench exits 0 with verified frames > 0):
//   --dma-only --duration-s 25 + ffmpeg_cpu_probe 300f real720p_2600k
//   decode < 41.667 → Route A REAL; else fabric owns pixels
//
// Usage on MiSTer (parent):
//   # Sustained DMA for decode-overlap experiment (THE gate):
//   ddr_pl330_ingest_bench --dma-only --duration-s 25 --bank 1
//   # Bank anomaly chase (serial only, several phys targets):
//   ddr_pl330_ingest_bench --bank-sweep --len 1382400 --loops 40
//   # Classic compare (serial then DMA; DMA timed WITHOUT restage by default):
//   ddr_pl330_ingest_bench --len 1382400 --loops 50 --bank 1
//   ddr_pl330_ingest_bench --len 1382400 --loops 30 --bank 1 --overlap-burn-ms 37

#include "libmisterplex/ddr_frame_layout.hpp"
#include "libmisterplex/ddr_zero_copy_ingest.hpp"
#include "libmisterplex/pl330_mem2mem.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {

double nowSec() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void fill(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        p[i] = static_cast<uint8_t>(i * 131u);
}

struct BurnArg {
    std::atomic<int>* stop;
};

void* burnThread(void* arg) {
    auto* a = static_cast<BurnArg*>(arg);
    volatile uint32_t x = 1;
    while (a->stop->load() == 0)
        x = x * 1664525u + 1013904223u;
    (void)x;
    return nullptr;
}

struct MapWin {
    int fd = -1;
    void* map = MAP_FAILED;
    size_t map_len = 0;
    uint32_t map_phys = 0;

    uint8_t* ptrAt(uint32_t phys) const {
        if (map == MAP_FAILED || phys < map_phys)
            return nullptr;
        const uint64_t off = static_cast<uint64_t>(phys) - map_phys;
        if (off + 64 > map_len)
            return nullptr;
        return static_cast<uint8_t*>(map) + static_cast<size_t>(off);
    }

    void close() {
        if (map != MAP_FAILED) {
            ::munmap(map, map_len);
            map = MAP_FAILED;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

// Map a window covering [phys, phys+need). Uses O_SYNC like product path.
bool mapWindow(MapWin& w, uint32_t phys, size_t need, std::string* err) {
    w.close();
    const uint32_t page = phys & ~0xFFFu;
    const size_t off = static_cast<size_t>(phys - page);
    const size_t map_need = off + need;
    size_t map_len = (map_need + 0xFFFu) & ~static_cast<size_t>(0xFFFu);
    if (map_len < 0x400000u && phys >= 0x30000000u && phys < 0x31000000u)
        map_len = 0x400000u;
    w.fd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (w.fd < 0) {
        if (err)
            *err = "open /dev/mem failed";
        return false;
    }
    w.map = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, w.fd,
                   static_cast<off_t>(page));
    if (w.map == MAP_FAILED) {
        ::close(w.fd);
        w.fd = -1;
        if (err)
            *err = "mmap failed";
        return false;
    }
    w.map_len = map_len;
    w.map_phys = page;
    return true;
}

double timeSerialCopy(uint8_t* dst, const uint8_t* src, size_t len, int loops) {
    const double t0 = nowSec();
    for (int i = 0; i < loops; ++i) {
        std::memcpy(dst, src, len);
        __sync_synchronize();
    }
    return (nowSec() - t0) * 1000.0 / loops;
}

} // namespace

int main(int argc, char** argv) {
    size_t len = static_cast<size_t>(misterplex::kPlex720pYuv420pBytes);
    int loops = 30;
    int bank = 1;
    double burn_ms = 0.0;
    double duration_s = 0.0;
    bool do_pl330 = true;
    bool host_only = false;
    bool use_option_c = true;
    bool dma_only = false;
    bool bank_sweep = false;
    unsigned force_burst = 0; // 0=auto, 1..16 pin CCR burst
    unsigned force_ns = 0xFFu; // 0xFF=try both, 0/1 pin
    bool allow_channel_kill = false; // default OFF — kernel owns dma-pl330
    bool restage_each = false; // default OFF for fair DMA wall
    bool verify_each = false;
    uint32_t phys_override = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--len" && i + 1 < argc)
            len = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 0));
        else if (a == "--loops" && i + 1 < argc)
            loops = std::atoi(argv[++i]);
        else if (a == "--bank" && i + 1 < argc)
            bank = std::atoi(argv[++i]);
        else if (a == "--overlap-burn-ms" && i + 1 < argc)
            burn_ms = std::atof(argv[++i]);
        else if (a == "--duration-s" && i + 1 < argc)
            duration_s = std::atof(argv[++i]);
        else if (a == "--burst" && i + 1 < argc)
            force_burst = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--force-ns" && i + 1 < argc)
            force_ns = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--allow-channel-kill")
            allow_channel_kill = true;
        else if (a == "--dma-only")
            dma_only = true;
        else if (a == "--bank-sweep")
            bank_sweep = true;
        else if (a == "--restage")
            restage_each = true;
        else if (a == "--verify-each")
            verify_each = true;
        else if (a == "--no-pl330")
            do_pl330 = false;
        else if (a == "--host-copy")
            host_only = true;
        else if (a == "--legacy-base")
            use_option_c = false;
        else if (a == "--phys" && i + 1 < argc)
            phys_override = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (a == "--help") {
            std::printf(
                "ddr_pl330_ingest_bench [options]\n"
                "  --dma-only --duration-s N   sustained DMA only (parent decode gate)\n"
                "  --bank-sweep                serial copy table across phys targets\n"
                "  --len N --loops N --bank 0|1 --phys 0x...\n"
                "  --burst N                  force CCR burst 1..16 (0=auto)\n"
                "  --force-ns 0|1             pin DMAGO NS bit (default try both)\n"
                "  --allow-channel-kill       LAB ONLY DBGINST KILL (default OFF)\n"
                "  --restage                   reload staging every DMA iter (default off)\n"
                "  --verify-each               memcmp bank vs src each DMA iter (slow)\n"
                "  --overlap-burn-ms MS --no-pl330 --host-copy --legacy-base\n"
                "PL330 scratch 0x30600000 staging 0x30601000; Option-C banks default.\n");
            return 0;
        }
    }
    if (len < 64 || loops < 1 || (bank != 0 && bank != 1 && phys_override == 0)) {
        std::fprintf(stderr, "bad args\n");
        return 2;
    }
    if (len > misterplex::kPl330StagingBytes) {
        std::fprintf(stderr, "len exceeds PL330 staging (%u)\n",
                     misterplex::kPl330StagingBytes);
        return 2;
    }

    const uint32_t phys_base =
        use_option_c ? misterplex::kPlex720pDdrFramePhysBase : misterplex::kDdrFramePhysBase;
    const uint32_t stride = use_option_c ? misterplex::kPlex720pYuv420pBankStride
                                         : misterplex::kPlex480pYuv420pBankStride;
    const uint32_t bank_phys =
        phys_override ? phys_override : (phys_base + static_cast<uint32_t>(bank) * stride);

    std::printf("PRE P1 real_or_honest_fail P2 ceiling P3 content_ok_frac P4 usleep_poll "
                "P5 bank_sweep_standdown\n");
    std::printf("INSTRUMENT: DMAGO=linux_manager_encode; require saw_executing; "
                "ok=content_match; fail if >%.1f GB/s\n",
                misterplex::kPl330MaxPlausibleThroughputGBs);
    std::printf("len=%zu loops=%d bank=%d burn_ms=%.3f duration_s=%.3f dma_only=%d "
                "bank_sweep=%d restage=%d host_only=%d option_c=%d\n",
                len, loops, bank, burn_ms, duration_s, dma_only ? 1 : 0, bank_sweep ? 1 : 0,
                restage_each ? 1 : 0, host_only ? 1 : 0, use_option_c ? 1 : 0);
    std::printf("bank_phys=0x%08x pl330_scratch=0x%08x pl330_staging=0x%08x\n", bank_phys,
                misterplex::kPl330ProgScratchPhys, misterplex::kPl330StagingPhys);
    std::printf("abi_overlap_option_c=%d mmap_flags=O_RDWR|O_SYNC\n",
                misterplex::pl330AbiOverlapsOptionCBanks(misterplex::kPl330AbiRegionPhys,
                                                        misterplex::kPl330AbiRegionBytes)
                    ? 1
                    : 0);

    std::vector<uint8_t> src(len);
    fill(src.data(), len);

    // -------- bank-sweep: serial only, several phys (anomaly chase) --------
    if (bank_sweep) {
        // Parent 2026-08-03: stand down — prior table contaminated by rogue awk;
        // same address read 20.5 and 25.6 ms in one run. Do not emit numbers.
        std::printf("bank_sweep=STAND_DOWN parent_contaminated_by_awk\n");
        std::printf("ddr_pl330_ingest_bench: DONE true_rc=0\n");
        return 0;
    }

    // -------- map destination bank --------
    MapWin win;
    uint8_t* dst = nullptr;
    std::vector<uint8_t> host_dst;
    if (host_only) {
        host_dst.resize(len + 64);
        dst = host_dst.data();
    } else {
        std::string err;
        if (!mapWindow(win, bank_phys & ~0xFFFu, static_cast<size_t>(stride) * 2u + 0x1000u,
                       &err)) {
            std::fprintf(stderr, "mmap bank failed: %s — try --host-copy\n", err.c_str());
            return 1;
        }
        dst = win.ptrAt(bank_phys);
        if (!dst) {
            std::fprintf(stderr, "bank_phys out of map\n");
            return 1;
        }
    }

    double serial_ms = -1.0;
    if (!dma_only) {
        serial_ms = timeSerialCopy(dst, src.data(), len, loops);
        std::printf("serial_cpu_copy_ms_per_frame=%.3f used_dma=0\n", serial_ms);

        if (burn_ms > 0.0) {
            std::atomic<int> stop{0};
            BurnArg ba{&stop};
            pthread_t th;
            pthread_create(&th, nullptr, burnThread, &ba);
            usleep(1000);
            const double wall = timeSerialCopy(dst, src.data(), len, loops);
            stop.store(1);
            pthread_join(th, nullptr);
            std::printf("copy_under_burn_ms_per_frame=%.3f serial_ms=%.3f inflate_ms=%.3f "
                        "burn_peer_requested_ms=%.3f\n",
                        wall, serial_ms, wall - serial_ms, burn_ms);
        }
    } else {
        std::printf("serial_cpu_copy_ms_per_frame=skipped dma_only=1\n");
    }

    // -------- PL330 --------
    if (do_pl330 && !host_only) {
        misterplex::Pl330Mem2Mem dma;
        std::string err;
        if (!dma.open(&err)) {
            std::printf("pl330_open=FAIL detail=%s\n", err.c_str());
        } else {
            auto pr = dma.probe();
            std::printf("pl330_probe ok=%d hw=%d cr0=0x%08x dsr=0x%08x detail=%s\n",
                        pr.ok ? 1 : 0, pr.hardware_present ? 1 : 0, pr.cr0, pr.dsr,
                        pr.detail.c_str());
            std::printf("pl330_crd=0x%08x mfifo_words=%u fsrd=0x%08x fsrc=0x%08x ftrd=0x%08x\n",
                        pr.crd, pr.mfifo_words, pr.fsrd, pr.fsrc, pr.ftrd);
            std::printf("pl330_scratch_mapped phys=0x%08x virt=%p staging_phys=0x%08x\n",
                        dma.progScratchPhys(), dma.progScratchVirt(), dma.stagingPhys());

            unsigned burst = 0;
            uint8_t prog[64];
            const unsigned mfifo_cap = pr.mfifo_words == 0 ? 1u
                : (pr.mfifo_words < 16u ? pr.mfifo_words : 16u);
            uint32_t ccr_preview = 0;
            const bool ns_ccr_preview = (force_ns != 0u);
            const size_t pn = misterplex::pl330BuildMem2MemProgramAuto(
                prog, sizeof(prog), dma.stagingPhys(), bank_phys, len, &burst, mfifo_cap,
                force_burst, ns_ccr_preview, &ccr_preview);
            std::printf("pl330_ccr=0x%08x ns_ccr=%u (AxPROT NS bits set when ns_ccr=1)\n",
                        ccr_preview, ns_ccr_preview ? 1u : 0u);
            std::printf("pl330_encode_auto ok=%d prog_bytes=%zu burst=%u\n", pn ? 1 : 0, pn,
                        burst);
            if (pn) {
                std::printf("pl330_prog_disasm (cpc stop at +6 was 2nd insn):\n");
                for (size_t off = 0; off < pn; ) {
                    char line[128];
                    const size_t step =
                        misterplex::pl330DisasmProgramLine(prog, pn, off, line, sizeof(line));
                    if (step == 0)
                        break;
                    std::printf("  %s\n", line);
                    off += step;
                }
                // Highlight byte at +6
                if (pn > 6)
                    std::printf("pl330_prog_byte_at_plus6=0x%02x (expect DMAMOV=0xbc)\n", prog[6]);
            }

            // Unique payload so a no-op transfer cannot match by accident.
            for (size_t i = 0; i < len; ++i)
                src[i] = static_cast<uint8_t>((i * 131u) ^ 0x5Au);
            // Embed a marker the verifier can spot at offset 0.
            src[0] = 0xD3;
            src[1] = 0x30;
            src[2] = 0xA1;
            src[3] = 0x11;

            if (!dma.loadStaging(src.data(), len, &err)) {
                std::printf("pl330_staging_load=FAIL detail=%s\n", err.c_str());
            } else {
                misterplex::Pl330Mem2MemRequest req;
                req.src_phys = dma.stagingPhys();
                req.dst_phys = bank_phys;
                req.len = len;
                req.channel = 0;
                req.force_burst = force_burst;
                req.force_ns = force_ns;
                req.allow_channel_kill = allow_channel_kill;
                std::printf("pl330_src_phys=0x%08x dst_phys=0x%08x len=%zu\n", req.src_phys,
                            req.dst_phys, req.len);
                std::printf("pl330_dbginst0_go_ns1=0x%08x (linux manager encode)\n",
                            misterplex::pl330DbgInst0GoManager(0, 1));
                std::printf("poll_mode=usleep_100us (no tight spin)\n");
                std::printf("min_plausible_ms=%.3f (ceiling %.1f GB/s)\n",
                            misterplex::pl330MinPlausibleMsForBytes(len),
                            misterplex::kPl330MaxPlausibleThroughputGBs);

                // Poison destination so idle/false-complete cannot content-match.
                std::memset(dst, 0xA5, len);
                __sync_synchronize();

                auto tr0 = dma.transferBlocking(req, 2000);
                std::printf("pl330_transfer ok=%d started=%d saw_exec=%d ch=%d ns=%u "
                            "burst=%u dbginst0=0x%08x csr=0x%08x cpc=0x%08x detail=%s\n",
                            tr0.ok ? 1 : 0, tr0.channel_started ? 1 : 0,
                            tr0.saw_executing ? 1 : 0, tr0.channel_used, tr0.ns_used,
                            tr0.burst_used, tr0.dbginst0, tr0.csr_final, tr0.cpc_final,
                            tr0.detail.c_str());
                char ftr_bits[160];
                misterplex::pl330FtrChannelBitsStr(tr0.ftr_ch, ftr_bits, sizeof(ftr_bits));
                std::printf("pl330_fault_regs fsrd=0x%08x fsrc=0x%08x ftrd=0x%08x "
                            "FTR0=0x%08x ftr_tag=%s ftr_bits=%s csr_state=%s cns=%u\n",
                            tr0.fsrd, tr0.fsrc, tr0.ftrd, tr0.ftr_ch,
                            misterplex::pl330FtrChannelTag(tr0.ftr_ch), ftr_bits,
                            misterplex::pl330CsrStateTag(tr0.csr_final),
                            (tr0.csr_final >> 21) & 1u);
                std::printf("pl330_cpc=0x%08x scratch=0x%08x cpc_off=%d\n", tr0.cpc_final,
                            dma.progScratchPhys(),
                            tr0.cpc_final ? (int)(tr0.cpc_final - dma.progScratchPhys()) : -1);
                std::printf("pl330_prog_pc=0x%08x mfifo_words=%u force_burst=%u force_ns=%u\n",
                            dma.progScratchPhys(), tr0.mfifo_words, force_burst,
                            force_ns == 0xFFu ? 99u : force_ns);

                const bool sample0 = misterplex::pl330BufferSamplesMatch(dst, src.data(), len);
                const bool full0 = misterplex::pl330BufferFullMatch(dst, src.data(), len);
                std::printf("pl330_content_sample_match=%d full_match=%d "
                            "(ok_frac requires content, not CSR alone)\n",
                            sample0 ? 1 : 0, full0 ? 1 : 0);

                if (!tr0.ok || !tr0.saw_executing) {
                    std::printf("pl330_verdict=NO_REAL_DMA detail=%s\n", tr0.detail.c_str());
                    std::printf("NOTE: prior 23.8ms/0.01ms figures INVALID (false complete). "
                                "Route A concurrency unmeasured until content-verified DMA.\n");
                    dma.close();
                    win.close();
                    std::printf("ddr_pl330_ingest_bench: DONE true_rc=0 (honest_no_dma)\n");
                    return 0;
                }
                if (!full0) {
                    std::printf("pl330_verdict=EXEC_BUT_CONTENT_MISMATCH "
                                "saw_exec=1 but bank bytes != staging\n");
                    dma.close();
                    win.close();
                    std::printf("ddr_pl330_ingest_bench: DONE true_rc=1\n");
                    return 1;
                }

                // Timed loop: count only content-verified frames.
                // Poison ONLY sentinel windows (not full 1.38MB) so the instrument
                // itself is not a DDR writer competing with DMA/decode.
                // Full-bank memcmp every 8th frame (+ first). --verify-each → every frame.
                int attempts = 0;
                int verified = 0;
                int content_fail = 0;
                int csr_fail = 0;
                double pure_dma_ms_sum = 0.0;
                dma.loadStaging(src.data(), len);
                const size_t sn = len < 64 ? len : 64;
                const size_t mid = len / 2;
                auto poison_sentinels = [&]() {
                    std::memset(dst, 0xA5, sn);
                    std::memset(dst + (len - sn), 0xA5, sn);
                    if (len > 128)
                        std::memset(dst + mid, 0xA5, sn);
                    __sync_synchronize();
                };
                auto sentinels_match = [&]() -> bool {
                    return misterplex::pl330BufferSamplesMatch(dst, src.data(), len);
                };

                struct rusage ru0 {};
                getrusage(RUSAGE_SELF, &ru0);
                const double t0 = nowSec();
                auto one_verified = [&](int seq) -> bool {
                    if (restage_each)
                        dma.loadStaging(src.data(), len);
                    poison_sentinels();
                    const double td0 = nowSec();
                    auto tr = dma.transferBlocking(req, 2000);
                    const double td1 = nowSec();
                    ++attempts;
                    if (!tr.ok || !tr.saw_executing) {
                        ++csr_fail;
                        return false;
                    }
                    pure_dma_ms_sum += (td1 - td0) * 1000.0;
                    const bool do_full = verify_each || (seq % 8) == 0;
                    const bool match =
                        do_full ? misterplex::pl330BufferFullMatch(dst, src.data(), len)
                                : sentinels_match();
                    if (!match) {
                        ++content_fail;
                        return false;
                    }
                    ++verified;
                    return true;
                };

                if (duration_s > 0.0) {
                    const double deadline = t0 + duration_s;
                    int seq = 0;
                    while (nowSec() < deadline) {
                        one_verified(seq++);
                    }
                } else {
                    for (int i = 0; i < loops; ++i)
                        one_verified(i);
                }
                const double wall_s = nowSec() - t0;
                struct rusage ru1 {};
                getrusage(RUSAGE_SELF, &ru1);
                const double cpu_s =
                    (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) +
                    (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6 +
                    (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) +
                    (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;

                if (verified <= 0) {
                    std::printf("pl330_verdict=ZERO_VERIFIED attempts=%d csr_fail=%d "
                                "content_fail=%d\n",
                                attempts, csr_fail, content_fail);
                    dma.close();
                    win.close();
                    std::printf("ddr_pl330_ingest_bench: DONE true_rc=1\n");
                    return 1;
                }

                // Primary metric: pure transferBlocking wall / verified (no poison).
                const double dma_ms = pure_dma_ms_sum / static_cast<double>(verified);
                // Secondary: whole-loop wall / verified (includes sentinel poison+check).
                const double loop_ms = (wall_s * 1000.0) / static_cast<double>(verified);
                const double gbs = misterplex::pl330ThroughputGBs(len, dma_ms);
                const double fps = wall_s > 0 ? verified / wall_s : 0;
                const double ok_frac =
                    attempts ? static_cast<double>(verified) / attempts : 0.0;
                std::printf("pl330_dma_ms_per_frame=%.3f (pure transferBlocking/verified)\n",
                            dma_ms);
                std::printf("pl330_loop_ms_per_frame=%.3f (wall includes sentinel "
                            "poison+verify)\n",
                            loop_ms);
                std::printf("verified_frames=%d attempts=%d ok_frac=%.3f content_fail=%d "
                            "csr_fail=%d\n",
                            verified, attempts, ok_frac, content_fail, csr_fail);
                std::printf("pl330_throughput_GBs=%.3f sustained_verified_fps=%.2f wall_s=%.3f "
                            "cpu_time_s=%.3f cpu_over_wall=%.3f\n",
                            gbs, fps, wall_s, cpu_s, wall_s > 0 ? cpu_s / wall_s : -1);
                if (serial_ms > 0)
                    std::printf("dma_minus_serial=%.3f serial_ms=%.3f\n", dma_ms - serial_ms,
                                serial_ms);

                if (misterplex::pl330ThroughputImpossible(len, dma_ms)) {
                    std::printf("INSTRUMENT_FAIL throughput=%.3f GB/s exceeds ceiling %.1f "
                                "GB/s (min_ms=%.3f measured_ms=%.3f) — refusing fiction\n",
                                gbs, misterplex::kPl330MaxPlausibleThroughputGBs,
                                misterplex::pl330MinPlausibleMsForBytes(len), dma_ms);
                    dma.close();
                    win.close();
                    std::printf("ddr_pl330_ingest_bench: DONE true_rc=2\n");
                    return 2;
                }
                std::printf("pl330_verdict=VERIFIED_REAL_DMA\n");
                std::printf("NOTE: cpu_over_wall near 1.0 means completion wait still "
                            "burns a core — Route A off-CPU claim weak even if DMA real.\n");
            }
            dma.close();
        }
    }

    if (!dma_only && serial_ms > 0) {
        const double dec = misterplex::kParentDecode720pProduct2600kMs;
        std::printf("budget_serial_product_fits=%d budget_overlap_max_product_fits=%d "
                    "decode_ms=%.3f copy_ms_measured=%.3f\n",
                    misterplex::ddrIngestSerialDecodePlusCopyFits(dec, serial_ms) ? 1 : 0,
                    misterplex::ddrIngestOverlapMaxFits(dec, serial_ms) ? 1 : 0, dec, serial_ms);
    }

    win.close();
    std::printf("ddr_pl330_ingest_bench: DONE true_rc=0\n");
    return 0;
}
